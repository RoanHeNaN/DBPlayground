#ifndef DBPLAYGROUND_CLOUDTABLEWRITER_H
#define DBPLAYGROUND_CLOUDTABLEWRITER_H

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "Cloud/CloudTypes.h"
#include "Cloud/IBatchCommitResolver.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Metadata/IMetadataStore.h"
#include "Metadata/TableCurrentStateStore.h"
#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"

namespace dbplay {

// Table-local writer state machine. A coordinator must serialize calls to this
// object; client concurrency is combined above it into CloudImportBatch.
class CloudTableWriter {
 public:
  CloudTableWriter(TableDescriptor table, std::shared_ptr<IStorage> files, std::shared_ptr<IMetadataStore> metadata,
                   std::shared_ptr<IFileFormat> wal_format, std::shared_ptr<IObjectKeyGenerator> keys,
                   std::shared_ptr<IBatchCommitResolver> batches, size_t max_publish_attempts = 4);

  CloudWriterStartResult Start();
  CloudImportResult Import(const CloudImportBatch &batch);

  bool started() const { return current_state_.has_value(); }
  uint64_t writer_epoch() const { return current_state_.has_value() ? current_state_->current_state.writer_epoch : 0; }

 private:
  bool IsValidBatch(const CloudImportBatch &batch) const;
  std::string ResolveWalKey(const std::string &relative_key) const;
  CloudImportResult Result(CloudImportCode code) const;

  TableDescriptor table_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IFileFormat> wal_format_;
  std::shared_ptr<IObjectKeyGenerator> keys_;
  std::shared_ptr<IBatchCommitResolver> batches_;
  TableCurrentStateStore current_state_store_;
  size_t max_publish_attempts_;
  std::optional<VersionedCurrentTableState> current_state_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTABLEWRITER_H
