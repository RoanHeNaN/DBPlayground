#include "Cloud/CloudTableCompactor.h"

#include <stdexcept>
#include <utility>
#include <vector>

#include "Cloud/CloudValidation.h"
#include "Cloud/MetadataManifestStore.h"
#include "Cloud/TableSnapshotLoader.h"
#include "Table/Chunk.h"

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
                                         std::shared_ptr<IFileFormat> base_format,
                                         std::shared_ptr<IFileFormat> wal_format,
                                         std::shared_ptr<IObjectKeyGenerator> keys, size_t max_publish_attempts)
    : table_(std::move(table)),
      files_(std::move(files)),
      metadata_(std::move(metadata)),
      base_format_(std::move(base_format)),
      wal_format_(std::move(wal_format)),
      keys_(std::move(keys)),
      table_metadata_(table_.table_id, table_.metadata_prefix, metadata_),
      max_publish_attempts_(max_publish_attempts) {
  table_.file_prefix = NormalizePrefix(std::move(table_.file_prefix));
  if (files_ == nullptr || metadata_ == nullptr || base_format_ == nullptr || wal_format_ == nullptr ||
      keys_ == nullptr || max_publish_attempts_ == 0 || !SchemasEqual(table_.schema, base_format_->schema()) ||
      !SchemasEqual(table_.schema, wal_format_->schema())) {
    throw std::invalid_argument("CloudTableCompactor: dependency is null or retry limit is zero");
  }
}

CloudCompactResult CloudTableCompactor::Compact() {
  std::string data_path;
  std::string manifest_key;
  BaseManifest manifest;
  bool have_output = false;
  VersionedTableState expected;

  for (size_t attempt = 0; attempt < max_publish_attempts_; ++attempt) {
    if (!have_output) {
      // One consistent read of CURRENT: the snapshot carries the metadata
      // version it was loaded at, and we rebuild `expected` from that same
      // read. Deriving the manifest from one load and the CAS precondition
      // from a second (independent) load would let a concurrent commit make
      // manifest.indexed_cursor exceed expected.committed_cursor, turning a
      // benign race into a non-retryable InvalidManifest.
      auto manifests = std::make_shared<MetadataManifestStore>(table_.table_id, table_.metadata_prefix, metadata_);
      TableSnapshotLoader loader(table_, metadata_, manifests);
      const auto snapshot = loader.Load();
      if (!snapshot.has_value()) {
        return Result(CloudCompactCode::NothingToDo, TableState{});
      }

      TableState state;
      state.table_id = table_.table_id;
      state.state_version = snapshot->state_version;
      state.writer_epoch = snapshot->writer_epoch;
      state.committed_cursor = snapshot->committed_cursor;
      state.indexed_cursor = snapshot->indexed_cursor;
      state.base_manifest = snapshot->base_manifest;
      state.commit_head = snapshot->commit_head;

      if (snapshot->indexed_cursor == snapshot->committed_cursor || snapshot->wal_files.empty()) {
        return Result(CloudCompactCode::NothingToDo, state);
      }

      const std::string data_key = keys_->NewDataFileKey(table_.table_id);
      try {
        data_path = ResolveFileKey(data_key, "data/");
      } catch (const std::invalid_argument &) {
        return Result(CloudCompactCode::InvalidState, state);
      }
      WriteBaseFile(data_path, snapshot->wal_files);

      manifest = BaseManifest{};
      manifest.table_id = table_.table_id;
      manifest.indexed_cursor = snapshot->committed_cursor;
      manifest.data_files = snapshot->data_files;
      manifest.data_files.push_back(data_path);
      manifest_key = keys_->NewManifestKey(table_.table_id);
      expected = VersionedTableState{state, snapshot->metadata_version};
      have_output = true;
    }

    VersionedTableState published;
    switch (table_metadata_.PublishCompaction(expected, manifest_key, manifest, &published)) {
      case PublishCompactionResult::Published:
        return Result(CloudCompactCode::Compacted, published.state);
      case PublishCompactionResult::AlreadyPublished:
        return Result(published.state.base_manifest == manifest_key ? CloudCompactCode::Compacted
                                                                    : CloudCompactCode::AlreadyPublished,
                      published.state);
      case PublishCompactionResult::InvalidManifest:
      case PublishCompactionResult::ManifestKeyCollision:
        return Result(CloudCompactCode::InvalidState, expected.state);
      case PublishCompactionResult::RetryableConflict:
        break;
      case PublishCompactionResult::StaleVersion: {
        const auto latest = table_metadata_.Load();
        if (!latest.has_value()) {
          return Result(CloudCompactCode::RetryableConflict, expected.state);
        }
        if (latest->state.indexed_cursor >= manifest.indexed_cursor) {
          return Result(CloudCompactCode::AlreadyPublished, latest->state);
        }
        have_output = false;
        break;
      }
    }
  }
  const auto latest = table_metadata_.Load();
  return Result(CloudCompactCode::RetryableConflict, latest.has_value() ? latest->state : TableState{});
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

CloudCompactResult CloudTableCompactor::Result(CloudCompactCode code, const TableState &state) const {
  CloudCompactResult result;
  result.code = code;
  result.indexed_cursor = state.indexed_cursor;
  result.committed_cursor = state.committed_cursor;
  return result;
}

void CloudTableCompactor::WriteBaseFile(const std::string &path, const std::vector<std::string> &wal_files) const {
  auto writer = base_format_->OpenWriter(*files_, path);
  if (writer == nullptr) {
    throw std::runtime_error("CloudTableCompactor: base format returned a null writer");
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
