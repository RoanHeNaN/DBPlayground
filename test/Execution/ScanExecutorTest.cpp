//
// T4 acceptance: a scan->project->collect operator running purely against
// ITableSource. See docs/design/ColumnarTableSource.md.
//

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Common/Codec.h"
#include "Common/Slice.h"
#include "Common/Type.h"
#include "Execution/ScanExecutor.h"
#include "Storage/BPlusTreeEngine.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "Table/ITableSource.h"
#include "Table/RowCodec.h"
#include "Table/RowTableSource.h"
#include "Table/Value.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
constexpr char kDb[] = "scanexec_test.db";

Schema MakeSchema() {
  return {{"id", Type::Int64}, {"name", Type::String}, {"score", Type::Double}, {"active", Type::Bool}};
}

// Build a table over the B+Tree engine with ids 0..n-1 and return the source.
// The executor under test only ever sees it as an ITableSource&.
std::unique_ptr<RowTableSource> BuildTable(BPlusTreeEngine *engine, const Schema &schema, int64_t n) {
  RowCodec codec(schema);
  for (int64_t id = 0; id < n; ++id) {
    std::vector<Value> row{Value::Int64(id), Value::String("name" + std::to_string(id)),
                           Value::Double(static_cast<double>(id) * 1.5), Value::Bool(id % 2 == 0)};
    (void)engine->Insert(encode_key<int64_t>(id), Slice(codec.Encode(row)));
  }
  return std::make_unique<RowTableSource>(schema, engine, /*batch_rows=*/32);
}
}  // namespace

TEST(ScanExecutorTest, ProjectReordersColumnsAndCollectsAllRows) {
  remove(kDb);
  auto dm = std::make_shared<DiskManager>(kDb);
  auto bpm = std::make_shared<BufferPoolManager>(200, dm);
  BPlusTreeEngine engine(bpm, /*is_new=*/true);
  const Schema schema = MakeSchema();
  auto table = BuildTable(&engine, schema, 100);

  // Projection {score(2), id(0)} -- note the reordered output columns.
  ITableSource &src = *table;  // the operator sees only this
  std::vector<std::vector<Value>> rows = CollectProjected(src, {2, 0});

  ASSERT_EQ(rows.size(), 100u);
  for (int64_t i = 0; i < 100; ++i) {
    // column 0 of the output is score, column 1 is id (projection order).
    EXPECT_DOUBLE_EQ(rows[i][0].AsDouble(), static_cast<double>(i) * 1.5);
    EXPECT_EQ(rows[i][1].AsInt64(), i);
  }

  remove(kDb);
}

TEST(ScanExecutorTest, ProjectStringAndBoolColumns) {
  remove(kDb);
  auto dm = std::make_shared<DiskManager>(kDb);
  auto bpm = std::make_shared<BufferPoolManager>(200, dm);
  BPlusTreeEngine engine(bpm, /*is_new=*/true);
  const Schema schema = MakeSchema();
  auto table = BuildTable(&engine, schema, 5);

  ITableSource &src = *table;
  std::vector<std::vector<Value>> rows = CollectProjected(src, {1, 3});  // name, active

  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0][0].AsString(), "name0");
  EXPECT_EQ(rows[4][0].AsString(), "name4");
  EXPECT_EQ(rows[0][1].AsBool(), true);   // id 0 -> active
  EXPECT_EQ(rows[1][1].AsBool(), false);  // id 1 -> inactive

  remove(kDb);
}

}  // namespace dbplay
