#ifndef DBPLAYGROUND_TABLEMETADATATYPES_H
#define DBPLAYGROUND_TABLEMETADATATYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace dbplay {

// The complete, small root state stored in a table's mutable CURRENT key.
// MetadataVersion is deliberately not part of this value: it belongs to the
// backing IMetadataStore and is only used as the CAS precondition.
struct CurrentTableState {
  static constexpr uint32_t kFormatVersion = 1;

  uint32_t format_version = kFormatVersion;
  std::string table_id;
  uint64_t current_state_version = 0;
  uint64_t writer_epoch = 0;
  uint64_t committed_cursor = 0;
  uint64_t compacted_cursor = 0;
  std::string compacted_data_manifest_key;
  std::string latest_commit_key;

  // compacted_cursor and compacted_data_manifest_key describe the same
  // immutable compacted-data set: the manifest contains files covering rows
  // through this cursor.

  bool operator==(const CurrentTableState &other) const;
  bool operator!=(const CurrentTableState &other) const { return !(*this == other); }
};

// An immutable description of one published WAL range. CURRENT points at the
// newest record; parent_commit_key makes all committed WAL discoverable without
// LIST. A later snapshot loader walks only the WAL tail after compacted_cursor.
struct CommitRecord {
  static constexpr uint32_t kFormatVersion = 1;

  uint32_t format_version = kFormatVersion;
  std::string table_id;
  uint64_t writer_epoch = 0;
  uint64_t first_cursor = 0;
  uint64_t last_cursor = 0;
  std::string parent_commit_key;
  std::vector<std::string> wal_files;
  std::vector<std::string> batch_ids;

  bool operator==(const CommitRecord &other) const;
  bool operator!=(const CommitRecord &other) const { return !(*this == other); }
};

// Immutable compacted-data file set covering rows through compacted_cursor.
// CURRENT stores only the manifest key; snapshot loading reads this object by
// that known key.
struct CompactedDataManifest {
  static constexpr uint32_t kFormatVersion = 1;

  uint32_t format_version = kFormatVersion;
  std::string table_id;
  uint64_t compacted_cursor = 0;
  std::vector<std::string> compacted_data_files;

  bool operator==(const CompactedDataManifest &other) const;
  bool operator!=(const CompactedDataManifest &other) const { return !(*this == other); }
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLEMETADATATYPES_H
