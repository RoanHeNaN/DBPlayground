//
// T4 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// A minimal query operator: scan a table source with a projection and collect
// the rows. It depends ONLY on ITableSource / Chunk / Column / Value -- not on
// the KV engine, RowCodec, or whether the source is row- or column-oriented.
// That decoupling is the whole point of the ITableSource seam.
//

#ifndef DBPLAYGROUND_SCANEXECUTOR_H
#define DBPLAYGROUND_SCANEXECUTOR_H

#include <vector>

#include "Table/Column.h"
#include "Table/ITableSource.h"
#include "Table/Value.h"

namespace dbplay {

// Read one cell of a column as a typed Value (column -> Value, by type).
Value ReadCell(const Column &col, size_t row);

// Scan `src` projecting `projection` and collect every row as a tuple of Values
// (one Value per projected column, in projection order).
std::vector<std::vector<Value>> CollectProjected(ITableSource &src, const std::vector<int> &projection);

}  // namespace dbplay

#endif  // DBPLAYGROUND_SCANEXECUTOR_H
