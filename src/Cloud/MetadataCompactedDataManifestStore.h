#ifndef DBPLAYGROUND_METADATACOMPACTEDDATAMANIFESTSTORE_H
#define DBPLAYGROUND_METADATACOMPACTEDDATAMANIFESTSTORE_H

#include <memory>
#include <string>

#include "Cloud/ICompactedDataManifestStore.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableCurrentStateStore.h"

namespace dbplay {

// Loads immutable compacted-data manifests from the same IMetadataStore prefix
// as CURRENT.
class MetadataCompactedDataManifestStore : public ICompactedDataManifestStore {
 public:
  MetadataCompactedDataManifestStore(std::string table_id, std::string metadata_prefix,
                                     std::shared_ptr<IMetadataStore> metadata);

  std::optional<CompactedDataManifest> Load(const TableDescriptor &table,
                                            const std::string &compacted_data_manifest_key) const override;

 private:
  TableCurrentStateStore current_state_store_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_METADATACOMPACTEDDATAMANIFESTSTORE_H
