#include "Cloud/CloudTableSource.h"

#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"

namespace dbplay {
namespace {

class CompositeBatchCursor : public IBatchCursor {
 public:
  CompositeBatchCursor(std::unique_ptr<IBatchCursor> compacted_data, std::unique_ptr<IBatchCursor> wal)
      : compacted_data_(std::move(compacted_data)), wal_(std::move(wal)) {}

  bool Next(Chunk *out) override {
    if (reading_compacted_data_) {
      if (compacted_data_ != nullptr && compacted_data_->Next(out)) {
        return true;
      }
      reading_compacted_data_ = false;
    }
    return wal_ != nullptr && wal_->Next(out);
  }

 private:
  std::unique_ptr<IBatchCursor> compacted_data_;
  std::unique_ptr<IBatchCursor> wal_;
  bool reading_compacted_data_ = true;
};

}  // namespace

CloudTableSource::CloudTableSource(Schema schema, TableSnapshot snapshot,
                                   std::shared_ptr<IFileFormat> compacted_data_format,
                                   std::shared_ptr<IFileFormat> wal_format, std::shared_ptr<IStorage> files)
    : schema_(std::move(schema)),
      snapshot_(std::move(snapshot)),
      compacted_data_format_(std::move(compacted_data_format)),
      wal_format_(std::move(wal_format)),
      files_(std::move(files)) {
  if (compacted_data_format_ == nullptr || wal_format_ == nullptr || files_ == nullptr ||
      !SchemasEqual(schema_, compacted_data_format_->schema()) || !SchemasEqual(schema_, wal_format_->schema())) {
    throw std::invalid_argument("CloudTableSource: format or file storage is null");
  }
}

std::unique_ptr<IBatchCursor> CloudTableSource::Scan(const std::vector<int> &projection) {
  auto compacted_data = compacted_data_format_->Scan(*files_, snapshot_.compacted_data_files, projection);
  auto wal = wal_format_->Scan(*files_, snapshot_.wal_files, projection);
  return std::make_unique<CompositeBatchCursor>(std::move(compacted_data), std::move(wal));
}

}  // namespace dbplay
