//
// C3 of the composable storage model (docs/design/StorageAbstraction.md).
//
// NativeColumnarFileFormat: our own column-major file format, the first place
// C1 (IStorage bytes) and C2 (Codec) meet. One file = one row group; within it
// each column is stored contiguously as an encoded+compressed page, and a footer
// records per-column {encoding, compression, offset, sizes, value_count}. A scan
// reads ONLY the projected columns' byte ranges (projection pushdown) and
// resolves each column's codec from its persisted ids (self-describing).
//
// File layout:
//   [magic "DBC1"]
//   [column 0 page][column 1 page]...        (stored = encoded then compressed)
//   [footer]
//   [footer_size: uint64]
//   [magic "DBC1"]                            (trailing magic locates the footer)
//
// Schema types come from the format (given at construction, as RowTableSource
// takes its schema in memory); persisting the schema itself is the catalog TODO.
//

#ifndef DBPLAYGROUND_NATIVECOLUMNARFILEFORMAT_H
#define DBPLAYGROUND_NATIVECOLUMNARFILEFORMAT_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Storage/Encoding/Codec.h"
#include "Storage/File/IStorage.h"
#include "Table/Chunk.h"
#include "Table/Column.h"
#include "Table/Format/IFileFormat.h"
#include "Table/Schema.h"

namespace dbplay {

// R1 (docs/design/ColumnarReadPath.md): the positional read primitive for one
// file. It parses the footer and decodes the projected columns up front (bounded
// *output* per ReadRange; bounded *decode* arrives with row groups in R3), then
// serves any row range by slicing the decoded columns.
class NativeColumnarReader {
 public:
  NativeColumnarReader(const IInputFile &in, const Schema &schema, std::vector<int> projection);

  uint64_t row_count() const { return row_count_; }

  // Fill *out with rows [first_row, first_row+num_rows) (num_rows clamped to the
  // file end) of the projected columns. Returns false if the range is empty.
  bool ReadRange(uint64_t first_row, uint64_t num_rows, Chunk *out) const;

 private:
  std::vector<int> projection_;
  std::vector<Column> decoded_;  // projected columns, fully decoded (one per projection entry)
  uint64_t row_count_ = 0;
};

class NativeColumnarFileFormat : public IFileFormat {
 public:
  // `compression` is the codec new files are WRITTEN with; reads always resolve
  // each column's codec from the ids stored in the file, so a reader can open a
  // file regardless of how this format is configured. `batch_rows` is the row
  // window a Scan cursor yields per Chunk.
  explicit NativeColumnarFileFormat(Schema schema, CompressionId compression = CompressionId::None,
                                    size_t batch_rows = 1024)
      : schema_(std::move(schema)), write_compression_(compression), batch_rows_(batch_rows) {}

  const Schema &schema() const override { return schema_; }

  std::unique_ptr<IBatchCursor> Scan(IStorage &store, const std::vector<std::string> &files,
                                     const std::vector<int> &projection) override;

  std::unique_ptr<IChunkWriter> OpenWriter(IStorage &store, const std::string &path) override;

 private:
  Schema schema_;
  CompressionId write_compression_;
  size_t batch_rows_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_NATIVECOLUMNARFILEFORMAT_H
