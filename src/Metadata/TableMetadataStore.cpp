#include "Metadata/TableMetadataStore.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "Metadata/TableMetadataCodec.h"

namespace dbplay {
namespace {

std::string NormalizePrefix(std::string prefix) {
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.pop_back();
  }
  if (prefix.empty()) {
    throw std::invalid_argument("TableMetadataStore: metadata_prefix is empty");
  }
  return prefix;
}

}  // namespace

TableMetadataStore::TableMetadataStore(std::string table_id, std::string metadata_prefix,
                                       std::shared_ptr<IMetadataStore> metadata)
    : table_id_(std::move(table_id)),
      metadata_prefix_(NormalizePrefix(std::move(metadata_prefix))),
      current_key_(metadata_prefix_ + "/CURRENT"),
      metadata_(std::move(metadata)) {
  if (table_id_.empty()) {
    throw std::invalid_argument("TableMetadataStore: table_id is empty");
  }
  if (metadata_ == nullptr) {
    throw std::invalid_argument("TableMetadataStore: metadata store is null");
  }
}

TableState TableMetadataStore::NewTableState() const {
  TableState state;
  state.table_id = table_id_;
  return state;
}

std::optional<VersionedTableState> TableMetadataStore::Load() const {
  auto value = metadata_->Get(current_key_);
  if (!value.has_value()) {
    return std::nullopt;
  }
  TableState state = TableMetadataCodec::DecodeTableState(Slice(value->value));
  if (state.table_id != table_id_) {
    throw std::runtime_error("TableMetadataStore: CURRENT belongs to another table");
  }
  return VersionedTableState{std::move(state), std::move(value->version)};
}

InitializeTableResult TableMetadataStore::Initialize(const TableState &initial, VersionedTableState *created) {
  if (!IsValidInitialState(initial)) {
    return InitializeTableResult::InvalidState;
  }

  const std::string encoded = TableMetadataCodec::EncodeTableState(initial);
  MetadataVersion version;
  switch (metadata_->PutIfAbsent(current_key_, Slice(encoded), &version)) {
    case ConditionalWriteResult::Applied:
      if (created != nullptr) {
        *created = VersionedTableState{initial, std::move(version)};
      }
      return InitializeTableResult::Created;
    case ConditionalWriteResult::PreconditionFailed:
      return InitializeTableResult::AlreadyExists;
    case ConditionalWriteResult::RetryableConflict:
      return InitializeTableResult::RetryableConflict;
  }
  throw std::logic_error("TableMetadataStore: unknown conditional write result");
}

AcquireWriterResult TableMetadataStore::AcquireWriter(const VersionedTableState &expected,
                                                      VersionedTableState *acquired) {
  if (expected.state.format_version != TableState::kFormatVersion || expected.state.table_id != table_id_ ||
      expected.state.indexed_cursor > expected.state.committed_cursor) {
    return AcquireWriterResult::InvalidState;
  }
  if (expected.state.state_version == std::numeric_limits<uint64_t>::max() ||
      expected.state.writer_epoch == std::numeric_limits<uint64_t>::max()) {
    return AcquireWriterResult::EpochExhausted;
  }

  TableState next = expected.state;
  ++next.state_version;
  ++next.writer_epoch;
  const std::string encoded = TableMetadataCodec::EncodeTableState(next);
  MetadataVersion version;
  switch (metadata_->CompareExchange(current_key_, expected.metadata_version, Slice(encoded), &version)) {
    case ConditionalWriteResult::Applied:
      if (acquired != nullptr) {
        *acquired = VersionedTableState{std::move(next), std::move(version)};
      }
      return AcquireWriterResult::Acquired;
    case ConditionalWriteResult::PreconditionFailed:
      return AcquireWriterResult::StaleVersion;
    case ConditionalWriteResult::RetryableConflict:
      return AcquireWriterResult::RetryableConflict;
  }
  throw std::logic_error("TableMetadataStore: unknown conditional write result");
}

PublishWalResult TableMetadataStore::PublishWal(const VersionedTableState &expected, const std::string &commit_key,
                                                const CommitRecord &record, VersionedTableState *published) {
  if (record.format_version != CommitRecord::kFormatVersion || record.table_id != table_id_ ||
      record.writer_epoch == 0 || record.writer_epoch != expected.state.writer_epoch || record.first_cursor == 0 ||
      expected.state.committed_cursor == std::numeric_limits<uint64_t>::max() ||
      record.first_cursor != expected.state.committed_cursor + 1 || record.first_cursor > record.last_cursor ||
      record.parent_commit != expected.state.commit_head || record.wal_files.empty() || record.batch_ids.empty()) {
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

  TableState next = expected.state;
  if (next.state_version == std::numeric_limits<uint64_t>::max()) {
    return PublishWalResult::InvalidCommit;
  }
  ++next.state_version;
  next.committed_cursor = record.last_cursor;
  next.commit_head = commit_key;
  if (!IsValidTransition(expected.state, next)) {
    return PublishWalResult::InvalidCommit;
  }

  const std::string encoded_state = TableMetadataCodec::EncodeTableState(next);
  MetadataVersion version;
  switch (metadata_->CompareExchange(current_key_, expected.metadata_version, Slice(encoded_state), &version)) {
    case ConditionalWriteResult::Applied:
      if (published != nullptr) {
        *published = VersionedTableState{std::move(next), std::move(version)};
      }
      return PublishWalResult::Committed;
    case ConditionalWriteResult::RetryableConflict:
      return PublishWalResult::RetryableConflict;
    case ConditionalWriteResult::PreconditionFailed:
      break;
  }

  const auto current = Load();
  if (!current.has_value()) {
    return PublishWalResult::RetryableConflict;
  }
  if (current->state.commit_head == commit_key && current->state.committed_cursor == record.last_cursor) {
    if (published != nullptr) {
      *published = *current;
    }
    return PublishWalResult::AlreadyCommitted;
  }
  if (current->state.writer_epoch != expected.state.writer_epoch) {
    return PublishWalResult::Fenced;
  }
  return PublishWalResult::RebaseRequired;
}

PublishCompactionResult TableMetadataStore::PublishCompaction(const VersionedTableState &expected,
                                                              const std::string &manifest_key,
                                                              const BaseManifest &manifest,
                                                              VersionedTableState *published) {
  if (expected.state.format_version != TableState::kFormatVersion || expected.state.table_id != table_id_ ||
      manifest.format_version != BaseManifest::kFormatVersion || manifest.table_id != table_id_ ||
      manifest.indexed_cursor == 0 || manifest.indexed_cursor <= expected.state.indexed_cursor ||
      manifest.indexed_cursor > expected.state.committed_cursor || manifest.data_files.empty()) {
    return PublishCompactionResult::InvalidManifest;
  }

  std::string resolved_key;
  try {
    resolved_key = ResolveRelativeKey(manifest_key, "manifest/");
  } catch (const std::invalid_argument &) {
    return PublishCompactionResult::InvalidManifest;
  }

  std::string encoded_manifest;
  try {
    encoded_manifest = TableMetadataCodec::EncodeBaseManifest(manifest);
  } catch (const std::invalid_argument &) {
    return PublishCompactionResult::InvalidManifest;
  }
  switch (metadata_->PutIfAbsent(resolved_key, Slice(encoded_manifest), nullptr)) {
    case ConditionalWriteResult::Applied:
      break;
    case ConditionalWriteResult::PreconditionFailed: {
      const auto existing = metadata_->Get(resolved_key);
      if (!existing.has_value()) {
        return PublishCompactionResult::RetryableConflict;
      }
      BaseManifest decoded;
      try {
        decoded = TableMetadataCodec::DecodeBaseManifest(Slice(existing->value));
      } catch (const std::invalid_argument &) {
        return PublishCompactionResult::ManifestKeyCollision;
      }
      if (decoded != manifest) {
        return PublishCompactionResult::ManifestKeyCollision;
      }
      break;
    }
    case ConditionalWriteResult::RetryableConflict:
      return PublishCompactionResult::RetryableConflict;
  }

  TableState next = expected.state;
  if (next.state_version == std::numeric_limits<uint64_t>::max()) {
    return PublishCompactionResult::InvalidManifest;
  }
  ++next.state_version;
  next.indexed_cursor = manifest.indexed_cursor;
  next.base_manifest = manifest_key;
  if (!IsValidTransition(expected.state, next)) {
    return PublishCompactionResult::InvalidManifest;
  }

  const std::string encoded_state = TableMetadataCodec::EncodeTableState(next);
  MetadataVersion version;
  switch (metadata_->CompareExchange(current_key_, expected.metadata_version, Slice(encoded_state), &version)) {
    case ConditionalWriteResult::Applied:
      if (published != nullptr) {
        *published = VersionedTableState{std::move(next), std::move(version)};
      }
      return PublishCompactionResult::Published;
    case ConditionalWriteResult::RetryableConflict:
      return PublishCompactionResult::RetryableConflict;
    case ConditionalWriteResult::PreconditionFailed:
      break;
  }

  const auto current = Load();
  if (!current.has_value()) {
    return PublishCompactionResult::RetryableConflict;
  }
  if (current->state.base_manifest == manifest_key && current->state.indexed_cursor == manifest.indexed_cursor) {
    if (published != nullptr) {
      *published = *current;
    }
    return PublishCompactionResult::AlreadyPublished;
  }
  if (current->state.indexed_cursor >= manifest.indexed_cursor) {
    if (published != nullptr) {
      *published = *current;
    }
    return PublishCompactionResult::AlreadyPublished;
  }
  return PublishCompactionResult::StaleVersion;
}

std::optional<CommitRecord> TableMetadataStore::LoadCommitRecord(const std::string &commit_key) const {
  const auto value = metadata_->Get(ResolveRelativeKey(commit_key, "commit/"));
  if (!value.has_value()) {
    return std::nullopt;
  }
  CommitRecord record = TableMetadataCodec::DecodeCommitRecord(Slice(value->value));
  if (record.table_id != table_id_) {
    throw std::runtime_error("TableMetadataStore: commit record belongs to another table");
  }
  return record;
}

std::optional<BaseManifest> TableMetadataStore::LoadManifest(const std::string &manifest_key) const {
  const auto value = metadata_->Get(ResolveRelativeKey(manifest_key, "manifest/"));
  if (!value.has_value()) {
    return std::nullopt;
  }
  BaseManifest manifest = TableMetadataCodec::DecodeBaseManifest(Slice(value->value));
  if (manifest.table_id != table_id_) {
    throw std::runtime_error("TableMetadataStore: base manifest belongs to another table");
  }
  return manifest;
}

bool TableMetadataStore::IsValidInitialState(const TableState &state) const {
  return state.format_version == TableState::kFormatVersion && state.table_id == table_id_ &&
         state.state_version == 0 && state.writer_epoch == 0 && state.committed_cursor == 0 &&
         state.indexed_cursor == 0 && state.base_manifest.empty() && state.commit_head.empty();
}

bool TableMetadataStore::IsValidTransition(const TableState &previous, const TableState &next) const {
  if (previous.format_version != TableState::kFormatVersion || next.format_version != TableState::kFormatVersion ||
      previous.table_id != table_id_ || next.table_id != table_id_) {
    return false;
  }
  if (previous.state_version == std::numeric_limits<uint64_t>::max() ||
      next.state_version != previous.state_version + 1) {
    return false;
  }
  if (next.writer_epoch < previous.writer_epoch || next.committed_cursor < previous.committed_cursor ||
      next.indexed_cursor < previous.indexed_cursor || next.indexed_cursor > next.committed_cursor) {
    return false;
  }
  return true;
}

std::string TableMetadataStore::ResolveRelativeKey(const std::string &relative_key, const char *required_prefix) const {
  const size_t prefix_len = std::char_traits<char>::length(required_prefix);
  if (relative_key.size() <= prefix_len || relative_key.compare(0, prefix_len, required_prefix) != 0 ||
      relative_key.back() == '/') {
    throw std::invalid_argument("TableMetadataStore: invalid relative key");
  }
  size_t begin = 0;
  while (begin < relative_key.size()) {
    const size_t end = relative_key.find('/', begin);
    const std::string component = relative_key.substr(begin, end == std::string::npos ? end : end - begin);
    if (component.empty() || component == "." || component == "..") {
      throw std::invalid_argument("TableMetadataStore: invalid relative key");
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return metadata_prefix_ + "/" + relative_key;
}

}  // namespace dbplay
