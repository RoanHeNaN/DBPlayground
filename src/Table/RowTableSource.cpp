//
// T3 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//

#include "Table/RowTableSource.h"

#include <utility>

#include "Storage/IKvCursor.h"

namespace dbplay {

namespace {

// Pulls rows from an ordered KV cursor and materializes the projected fields of
// each row blob into a column batch.
class RowBatchCursor : public IBatchCursor {
 public:
  RowBatchCursor(std::unique_ptr<IKvCursor> kv, const RowCodec *codec, std::vector<int> projection, size_t batch_rows)
      : kv_(std::move(kv)), codec_(codec), projection_(std::move(projection)), batch_rows_(batch_rows) {}

  bool Next(Chunk *out) override {
    if (!kv_->Valid()) {
      return false;
    }

    const Schema &schema = codec_->schema();
    Chunk chunk;
    chunk.column_ids = projection_;
    for (int field_idx : projection_) {
      chunk.columns.emplace_back(schema[field_idx].type);
    }

    size_t n = 0;
    while (kv_->Valid() && n < batch_rows_) {
      // Each KV value is a row blob; decode only the projected fields.
      codec_->DecodeInto(kv_->Value(), projection_, &chunk.columns);
      kv_->Next();
      ++n;
    }
    chunk.row_count = n;
    *out = std::move(chunk);
    return true;  // n >= 1 since kv_ was Valid() on entry
  }

 private:
  std::unique_ptr<IKvCursor> kv_;
  const RowCodec *codec_;
  std::vector<int> projection_;
  size_t batch_rows_;
};

}  // namespace

RowTableSource::RowTableSource(Schema schema, IStorageEngine *engine, size_t batch_rows)
    : engine_(engine), codec_(std::move(schema)), batch_rows_(batch_rows) {}

std::unique_ptr<IBatchCursor> RowTableSource::Scan(const std::vector<int> &projection) {
  return std::make_unique<RowBatchCursor>(engine_->NewCursor(), &codec_, projection, batch_rows_);
}

}  // namespace dbplay
