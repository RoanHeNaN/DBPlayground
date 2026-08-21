#ifndef DBPLAYGROUND_IMANIFESTSTORE_H
#define DBPLAYGROUND_IMANIFESTSTORE_H

#include <optional>
#include <string>

#include "Cloud/CloudTypes.h"

namespace dbplay {

// Immutable base-manifest access. A concrete implementation may store the
// encoded bytes in IMetadataStore or in the file store; snapshot loading only
// needs this domain view.
class IManifestStore {
 public:
  virtual ~IManifestStore() = default;
  virtual std::optional<BaseManifest> Load(const TableDescriptor &table, const std::string &manifest_key) const = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IMANIFESTSTORE_H
