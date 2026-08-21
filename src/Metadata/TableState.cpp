#include "Metadata/TableState.h"

namespace dbplay {

bool TableState::operator==(const TableState &other) const {
  return format_version == other.format_version && table_id == other.table_id && state_version == other.state_version &&
         writer_epoch == other.writer_epoch && committed_cursor == other.committed_cursor &&
         indexed_cursor == other.indexed_cursor && base_manifest == other.base_manifest &&
         commit_head == other.commit_head;
}

bool CommitRecord::operator==(const CommitRecord &other) const {
  return format_version == other.format_version && table_id == other.table_id && writer_epoch == other.writer_epoch &&
         first_cursor == other.first_cursor && last_cursor == other.last_cursor &&
         parent_commit == other.parent_commit && wal_files == other.wal_files && batch_ids == other.batch_ids;
}

}  // namespace dbplay
