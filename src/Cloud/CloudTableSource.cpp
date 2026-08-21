#include "Cloud/CloudTableSource.h"

#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"

namespace dbplay {
namespace {

class CompositeBatchCursor : public IBatchCursor {
 public:
  CompositeBatchCursor(std::unique_ptr<IBatchCursor> base, std::unique_ptr<IBatchCursor> wal)
      : base_(std::move(base)), wal_(std::move(wal)) {}

  bool Next(Chunk *out) override {
    if (reading_base_) {
      if (base_ != nullptr && base_->Next(out)) {
        return true;
      }
      reading_base_ = false;
    }
    return wal_ != nullptr && wal_->Next(out);
  }

 private:
  std::unique_ptr<IBatchCursor> base_;
  std::unique_ptr<IBatchCursor> wal_;
  bool reading_base_ = true;
};

}  // namespace

CloudTableSource::CloudTableSource(Schema schema, TableSnapshot snapshot, std::shared_ptr<IFileFormat> base_format,
                                   std::shared_ptr<IFileFormat> wal_format, std::shared_ptr<IStorage> files)
    : schema_(std::move(schema)),
      snapshot_(std::move(snapshot)),
      base_format_(std::move(base_format)),
      wal_format_(std::move(wal_format)),
      files_(std::move(files)) {
  if (base_format_ == nullptr || wal_format_ == nullptr || files_ == nullptr ||
      !SchemasEqual(schema_, base_format_->schema()) || !SchemasEqual(schema_, wal_format_->schema())) {
    throw std::invalid_argument("CloudTableSource: format or file storage is null");
  }
}

std::unique_ptr<IBatchCursor> CloudTableSource::Scan(const std::vector<int> &projection) {
  auto base = base_format_->Scan(*files_, snapshot_.data_files, projection);
  auto wal = wal_format_->Scan(*files_, snapshot_.wal_files, projection);
  return std::make_unique<CompositeBatchCursor>(std::move(base), std::move(wal));
}

}  // namespace dbplay
