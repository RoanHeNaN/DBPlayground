//
// T3 acceptance: RowTableSource turns the B+Tree KV engine into a schema-aware,
// projectable, batch-yielding ITableSource. See docs/design/ColumnarTableSource.md.
//

#include <algorithm>
#include <cstdio>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "Common/Codec.h"
#include "Common/Slice.h"
#include "Common/Type.h"
#include "Storage/BPlusTreeEngine.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "Table/RowCodec.h"
#include "Table/RowTableSource.h"
#include "Table/Value.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
constexpr char kDb[] = "rowtablesource_test.db";

// (id INT64, name STRING, score DOUBLE, active BOOL)
Schema MakeSchema() {
  return {{"id", Type::Int64}, {"name", Type::String}, {"score", Type::Double}, {"active", Type::Bool}};
}
}  // namespace

TEST(RowTableSourceTest, ScanWithProjectionAndBatching) {
  remove(kDb);
  auto dm = std::make_shared<DiskManager>(kDb);
  auto bpm = std::make_shared<BufferPoolManager>(500, dm);
  BPlusTreeEngine engine(bpm, /*is_new=*/true);

  const Schema schema = MakeSchema();
  RowCodec codec(schema);

  // Insert N rows (shuffled) keyed by id; value = full-row blob.
  const int64_t kN = 250;
  std::vector<int64_t> ids(kN);
  std::iota(ids.begin(), ids.end(), 0);
  std::shuffle(ids.begin(), ids.end(), std::mt19937(7));
  for (int64_t id : ids) {
    std::vector<Value> row{Value::Int64(id), Value::String("row" + std::to_string(id)),
                           Value::Double(static_cast<double>(id) * 1.5), Value::Bool(id % 2 == 0)};
    ASSERT_TRUE(engine.Insert(encode_key<int64_t>(id), Slice(codec.Encode(row)))) << "id=" << id;
  }

  // Project {id(0), score(2)}; small batches to exercise multi-Chunk output.
  RowTableSource src(schema, &engine, /*batch_rows=*/100);
  auto cursor = src.Scan({0, 2});

  int64_t expected_id = 0;
  int chunks = 0;
  Chunk chunk;
  while (cursor->Next(&chunk)) {
    ++chunks;
    ASSERT_EQ(chunk.columns.size(), 2u);  // only projected columns
    EXPECT_EQ(chunk.column_ids, (std::vector<int>{0, 2}));
    EXPECT_EQ(chunk.columns[0].type(), Type::Int64);   // id
    EXPECT_EQ(chunk.columns[1].type(), Type::Double);  // score
    for (size_t i = 0; i < chunk.row_count; ++i) {
      // Rows arrive in ascending id order (the KV cursor is ordered).
      EXPECT_EQ(chunk.columns[0].Get<int64_t>(i), expected_id);
      EXPECT_DOUBLE_EQ(chunk.columns[1].Get<double>(i), static_cast<double>(expected_id) * 1.5);
      ++expected_id;
    }
  }
  EXPECT_EQ(expected_id, kN);  // saw every row
  EXPECT_EQ(chunks, 3);        // 250 rows / 100 per batch -> 100,100,50

  remove(kDb);
}

TEST(RowTableSourceTest, FullProjectionIncludesStringColumn) {
  remove(kDb);
  auto dm = std::make_shared<DiskManager>(kDb);
  auto bpm = std::make_shared<BufferPoolManager>(100, dm);
  BPlusTreeEngine engine(bpm, /*is_new=*/true);

  const Schema schema = MakeSchema();
  RowCodec codec(schema);
  for (int64_t id = 0; id < 5; ++id) {
    std::vector<Value> row{Value::Int64(id), Value::String("name" + std::to_string(id)),
                           Value::Double(static_cast<double>(id)), Value::Bool(true)};
    ASSERT_TRUE(engine.Insert(encode_key<int64_t>(id), Slice(codec.Encode(row))));
  }

  RowTableSource src(schema, &engine);
  auto cursor = src.Scan({0, 1, 2, 3});  // all columns
  Chunk chunk;
  ASSERT_TRUE(cursor->Next(&chunk));
  ASSERT_EQ(chunk.columns.size(), 4u);
  ASSERT_EQ(chunk.row_count, 5u);
  EXPECT_EQ(chunk.columns[1].GetBytes(0).ToString(), "name0");
  EXPECT_EQ(chunk.columns[1].GetBytes(4).ToString(), "name4");
  EXPECT_EQ(chunk.columns[3].Get<bool>(2), true);
  EXPECT_FALSE(cursor->Next(&chunk));  // exhausted

  remove(kDb);
}

}  // namespace dbplay
