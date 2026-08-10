//
// T2 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// RowCodec is the schema-aware bridge between a row/KV engine's opaque value
// blob and typed columns. It lives in the table/application layer (it knows the
// schema); the storage engine stays schema-agnostic.
//
// Blob format (fields in schema order):
//   fixed-width types  -> raw value bytes (Int32=4, Int64=8, Float=4,
//                         Double=8, Bool=1)
//   variable-length    -> uint32 length + that many bytes (String/Blob)
//

#ifndef DBPLAYGROUND_ROWCODEC_H
#define DBPLAYGROUND_ROWCODEC_H

#include <string>
#include <vector>

#include "Common/Slice.h"
#include "Table/Column.h"
#include "Table/Schema.h"
#include "Table/Value.h"

namespace dbplay {

class RowCodec {
 public:
  explicit RowCodec(Schema schema) : schema_(std::move(schema)) {}

  const Schema &schema() const { return schema_; }

  // Serialize a full row (row.size() must equal schema size) to a value blob.
  std::string Encode(const std::vector<Value> &row) const;

  // Deserialize a whole row.
  std::vector<Value> Decode(const Slice &blob) const;

  // Decode only the projected fields, appending each into cols[k] (cols is
  // parallel to `projection`; projection holds schema field indices in output
  // order, and cols[k].type() must match schema[projection[k]].type). Non-
  // projected fields are walked and skipped. This is the row -> column-batch
  // materialization used by RowTableSource (T3).
  void DecodeInto(const Slice &blob, const std::vector<int> &projection, std::vector<Column> *cols) const;

 private:
  Schema schema_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ROWCODEC_H
