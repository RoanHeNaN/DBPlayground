#ifndef DBPLAYGROUND_CLOUDTYPES_H
#define DBPLAYGROUND_CLOUDTYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Metadata/IMetadataStore.h"
#include "Table/Chunk.h"
#include "Table/Schema.h"

namespace dbplay {

struct TableName {
  std::string database;
  std::string table;
};

// Catalog output. metadata_prefix addresses coordination objects; file_prefix
// addresses WAL/DBC1 bytes. They may map to the same S3 prefix but remain
// separate concepts at this layer.
struct TableDescriptor {
  std::string table_id;
  std::string metadata_prefix;
  std::string file_prefix;
  Schema schema;
};

struct WalProbePolicy {
  size_t initial = 8;
  size_t minimum = 1;
  size_t maximum = 64;
};

struct TableSnapshot {
  MetadataVersion current_metadata_version;
  uint64_t current_state_version = 0;
  uint64_t writer_epoch = 0;
  std::string manifest_key;
  std::vector<std::string> data_files;
  std::vector<std::string> wal_files;
  uint64_t published_seq = 0;
  uint64_t indexed_seq = 0;
  std::vector<Chunk> wal_chunks;
  size_t probed_wal_count = 0;
  bool probe_limit_reached = false;
  // Query data is the indexed data set through indexed_seq followed by
  // published WAL and, when requested, a bounded speculative WAL tail.
};

// A group-commit unit. The coordinator above CloudTableWriter may combine
// several client INSERTs into one request while preserving every batch id.
struct CloudImportBatch {
  std::vector<std::string> batch_ids;
  std::vector<Chunk> chunks;
  enum class Visibility { Immediate, Deferred } visibility = Visibility::Immediate;
};

enum class CloudImportCode {
  Committed,
  Durable,
  AlreadyCommitted,
  TableNotFound,
  NoWriterAvailable,
  NotStarted,
  Fenced,
  RetryableConflict,
  InvalidRequest
};

struct CloudImportResult {
  CloudImportCode code = CloudImportCode::InvalidRequest;
  uint64_t writer_epoch = 0;
  uint64_t published_seq = 0;
  uint64_t wal_seq = 0;
};

enum class CloudWriterStartCode { Started, Contended, RetryableConflict, InvalidTable };

struct CloudWriterStartResult {
  CloudWriterStartCode code = CloudWriterStartCode::InvalidTable;
  uint64_t writer_epoch = 0;
};

enum class CloudIndexCode { Indexed, AlreadyPublished, NothingToDo, RetryableConflict, InvalidState };

struct CloudIndexResult {
  CloudIndexCode code = CloudIndexCode::InvalidState;
  uint64_t indexed_seq = 0;
  uint64_t published_seq = 0;
};

struct WriterTarget {
  std::string writer_id;
  std::string endpoint;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTYPES_H
