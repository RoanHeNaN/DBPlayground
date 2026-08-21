#ifndef DBPLAYGROUND_CLOUDTYPES_H
#define DBPLAYGROUND_CLOUDTYPES_H

#include <cstdint>
#include <string>
#include <vector>

#include "Metadata/IMetadataStore.h"
#include "Metadata/TableState.h"
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

struct BaseManifest {
  std::string table_id;
  uint64_t indexed_cursor = 0;
  std::vector<std::string> data_files;
};

struct TableSnapshot {
  MetadataVersion metadata_version;
  uint64_t state_version = 0;
  uint64_t writer_epoch = 0;
  uint64_t indexed_cursor = 0;
  uint64_t committed_cursor = 0;
  std::string base_manifest;
  std::string commit_head;
  std::vector<std::string> data_files;
  std::vector<std::string> wal_files;
  std::vector<CommitRecord> wal_commits;
};

// A group-commit unit. The coordinator above CloudTableWriter may combine
// several client INSERTs into one request while preserving every batch id.
struct CloudImportBatch {
  std::vector<std::string> batch_ids;
  std::vector<Chunk> chunks;
};

enum class CloudImportCode {
  Committed,
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
  uint64_t committed_cursor = 0;
};

enum class CloudWriterStartCode { Started, Contended, RetryableConflict, InvalidTable };

struct CloudWriterStartResult {
  CloudWriterStartCode code = CloudWriterStartCode::InvalidTable;
  uint64_t writer_epoch = 0;
};

struct WriterTarget {
  std::string writer_id;
  std::string endpoint;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTYPES_H
