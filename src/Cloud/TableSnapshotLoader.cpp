#include "Cloud/TableSnapshotLoader.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "glog/logging.h"

namespace dbplay {

TableSnapshotLoader::TableSnapshotLoader(TableDescriptor table, std::shared_ptr<IMetadataStore> metadata,
                                         std::shared_ptr<ICompactedDataManifestStore> compacted_data_manifest_store)
    : table_(std::move(table)),
      compacted_data_manifest_store_(std::move(compacted_data_manifest_store)),
      current_state_store_(table_.table_id, table_.metadata_prefix, std::move(metadata)) {
  if (compacted_data_manifest_store_ == nullptr) {
    throw std::invalid_argument("TableSnapshotLoader: compacted data manifest store is null");
  }
}

std::optional<TableSnapshot> TableSnapshotLoader::Load() const {
  const auto current_state = current_state_store_.Load();
  if (!current_state.has_value()) {
    return std::nullopt;
  }

  const CurrentTableState &state = current_state->current_state;
  TableSnapshot snapshot;
  snapshot.current_metadata_version = current_state->current_metadata_version;
  snapshot.current_state_version = state.current_state_version;
  snapshot.writer_epoch = state.writer_epoch;
  snapshot.compacted_cursor = state.compacted_cursor;
  snapshot.committed_cursor = state.committed_cursor;
  snapshot.compacted_data_manifest_key = state.compacted_data_manifest_key;
  snapshot.latest_commit_key = state.latest_commit_key;

  if (state.compacted_data_manifest_key.empty()) {
    if (state.compacted_cursor != 0) {
      throw std::runtime_error("TableSnapshotLoader: compacted data has no manifest");
    }
  } else {
    const auto manifest = compacted_data_manifest_store_->Load(table_, state.compacted_data_manifest_key);
    if (!manifest.has_value()) {
      throw std::runtime_error("TableSnapshotLoader: compacted data manifest is missing");
    }
    if (manifest->table_id != table_.table_id || manifest->compacted_cursor != state.compacted_cursor) {
      throw std::runtime_error("TableSnapshotLoader: compacted data manifest does not match CURRENT");
    }
    snapshot.compacted_data_files = manifest->compacted_data_files;
  }

  // Walk the commit chain backward from latest_commit_key to compacted_cursor
  // using only GETs (parent_commit_key pointers) -- the hot-path zero-LIST contract. Each
  // step validates cursor contiguity so a broken/forked chain faults loudly
  // rather than silently returning a snapshot with missing rows.
  uint64_t cursor = state.committed_cursor;
  std::string commit_key = state.latest_commit_key;
  while (cursor > state.compacted_cursor) {
    if (commit_key.empty()) {
      throw std::runtime_error("TableSnapshotLoader: commit chain ended before compacted cursor");
    }
    const auto record = current_state_store_.LoadCommitRecord(commit_key);
    if (!record.has_value()) {
      throw std::runtime_error("TableSnapshotLoader: commit record is missing");
    }
    if (record->last_cursor != cursor || record->first_cursor <= state.compacted_cursor ||
        record->first_cursor > record->last_cursor) {
      throw std::runtime_error("TableSnapshotLoader: commit chain is not contiguous");
    }
    snapshot.wal_commit_records.push_back(*record);
    cursor = record->first_cursor - 1;
    commit_key = record->parent_commit_key;
  }
  if (cursor != state.compacted_cursor) {
    throw std::runtime_error("TableSnapshotLoader: commit chain crossed compacted cursor");
  }

  std::reverse(snapshot.wal_commit_records.begin(), snapshot.wal_commit_records.end());
  for (const auto &record : snapshot.wal_commit_records) {
    snapshot.wal_files.insert(snapshot.wal_files.end(), record.wal_files.begin(), record.wal_files.end());
  }
  VLOG(1) << "TableSnapshotLoader: table=" << table_.table_id
          << " loaded snapshot compacted=" << snapshot.compacted_cursor << " committed=" << snapshot.committed_cursor
          << " compacted_data_files=" << snapshot.compacted_data_files.size()
          << " wal_files=" << snapshot.wal_files.size();
  return snapshot;
}

}  // namespace dbplay
