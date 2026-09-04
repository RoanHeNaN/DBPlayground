#include <memory>
#include <string>
#include <vector>

#include "Execution/ScanExecutor.h"
#include "Storage/File/MemStorage.h"
#include "Table/Format/TableSource.h"
#include "Table/Format/WalFileFormat.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

Schema MakeSchema() { return {{"id", Type::Int64}, {"name", Type::String}}; }

Chunk MakeChunk(int64_t start, int64_t count) {
  Chunk chunk;
  chunk.column_ids = {0, 1};
  chunk.columns.emplace_back(Type::Int64);
  chunk.columns.emplace_back(Type::String);
  for (int64_t i = 0; i < count; ++i) {
    const int64_t id = start + i;
    chunk.columns[0].Append<int64_t>(id);
    chunk.columns[1].AppendBytes(Slice("row" + std::to_string(id)));
  }
  chunk.row_count = static_cast<size_t>(count);
  return chunk;
}

}  // namespace

TEST(WalFileFormatTest, RoundTripsProjectedRowsAndRejectsCorruptChecksum) {
  auto store = std::make_shared<MemStorage>();
  auto format = std::make_shared<WalFileFormat>(MakeSchema(), /*batch_rows=*/2);
  auto writer = format->OpenWriter(*store, "wal/1.wal");
  writer->Write(MakeChunk(0, 3));
  writer->Write(MakeChunk(10, 1));
  writer->Close();

  TableSource source(format, store, {"wal/1.wal"});
  auto rows = CollectProjected(source, {1});
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0][0].AsString(), "row0");
  EXPECT_EQ(rows[3][0].AsString(), "row10");

  auto in = store->OpenInput("wal/1.wal");
  std::string bytes;
  ASSERT_TRUE(in->ReadAt(0, static_cast<size_t>(in->Size()), &bytes));
  bytes[bytes.size() / 2] ^= 0x5a;
  auto out = store->OpenOutput("wal/1.wal");
  out->Append(Slice(bytes));
  out->Close();

  EXPECT_THROW(CollectProjected(source, {0}), std::runtime_error);
}

TEST(WalFileFormatTest, ScanYieldsConfiguredBatchWindows) {
  auto store = std::make_shared<MemStorage>();
  auto format = std::make_shared<WalFileFormat>(MakeSchema(), /*batch_rows=*/2);
  auto writer = format->OpenWriter(*store, "wal/batches.wal");
  writer->Write(MakeChunk(0, 5));
  writer->Close();

  auto cursor = format->Scan(*store, {"wal/batches.wal"}, {0});
  Chunk chunk;
  ASSERT_TRUE(cursor->Next(&chunk));
  EXPECT_EQ(chunk.row_count, 2u);
  ASSERT_TRUE(cursor->Next(&chunk));
  EXPECT_EQ(chunk.row_count, 2u);
  ASSERT_TRUE(cursor->Next(&chunk));
  EXPECT_EQ(chunk.row_count, 1u);
  EXPECT_FALSE(cursor->Next(&chunk));
}

}  // namespace dbplay
