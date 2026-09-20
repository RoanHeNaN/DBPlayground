#include "Cloud/MetadataCompactedDataManifestStore.h"

#include <utility>

namespace dbplay {

MetadataCompactedDataManifestStore::MetadataCompactedDataManifestStore(std::string table_id,
                                                                       std::string metadata_prefix,
                                                                       std::shared_ptr<IMetadataStore> metadata)
    : current_state_store_(std::move(table_id), std::move(metadata_prefix), std::move(metadata)) {}

std::optional<CompactedDataManifest> MetadataCompactedDataManifestStore::Load(
    const TableDescriptor &table, const std::string &compacted_data_manifest_key) const {
  if (table.table_id != current_state_store_.table_id()) {
    return std::nullopt;
  }
  return current_state_store_.LoadCompactedDataManifest(compacted_data_manifest_key);
}

}  // namespace dbplay
