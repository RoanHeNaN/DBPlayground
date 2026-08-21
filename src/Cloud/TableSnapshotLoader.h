#ifndef DBPLAYGROUND_TABLESNAPSHOTLOADER_H
#define DBPLAYGROUND_TABLESNAPSHOTLOADER_H

#include <memory>
#include <optional>

#include "Cloud/CloudTypes.h"
#include "Cloud/IManifestStore.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableMetadataStore.h"

namespace dbplay {

// Resolves one immutable query view from known keys only:
// CURRENT -> base manifest + backward commit chain. It never calls LIST.
class TableSnapshotLoader {
 public:
  TableSnapshotLoader(TableDescriptor table, std::shared_ptr<IMetadataStore> metadata,
                      std::shared_ptr<IManifestStore> manifests);

  std::optional<TableSnapshot> Load() const;

 private:
  TableDescriptor table_;
  std::shared_ptr<IManifestStore> manifests_;
  TableMetadataStore table_metadata_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLESNAPSHOTLOADER_H
