#include "Cloud/CloudTableWriter.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "Cloud/CloudValidation.h"
#include "glog/logging.h"

namespace dbplay {

namespace {

bool IsSafeRelativeKey(const std::string &key, const std::string &required_prefix) {
  if (key.size() <= required_prefix.size() || key.compare(0, required_prefix.size(), required_prefix) != 0 ||
      key.back() == '/') {
    return false;
  }
  size_t begin = 0;
  while (begin < key.size()) {
    const size_t end = key.find('/', begin);
    const std::string component = key.substr(begin, end == std::string::npos ? end : end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return true;
}

std::string NormalizePrefix(std::string prefix) {
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.pop_back();
  }
  if (prefix.empty()) {
    throw std::invalid_argument("CloudTableWriter: file_prefix is empty");
  }
  return prefix;
}

}  // namespace

CloudTableWriter::CloudTableWriter(TableDescriptor table, std::shared_ptr<IStorage> files,
                                   std::shared_ptr<IMetadataStore> metadata, std::shared_ptr<IFileFormat> wal_format,
                                   std::shared_ptr<IObjectKeyGenerator> keys,
                                   std::shared_ptr<IBatchCommitResolver> batches, size_t max_publish_attempts)
    : table_(std::move(table)),
      files_(std::move(files)),
      wal_format_(std::move(wal_format)),
      keys_(std::move(keys)),
      batches_(std::move(batches)),
      current_state_store_(table_.table_id, table_.metadata_prefix, std::move(metadata)),
      max_publish_attempts_(max_publish_attempts) {
  table_.file_prefix = NormalizePrefix(std::move(table_.file_prefix));
  if (files_ == nullptr || wal_format_ == nullptr || keys_ == nullptr || batches_ == nullptr ||
      max_publish_attempts_ == 0 || !SchemasEqual(table_.schema, wal_format_->schema())) {
    throw std::invalid_argument("CloudTableWriter: dependency is null or retry limit is zero");
  }
}

CloudWriterStartResult CloudTableWriter::Start() {
  if (current_state_.has_value()) {
    return CloudWriterStartResult{CloudWriterStartCode::Started, current_state_->current_state.writer_epoch};
  }

  auto current_state = current_state_store_.Load();
  if (!current_state.has_value()) {
    switch (current_state_store_.Initialize(current_state_store_.NewCurrentTableState())) {
      case InitializeTableResult::Created:
      case InitializeTableResult::AlreadyExists:
        break;
      case InitializeTableResult::RetryableConflict:
        return CloudWriterStartResult{CloudWriterStartCode::RetryableConflict, 0};
      case InitializeTableResult::InvalidState:
        return CloudWriterStartResult{CloudWriterStartCode::InvalidTable, 0};
    }
    current_state = current_state_store_.Load();
    if (!current_state.has_value()) {
      return CloudWriterStartResult{CloudWriterStartCode::RetryableConflict, 0};
    }
  }

  VersionedCurrentTableState acquired;
  switch (current_state_store_.AcquireWriter(*current_state, &acquired)) {
    case AcquireWriterResult::Acquired:
      current_state_ = std::move(acquired);
      LOG(INFO) << "CloudTableWriter: table=" << table_.table_id
                << " started as writer epoch=" << current_state_->current_state.writer_epoch
                << " committed_cursor=" << current_state_->current_state.committed_cursor;
      return CloudWriterStartResult{CloudWriterStartCode::Started, current_state_->current_state.writer_epoch};
    case AcquireWriterResult::StaleVersion:
      return CloudWriterStartResult{CloudWriterStartCode::Contended, 0};
    case AcquireWriterResult::RetryableConflict:
      return CloudWriterStartResult{CloudWriterStartCode::RetryableConflict, 0};
    case AcquireWriterResult::EpochExhausted:
    case AcquireWriterResult::InvalidState:
      return CloudWriterStartResult{CloudWriterStartCode::InvalidTable, 0};
  }
  throw std::logic_error("CloudTableWriter: unknown acquire result");
}

CloudImportResult CloudTableWriter::Import(const CloudImportBatch &batch) {
  if (!current_state_.has_value()) {
    return Result(CloudImportCode::NotStarted);
  }
  if (!IsValidBatch(batch) || current_state_->current_state.committed_cursor == std::numeric_limits<uint64_t>::max()) {
    return Result(CloudImportCode::InvalidRequest);
  }
  switch (batches_->Lookup(table_, *current_state_, batch.batch_ids)) {
    case BatchCommitStatus::Committed:
      return Result(CloudImportCode::AlreadyCommitted);
    case BatchCommitStatus::Partial:
      return Result(CloudImportCode::InvalidRequest);
    case BatchCommitStatus::NotCommitted:
      break;
  }

  const std::string wal_key = keys_->NewWalKey(table_.table_id);
  std::string wal_path;
  try {
    wal_path = ResolveWalKey(wal_key);
  } catch (const std::invalid_argument &) {
    return Result(CloudImportCode::InvalidRequest);
  }

  // Persistence: write the WAL object durably BEFORE any attempt to publish it.
  // Until the CURRENT CAS below references it, this object is an invisible
  // orphan (reclaimed later by GC), so writing it first is always safe.
  auto writer = wal_format_->OpenWriter(*files_, wal_path);
  if (writer == nullptr) {
    throw std::runtime_error("CloudTableWriter: WAL format returned a null writer");
  }
  for (const auto &chunk : batch.chunks) {
    writer->Write(chunk);
  }
  writer->Close();
  VLOG(1) << "CloudTableWriter: wrote WAL " << wal_path << " epoch=" << current_state_->current_state.writer_epoch;

  VersionedCurrentTableState expected = *current_state_;
  CommitRecord record;
  record.table_id = table_.table_id;
  record.writer_epoch = expected.current_state.writer_epoch;
  record.wal_files = {wal_path};
  record.batch_ids = batch.batch_ids;
  std::string commit_key = keys_->NewCommitKey(table_.table_id);

  // Publish retry loop. Each terminal case returns; the break cases fall through
  // to retry with adjusted state: CommitKeyCollision mints a fresh key,
  // RetryableConflict re-runs verbatim (idempotent), RebaseRequired re-anchors
  // the same WAL onto the new commit head with a bumped cursor. Fenced stops.
  for (size_t attempt = 0; attempt < max_publish_attempts_; ++attempt) {
    record.first_cursor = expected.current_state.committed_cursor + 1;
    record.last_cursor = record.first_cursor;
    record.parent_commit_key = expected.current_state.latest_commit_key;

    VersionedCurrentTableState published;
    switch (current_state_store_.PublishWal(expected, commit_key, record, &published)) {
      case PublishWalResult::Committed:
        current_state_ = std::move(published);
        return Result(CloudImportCode::Committed);
      case PublishWalResult::AlreadyCommitted:
        current_state_ = std::move(published);
        return Result(CloudImportCode::AlreadyCommitted);
      case PublishWalResult::Fenced:
        if (const auto latest = current_state_store_.Load();
            latest.has_value() && batches_->Lookup(table_, *latest, batch.batch_ids) == BatchCommitStatus::Committed) {
          current_state_ = *latest;
          return Result(CloudImportCode::AlreadyCommitted);
        }
        LOG(WARNING) << "CloudTableWriter: table=" << table_.table_id
                     << " import fenced at epoch=" << record.writer_epoch << "; writer stops";
        current_state_.reset();
        return Result(CloudImportCode::Fenced);
      case PublishWalResult::InvalidCommit:
        return Result(CloudImportCode::InvalidRequest);
      case PublishWalResult::CommitKeyCollision:
        commit_key = keys_->NewCommitKey(table_.table_id);
        break;
      case PublishWalResult::RetryableConflict:
        break;  // Retry the identical operation to resolve an unknown outcome.
      case PublishWalResult::RebaseRequired: {
        const auto latest = current_state_store_.Load();
        if (!latest.has_value()) {
          return Result(CloudImportCode::RetryableConflict);
        }
        if (latest->current_state.writer_epoch != record.writer_epoch) {
          if (batches_->Lookup(table_, *latest, batch.batch_ids) == BatchCommitStatus::Committed) {
            current_state_ = *latest;
            return Result(CloudImportCode::AlreadyCommitted);
          }
          current_state_.reset();
          return Result(CloudImportCode::Fenced);
        }
        switch (batches_->Lookup(table_, *latest, batch.batch_ids)) {
          case BatchCommitStatus::Committed:
            current_state_ = *latest;
            return Result(CloudImportCode::AlreadyCommitted);
          case BatchCommitStatus::Partial:
            return Result(CloudImportCode::InvalidRequest);
          case BatchCommitStatus::NotCommitted:
            break;
        }
        expected = *latest;
        commit_key = keys_->NewCommitKey(table_.table_id);
        break;
      }
    }
  }
  // Exhausted all attempts without a definitive commit. If the batch turns out
  // to already be committed (a prior attempt won unobserved), report success;
  // otherwise the caller must retry.
  const auto latest = current_state_store_.Load();
  if (latest.has_value() && batches_->Lookup(table_, *latest, batch.batch_ids) == BatchCommitStatus::Committed) {
    current_state_ = *latest;
    return Result(CloudImportCode::AlreadyCommitted);
  }
  LOG(WARNING) << "CloudTableWriter: table=" << table_.table_id << " import exhausted " << max_publish_attempts_
               << " publish attempts; returning RetryableConflict";
  return Result(CloudImportCode::RetryableConflict);
}

bool CloudTableWriter::IsValidBatch(const CloudImportBatch &batch) const {
  if (batch.batch_ids.empty() || batch.chunks.empty()) {
    return false;
  }
  std::unordered_set<std::string> unique_ids;
  for (const auto &batch_id : batch.batch_ids) {
    if (batch_id.empty() || !unique_ids.insert(batch_id).second) {
      return false;
    }
  }
  for (const auto &chunk : batch.chunks) {
    if (!IsFullSchemaChunk(chunk, table_.schema)) {
      return false;
    }
  }
  return true;
}

std::string CloudTableWriter::ResolveWalKey(const std::string &relative_key) const {
  if (!IsSafeRelativeKey(relative_key, "wal/")) {
    throw std::invalid_argument("CloudTableWriter: invalid WAL key");
  }
  return table_.file_prefix + "/" + relative_key;
}

CloudImportResult CloudTableWriter::Result(CloudImportCode code) const {
  CloudImportResult result;
  result.code = code;
  if (current_state_.has_value()) {
    result.writer_epoch = current_state_->current_state.writer_epoch;
    result.committed_cursor = current_state_->current_state.committed_cursor;
  }
  return result;
}

}  // namespace dbplay
