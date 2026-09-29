#ifndef DBPLAYGROUND_CLOUDWALINDEXER_H
#define DBPLAYGROUND_CLOUDWALINDEXER_H

#include <cstddef>
#include <memory>
#include <string>

#include "Cloud/CloudTypes.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/SequentialProtocol.h"
#include "Metadata/IMetadataStore.h"
#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"

namespace dbplay {

// Background sequential WAL -> columnar indexing. It discovers the contiguous
// tail beyond CURRENT, including deferred writes, then atomically publishes
// the new manifest and indexed/published watermarks.
class CloudWalIndexer {
 public:
  CloudWalIndexer(TableDescriptor table, std::shared_ptr<IStorage> files, std::shared_ptr<IMetadataStore> metadata,
                  std::shared_ptr<IFileFormat> compacted_data_format, std::shared_ptr<IFileFormat> wal_format,
                  std::shared_ptr<IObjectKeyGenerator> keys, size_t max_publish_attempts = 4);

  // Materialize up to one bounded pass of contiguous WAL files. The output is
  // invisible until CURRENT CAS succeeds; a concurrent writer does not block.
  CloudIndexResult Index();

 private:
  CloudIndexResult Result(CloudIndexCode code, const SequentialState &state) const;

  TableDescriptor table_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IMetadataStore> metadata_;
  std::shared_ptr<IFileFormat> compacted_data_format_;
  std::shared_ptr<IFileFormat> wal_format_;
  std::shared_ptr<IObjectKeyGenerator> keys_;
  SequentialStateStore current_state_store_;
  size_t max_publish_attempts_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDWALINDEXER_H
