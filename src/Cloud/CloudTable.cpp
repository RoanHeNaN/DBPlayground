#include "Cloud/CloudTable.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"
#include "Cloud/CloudWalIndexer.h"

namespace dbplay {

CloudTable::CloudTable(TableDescriptor descriptor, std::shared_ptr<IStorage> files,
                       std::shared_ptr<IMetadataStore> metadata,
                       std::shared_ptr<ICompactedDataManifestStore> compacted_data_manifest_store,
                       std::shared_ptr<IFileFormat> compacted_data_format, std::shared_ptr<IFileFormat> wal_format,
                       WalProbePolicy probe_policy)
    : descriptor_(std::move(descriptor)),
      files_(std::move(files)),
      metadata_(std::move(metadata)),
      compacted_data_manifest_store_(std::move(compacted_data_manifest_store)),
      compacted_data_format_(std::move(compacted_data_format)),
      wal_format_(std::move(wal_format)),
      probe_policy_(probe_policy),
      probe_limit_(probe_policy.initial) {
  if (descriptor_.table_id.empty() || descriptor_.metadata_prefix.empty() || descriptor_.file_prefix.empty() ||
      files_ == nullptr || metadata_ == nullptr || compacted_data_manifest_store_ == nullptr ||
      compacted_data_format_ == nullptr || wal_format_ == nullptr ||
      !SchemasEqual(descriptor_.schema, compacted_data_format_->schema()) ||
      !SchemasEqual(descriptor_.schema, wal_format_->schema()) || probe_policy_.minimum == 0 ||
      probe_policy_.maximum < probe_policy_.minimum || probe_policy_.initial < probe_policy_.minimum ||
      probe_policy_.initial > probe_policy_.maximum) {
    throw std::invalid_argument("CloudTable: invalid descriptor or null dependency");
  }
}

std::optional<TableSnapshot> CloudTable::LoadSnapshot(bool probe_unpublished) const {
  TableSnapshotLoader loader(descriptor_, metadata_, compacted_data_manifest_store_, files_);
  const size_t limit = probe_unpublished ? probe_limit_.load() : 0;
  auto snapshot = loader.Load(limit);
  if (probe_unpublished && snapshot) {
    if (snapshot->probe_limit_reached && limit < probe_policy_.maximum)
      probe_limit_.store(limit > probe_policy_.maximum / 2 ? probe_policy_.maximum : limit * 2);
    else if (snapshot->probed_wal_count < limit / 4 && limit > probe_policy_.minimum)
      probe_limit_.store(std::max(probe_policy_.minimum, limit / 2));
  }
  return snapshot;
}

std::unique_ptr<ITableSource> CloudTable::OpenSnapshot(bool probe_unpublished) const {
  auto snapshot = LoadSnapshot(probe_unpublished);
  if (!snapshot.has_value()) {
    return nullptr;
  }
  return std::make_unique<CloudTableSource>(descriptor_.schema, std::move(*snapshot), compacted_data_format_,
                                            wal_format_, files_);
}

std::unique_ptr<CloudTableWriter> CloudTable::NewWriter(std::shared_ptr<IObjectKeyGenerator> keys,
                                                        std::shared_ptr<IBatchCommitResolver> batches,
                                                        size_t max_publish_attempts) const {
  return std::make_unique<CloudTableWriter>(descriptor_, files_, metadata_, wal_format_, std::move(keys),
                                            std::move(batches), max_publish_attempts);
}

std::unique_ptr<CloudWalIndexer> CloudTable::NewIndexer(std::shared_ptr<IObjectKeyGenerator> keys,
                                                        size_t max_publish_attempts) const {
  return std::make_unique<CloudWalIndexer>(descriptor_, files_, metadata_, compacted_data_format_, wal_format_,
                                           std::move(keys), max_publish_attempts);
}

}  // namespace dbplay
