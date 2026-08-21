#ifndef DBPLAYGROUND_TABLESTATE_H
#define DBPLAYGROUND_TABLESTATE_H

#include <cstdint>
#include <string>
#include <vector>

namespace dbplay {

// The complete, small root state stored in a table's mutable CURRENT key.
// MetadataVersion is deliberately not part of this value: it belongs to the
// backing IMetadataStore and is only used as the CAS precondition.
struct TableState {
  static constexpr uint32_t kFormatVersion = 1;

  uint32_t format_version = kFormatVersion;
  std::string table_id;
  uint64_t state_version = 0;
  uint64_t writer_epoch = 0;
  uint64_t committed_cursor = 0;
  uint64_t indexed_cursor = 0;
  std::string base_manifest;
  std::string commit_head;

  bool operator==(const TableState &other) const;
  bool operator!=(const TableState &other) const { return !(*this == other); }
};

// An immutable description of one published WAL range. CURRENT points at the
// newest record; parent_commit makes all committed WAL discoverable without
// LIST. A later snapshot loader will walk only as far as indexed_cursor.
struct CommitRecord {
  static constexpr uint32_t kFormatVersion = 1;

  uint32_t format_version = kFormatVersion;
  std::string table_id;
  uint64_t writer_epoch = 0;
  uint64_t first_cursor = 0;
  uint64_t last_cursor = 0;
  std::string parent_commit;
  std::vector<std::string> wal_files;
  std::vector<std::string> batch_ids;

  bool operator==(const CommitRecord &other) const;
  bool operator!=(const CommitRecord &other) const { return !(*this == other); }
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLESTATE_H
