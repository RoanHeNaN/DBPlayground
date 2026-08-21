#include "Cloud/CloudServices.h"

#include <stdexcept>
#include <utility>

namespace dbplay {

CloudImportService::CloudImportService(std::shared_ptr<ICloudCatalog> catalog, std::shared_ptr<IWriterRouter> router,
                                       std::shared_ptr<IWriterTransport> transport)
    : catalog_(std::move(catalog)), router_(std::move(router)), transport_(std::move(transport)) {
  if (catalog_ == nullptr || router_ == nullptr || transport_ == nullptr) {
    throw std::invalid_argument("CloudImportService: dependency is null");
  }
}

CloudImportResult CloudImportService::Import(const TableName &table, const CloudImportBatch &batch) {
  const auto descriptor = catalog_->Resolve(table);
  if (!descriptor.has_value()) {
    return CloudImportResult{CloudImportCode::TableNotFound, 0, 0};
  }
  const auto target = router_->Route(descriptor->table_id);
  if (!target.has_value()) {
    return CloudImportResult{CloudImportCode::NoWriterAvailable, 0, 0};
  }
  return transport_->Import(*target, descriptor->table_id, batch);
}

CloudWriterService::CloudWriterService(std::shared_ptr<IWriteCoordinatorProvider> coordinators)
    : coordinators_(std::move(coordinators)) {
  if (coordinators_ == nullptr) {
    throw std::invalid_argument("CloudWriterService: coordinator provider is null");
  }
}

CloudImportResult CloudWriterService::Import(const std::string &table_id, const CloudImportBatch &batch) {
  auto coordinator = coordinators_->Get(table_id);
  if (coordinator == nullptr) {
    return CloudImportResult{CloudImportCode::TableNotFound, 0, 0};
  }
  return coordinator->Import(batch);
}

CloudQueryService::CloudQueryService(std::shared_ptr<ICloudCatalog> catalog,
                                     std::shared_ptr<ICloudTableProvider> tables)
    : catalog_(std::move(catalog)), tables_(std::move(tables)) {
  if (catalog_ == nullptr || tables_ == nullptr) {
    throw std::invalid_argument("CloudQueryService: dependency is null");
  }
}

std::unique_ptr<ITableSource> CloudQueryService::Open(const TableName &table) {
  const auto descriptor = catalog_->Resolve(table);
  if (!descriptor.has_value()) {
    return nullptr;
  }
  auto cloud_table = tables_->Get(*descriptor);
  if (cloud_table == nullptr) {
    return nullptr;
  }
  return cloud_table->OpenSnapshot();
}

}  // namespace dbplay
