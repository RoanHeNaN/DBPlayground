#include "Metadata/TableMetadataTypes.h"

namespace dbplay {

bool CurrentTableState::operator==(const CurrentTableState &other) const {
  return format_version == other.format_version && table_id == other.table_id &&
         current_state_version == other.current_state_version && writer_epoch == other.writer_epoch &&
         committed_cursor == other.committed_cursor && compacted_cursor == other.compacted_cursor &&
         compacted_data_manifest_key == other.compacted_data_manifest_key &&
         latest_commit_key == other.latest_commit_key;
}

bool CommitRecord::operator==(const CommitRecord &other) const {
  return format_version == other.format_version && table_id == other.table_id && writer_epoch == other.writer_epoch &&
         first_cursor == other.first_cursor && last_cursor == other.last_cursor &&
         parent_commit_key == other.parent_commit_key && wal_files == other.wal_files && batch_ids == other.batch_ids;
}

bool CompactedDataManifest::operator==(const CompactedDataManifest &other) const {
  return format_version == other.format_version && table_id == other.table_id &&
         compacted_cursor == other.compacted_cursor && compacted_data_files == other.compacted_data_files;
}

}  // namespace dbplay
