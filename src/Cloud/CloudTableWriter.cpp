#include "Cloud/CloudTableWriter.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Cloud/CloudValidation.h"
#include "glog/logging.h"

namespace dbplay {

CloudTableWriter::CloudTableWriter(TableDescriptor table, std::shared_ptr<IStorage> files,
                                   std::shared_ptr<IMetadataStore> metadata, size_t max_publish_attempts)
    : table_(std::move(table)),
      files_(std::move(files)),
      current_state_store_(table_, std::move(metadata)),
      max_publish_attempts_(max_publish_attempts) {
  if (!files_ || max_publish_attempts_ == 0) throw std::invalid_argument("CloudTableWriter: invalid dependency");
}

CloudWriterStartResult CloudTableWriter::Start() {
  if (current_state_) {
    return {CloudWriterStartCode::Started, current_state_->state.writer_epoch};
  }
  auto state = current_state_store_.Load();
  if (!state) {
    const auto init = current_state_store_.Initialize();
    if (init == ConditionalWriteResult::RetryableConflict) return {CloudWriterStartCode::RetryableConflict, 0};
    state = current_state_store_.Load();
    if (!state) return {CloudWriterStartCode::RetryableConflict, 0};
  }
  bool acquired = false;
  for (size_t attempt = 0; attempt < max_publish_attempts_; ++attempt) {
    if (state->state.writer_epoch == UINT64_MAX) return {CloudWriterStartCode::InvalidTable, 0};
    auto next = state->state;
    ++next.writer_epoch;
    ++next.state_version;
    VersionedSequentialState written;
    const auto result = current_state_store_.CompareExchange(*state, next, &written);
    if (result == ConditionalWriteResult::Applied) {
      state = std::move(written);
      acquired = true;
      break;
    }
    if (result == ConditionalWriteResult::RetryableConflict) return {CloudWriterStartCode::RetryableConflict, 0};
    state = current_state_store_.Load();
    if (!state) return {CloudWriterStartCode::RetryableConflict, 0};
  }
  if (!acquired) return {CloudWriterStartCode::Contended, 0};

  uint64_t seq = state->state.indexed_seq + 1;
  for (;;) {
    const auto latest = current_state_store_.Load();
    if (!latest) return {CloudWriterStartCode::RetryableConflict, 0};
    if (latest->state.writer_epoch != state->state.writer_epoch) return {CloudWriterStartCode::Contended, 0};
    if (seq <= latest->state.indexed_seq) seq = latest->state.indexed_seq + 1;
    auto existing = LoadSequentialWal(*files_, table_, seq);
    if (existing) {
      ++seq;
      continue;
    }
    SequentialWal fence;
    fence.kind = SequentialWal::Kind::Fence;
    fence.table_id = table_.table_id;
    fence.seq = seq;
    fence.writer_epoch = state->state.writer_epoch;
    fence.operation_id = "fence-" + std::to_string(fence.writer_epoch);
    const std::string bytes = EncodeSequentialWal(fence, table_.schema);
    const auto result = files_->CreateIfAbsent(SequentialWalKey(table_, seq), Slice(bytes));
    if (result == CreateFileResult::AlreadyExists) {
      ++seq;
      continue;
    }
    if (result == CreateFileResult::RetryableConflict) {
      auto landed = LoadSequentialWal(*files_, table_, seq);
      if (!landed || landed->kind != SequentialWal::Kind::Fence || landed->writer_epoch != fence.writer_epoch ||
          landed->operation_id != fence.operation_id)
        return {CloudWriterStartCode::RetryableConflict, 0};
    }
    current_state_ = current_state_store_.Load();
    if (!current_state_ || current_state_->state.writer_epoch != fence.writer_epoch) {
      current_state_.reset();
      return {CloudWriterStartCode::Contended, 0};
    }
    next_seq_ = seq + 1;
    return {CloudWriterStartCode::Started, fence.writer_epoch};
  }
}

CloudImportResult CloudTableWriter::Import(const CloudImportBatch &batch) {
  if (!current_state_) return Result(CloudImportCode::NotStarted);
  if (!IsValidBatch(batch)) return Result(CloudImportCode::InvalidRequest);
  auto latest = current_state_store_.Load();
  if (!latest) return Result(CloudImportCode::RetryableConflict);
  if (latest->state.writer_epoch != current_state_->state.writer_epoch) {
    current_state_.reset();
    return Result(CloudImportCode::Fenced);
  }
  current_state_ = *latest;

  SequentialWal requested;
  requested.table_id = table_.table_id;
  requested.seq = 1;
  requested.writer_epoch = 1;
  requested.operation_id = batch.batch_ids.front();
  requested.batch_ids = batch.batch_ids;
  requested.chunks = batch.chunks;
  const std::string request_bytes = EncodeSequentialWal(requested, table_.schema);
  const std::string request_digest = DecodeSequentialWal(Slice(request_bytes), table_.schema).payload_digest;
  std::unordered_map<std::string, SequentialBatchEntry> known;
  const auto manifest = current_state_store_.LoadManifest(latest->state);
  for (const auto &entry : current_state_store_.LoadBatchIndex(manifest)) {
    known.emplace(entry.batch_id, entry);
  }
  uint64_t scan_seq = latest->state.indexed_seq + 1;
  while (scan_seq < next_seq_) {
    auto wal = LoadSequentialWal(*files_, table_, scan_seq);
    if (!wal) return Result(CloudImportCode::RetryableConflict);
    for (const auto &id : wal->batch_ids)
      known.emplace(id, SequentialBatchEntry{id, scan_seq, wal->operation_id, wal->payload_digest});
    ++scan_seq;
  }
  size_t already = 0;
  uint64_t prior_seq = 0;
  for (const auto &id : batch.batch_ids) {
    auto it = known.find(id);
    if (it != known.end()) {
      ++already;
      if (it->second.operation_id != requested.operation_id || it->second.payload_digest != request_digest ||
          (prior_seq != 0 && prior_seq != it->second.seq))
        return Result(CloudImportCode::InvalidRequest);
      prior_seq = it->second.seq;
    }
  }
  if (already == batch.batch_ids.size()) {
    if (prior_seq <= latest->state.published_seq) {
      auto result = Result(CloudImportCode::AlreadyCommitted);
      result.wal_seq = prior_seq;
      return result;
    }
    if (batch.visibility == CloudImportBatch::Visibility::Immediate)
      return Publish(prior_seq, latest->state.writer_epoch);
    auto result = Result(CloudImportCode::Durable);
    result.wal_seq = prior_seq;
    return result;
  }
  if (already != 0) {
    return Result(CloudImportCode::InvalidRequest);
  }

  if (next_seq_ == UINT64_MAX) {
    return Result(CloudImportCode::InvalidRequest);
  }
  SequentialWal wal;
  wal.table_id = table_.table_id;
  wal.seq = next_seq_;
  wal.writer_epoch = current_state_->state.writer_epoch;
  wal.operation_id = batch.batch_ids.front();
  wal.batch_ids = batch.batch_ids;
  wal.chunks = batch.chunks;
  const std::string path = SequentialWalKey(table_, wal.seq);
  std::string bytes;
  try {
    bytes = EncodeSequentialWal(wal, table_.schema);
  } catch (const std::invalid_argument &) {
    return Result(CloudImportCode::InvalidRequest);
  }
  VLOG(1) << "A new wal path " << path << " created for table " << table_.table_id;
  const auto create = files_->CreateIfAbsent(path, Slice(bytes));
  if (create != CreateFileResult::Created) {
    auto landed = LoadSequentialWal(*files_, table_, wal.seq);
    if (!landed) return Result(CloudImportCode::RetryableConflict);
    if (landed->writer_epoch != wal.writer_epoch) {
      current_state_.reset();
      return Result(CloudImportCode::Fenced);
    }
    if (landed->kind != SequentialWal::Kind::Append || landed->operation_id != wal.operation_id ||
        EncodeSequentialWal(*landed, table_.schema) != bytes)
      return Result(CloudImportCode::RetryableConflict);
  }
  ++next_seq_;
  if (batch.visibility == CloudImportBatch::Visibility::Deferred) {
    auto result = Result(CloudImportCode::Durable);
    result.wal_seq = wal.seq;
    return result;
  }
  return Publish(wal.seq, wal.writer_epoch);
}

CloudImportResult CloudTableWriter::Publish(uint64_t seq, uint64_t epoch) {
  CloudImportResult result = Result(CloudImportCode::RetryableConflict);
  result.wal_seq = seq;
  for (size_t attempt = 0; attempt < max_publish_attempts_; ++attempt) {
    auto latest = current_state_store_.Load();
    if (!latest) break;
    if (latest->state.published_seq >= seq) {
      current_state_ = *latest;
      result = Result(CloudImportCode::Committed);
      result.wal_seq = seq;
      return result;
    }
    if (latest->state.writer_epoch != epoch) break;
    VersionedSequentialState written;
    const auto cas = current_state_store_.Publish(*latest, SequentialPublishUpdate{seq, std::nullopt}, &written);
    if (cas == ConditionalWriteResult::Applied) {
      current_state_ = std::move(written);
      result = Result(CloudImportCode::Committed);
      result.wal_seq = seq;
      return result;
    }
  }
  result.code = CloudImportCode::RetryableConflict;
  return result;
}

bool CloudTableWriter::IsValidBatch(const CloudImportBatch &batch) const {
  if (batch.batch_ids.empty() || batch.chunks.empty()) return false;
  std::unordered_set<std::string> ids;
  for (const auto &id : batch.batch_ids)
    if (id.empty() || !ids.insert(id).second) return false;
  for (const auto &chunk : batch.chunks)
    if (!IsFullSchemaChunk(chunk, table_.schema)) return false;
  return true;
}

CloudImportResult CloudTableWriter::Result(CloudImportCode code) const {
  CloudImportResult result;
  result.code = code;
  if (current_state_) {
    result.writer_epoch = current_state_->state.writer_epoch;
    result.published_seq = current_state_->state.published_seq;
  }
  return result;
}

}  // namespace dbplay
