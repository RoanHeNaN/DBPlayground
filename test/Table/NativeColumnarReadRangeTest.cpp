//
// R1 acceptance (docs/design/ColumnarReadPath.md): positional ReadRange and a
// batch_rows-windowed Scan cursor both agree with the whole-file read.
//

#include <memory>
#include <string>
#include <vector>

#include "Common/Slice.h"
#include "Storage/File/MemStorage.h"
#include "Table/Format/NativeColumnarFileFormat.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
Schema MakeSchema() {
  return {{"id", Type::Int64}, {"name", Type::String}, {"score", Type::Double}, {"active", Type::Bool}};
}

Chunk MakeChunk(int64_t n) {
  Chunk c;
  c.column_ids = {0, 1, 2, 3};
  c.columns.emplace_back(Type::Int64);
  c.columns.emplace_back(Type::String);
  c.columns.emplace_back(Type::Double);
  c.columns.emplace_back(Type::Bool);
  for (int64_t i = 0; i < n; ++i) {
    c.columns[0].Append<int64_t>(i);
    c.columns[1].AppendBytes(Slice("row" + std::to_string(i)));
    c.columns[2].Append<double>(static_cast<double>(i) * 1.5);
    c.columns[3].Append<bool>(i % 2 == 0);
  }
  c.row_count = static_cast<size_t>(n);
  return c;
}

// Write `n` rows into one file and return the storage holding it.
std::shared_ptr<MemStorage> WriteFile(const Schema &schema, const std::string &path, int64_t n) {
  auto store = std::make_shared<MemStorage>();
  NativeColumnarFileFormat fmt(schema);
  auto w = fmt.OpenWriter(*store, path);
  w->Write(MakeChunk(n));
  w->Close();
  return store;
}
}  // namespace

TEST(NativeColumnarReadRangeTest, ReadRangeSubrangesMatchWhole) {
  const Schema schema = MakeSchema();
  const int64_t kN = 100;
  auto store = WriteFile(schema, "t.dbc", kN);
  NativeColumnarFileFormat fmt(schema);

  // Project {id(0), score(2)}. OpenReader reads the footer (metadata).
  auto reader = fmt.OpenReader(*store, "t.dbc", {0, 2});
  ASSERT_NE(reader, nullptr);
  ASSERT_EQ(reader->row_count(), static_cast<uint64_t>(kN));

  // Concatenate a few sub-ranges and compare against a single full read.
  auto check_cell = [](const Chunk &c, size_t r, int64_t id) {
    EXPECT_EQ(c.columns[0].Get<int64_t>(r), id);
    EXPECT_DOUBLE_EQ(c.columns[1].Get<double>(r), static_cast<double>(id) * 1.5);
  };

  Chunk full;
  ASSERT_TRUE(reader->ReadRange(0, kN, &full));
  ASSERT_EQ(full.row_count, static_cast<size_t>(kN));
  ASSERT_EQ(full.column_ids, (std::vector<int>{0, 2}));
  for (int64_t i = 0; i < kN; ++i) check_cell(full, i, i);

  int64_t expected = 0;
  for (const std::pair<uint64_t, uint64_t> &win : {std::pair<uint64_t, uint64_t>{0, 30}, {30, 25}, {55, 45}}) {
    Chunk c;
    ASSERT_TRUE(reader->ReadRange(win.first, win.second, &c));
    EXPECT_EQ(c.row_count, win.second);
    for (size_t r = 0; r < c.row_count; ++r) check_cell(c, r, expected++);
  }
  EXPECT_EQ(expected, kN);

  // Clamping and out-of-range.
  Chunk tail;
  ASSERT_TRUE(reader->ReadRange(90, 1000, &tail));  // clamps to 10
  EXPECT_EQ(tail.row_count, 10u);
  Chunk none;
  EXPECT_FALSE(reader->ReadRange(kN, 5, &none));  // at/after end -> empty
}

TEST(NativeColumnarReadRangeTest, BatchedCursorEqualsWhole) {
  const Schema schema = MakeSchema();
  const int64_t kN = 100;
  auto store = WriteFile(schema, "t.dbc", kN);

  // batch_rows = 32 -> windows 32,32,32,4.
  NativeColumnarFileFormat fmt(schema, CompressionId::None, /*batch_rows=*/32);
  auto cursor = fmt.Scan(*store, {"t.dbc"}, {0, 2});

  int chunks = 0;
  int64_t expected = 0;
  Chunk c;
  while (cursor->Next(&c)) {
    ++chunks;
    EXPECT_LE(c.row_count, 32u);
    for (size_t r = 0; r < c.row_count; ++r) {
      EXPECT_EQ(c.columns[0].Get<int64_t>(r), expected);
      EXPECT_DOUBLE_EQ(c.columns[1].Get<double>(r), static_cast<double>(expected) * 1.5);
      ++expected;
    }
  }
  EXPECT_EQ(expected, kN);
  EXPECT_EQ(chunks, 4);
}

}  // namespace dbplay
