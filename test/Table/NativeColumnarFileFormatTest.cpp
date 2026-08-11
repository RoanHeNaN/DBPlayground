//
// C3 acceptance (docs/design/StorageAbstraction.md): the native columnar file
// format, where C1 (IStorage bytes) + C2 (Codec) first meet. We write a file
// through the format, wrap it as a composed TableSource, and read it back via
// the ITableSource& seam only -- proving projection pushdown and the row/columnar
// decoupling. A second case swaps MemStorage -> LocalStorage with no other
// change, proving the medium is pluggable.
//

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Execution/ScanExecutor.h"
#include "Storage/File/LocalStorage.h"
#include "Storage/File/MemStorage.h"
#include "Table/Format/NativeColumnarFileFormat.h"
#include "Table/Format/TableSource.h"
#include "Table/ITableSource.h"
#include "gtest/gtest.h"

namespace fs = std::filesystem;

namespace dbplay {

namespace {

// (id INT64, name STRING, score DOUBLE, active BOOL)
Schema MakeSchema() {
  return {{"id", Type::Int64}, {"name", Type::String}, {"score", Type::Double}, {"active", Type::Bool}};
}

// A full-schema batch of `count` rows starting at id=`start`.
Chunk MakeChunk(int64_t start, int64_t count) {
  Chunk c;
  c.column_ids = {0, 1, 2, 3};
  c.columns.emplace_back(Type::Int64);
  c.columns.emplace_back(Type::String);
  c.columns.emplace_back(Type::Double);
  c.columns.emplace_back(Type::Bool);
  for (int64_t i = 0; i < count; ++i) {
    const int64_t id = start + i;
    c.columns[0].Append<int64_t>(id);
    c.columns[1].AppendBytes(Slice("row" + std::to_string(id)));
    c.columns[2].Append<double>(static_cast<double>(id) * 1.5);
    c.columns[3].Append<bool>(id % 2 == 0);
  }
  c.row_count = static_cast<size_t>(count);
  return c;
}

void WriteFile(IFileFormat &fmt, IStorage &store, const std::string &path, int64_t start, int64_t count) {
  auto w = fmt.OpenWriter(store, path);
  w->Write(MakeChunk(start, count));
  w->Close();
}

}  // namespace

TEST(NativeColumnarFileFormatTest, ProjectionPushdownReadsOnlyProjectedColumns) {
  auto store = std::make_shared<MemStorage>();
  auto fmt = std::make_shared<NativeColumnarFileFormat>(MakeSchema());
  WriteFile(*fmt, *store, "t.dbc", /*start=*/0, /*count=*/50);

  // Read back through the ITableSource& seam only.
  TableSource src(fmt, store, {"t.dbc"});
  ITableSource &table = src;

  // Manual cursor: assert only the projected columns are materialized.
  {
    auto cursor = table.Scan({0, 2});  // id, score
    Chunk chunk;
    ASSERT_TRUE(cursor->Next(&chunk));
    EXPECT_EQ(chunk.columns.size(), 2u);
    EXPECT_EQ(chunk.column_ids, (std::vector<int>{0, 2}));
    EXPECT_EQ(chunk.columns[0].type(), Type::Int64);
    EXPECT_EQ(chunk.columns[1].type(), Type::Double);
    EXPECT_EQ(chunk.row_count, 50u);
    EXPECT_FALSE(cursor->Next(&chunk));  // one file -> one chunk
  }

  // Values via the collect operator (also ITableSource-only).
  auto rows = CollectProjected(table, {0, 2});
  ASSERT_EQ(rows.size(), 50u);
  for (size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i][0].AsInt64(), static_cast<int64_t>(i));
    EXPECT_DOUBLE_EQ(rows[i][1].AsDouble(), static_cast<double>(i) * 1.5);
  }
}

TEST(NativeColumnarFileFormatTest, FullProjectionIncludesStringAndBool) {
  auto store = std::make_shared<MemStorage>();
  auto fmt = std::make_shared<NativeColumnarFileFormat>(MakeSchema());
  WriteFile(*fmt, *store, "t.dbc", 0, 5);

  TableSource src(fmt, store, {"t.dbc"});
  auto rows = CollectProjected(src, {0, 1, 2, 3});
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0][1].AsString(), "row0");
  EXPECT_EQ(rows[4][1].AsString(), "row4");
  EXPECT_EQ(rows[2][3].AsBool(), true);   // id 2 even
  EXPECT_EQ(rows[3][3].AsBool(), false);  // id 3 odd
}

TEST(NativeColumnarFileFormatTest, MultipleFilesYieldConcatenatedRowGroups) {
  auto store = std::make_shared<MemStorage>();
  auto fmt = std::make_shared<NativeColumnarFileFormat>(MakeSchema());
  WriteFile(*fmt, *store, "g0.dbc", /*start=*/0, /*count=*/3);
  WriteFile(*fmt, *store, "g1.dbc", /*start=*/100, /*count=*/2);

  TableSource src(fmt, store, {"g0.dbc", "g1.dbc"});

  int chunks = 0;
  auto cursor = src.Scan({0});
  Chunk chunk;
  std::vector<int64_t> ids;
  while (cursor->Next(&chunk)) {
    ++chunks;
    for (size_t i = 0; i < chunk.row_count; ++i) {
      ids.push_back(chunk.columns[0].Get<int64_t>(i));
    }
  }
  EXPECT_EQ(chunks, 2);  // one chunk per file (row group)
  EXPECT_EQ(ids, (std::vector<int64_t>{0, 1, 2, 100, 101}));
}

TEST(NativeColumnarFileFormatTest, ZlibCompressedFileIsSelfDescribing) {
  auto store = std::make_shared<MemStorage>();

  // Write with zlib compression...
  {
    auto writer_fmt = std::make_shared<NativeColumnarFileFormat>(MakeSchema(), CompressionId::Zlib);
    WriteFile(*writer_fmt, *store, "z.dbc", /*start=*/0, /*count=*/200);
  }

  // ...and read it back through a format configured with the DEFAULT (None)
  // compression: the reader resolves each column's codec from the ids stored in
  // the file, so the writer's choice is invisible to it -- the file self-describes.
  auto reader_fmt = std::make_shared<NativeColumnarFileFormat>(MakeSchema());
  TableSource src(reader_fmt, store, {"z.dbc"});
  auto rows = CollectProjected(src, {0, 1, 2});
  ASSERT_EQ(rows.size(), 200u);
  EXPECT_EQ(rows[0][1].AsString(), "row0");
  EXPECT_EQ(rows[123][0].AsInt64(), 123);
  EXPECT_DOUBLE_EQ(rows[123][2].AsDouble(), 123.0 * 1.5);
}

TEST(NativeColumnarFileFormatTest, PersistsAcrossStorageAndReopen) {
  const std::string root = "nativecolumnar_test_dir";
  fs::remove_all(root);

  const Schema schema = MakeSchema();
  {
    LocalStorage store(root);
    NativeColumnarFileFormat fmt(schema);
    WriteFile(fmt, store, "t.dbc", /*start=*/0, /*count=*/10);
  }
  // Fresh storage + format objects, same on-disk file -> data survives.
  {
    auto store = std::make_shared<LocalStorage>(root);
    auto fmt = std::make_shared<NativeColumnarFileFormat>(schema);
    TableSource src(fmt, store, {"t.dbc"});
    auto rows = CollectProjected(src, {0, 1});
    ASSERT_EQ(rows.size(), 10u);
    EXPECT_EQ(rows[7][0].AsInt64(), 7);
    EXPECT_EQ(rows[7][1].AsString(), "row7");
  }

  fs::remove_all(root);
}

}  // namespace dbplay
