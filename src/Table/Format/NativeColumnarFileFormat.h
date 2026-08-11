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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"
#include "Table/Schema.h"

namespace dbplay {

class NativeColumnarFileFormat : public IFileFormat {
 public:
  explicit NativeColumnarFileFormat(Schema schema) : schema_(std::move(schema)) {}

  const Schema &schema() const override { return schema_; }

  std::unique_ptr<IBatchCursor> Scan(IStorage &store, const std::vector<std::string> &files,
                                     const std::vector<int> &projection) override;

  std::unique_ptr<IChunkWriter> OpenWriter(IStorage &store, const std::string &path) override;

 private:
  Schema schema_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_NATIVECOLUMNARFILEFORMAT_H
