//
// T1 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// A Chunk is one batch of rows in column-major form: a set of Columns for the
// projected schema fields, all of the same length (row_count). It carries only
// data + which schema fields it holds (column_ids); the actual field
// names/types come from the ITableSource's Schema, not from the Chunk.
// (This is the ClickHouse/DuckDB "Chunk" / "DataChunk" notion, not a
// self-describing "Block".)
//

#ifndef DBPLAYGROUND_CHUNK_H
#define DBPLAYGROUND_CHUNK_H

#include <vector>

#include "Table/Column.h"

namespace dbplay {

struct Chunk {
  std::vector<int> column_ids;  // projected schema indices, parallel to columns
  std::vector<Column> columns;  // one Column per projected field
  size_t row_count = 0;         // rows in this batch (length of each Column)
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CHUNK_H
