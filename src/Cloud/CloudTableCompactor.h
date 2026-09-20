#ifndef DBPLAYGROUND_CLOUDTABLECOMPACTOR_H
#define DBPLAYGROUND_CLOUDTABLECOMPACTOR_H

#include <cstddef>
#include <memory>
#include <string>

#include "Cloud/CloudTypes.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableCurrentStateStore.h"
#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"

namespace dbplay {

// Background WAL -> compacted-data flush. It never fences a writer: a
// successful publish only advances compacted_cursor and swaps the immutable
// compacted-data manifest pointer.
class CloudTableCompactor {
 public:
  CloudTableCompactor(TableDescriptor table, std::shared_ptr<IStorage> files, std::shared_ptr<IMetadataStore> metadata,
                      std::shared_ptr<IFileFormat> compacted_data_format, std::shared_ptr<IFileFormat> wal_format,
                      std::shared_ptr<IObjectKeyGenerator> keys, size_t max_publish_attempts = 4);

  // Materialize the committed WAL tail into an immutable compacted-data file
  // and publish it as the new snapshot compacted-data set. The data-file write
  // and manifest write happen before the CURRENT CAS, so an unsuccessful
  // attempt is invisible (but may leave an orphan object for a future GC pass).
  // A concurrent writer is not blocked or fenced; the compactor retries
  // against the newer CURRENT state.
  CloudCompactResult Compact();

 private:
  bool IsSafeRelativeKey(const std::string &key, const std::string &required_prefix) const;
  std::string ResolveFileKey(const std::string &relative_key, const std::string &required_prefix) const;
  CloudCompactResult Result(CloudCompactCode code, const CurrentTableState &current_state) const;
  // Read the specified WAL files in cursor order and write their rows through
  // the compacted-data format. Existing compacted-data files are not read
  // here: Compact() retains them in the manifest and appends the new file.
  void WriteCompactedDataFile(const std::string &path, const std::vector<std::string> &wal_files) const;

  TableDescriptor table_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IMetadataStore> metadata_;
  std::shared_ptr<IFileFormat> compacted_data_format_;
  std::shared_ptr<IFileFormat> wal_format_;
  std::shared_ptr<IObjectKeyGenerator> keys_;
  TableCurrentStateStore current_state_store_;
  size_t max_publish_attempts_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTABLECOMPACTOR_H
