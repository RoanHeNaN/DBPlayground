#ifndef DBPLAYGROUND_METADATAMANIFESTSTORE_H
#define DBPLAYGROUND_METADATAMANIFESTSTORE_H

#include <memory>
#include <string>

#include "Cloud/IManifestStore.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableMetadataStore.h"

namespace dbplay {

// Loads immutable base manifests from the same IMetadataStore prefix as CURRENT.
class MetadataManifestStore : public IManifestStore {
 public:
  MetadataManifestStore(std::string table_id, std::string metadata_prefix, std::shared_ptr<IMetadataStore> metadata);

  std::optional<BaseManifest> Load(const TableDescriptor &table, const std::string &manifest_key) const override;

 private:
  TableMetadataStore tables_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_METADATAMANIFESTSTORE_H
