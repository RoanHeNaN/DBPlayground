#ifndef DBPLAYGROUND_CLOUDSERVICES_H
#define DBPLAYGROUND_CLOUDSERVICES_H

#include <memory>

#include "Cloud/CloudTable.h"
#include "Cloud/CloudTypes.h"
#include "Cloud/ICloudCatalog.h"
#include "Cloud/IWriterRouting.h"
#include "Table/ITableSource.h"

namespace dbplay {

// Stateless SQL/frontend import path. Consistent hashing and RPC stay behind
// interfaces so this class contains no membership or network implementation.
class CloudImportService {
 public:
  CloudImportService(std::shared_ptr<ICloudCatalog> catalog, std::shared_ptr<IWriterRouter> router,
                     std::shared_ptr<IWriterTransport> transport);

  CloudImportResult Import(const TableName &table, const CloudImportBatch &batch);

 private:
  std::shared_ptr<ICloudCatalog> catalog_;
  std::shared_ptr<IWriterRouter> router_;
  std::shared_ptr<IWriterTransport> transport_;
};

// Writer-node RPC handler. The provider owns table-local lifecycle and group
// commit; this endpoint only dispatches a routed request to that coordinator.
class CloudWriterService {
 public:
  explicit CloudWriterService(std::shared_ptr<IWriteCoordinatorProvider> coordinators);

  CloudImportResult Import(const std::string &table_id, const CloudImportBatch &batch);

 private:
  std::shared_ptr<IWriteCoordinatorProvider> coordinators_;
};

// Any query node can open a table directly from committed object-store state;
// it does not route through the writer node.
class CloudQueryService {
 public:
  CloudQueryService(std::shared_ptr<ICloudCatalog> catalog, std::shared_ptr<ICloudTableProvider> tables);

  std::unique_ptr<ITableSource> Open(const TableName &table);

 private:
  std::shared_ptr<ICloudCatalog> catalog_;
  std::shared_ptr<ICloudTableProvider> tables_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDSERVICES_H
