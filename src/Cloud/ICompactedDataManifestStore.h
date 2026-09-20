#ifndef DBPLAYGROUND_COMPACTEDDATAMANIFESTSTORE_H
#define DBPLAYGROUND_COMPACTEDDATAMANIFESTSTORE_H

#include <optional>
#include <string>

#include "Cloud/CloudTypes.h"

namespace dbplay {

// Immutable compacted-data manifest access. A concrete implementation may
// store the encoded bytes in IMetadataStore or in the file store; snapshot
// loading only needs this domain view.
class ICompactedDataManifestStore {
 public:
  virtual ~ICompactedDataManifestStore() = default;
  virtual std::optional<CompactedDataManifest> Load(const TableDescriptor &table,
                                                    const std::string &compacted_data_manifest_key) const = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_COMPACTEDDATAMANIFESTSTORE_H
