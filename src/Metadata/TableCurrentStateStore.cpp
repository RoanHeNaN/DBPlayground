#include "Metadata/TableCurrentStateStore.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "Metadata/TableMetadataCodec.h"
#include "glog/logging.h"

namespace dbplay {
namespace {

std::string NormalizePrefix(std::string prefix) {
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.pop_back();
  }
  if (prefix.empty()) {
    throw std::invalid_argument("TableCurrentStateStore: metadata_prefix is empty");
  }
  return prefix;
}

}  // namespace

TableCurrentStateStore::TableCurrentStateStore(std::string table_id, std::string metadata_prefix,
                                               std::shared_ptr<IMetadataStore> metadata)
    : table_id_(std::move(table_id)),
      metadata_prefix_(NormalizePrefix(std::move(metadata_prefix))),
      current_key_(metadata_prefix_ + "/CURRENT"),
      metadata_(std::move(metadata)) {
  if (table_id_.empty()) {
    throw std::invalid_argument("TableCurrentStateStore: table_id is empty");
  }
  if (metadata_ == nullptr) {
    throw std::invalid_argument("TableCurrentStateStore: metadata store is null");
  }
}

CurrentTableState TableCurrentStateStore::NewCurrentTableState() const {
  CurrentTableState state;
  state.table_id = table_id_;
  return state;
}

std::optional<VersionedCurrentTableState> TableCurrentStateStore::Load() const {
  auto value = metadata_->Get(current_key_);
  if (!value.has_value()) {
    return std::nullopt;
  }
  CurrentTableState state = TableMetadataCodec::DecodeCurrentTableState(Slice(value->value));
  if (state.table_id != table_id_) {
    throw std::runtime_error("TableCurrentStateStore: CURRENT belongs to another table");
  }
  return VersionedCurrentTableState{std::move(state), std::move(value->version)};
}

InitializeTableResult TableCurrentStateStore::Initialize(const CurrentTableState &initial,
                                                         VersionedCurrentTableState *created) {
  if (!IsValidInitialState(initial)) {
    return InitializeTableResult::InvalidState;
  }

  const std::string encoded = TableMetadataCodec::EncodeCurrentTableState(initial);
  MetadataVersion version;
  switch (metadata_->PutIfAbsent(current_key_, Slice(encoded), &version)) {
    case ConditionalWriteResult::Applied:
      if (created != nullptr) {
        *created = VersionedCurrentTableState{initial, std::move(version)};
      }
      return InitializeTableResult::Created;
    case ConditionalWriteResult::PreconditionFailed:
      return InitializeTableResult::AlreadyExists;
    case ConditionalWriteResult::RetryableConflict:
      return InitializeTableResult::RetryableConflict;
  }
  throw std::logic_error("TableCurrentStateStore: unknown conditional write result");
}

AcquireWriterResult TableCurrentStateStore::AcquireWriter(const VersionedCurrentTableState &expected,
                                                          VersionedCurrentTableState *acquired) {
  if (expected.current_state.format_version != CurrentTableState::kFormatVersion ||
      expected.current_state.table_id != table_id_ ||
      expected.current_state.compacted_cursor > expected.current_state.committed_cursor) {
    return AcquireWriterResult::InvalidState;
  }
  if (expected.current_state.current_state_version == std::numeric_limits<uint64_t>::max() ||
      expected.current_state.writer_epoch == std::numeric_limits<uint64_t>::max()) {
    return AcquireWriterResult::EpochExhausted;
  }

  // Key state transition: bump the monotonic writer_epoch. The CAS on CURRENT
  // is what fences any prior writer -- once this lands, an older epoch can no
  // longer publish (its PublishWal CAS will fail and classify as Fenced). A
  // lost CAS here means someone else moved CURRENT first; we are Contended.
  CurrentTableState next = expected.current_state;
  ++next.current_state_version;
  ++next.writer_epoch;
  const uint64_t new_epoch = next.writer_epoch;
  const std::string encoded = TableMetadataCodec::EncodeCurrentTableState(next);
  MetadataVersion version;
  switch (metadata_->CompareExchange(current_key_, expected.current_metadata_version, Slice(encoded), &version)) {
    case ConditionalWriteResult::Applied:
      LOG(INFO) << "AcquireWriter: table=" << table_id_ << " acquired writer_epoch=" << new_epoch << " (fences epoch<"
                << new_epoch << ")";
      if (acquired != nullptr) {
        *acquired = VersionedCurrentTableState{std::move(next), std::move(version)};
      }
      return AcquireWriterResult::Acquired;
    case ConditionalWriteResult::PreconditionFailed:
      VLOG(1) << "AcquireWriter: table=" << table_id_ << " contended at epoch=" << expected.current_state.writer_epoch;
      return AcquireWriterResult::StaleVersion;
    case ConditionalWriteResult::RetryableConflict:
      return AcquireWriterResult::RetryableConflict;
  }
  throw std::logic_error("TableCurrentStateStore: unknown conditional write result");
}

PublishWalResult TableCurrentStateStore::PublishWal(const VersionedCurrentTableState &expected,
                                                    const std::string &commit_key, const CommitRecord &record,
                                                    VersionedCurrentTableState *published) {
  if (record.format_version != CommitRecord::kFormatVersion || record.table_id != table_id_ ||
      record.writer_epoch == 0 || record.writer_epoch != expected.current_state.writer_epoch ||
      record.first_cursor == 0 || expected.current_state.committed_cursor == std::numeric_limits<uint64_t>::max() ||
      record.first_cursor != expected.current_state.committed_cursor + 1 || record.first_cursor > record.last_cursor ||
      record.parent_commit_key != expected.current_state.latest_commit_key || record.wal_files.empty() ||
      record.batch_ids.empty()) {
    return PublishWalResult::InvalidCommit;
  }

  std::string resolved_key;
  try {
    resolved_key = ResolveRelativeKey(commit_key, "commit/");
  } catch (const std::invalid_argument &) {
    return PublishWalResult::InvalidCommit;
  }

  std::string encoded_record;
  try {
    encoded_record = TableMetadataCodec::EncodeCommitRecord(record);
  } catch (const std::invalid_argument &) {
    return PublishWalResult::InvalidCommit;
  }

  // Persistence step 1 of 2: write the immutable CommitRecord (idempotent via
  // PutIfAbsent). A key collision whose bytes differ is a real conflict; a key
  // collision whose bytes match is our own retry landing again -- treat as done.
  switch (metadata_->PutIfAbsent(resolved_key, Slice(encoded_record), nullptr)) {
    case ConditionalWriteResult::Applied:
      break;
    case ConditionalWriteResult::PreconditionFailed: {
      const auto existing = metadata_->Get(resolved_key);
      if (!existing.has_value()) {
        return PublishWalResult::RetryableConflict;
      }
      CommitRecord decoded;
      try {
        decoded = TableMetadataCodec::DecodeCommitRecord(Slice(existing->value));
      } catch (const std::invalid_argument &) {
        return PublishWalResult::CommitKeyCollision;
      }
      if (decoded != record) {
        return PublishWalResult::CommitKeyCollision;
      }
      break;
    }
    case ConditionalWriteResult::RetryableConflict:
      return PublishWalResult::RetryableConflict;
  }

  CurrentTableState next = expected.current_state;
  if (next.current_state_version == std::numeric_limits<uint64_t>::max()) {
    return PublishWalResult::InvalidCommit;
  }
  ++next.current_state_version;
  next.committed_cursor = record.last_cursor;
  next.latest_commit_key = commit_key;
  if (!IsValidTransition(expected.current_state, next)) {
    return PublishWalResult::InvalidCommit;
  }

  // Persistence step 2 of 2: the VISIBILITY POINT. Only this CAS on CURRENT
  // makes the commit observable; a durable WAL/CommitRecord not yet referenced
  // here is an invisible orphan. Winning the CAS is the commit.
  const std::string encoded_state = TableMetadataCodec::EncodeCurrentTableState(next);
  MetadataVersion version;
  switch (metadata_->CompareExchange(current_key_, expected.current_metadata_version, Slice(encoded_state), &version)) {
    case ConditionalWriteResult::Applied:
      LOG(INFO) << "PublishWal: table=" << table_id_ << " committed cursor=[" << record.first_cursor << ","
                << record.last_cursor << "] epoch=" << record.writer_epoch << " head=" << commit_key;
      if (published != nullptr) {
        *published = VersionedCurrentTableState{std::move(next), std::move(version)};
      }
      return PublishWalResult::Committed;
    case ConditionalWriteResult::RetryableConflict:
      return PublishWalResult::RetryableConflict;
    case ConditionalWriteResult::PreconditionFailed:
      break;  // CURRENT moved under us -- classify why below.
  }

  // CAS lost. Re-read CURRENT and classify the outcome; never blind-retry.
  const auto current_state = Load();
  if (!current_state.has_value()) {
    return PublishWalResult::RetryableConflict;
  }
  if (current_state->current_state.latest_commit_key == commit_key &&
      current_state->current_state.committed_cursor == record.last_cursor) {
    // Our own commit is already the head: a prior attempt's CAS actually landed.
    if (published != nullptr) {
      *published = *current_state;
    }
    return PublishWalResult::AlreadyCommitted;
  }
  if (current_state->current_state.writer_epoch != expected.current_state.writer_epoch) {
    // A newer epoch owns the table now -- we have been fenced.
    LOG(WARNING) << "PublishWal: table=" << table_id_ << " fenced: our epoch=" << expected.current_state.writer_epoch
                 << " current epoch=" << current_state->current_state.writer_epoch;
    return PublishWalResult::Fenced;
  }
  // Same epoch but the root moved (another commit of ours): rebase onto it.
  return PublishWalResult::RebaseRequired;
}

PublishCompactionResult TableCurrentStateStore::PublishCompaction(const VersionedCurrentTableState &expected,
                                                                  const std::string &compacted_data_manifest_key,
                                                                  const CompactedDataManifest &compacted_data_manifest,
                                                                  VersionedCurrentTableState *published) {
  // The compacted-data manifest and CURRENT are separate records, so
  // publishing is a two-step operation. PutIfAbsent makes the immutable
  // manifest idempotent; the subsequent CURRENT CAS is the sole visibility
  // point. Readers that still hold the old CURRENT version keep seeing the old
  // compacted-data set, while a reader of the new version sees the complete
  // manifest.
  if (expected.current_state.format_version != CurrentTableState::kFormatVersion ||
      expected.current_state.table_id != table_id_ ||
      compacted_data_manifest.format_version != CompactedDataManifest::kFormatVersion ||
      compacted_data_manifest.table_id != table_id_ || compacted_data_manifest.compacted_cursor == 0 ||
      compacted_data_manifest.compacted_cursor <= expected.current_state.compacted_cursor ||
      compacted_data_manifest.compacted_cursor > expected.current_state.committed_cursor ||
      compacted_data_manifest.compacted_data_files.empty()) {
    return PublishCompactionResult::InvalidCompactedDataManifest;
  }

  std::string resolved_key;
  try {
    resolved_key = ResolveRelativeKey(compacted_data_manifest_key, "manifest/");
  } catch (const std::invalid_argument &) {
    return PublishCompactionResult::InvalidCompactedDataManifest;
  }

  std::string encoded_compacted_data_manifest;
  try {
    encoded_compacted_data_manifest = TableMetadataCodec::EncodeCompactedDataManifest(compacted_data_manifest);
  } catch (const std::invalid_argument &) {
    return PublishCompactionResult::InvalidCompactedDataManifest;
  }
  switch (metadata_->PutIfAbsent(resolved_key, Slice(encoded_compacted_data_manifest), nullptr)) {
    case ConditionalWriteResult::Applied:
      break;
    case ConditionalWriteResult::PreconditionFailed: {
      const auto existing = metadata_->Get(resolved_key);
      if (!existing.has_value()) {
        return PublishCompactionResult::RetryableConflict;
      }
      CompactedDataManifest decoded;
      try {
        decoded = TableMetadataCodec::DecodeCompactedDataManifest(Slice(existing->value));
      } catch (const std::invalid_argument &) {
        return PublishCompactionResult::CompactedDataManifestKeyCollision;
      }
      if (decoded != compacted_data_manifest) {
        return PublishCompactionResult::CompactedDataManifestKeyCollision;
      }
      break;
    }
    case ConditionalWriteResult::RetryableConflict:
      return PublishCompactionResult::RetryableConflict;
  }

  CurrentTableState next = expected.current_state;
  if (next.current_state_version == std::numeric_limits<uint64_t>::max()) {
    return PublishCompactionResult::InvalidCompactedDataManifest;
  }
  ++next.current_state_version;
  next.compacted_cursor = compacted_data_manifest.compacted_cursor;
  next.compacted_data_manifest_key = compacted_data_manifest_key;
  if (!IsValidTransition(expected.current_state, next)) {
    return PublishCompactionResult::InvalidCompactedDataManifest;
  }

  // Visibility point for compaction: swap compacted_data_manifest_key and advance
  // compacted_cursor via CAS. Never fences a writer and never moves
  // committed_cursor; it only publishes already-committed rows into the
  // compacted-data set.
  const std::string encoded_state = TableMetadataCodec::EncodeCurrentTableState(next);
  MetadataVersion version;
  switch (metadata_->CompareExchange(current_key_, expected.current_metadata_version, Slice(encoded_state), &version)) {
    case ConditionalWriteResult::Applied:
      LOG(INFO) << "PublishCompaction: table=" << table_id_ << " compacted_cursor "
                << expected.current_state.compacted_cursor << "->" << compacted_data_manifest.compacted_cursor
                << " compacted_data_manifest=" << compacted_data_manifest_key;
      if (published != nullptr) {
        *published = VersionedCurrentTableState{std::move(next), std::move(version)};
      }
      return PublishCompactionResult::Published;
    case ConditionalWriteResult::RetryableConflict:
      return PublishCompactionResult::RetryableConflict;
    case ConditionalWriteResult::PreconditionFailed:
      break;  // CURRENT moved (a writer committed, or a rival compaction) -- reclassify below.
  }

  const auto current_state = Load();
  if (!current_state.has_value()) {
    return PublishCompactionResult::RetryableConflict;
  }
  if (current_state->current_state.compacted_data_manifest_key == compacted_data_manifest_key &&
      current_state->current_state.compacted_cursor == compacted_data_manifest.compacted_cursor) {
    if (published != nullptr) {
      *published = *current_state;
    }
    return PublishCompactionResult::AlreadyPublished;
  }
  if (current_state->current_state.compacted_cursor >= compacted_data_manifest.compacted_cursor) {
    if (published != nullptr) {
      *published = *current_state;
    }
    return PublishCompactionResult::AlreadyPublished;
  }
  return PublishCompactionResult::StaleVersion;
}

std::optional<CommitRecord> TableCurrentStateStore::LoadCommitRecord(const std::string &commit_key) const {
  const auto value = metadata_->Get(ResolveRelativeKey(commit_key, "commit/"));
  if (!value.has_value()) {
    return std::nullopt;
  }
  CommitRecord record = TableMetadataCodec::DecodeCommitRecord(Slice(value->value));
  if (record.table_id != table_id_) {
    throw std::runtime_error("TableCurrentStateStore: commit record belongs to another table");
  }
  return record;
}

std::optional<CompactedDataManifest> TableCurrentStateStore::LoadCompactedDataManifest(
    const std::string &compacted_data_manifest_key) const {
  const auto value = metadata_->Get(ResolveRelativeKey(compacted_data_manifest_key, "manifest/"));
  if (!value.has_value()) {
    return std::nullopt;
  }
  CompactedDataManifest compacted_data_manifest = TableMetadataCodec::DecodeCompactedDataManifest(Slice(value->value));
  if (compacted_data_manifest.table_id != table_id_) {
    throw std::runtime_error("TableCurrentStateStore: compacted data manifest belongs to another table");
  }
  return compacted_data_manifest;
}

bool TableCurrentStateStore::IsValidInitialState(const CurrentTableState &state) const {
  return state.format_version == CurrentTableState::kFormatVersion && state.table_id == table_id_ &&
         state.current_state_version == 0 && state.writer_epoch == 0 && state.committed_cursor == 0 &&
         state.compacted_cursor == 0 && state.compacted_data_manifest_key.empty() && state.latest_commit_key.empty();
}

bool TableCurrentStateStore::IsValidTransition(const CurrentTableState &previous, const CurrentTableState &next) const {
  if (previous.format_version != CurrentTableState::kFormatVersion ||
      next.format_version != CurrentTableState::kFormatVersion || previous.table_id != table_id_ ||
      next.table_id != table_id_) {
    return false;
  }
  if (previous.current_state_version == std::numeric_limits<uint64_t>::max() ||
      next.current_state_version != previous.current_state_version + 1) {
    return false;
  }
  if (next.writer_epoch < previous.writer_epoch || next.committed_cursor < previous.committed_cursor ||
      next.compacted_cursor < previous.compacted_cursor || next.compacted_cursor > next.committed_cursor) {
    return false;
  }
  return true;
}

std::string TableCurrentStateStore::ResolveRelativeKey(const std::string &relative_key,
                                                       const char *required_prefix) const {
  const size_t prefix_len = std::char_traits<char>::length(required_prefix);
  if (relative_key.size() <= prefix_len || relative_key.compare(0, prefix_len, required_prefix) != 0 ||
      relative_key.back() == '/') {
    throw std::invalid_argument("TableCurrentStateStore: invalid relative key");
  }
  size_t begin = 0;
  while (begin < relative_key.size()) {
    const size_t end = relative_key.find('/', begin);
    const std::string component = relative_key.substr(begin, end == std::string::npos ? end : end - begin);
    if (component.empty() || component == "." || component == "..") {
      throw std::invalid_argument("TableCurrentStateStore: invalid relative key");
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return metadata_prefix_ + "/" + relative_key;
}

}  // namespace dbplay
