//
// T1 acceptance: Column / Chunk data structures (Arrow-lite, no nulls).
// See docs/design/ColumnarTableSource.md.
//

#include <cstdint>
#include <string>

#include "Common/Slice.h"
#include "Common/Type.h"
#include "Table/Chunk.h"
#include "Table/Column.h"
#include "Table/Schema.h"
#include "gtest/gtest.h"

namespace dbplay {

TEST(ColumnTest, FixedWidthRoundTrip) {
  Column c(Type::Int64);
  c.Append<int64_t>(10);
  c.Append<int64_t>(-5);
  c.Append<int64_t>(1234567890123LL);

  ASSERT_EQ(c.size(), 3u);
  EXPECT_EQ(c.Get<int64_t>(0), 10);
  EXPECT_EQ(c.Get<int64_t>(1), -5);
  EXPECT_EQ(c.Get<int64_t>(2), 1234567890123LL);
  EXPECT_EQ(c.type(), Type::Int64);
}

TEST(ColumnTest, VarLengthRoundTripInclEmpty) {
  Column c(Type::String);
  c.AppendBytes(Slice("a"));
  c.AppendBytes(Slice("bcd"));
  c.AppendBytes(Slice(""));  // empty value
  std::string with_nul("x\0y", 3);
  c.AppendBytes(Slice(with_nul));

  ASSERT_EQ(c.size(), 4u);
  EXPECT_EQ(c.GetBytes(0).ToString(), "a");
  EXPECT_EQ(c.GetBytes(1).ToString(), "bcd");
  EXPECT_EQ(c.GetBytes(2).ToString(), "");
  EXPECT_EQ(c.GetBytes(3).ToString(), with_nul);
}

TEST(ChunkTest, MultiColumnBatch) {
  // Table (id INT64, name STRING); a batch of 3 rows.
  Schema schema{{"id", Type::Int64}, {"name", Type::String}};

  Chunk chunk;
  chunk.column_ids = {0, 1};
  chunk.columns.emplace_back(Type::Int64);
  chunk.columns.emplace_back(Type::String);
  chunk.row_count = 3;

  chunk.columns[0].Append<int64_t>(1);
  chunk.columns[0].Append<int64_t>(2);
  chunk.columns[0].Append<int64_t>(3);
  chunk.columns[1].AppendBytes(Slice("alice"));
  chunk.columns[1].AppendBytes(Slice("bob"));
  chunk.columns[1].AppendBytes(Slice("carol"));

  ASSERT_EQ(chunk.columns.size(), 2u);
  EXPECT_EQ(schema[chunk.column_ids[0]].name, "id");
  EXPECT_EQ(schema[chunk.column_ids[1]].name, "name");

  // Row i is (columns[0][i], columns[1][i]).
  for (size_t i = 0; i < chunk.row_count; ++i) {
    EXPECT_EQ(chunk.columns[0].Get<int64_t>(i), static_cast<int64_t>(i + 1));
  }
  EXPECT_EQ(chunk.columns[1].GetBytes(0).ToString(), "alice");
  EXPECT_EQ(chunk.columns[1].GetBytes(2).ToString(), "carol");
}

}  // namespace dbplay
