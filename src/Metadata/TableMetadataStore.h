#ifndef DBPLAYGROUND_TABLEMETADATASTORE_H
#define DBPLAYGROUND_TABLEMETADATASTORE_H

#include <memory>
#include <optional>
#include <string>

#include "Metadata/IMetadataStore.h"
#include "Metadata/TableState.h"

namespace dbplay {

struct VersionedTableState {
  TableState state;
  MetadataVersion metadata_version;
};

enum class InitializeTableResult { Created, AlreadyExists, RetryableConflict, InvalidState };
enum class AcquireWriterResult { Acquired, StaleVersion, RetryableConflict, EpochExhausted, InvalidState };
enum class PublishWalResult {
  Committed,
  AlreadyCommitted,
  Fenced,
  RebaseRequired,
  CommitKeyCollision,
  RetryableConflict,
  InvalidCommit
};
enum class PublishCompactionResult {
  Published,
  AlreadyPublished,
  StaleVersion,
  RetryableConflict,
  ManifestKeyCollision,
  InvalidManifest
};

// Implements the table-level CURRENT protocol on top of a versioned metadata
// key/value store. It owns logical transition validation, but not WAL/DBC1 IO.
class TableMetadataStore {
 public:
  TableMetadataStore(std::string table_id, std::string metadata_prefix, std::shared_ptr<IMetadataStore> metadata);

  const std::string &table_id() const { return table_id_; }
  const std::string &current_key() const { return current_key_; }

  TableState NewTableState() const;
  std::optional<VersionedTableState> Load() const;

  InitializeTableResult Initialize(const TableState &initial, VersionedTableState *created = nullptr);
  AcquireWriterResult AcquireWriter(const VersionedTableState &expected, VersionedTableState *acquired = nullptr);
  PublishWalResult PublishWal(const VersionedTableState &expected, const std::string &commit_key,
                              const CommitRecord &record, VersionedTableState *published = nullptr);
  PublishCompactionResult PublishCompaction(const VersionedTableState &expected, const std::string &manifest_key,
                                            const BaseManifest &manifest, VersionedTableState *published = nullptr);

  std::optional<CommitRecord> LoadCommitRecord(const std::string &commit_key) const;
  std::optional<BaseManifest> LoadManifest(const std::string &manifest_key) const;

 private:
  bool IsValidInitialState(const TableState &state) const;
  bool IsValidTransition(const TableState &previous, const TableState &next) const;
  std::string ResolveRelativeKey(const std::string &relative_key, const char *required_prefix) const;

  std::string table_id_;
  std::string metadata_prefix_;
  std::string current_key_;
  std::shared_ptr<IMetadataStore> metadata_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLEMETADATASTORE_H
