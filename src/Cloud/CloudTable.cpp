#include "Cloud/CloudTable.h"

#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"

namespace dbplay {

CloudTable::CloudTable(TableDescriptor descriptor, std::shared_ptr<IStorage> files,
                       std::shared_ptr<IMetadataStore> metadata, std::shared_ptr<IManifestStore> manifests,
                       std::shared_ptr<IFileFormat> base_format, std::shared_ptr<IFileFormat> wal_format)
    : descriptor_(std::move(descriptor)),
      files_(std::move(files)),
      metadata_(std::move(metadata)),
      manifests_(std::move(manifests)),
      base_format_(std::move(base_format)),
      wal_format_(std::move(wal_format)) {
  if (descriptor_.table_id.empty() || descriptor_.metadata_prefix.empty() || descriptor_.file_prefix.empty() ||
      files_ == nullptr || metadata_ == nullptr || manifests_ == nullptr || base_format_ == nullptr ||
      wal_format_ == nullptr || !SchemasEqual(descriptor_.schema, base_format_->schema()) ||
      !SchemasEqual(descriptor_.schema, wal_format_->schema())) {
    throw std::invalid_argument("CloudTable: invalid descriptor or null dependency");
  }
}

std::optional<TableSnapshot> CloudTable::LoadSnapshot() const {
  TableSnapshotLoader loader(descriptor_, metadata_, manifests_);
  return loader.Load();
}

std::unique_ptr<ITableSource> CloudTable::OpenSnapshot() const {
  auto snapshot = LoadSnapshot();
  if (!snapshot.has_value()) {
    return nullptr;
  }
  return std::make_unique<CloudTableSource>(descriptor_.schema, std::move(*snapshot), base_format_, wal_format_,
                                            files_);
}

std::unique_ptr<CloudTableWriter> CloudTable::NewWriter(std::shared_ptr<IObjectKeyGenerator> keys,
                                                        std::shared_ptr<IBatchCommitResolver> batches,
                                                        size_t max_publish_attempts) const {
  return std::make_unique<CloudTableWriter>(descriptor_, files_, metadata_, wal_format_, std::move(keys),
                                            std::move(batches), max_publish_attempts);
}

}  // namespace dbplay
