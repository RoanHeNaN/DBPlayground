//
// T3 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// ITableSource is the schema-aware, vectorized seam between the query layer and
// storage. The query layer calls Scan(projection) and consumes column batches
// (Chunks); it does not know whether the source is a row engine (RowTableSource)
// or a native columnar engine. This is the mediator the project set out to build.
//

#ifndef DBPLAYGROUND_ITABLESOURCE_H
#define DBPLAYGROUND_ITABLESOURCE_H

#include <memory>
#include <vector>

#include "Table/Chunk.h"
#include "Table/Schema.h"

namespace dbplay {

// Yields the scan result one Chunk (batch of rows, column-major) at a time.
class IBatchCursor {
 public:
  virtual ~IBatchCursor() = default;
  // Fill *out with the next batch; returns false when the scan is exhausted.
  virtual bool Next(Chunk *out) = 0;
};

class ITableSource {
 public:
  virtual ~ITableSource() = default;

  virtual const Schema &schema() const = 0;

  // Projection pushdown: `projection` are schema field indices to materialize,
  // in the desired output order. Only those columns appear in the Chunks.
  virtual std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &projection) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ITABLESOURCE_H
