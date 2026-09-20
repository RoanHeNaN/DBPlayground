#include "Cloud/CloudTableCompactor.h"

#include <stdexcept>
#include <utility>
#include <vector>

#include "Cloud/CloudValidation.h"
#include "Cloud/MetadataCompactedDataManifestStore.h"
#include "Cloud/TableSnapshotLoader.h"
#include "Table/Chunk.h"
#include "glog/logging.h"

namespace dbplay {
namespace {

std::string NormalizePrefix(std::string prefix) {
  while (!prefix.empty() && prefix.back() == '/') {
    prefix.pop_back();
  }
  if (prefix.empty()) {
    throw std::invalid_argument("CloudTableCompactor: file_prefix is empty");
  }
  return prefix;
}

std::vector<int> FullProjection(const Schema &schema) {
  std::vector<int> projection(schema.size());
  for (size_t i = 0; i < schema.size(); ++i) {
    projection[i] = static_cast<int>(i);
  }
  return projection;
}

}  // namespace

CloudTableCompactor::CloudTableCompactor(TableDescriptor table, std::shared_ptr<IStorage> files,
                                         std::shared_ptr<IMetadataStore> metadata,
                                         std::shared_ptr<IFileFormat> compacted_data_format,
                                         std::shared_ptr<IFileFormat> wal_format,
                                         std::shared_ptr<IObjectKeyGenerator> keys, size_t max_publish_attempts)
    : table_(std::move(table)),
      files_(std::move(files)),
      metadata_(std::move(metadata)),
      compacted_data_format_(std::move(compacted_data_format)),
      wal_format_(std::move(wal_format)),
      keys_(std::move(keys)),
      current_state_store_(table_.table_id, table_.metadata_prefix, metadata_),
      max_publish_attempts_(max_publish_attempts) {
  table_.file_prefix = NormalizePrefix(std::move(table_.file_prefix));
  if (files_ == nullptr || metadata_ == nullptr || compacted_data_format_ == nullptr || wal_format_ == nullptr ||
      keys_ == nullptr || max_publish_attempts_ == 0 ||
      !SchemasEqual(table_.schema, compacted_data_format_->schema()) ||
      !SchemasEqual(table_.schema, wal_format_->schema())) {
    throw std::invalid_argument("CloudTableCompactor: dependency is null or retry limit is zero");
  }
}

CloudCompactResult CloudTableCompactor::Compact() {
  // Compaction is a metadata transition around an immutable-file rewrite. It
  // deliberately has no in-place update: readers continue to use the old
  // snapshot while this method writes a new compacted-data file. The new file
  // becomes visible only after PublishCompaction successfully CASes CURRENT.
  //
  // Each attempt follows this protocol:
  //   1. Load one consistent snapshot/CURRENT version.
  //   2. If compacted_cursor already reaches committed_cursor, do nothing.
  //   3. Scan the committed WAL tail and write one immutable compacted-data file.
  //   4. Persist a manifest describing the old compacted-data files plus the new file.
  //   5. CAS CURRENT to advance compacted_cursor to the compacted cursor.
  //
  // The output files are intentionally written before the CAS. A lost CAS
  // therefore cannot expose partial data; it only leaves an unreferenced
  // object for the future garbage collector. If a writer advances CURRENT,
  // the output is discarded and the tail is recomputed from the newer snapshot.
  std::string compacted_data_path;
  std::string compacted_data_manifest_key;
  CompactedDataManifest compacted_data_manifest;
  bool have_output = false;
  VersionedCurrentTableState expected;

  // Retry loop. `have_output` caches the written compacted-data file + manifest across
  // attempts so a lost publish CAS re-tries the cheap CAS only; a StaleVersion
  // (a writer committed under us) resets it to recompute over the new tail.
  for (size_t attempt = 0; attempt < max_publish_attempts_; ++attempt) {
    if (!have_output) {
      // One consistent read of CURRENT: the snapshot carries the metadata
      // version it was loaded at, and we rebuild `expected` from that same
      // read. Deriving the manifest from one load and the CAS precondition
      // from a second (independent) load would let a concurrent commit make
      // compacted_data_manifest.compacted_cursor exceed expected.committed_cursor, turning a
      // benign race into a non-retryable InvalidCompactedDataManifest.
      auto compacted_data_manifest_store =
          std::make_shared<MetadataCompactedDataManifestStore>(table_.table_id, table_.metadata_prefix, metadata_);
      TableSnapshotLoader loader(table_, metadata_, compacted_data_manifest_store);
      const auto snapshot = loader.Load();
      if (!snapshot.has_value()) {
        return Result(CloudCompactCode::NothingToDo, CurrentTableState{});
      }

      CurrentTableState current_state;
      current_state.table_id = table_.table_id;
      current_state.current_state_version = snapshot->current_state_version;
      current_state.writer_epoch = snapshot->writer_epoch;
      current_state.committed_cursor = snapshot->committed_cursor;
      current_state.compacted_cursor = snapshot->compacted_cursor;
      current_state.compacted_data_manifest_key = snapshot->compacted_data_manifest_key;
      current_state.latest_commit_key = snapshot->latest_commit_key;

      if (snapshot->compacted_cursor == snapshot->committed_cursor || snapshot->wal_files.empty()) {
        return Result(CloudCompactCode::NothingToDo, current_state);
      }

      const std::string data_key = keys_->NewCompactedDataFileKey(table_.table_id);
      try {
        compacted_data_path = ResolveFileKey(data_key, "data/");
      } catch (const std::invalid_argument &) {
        return Result(CloudCompactCode::InvalidState, current_state);
      }
      // Persistence: merge the committed WAL tail into one new immutable
      // compacted-data file. It becomes live only when the manifest CAS below
      // references it; an unreferenced compacted-data file from a lost attempt
      // is an orphan for GC.
      WriteCompactedDataFile(compacted_data_path, snapshot->wal_files);
      VLOG(1) << "CloudTableCompactor: table=" << table_.table_id << " wrote compacted data " << compacted_data_path
              << " from " << snapshot->wal_files.size() << " WAL files up to cursor=" << snapshot->committed_cursor;

      compacted_data_manifest = CompactedDataManifest{};
      compacted_data_manifest.table_id = table_.table_id;
      compacted_data_manifest.compacted_cursor = snapshot->committed_cursor;
      compacted_data_manifest.compacted_data_files = snapshot->compacted_data_files;
      compacted_data_manifest.compacted_data_files.push_back(compacted_data_path);
      compacted_data_manifest_key = keys_->NewCompactedDataManifestKey(table_.table_id);
      expected = VersionedCurrentTableState{current_state, snapshot->current_metadata_version};
      have_output = true;
    }

    VersionedCurrentTableState published;
    switch (current_state_store_.PublishCompaction(expected, compacted_data_manifest_key, compacted_data_manifest,
                                                   &published)) {
      case PublishCompactionResult::Published:
        LOG(INFO) << "CloudTableCompactor: table=" << table_.table_id
                  << " compacted to compacted_cursor=" << published.current_state.compacted_cursor
                  << " compacted_data_manifest=" << compacted_data_manifest_key;
        return Result(CloudCompactCode::Compacted, published.current_state);
      case PublishCompactionResult::AlreadyPublished:
        return Result(published.current_state.compacted_data_manifest_key == compacted_data_manifest_key
                          ? CloudCompactCode::Compacted
                          : CloudCompactCode::AlreadyPublished,
                      published.current_state);
      case PublishCompactionResult::InvalidCompactedDataManifest:
      case PublishCompactionResult::CompactedDataManifestKeyCollision:
        return Result(CloudCompactCode::InvalidState, expected.current_state);
      case PublishCompactionResult::RetryableConflict:
        break;
      case PublishCompactionResult::StaleVersion: {
        const auto latest = current_state_store_.Load();
        if (!latest.has_value()) {
          return Result(CloudCompactCode::RetryableConflict, expected.current_state);
        }
        if (latest->current_state.compacted_cursor >= compacted_data_manifest.compacted_cursor) {
          return Result(CloudCompactCode::AlreadyPublished, latest->current_state);
        }
        have_output = false;
        break;
      }
    }
  }
  const auto latest = current_state_store_.Load();
  return Result(CloudCompactCode::RetryableConflict, latest.has_value() ? latest->current_state : CurrentTableState{});
}

bool CloudTableCompactor::IsSafeRelativeKey(const std::string &key, const std::string &required_prefix) const {
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

std::string CloudTableCompactor::ResolveFileKey(const std::string &relative_key,
                                                const std::string &required_prefix) const {
  if (!IsSafeRelativeKey(relative_key, required_prefix)) {
    throw std::invalid_argument("CloudTableCompactor: invalid file key");
  }
  return table_.file_prefix + "/" + relative_key;
}

CloudCompactResult CloudTableCompactor::Result(CloudCompactCode code, const CurrentTableState &current_state) const {
  CloudCompactResult result;
  result.code = code;
  result.compacted_cursor = current_state.compacted_cursor;
  result.committed_cursor = current_state.committed_cursor;
  return result;
}

void CloudTableCompactor::WriteCompactedDataFile(const std::string &path,
                                                 const std::vector<std::string> &wal_files) const {
  // WAL and compacted-data files use separate format instances. Scan() produces
  // logical Chunks from immutable WAL objects; the compacted-data writer
  // serializes those Chunks into the native columnar format. The caller has
  // already fixed the WAL list in a snapshot, so this method must not discover
  // files with LIST.
  auto writer = compacted_data_format_->OpenWriter(*files_, path);
  if (writer == nullptr) {
    throw std::runtime_error("CloudTableCompactor: compacted-data format returned a null writer");
  }
  auto cursor = wal_format_->Scan(*files_, wal_files, FullProjection(table_.schema));
  Chunk chunk;
  bool wrote = false;
  while (cursor->Next(&chunk)) {
    writer->Write(chunk);
    wrote = true;
  }
  if (!wrote) {
    throw std::runtime_error("CloudTableCompactor: compacted WAL produced no rows");
  }
  writer->Close();
}

}  // namespace dbplay
