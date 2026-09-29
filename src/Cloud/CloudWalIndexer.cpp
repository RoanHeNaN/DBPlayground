#include "Cloud/CloudWalIndexer.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"
#include "glog/logging.h"

namespace dbplay {

CloudWalIndexer::CloudWalIndexer(TableDescriptor table, std::shared_ptr<IStorage> files,
                                 std::shared_ptr<IMetadataStore> metadata, std::shared_ptr<IFileFormat> data_format,
                                 std::shared_ptr<IObjectKeyGenerator> keys, size_t max_publish_attempts)
    : table_(std::move(table)),
      files_(std::move(files)),
      metadata_(std::move(metadata)),
      data_format_(std::move(data_format)),
      keys_(std::move(keys)),
      current_state_store_(table_, metadata_),
      max_publish_attempts_(max_publish_attempts) {
  if (!files_ || !data_format_ || !keys_ || max_publish_attempts_ == 0 ||
      !SchemasEqual(table_.schema, data_format_->schema()))
    throw std::invalid_argument("CloudWalIndexer: invalid dependency");
}

CloudIndexResult CloudWalIndexer::Index() {
  constexpr uint64_t kMaxWalPerPass = 256;
  for (size_t attempt = 0; attempt < max_publish_attempts_; ++attempt) {
    auto current = current_state_store_.Load();
    if (!current) return Result(CloudIndexCode::NothingToDo, {});
    auto manifest = current_state_store_.LoadManifest(current->state);
    auto entries = current_state_store_.LoadBatchIndex(manifest);
    std::vector<Chunk> chunks;
    uint64_t rows = 0;
    uint64_t end = current->state.indexed_seq;
    while (end < UINT64_MAX && end - current->state.indexed_seq < kMaxWalPerPass) {
      auto wal = LoadSequentialWal(*files_, table_, end + 1);
      if (!wal) break;
      ++end;
      if (wal->kind == SequentialWal::Kind::Append) {
        for (const auto &id : wal->batch_ids) entries.push_back({id, wal->seq, wal->operation_id, wal->payload_digest});
        for (auto &chunk : wal->chunks) {
          rows += chunk.row_count;
          chunks.push_back(std::move(chunk));
        }
      }
    }
    if (end == current->state.indexed_seq) return Result(CloudIndexCode::NothingToDo, current->state);

    if (!chunks.empty()) {
      const std::string key = keys_->NewDataFileKey(table_.table_id);
      if (key.compare(0, 5, "data/") != 0) return Result(CloudIndexCode::InvalidState, current->state);
      const std::string path = table_.file_prefix + "/" + key;
      auto writer = data_format_->OpenWriter(*files_, path);
      if (!writer) throw std::runtime_error("CloudWalIndexer: null writer");
      for (const auto &chunk : chunks) writer->Write(chunk);
      writer->Close();
      manifest.data_files.push_back({path, current->state.indexed_seq + 1, end, rows});
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.batch_id < b.batch_id; });
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const auto &a, const auto &b) { return a.batch_id == b.batch_id; }),
                  entries.end());
    const std::string manifest_key = keys_->NewManifestKey(table_.table_id);
    if (manifest_key.compare(0, 9, "manifest/") != 0) return Result(CloudIndexCode::InvalidState, current->state);
    const std::string dedup_key = "dedup/" + manifest_key.substr(9) + ".idx";
    const auto dedup_result = current_state_store_.WriteBatchIndex(dedup_key, entries);
    if (dedup_result != ConditionalWriteResult::Applied) continue;
    manifest.batch_index_files = {dedup_key};
    manifest.covered_through_seq = end;
    const auto manifest_result = current_state_store_.WriteManifest(manifest_key, manifest);
    if (manifest_result != ConditionalWriteResult::Applied) continue;
    VersionedSequentialState written;
    const auto cas = current_state_store_.Publish(*current, SequentialPublishUpdate{end, manifest_key}, &written);
    if (cas == ConditionalWriteResult::Applied) {
      LOG(INFO) << "CloudWalIndexer: table=" << table_.table_id << " indexed through WAL " << end;
      return Result(CloudIndexCode::Indexed, written.state);
    }
  }
  auto current = current_state_store_.Load();
  return Result(CloudIndexCode::RetryableConflict, current ? current->state : SequentialState{});
}

CloudIndexResult CloudWalIndexer::Result(CloudIndexCode code, const SequentialState &state) const {
  CloudIndexResult result;
  result.code = code;
  result.indexed_seq = state.indexed_seq;
  result.published_seq = state.published_seq;
  return result;
}

}  // namespace dbplay
