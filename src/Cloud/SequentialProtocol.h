#ifndef DBPLAYGROUND_CLOUD_SEQUENTIALPROTOCOL_H
#define DBPLAYGROUND_CLOUD_SEQUENTIALPROTOCOL_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Cloud/CloudTypes.h"
#include "Metadata/IMetadataStore.h"
#include "Storage/File/IStorage.h"

namespace dbplay {

struct SequentialState {
  std::string table_id;
  uint64_t state_version = 0;
  uint64_t schema_version = 1;
  uint64_t writer_epoch = 0;
  uint64_t indexed_seq = 0;
  uint64_t published_seq = 0;
  std::string manifest_key;
};

struct SequentialManifest {
  std::string table_id;
  uint64_t covered_through_seq = 0;
  uint64_t schema_version = 1;
  Schema schema;
  struct DataFile {
    std::string path;
    uint64_t first_seq = 0;
    uint64_t last_seq = 0;
    uint64_t row_count = 0;
  };
  std::vector<DataFile> data_files;
  std::vector<std::string> batch_index_files;
};

struct SequentialBatchEntry {
  std::string batch_id;
  uint64_t seq = 0;
  std::string operation_id;
  std::string payload_digest;
};

struct SequentialWal {
  enum class Kind : uint8_t { Append = 1, Fence = 2 };
  Kind kind = Kind::Append;
  std::string table_id;
  uint64_t seq = 0;
  uint64_t writer_epoch = 0;
  uint64_t schema_version = 1;
  std::string operation_id;
  std::vector<std::string> batch_ids;
  std::vector<Chunk> chunks;
  std::string payload_digest;
};

struct VersionedSequentialState {
  SequentialState state;
  MetadataVersion version;
};

std::string SequentialWalKey(const TableDescriptor &table, uint64_t seq);
std::string EncodeSequentialState(const SequentialState &state);
SequentialState DecodeSequentialState(const Slice &bytes);
std::string EncodeSequentialManifest(const SequentialManifest &manifest);
SequentialManifest DecodeSequentialManifest(const Slice &bytes);
std::string EncodeSequentialBatchIndex(const std::vector<SequentialBatchEntry> &entries);
std::vector<SequentialBatchEntry> DecodeSequentialBatchIndex(const Slice &bytes);
std::string EncodeSequentialWal(const SequentialWal &wal, const Schema &schema);
SequentialWal DecodeSequentialWal(const Slice &bytes, const Schema &schema);
std::optional<SequentialWal> LoadSequentialWal(IStorage &files, const TableDescriptor &table, uint64_t seq);

class SequentialStateStore {
 public:
  SequentialStateStore(const TableDescriptor &table, std::shared_ptr<IMetadataStore> metadata);
  std::optional<VersionedSequentialState> Load() const;
  SequentialManifest LoadManifest(const SequentialState &state) const;
  std::vector<SequentialBatchEntry> LoadBatchIndex(const SequentialManifest &manifest) const;
  ConditionalWriteResult Initialize();
  ConditionalWriteResult CompareExchange(const VersionedSequentialState &expected, const SequentialState &next,
                                         VersionedSequentialState *written = nullptr);
  ConditionalWriteResult WriteManifest(const std::string &key, const SequentialManifest &manifest);
  ConditionalWriteResult WriteBatchIndex(const std::string &key, const std::vector<SequentialBatchEntry> &entries);

 private:
  TableDescriptor table_;
  std::shared_ptr<IMetadataStore> metadata_;
  std::string current_key_;
};

}  // namespace dbplay

#endif
