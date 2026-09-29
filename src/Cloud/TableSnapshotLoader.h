#ifndef DBPLAYGROUND_TABLESNAPSHOTLOADER_H
#define DBPLAYGROUND_TABLESNAPSHOTLOADER_H

#include <memory>
#include <optional>

#include "Cloud/CloudTypes.h"
#include "Cloud/ICompactedDataManifestStore.h"
#include "Cloud/SequentialProtocol.h"
#include "Metadata/IMetadataStore.h"

namespace dbplay {

// Resolves a CURRENT/manifest snapshot and contiguous WAL sequence without LIST.
class TableSnapshotLoader {
 public:
  TableSnapshotLoader(TableDescriptor table, std::shared_ptr<IMetadataStore> metadata,
                      std::shared_ptr<ICompactedDataManifestStore> compacted_data_manifest_store,
                      std::shared_ptr<IStorage> files);

  std::optional<TableSnapshot> Load(size_t probe_limit = 0) const;

 private:
  TableDescriptor table_;
  std::shared_ptr<IStorage> files_;
  SequentialStateStore state_store_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLESNAPSHOTLOADER_H
