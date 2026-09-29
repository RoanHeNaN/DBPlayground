#ifndef DBPLAYGROUND_CLOUDTABLEWRITER_H
#define DBPLAYGROUND_CLOUDTABLEWRITER_H

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "Cloud/CloudTypes.h"
#include "Cloud/IBatchCommitResolver.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/SequentialProtocol.h"
#include "Metadata/IMetadataStore.h"
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
  uint64_t writer_epoch() const { return current_state_.has_value() ? current_state_->state.writer_epoch : 0; }

 private:
  bool IsValidBatch(const CloudImportBatch &batch) const;
  std::string ResolveWalKey(const std::string &relative_key) const;
  CloudImportResult Result(CloudImportCode code) const;
  CloudImportResult Publish(uint64_t seq, uint64_t epoch);

  TableDescriptor table_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IFileFormat> wal_format_;
  std::shared_ptr<IObjectKeyGenerator> keys_;
  std::shared_ptr<IBatchCommitResolver> batches_;
  SequentialStateStore current_state_store_;
  size_t max_publish_attempts_;
  std::optional<VersionedSequentialState> current_state_;
  uint64_t next_seq_ = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDTABLEWRITER_H
