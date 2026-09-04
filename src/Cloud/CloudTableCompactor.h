#ifndef DBPLAYGROUND_CLOUDTABLECOMPACTOR_H
#define DBPLAYGROUND_CLOUDTABLECOMPACTOR_H

#include <cstddef>
#include <memory>
#include <string>

#include "Cloud/CloudTypes.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableMetadataStore.h"
#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"

namespace dbplay {

// Background WAL -> DBC1 flush. It never fences a writer: a successful publish
// only advances indexed_cursor and swaps the immutable base-manifest pointer.
class CloudTableCompactor {
 public:
  CloudTableCompactor(TableDescriptor table, std::shared_ptr<IStorage> files, std::shared_ptr<IMetadataStore> metadata,
                      std::shared_ptr<IFileFormat> base_format, std::shared_ptr<IFileFormat> wal_format,
                      std::shared_ptr<IObjectKeyGenerator> keys, size_t max_publish_attempts = 4);

  CloudCompactResult Compact();

 private:
  bool IsSafeRelativeKey(const std::string &key, const std::string &required_prefix) const;
  std::string ResolveFileKey(const std::string &relative_key, const std::string &required_prefix) const;
  CloudCompactResult Result(CloudCompactCode code, const TableState &state) const;
  void WriteBaseFile(const std::string &path, const std::vector<std::string> &wal_files) const;

  TableDescriptor table_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IMetadataStore> metadata_;
  std::shared_ptr<IFileFormat> base_format_;
  std::shared_ptr<IFileFormat> wal_format_;
  std::shared_ptr<IObjectKeyGenerator> keys_;
  TableMetadataStore table_metadata_;
  size_t max_publish_attempts_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTABLECOMPACTOR_H
