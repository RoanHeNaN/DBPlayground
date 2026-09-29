#include "Cloud/CloudTableSource.h"

#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"

namespace dbplay {
namespace {

class CompositeBatchCursor : public IBatchCursor {
 public:
  CompositeBatchCursor(std::unique_ptr<IBatchCursor> indexed_data, std::unique_ptr<IBatchCursor> wal)
      : indexed_data_(std::move(indexed_data)), wal_(std::move(wal)) {}

  bool Next(Chunk *out) override {
    if (reading_indexed_data_) {
      if (indexed_data_ != nullptr && indexed_data_->Next(out)) {
        return true;
      }
      reading_indexed_data_ = false;
    }
    return wal_ != nullptr && wal_->Next(out);
  }

 private:
  std::unique_ptr<IBatchCursor> indexed_data_;
  std::unique_ptr<IBatchCursor> wal_;
  bool reading_indexed_data_ = true;
};

class SnapshotWalCursor : public IBatchCursor {
 public:
  SnapshotWalCursor(std::vector<Chunk> chunks, std::vector<int> projection)
      : chunks_(std::move(chunks)), projection_(std::move(projection)) {}

  bool Next(Chunk *out) override {
    if (pos_ == chunks_.size()) return false;
    const Chunk &src = chunks_[pos_++];
    out->column_ids = projection_;
    out->columns.clear();
    for (int column : projection_) {
      if (column < 0 || static_cast<size_t>(column) >= src.columns.size())
        throw std::out_of_range("CloudTableSource: invalid projection");
      out->columns.push_back(src.columns[column]);
    }
    out->row_count = src.row_count;
    return true;
  }

 private:
  std::vector<Chunk> chunks_;
  std::vector<int> projection_;
  size_t pos_ = 0;
};

}  // namespace

CloudTableSource::CloudTableSource(Schema schema, TableSnapshot snapshot, std::shared_ptr<IFileFormat> data_format,
                                   std::shared_ptr<IStorage> files)
    : schema_(std::move(schema)),
      snapshot_(std::move(snapshot)),
      data_format_(std::move(data_format)),
      files_(std::move(files)) {
  if (data_format_ == nullptr || files_ == nullptr || !SchemasEqual(schema_, data_format_->schema())) {
    throw std::invalid_argument("CloudTableSource: format or file storage is null");
  }
}

std::unique_ptr<IBatchCursor> CloudTableSource::Scan(const std::vector<int> &projection) {
  auto indexed_data = data_format_->Scan(*files_, snapshot_.data_files, projection);
  auto wal = std::make_unique<SnapshotWalCursor>(snapshot_.wal_chunks, projection);
  return std::make_unique<CompositeBatchCursor>(std::move(indexed_data), std::move(wal));
}

}  // namespace dbplay
