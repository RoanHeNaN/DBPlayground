#include "Cloud/TableSnapshotLoader.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace dbplay {

TableSnapshotLoader::TableSnapshotLoader(TableDescriptor table, std::shared_ptr<IMetadataStore> metadata,
                                         std::shared_ptr<IStorage> files)
    : table_(std::move(table)), files_(std::move(files)), state_store_(table_, std::move(metadata)) {
  if (!files_) throw std::invalid_argument("TableSnapshotLoader: null file store");
}

std::optional<TableSnapshot> TableSnapshotLoader::Load(size_t probe_limit) const {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto current = state_store_.Load();
    if (!current) return std::nullopt;
    const auto manifest = state_store_.LoadManifest(current->state);
    TableSnapshot snapshot;
    snapshot.current_metadata_version = current->version;
    snapshot.current_state_version = current->state.state_version;
    snapshot.writer_epoch = current->state.writer_epoch;
    snapshot.indexed_seq = current->state.indexed_seq;
    snapshot.published_seq = current->state.published_seq;
    snapshot.manifest_key = current->state.manifest_key;
    for (const auto &file : manifest.data_files) snapshot.data_files.push_back(file.path);

    bool missing_required = false;
    uint64_t seq = current->state.indexed_seq;
    while (seq < current->state.published_seq) {
      ++seq;
      auto wal = LoadSequentialWal(*files_, table_, seq);
      if (!wal) {
        missing_required = true;
        break;
      }
      snapshot.wal_files.push_back(SequentialWalKey(table_, seq));
      if (wal->kind == SequentialWal::Kind::Append) {
        for (auto &chunk : wal->chunks) snapshot.wal_chunks.push_back(std::move(chunk));
      }
    }
    if (missing_required) continue;
    for (size_t i = 0; i < probe_limit && seq < std::numeric_limits<uint64_t>::max(); ++i) {
      ++seq;
      auto wal = LoadSequentialWal(*files_, table_, seq);
      if (!wal) break;
      ++snapshot.probed_wal_count;
      snapshot.wal_files.push_back(SequentialWalKey(table_, seq));
      if (wal->kind == SequentialWal::Kind::Append) {
        for (auto &chunk : wal->chunks) snapshot.wal_chunks.push_back(std::move(chunk));
      }
    }
    snapshot.probe_limit_reached = probe_limit > 0 && snapshot.probed_wal_count == probe_limit;
    return snapshot;
  }
  throw std::runtime_error("TableSnapshotLoader: published WAL is missing");
}

}  // namespace dbplay
