#ifndef DBPLAYGROUND_TABLECURRENTSTATESTORE_H
#define DBPLAYGROUND_TABLECURRENTSTATESTORE_H

#include <memory>
#include <optional>
#include <string>

#include "Metadata/IMetadataStore.h"
#include "Metadata/TableMetadataTypes.h"

namespace dbplay {

struct VersionedCurrentTableState {
  CurrentTableState current_state;
  MetadataVersion current_metadata_version;
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
  CompactedDataManifestKeyCollision,
  InvalidCompactedDataManifest
};

// Implements the table-level CURRENT protocol on top of a versioned metadata
// key/value store. It owns logical transition validation, but not WAL/DBC1 IO.
class TableCurrentStateStore {
 public:
  TableCurrentStateStore(std::string table_id, std::string metadata_prefix, std::shared_ptr<IMetadataStore> metadata);

  const std::string &table_id() const { return table_id_; }
  const std::string &current_key() const { return current_key_; }

  CurrentTableState NewCurrentTableState() const;
  std::optional<VersionedCurrentTableState> Load() const;

  InitializeTableResult Initialize(const CurrentTableState &initial, VersionedCurrentTableState *created = nullptr);
  AcquireWriterResult AcquireWriter(const VersionedCurrentTableState &expected,
                                    VersionedCurrentTableState *acquired = nullptr);
  PublishWalResult PublishWal(const VersionedCurrentTableState &expected, const std::string &commit_key,
                              const CommitRecord &record, VersionedCurrentTableState *published = nullptr);
  // Persist an immutable compacted-data manifest, then atomically publish it
  // through CURRENT. This advances compacted_cursor only; committed_cursor and
  // writer_epoch are preserved, so compaction never fences an active writer.
  // A failed CURRENT CAS leaves the manifest harmlessly unreachable and the
  // caller can classify/retry the race using the returned result.
  PublishCompactionResult PublishCompaction(const VersionedCurrentTableState &expected,
                                            const std::string &compacted_data_manifest_key,
                                            const CompactedDataManifest &compacted_data_manifest,
                                            VersionedCurrentTableState *published = nullptr);

  std::optional<CommitRecord> LoadCommitRecord(const std::string &commit_key) const;
  std::optional<CompactedDataManifest> LoadCompactedDataManifest(const std::string &compacted_data_manifest_key) const;

 private:
  bool IsValidInitialState(const CurrentTableState &state) const;
  bool IsValidTransition(const CurrentTableState &previous, const CurrentTableState &next) const;
  std::string ResolveRelativeKey(const std::string &relative_key, const char *required_prefix) const;

  std::string table_id_;
  std::string metadata_prefix_;
  std::string current_key_;
  std::shared_ptr<IMetadataStore> metadata_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLECURRENTSTATESTORE_H
