//
// T3 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// RowTableSource adapts a row/KV engine into an ITableSource: it scans the
// engine in key order (IKvCursor), decodes each row blob through a RowCodec for
// only the projected fields, and packs them into column batches (Chunks). So a
// schema-agnostic KV engine becomes a schema-aware, projectable table purely by
// row->column materialization -- reusing Phases 0-4 unchanged.
//

#ifndef DBPLAYGROUND_ROWTABLESOURCE_H
#define DBPLAYGROUND_ROWTABLESOURCE_H

#include <cstddef>
#include <memory>
#include <vector>

#include "Storage/IStorageEngine.h"
#include "Table/ITableSource.h"
#include "Table/RowCodec.h"
#include "Table/Schema.h"

namespace dbplay {

class RowTableSource : public ITableSource {
 public:
  // `engine` is borrowed (must outlive this source). Each KV value is a row
  // blob laid out per `schema` (see RowCodec).
  RowTableSource(Schema schema, IStorageEngine *engine, size_t batch_rows = 1024);

  const Schema &schema() const override { return codec_.schema(); }
  std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &projection) override;

 private:
  IStorageEngine *engine_;
  RowCodec codec_;
  size_t batch_rows_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ROWTABLESOURCE_H
