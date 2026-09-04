#include "Cloud/MetadataManifestStore.h"

#include <utility>

namespace dbplay {

MetadataManifestStore::MetadataManifestStore(std::string table_id, std::string metadata_prefix,
                                             std::shared_ptr<IMetadataStore> metadata)
    : tables_(std::move(table_id), std::move(metadata_prefix), std::move(metadata)) {}

std::optional<BaseManifest> MetadataManifestStore::Load(const TableDescriptor &table,
                                                        const std::string &manifest_key) const {
  if (table.table_id != tables_.table_id()) {
    return std::nullopt;
  }
  return tables_.LoadManifest(manifest_key);
}

}  // namespace dbplay
