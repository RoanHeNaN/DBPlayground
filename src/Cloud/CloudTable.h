#ifndef DBPLAYGROUND_CLOUDTABLE_H
#define DBPLAYGROUND_CLOUDTABLE_H

#include <cstddef>
#include <memory>
#include <optional>

#include "Cloud/CloudTableCompactor.h"
#include "Cloud/CloudTableSource.h"
#include "Cloud/CloudTableWriter.h"
#include "Cloud/CloudTypes.h"
#include "Cloud/IBatchCommitResolver.h"
#include "Cloud/IManifestStore.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/TableSnapshotLoader.h"
#include "Metadata/IMetadataStore.h"
#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"
#include "Table/ITableSource.h"

namespace dbplay {

// Long-lived per-table composition root cached by a node. Each query gets a
// new immutable CloudTableSource; each active owner gets a table-local writer.
class CloudTable {
 public:
  CloudTable(TableDescriptor descriptor, std::shared_ptr<IStorage> files, std::shared_ptr<IMetadataStore> metadata,
             std::shared_ptr<IManifestStore> manifests, std::shared_ptr<IFileFormat> base_format,
             std::shared_ptr<IFileFormat> wal_format);

  const TableDescriptor &descriptor() const { return descriptor_; }

  std::optional<TableSnapshot> LoadSnapshot() const;
  std::unique_ptr<ITableSource> OpenSnapshot() const;
  std::unique_ptr<CloudTableWriter> NewWriter(std::shared_ptr<IObjectKeyGenerator> keys,
                                              std::shared_ptr<IBatchCommitResolver> batches,
                                              size_t max_publish_attempts = 4) const;
  std::unique_ptr<CloudTableCompactor> NewCompactor(std::shared_ptr<IObjectKeyGenerator> keys,
                                                    size_t max_publish_attempts = 4) const;

 private:
  TableDescriptor descriptor_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IMetadataStore> metadata_;
  std::shared_ptr<IManifestStore> manifests_;
  std::shared_ptr<IFileFormat> base_format_;
  std::shared_ptr<IFileFormat> wal_format_;
};

class ICloudTableProvider {
 public:
  virtual ~ICloudTableProvider() = default;
  virtual std::shared_ptr<CloudTable> Get(const TableDescriptor &descriptor) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTABLE_H
