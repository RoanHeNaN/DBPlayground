#ifndef DBPLAYGROUND_TABLESNAPSHOTLOADER_H
#define DBPLAYGROUND_TABLESNAPSHOTLOADER_H

#include <memory>
#include <optional>

#include "Cloud/CloudTypes.h"
#include "Cloud/ICompactedDataManifestStore.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableCurrentStateStore.h"

namespace dbplay {

// Resolves one immutable query view from known keys only:
// CURRENT -> compacted-data manifest + backward commit chain. It never calls LIST.
class TableSnapshotLoader {
 public:
  TableSnapshotLoader(TableDescriptor table, std::shared_ptr<IMetadataStore> metadata,
                      std::shared_ptr<ICompactedDataManifestStore> compacted_data_manifest_store);

  std::optional<TableSnapshot> Load() const;

 private:
  TableDescriptor table_;
  std::shared_ptr<ICompactedDataManifestStore> compacted_data_manifest_store_;
  TableCurrentStateStore current_state_store_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLESNAPSHOTLOADER_H
