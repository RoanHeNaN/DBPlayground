#include "Cloud/TableSnapshotLoader.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dbplay {

TableSnapshotLoader::TableSnapshotLoader(TableDescriptor table, std::shared_ptr<IMetadataStore> metadata,
                                         std::shared_ptr<IManifestStore> manifests)
    : table_(std::move(table)),
      manifests_(std::move(manifests)),
      table_metadata_(table_.table_id, table_.metadata_prefix, std::move(metadata)) {
  if (manifests_ == nullptr) {
    throw std::invalid_argument("TableSnapshotLoader: manifest store is null");
  }
}

std::optional<TableSnapshot> TableSnapshotLoader::Load() const {
  const auto current = table_metadata_.Load();
  if (!current.has_value()) {
    return std::nullopt;
  }

  const TableState &state = current->state;
  TableSnapshot snapshot;
  snapshot.metadata_version = current->metadata_version;
  snapshot.state_version = state.state_version;
  snapshot.writer_epoch = state.writer_epoch;
  snapshot.indexed_cursor = state.indexed_cursor;
  snapshot.committed_cursor = state.committed_cursor;
  snapshot.base_manifest = state.base_manifest;
  snapshot.commit_head = state.commit_head;

  if (state.base_manifest.empty()) {
    if (state.indexed_cursor != 0) {
      throw std::runtime_error("TableSnapshotLoader: indexed data has no base manifest");
    }
  } else {
    const auto manifest = manifests_->Load(table_, state.base_manifest);
    if (!manifest.has_value()) {
      throw std::runtime_error("TableSnapshotLoader: base manifest is missing");
    }
    if (manifest->table_id != table_.table_id || manifest->indexed_cursor != state.indexed_cursor) {
      throw std::runtime_error("TableSnapshotLoader: base manifest does not match CURRENT");
    }
    snapshot.data_files = manifest->data_files;
  }

  uint64_t cursor = state.committed_cursor;
  std::string commit_key = state.commit_head;
  while (cursor > state.indexed_cursor) {
    if (commit_key.empty()) {
      throw std::runtime_error("TableSnapshotLoader: commit chain ended before indexed cursor");
    }
    const auto record = table_metadata_.LoadCommitRecord(commit_key);
    if (!record.has_value()) {
      throw std::runtime_error("TableSnapshotLoader: commit record is missing");
    }
    if (record->last_cursor != cursor || record->first_cursor <= state.indexed_cursor ||
        record->first_cursor > record->last_cursor) {
      throw std::runtime_error("TableSnapshotLoader: commit chain is not contiguous");
    }
    snapshot.wal_commits.push_back(*record);
    cursor = record->first_cursor - 1;
    commit_key = record->parent_commit;
  }
  if (cursor != state.indexed_cursor) {
    throw std::runtime_error("TableSnapshotLoader: commit chain crossed indexed cursor");
  }

  std::reverse(snapshot.wal_commits.begin(), snapshot.wal_commits.end());
  for (const auto &record : snapshot.wal_commits) {
    snapshot.wal_files.insert(snapshot.wal_files.end(), record.wal_files.begin(), record.wal_files.end());
  }
  return snapshot;
}

}  // namespace dbplay
