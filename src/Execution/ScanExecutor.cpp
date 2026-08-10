//
// T4 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//

#include "Execution/ScanExecutor.h"

#include <stdexcept>
#include <utility>

#include "Table/Chunk.h"

namespace dbplay {

Value ReadCell(const Column &col, size_t row) {
  switch (col.type()) {
    case Type::Int32:
      return Value::Int32(col.Get<int32_t>(row));
    case Type::Int64:
      return Value::Int64(col.Get<int64_t>(row));
    case Type::Float:
      return Value::Float(col.Get<float>(row));
    case Type::Double:
      return Value::Double(col.Get<double>(row));
    case Type::Bool:
      return Value::Bool(col.Get<bool>(row));
    case Type::String:
    case Type::Blob:
      return Value::String(col.GetBytes(row).ToString());
    default:
      throw std::invalid_argument("ReadCell: invalid column type");
  }
}

std::vector<std::vector<Value>> CollectProjected(ITableSource &src, const std::vector<int> &projection) {
  std::vector<std::vector<Value>> rows;
  auto cursor = src.Scan(projection);
  Chunk chunk;
  while (cursor->Next(&chunk)) {
    for (size_t r = 0; r < chunk.row_count; ++r) {
      std::vector<Value> row;
      row.reserve(chunk.columns.size());
      for (const Column &col : chunk.columns) {
        row.push_back(ReadCell(col, r));
      }
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

}  // namespace dbplay
