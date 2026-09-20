#ifndef DBPLAYGROUND_CLOUDTABLESOURCE_H
#define DBPLAYGROUND_CLOUDTABLESOURCE_H

#include <memory>
#include <vector>

#include "Cloud/CloudTypes.h"
#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"
#include "Table/ITableSource.h"

namespace dbplay {

// Immutable per-query source: compacted data files followed by the committed
// WAL tail captured in one TableSnapshot.
class CloudTableSource : public ITableSource {
 public:
  CloudTableSource(Schema schema, TableSnapshot snapshot, std::shared_ptr<IFileFormat> compacted_data_format,
                   std::shared_ptr<IFileFormat> wal_format, std::shared_ptr<IStorage> files);

  const Schema &schema() const override { return schema_; }
  std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &projection) override;

  const TableSnapshot &snapshot() const { return snapshot_; }

 private:
  Schema schema_;
  TableSnapshot snapshot_;
  std::shared_ptr<IFileFormat> compacted_data_format_;
  std::shared_ptr<IFileFormat> wal_format_;
  std::shared_ptr<IStorage> files_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTABLESOURCE_H
