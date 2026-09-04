//
// R2 acceptance (docs/design/ColumnarReadPath.md): a fixed-width + uncompressed
// column takes the direct-offset path (reads only the requested rows' bytes),
// while var-length / compressed columns fall back to a whole-page read. Results
// are identical either way.
//

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Common/Slice.h"
#include "Storage/Encoding/Codec.h"
#include "Storage/File/IStorage.h"
#include "Storage/File/MemStorage.h"
#include "Table/Format/NativeColumnarFileFormat.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {

// Wraps an IInputFile and tallies bytes returned by ReadAt into a shared counter.
class SpyInputFile : public IInputFile {
 public:
  SpyInputFile(std::unique_ptr<IInputFile> inner, uint64_t *counter) : inner_(std::move(inner)), counter_(counter) {}
  uint64_t Size() const override { return inner_->Size(); }
  bool ReadAt(uint64_t offset, size_t len, std::string *out) const override {
    const bool ok = inner_->ReadAt(offset, len, out);
    if (ok) *counter_ += out->size();
    return ok;
  }

 private:
  std::unique_ptr<IInputFile> inner_;
  uint64_t *counter_;
};

// Delegates to an inner IStorage but hands out SpyInputFiles.
class SpyStorage : public IStorage {
 public:
  explicit SpyStorage(IStorage *inner) : inner_(inner) {}
  std::unique_ptr<IInputFile> OpenInput(const std::string &p) override {
    auto in = inner_->OpenInput(p);
    if (in == nullptr) return nullptr;
    return std::make_unique<SpyInputFile>(std::move(in), &read_bytes);
  }
  std::unique_ptr<IOutputStream> OpenOutput(const std::string &p) override { return inner_->OpenOutput(p); }
  bool Exists(const std::string &p) const override { return inner_->Exists(p); }
  std::vector<std::string> List(const std::string &prefix) const override { return inner_->List(prefix); }
  void Delete(const std::string &p) override { inner_->Delete(p); }

  uint64_t read_bytes = 0;

 private:
  IStorage *inner_;
};

Schema MakeSchema() { return {{"id", Type::Int64}, {"name", Type::String}}; }

void WriteFile(IStorage &store, const std::string &path, int64_t n, CompressionId comp) {
  NativeColumnarFileFormat fmt(MakeSchema(), comp);
  auto w = fmt.OpenWriter(store, path);
  Chunk c;
  c.column_ids = {0, 1};
  c.columns.emplace_back(Type::Int64);
  c.columns.emplace_back(Type::String);
  for (int64_t i = 0; i < n; ++i) {
    c.columns[0].Append<int64_t>(i);
    c.columns[1].AppendBytes(Slice("row" + std::to_string(i)));
  }
  c.row_count = static_cast<size_t>(n);
  w->Write(c);
  w->Close();
}

}  // namespace

TEST(NativeColumnarFastPathTest, FixedWidthUncompressedReadsOnlyRequestedBytes) {
  MemStorage mem;
  SpyStorage spy(&mem);
  const int64_t kN = 1000;  // Int64 column page ~ 8000 bytes
  WriteFile(spy, "t.dbc", kN, CompressionId::None);

  NativeColumnarFileFormat fmt(MakeSchema());       // default None
  auto reader = fmt.OpenReader(spy, "t.dbc", {0});  // project the Int64 column only
  ASSERT_NE(reader, nullptr);

  spy.read_bytes = 0;  // isolate the column read from the footer read
  Chunk c;
  ASSERT_TRUE(reader->ReadRange(500, 4, &c));  // 4 rows deep in the file
  EXPECT_EQ(c.columns[0].Get<int64_t>(0), 500);
  EXPECT_EQ(c.columns[0].Get<int64_t>(3), 503);

  // Direct path: ~ 4 * sizeof(int64) bytes, nowhere near the whole 8000-byte page.
  EXPECT_LE(spy.read_bytes, 64u);
}

TEST(NativeColumnarFastPathTest, VarLengthReadsWholePage) {
  MemStorage mem;
  SpyStorage spy(&mem);
  const int64_t kN = 1000;
  WriteFile(spy, "t.dbc", kN, CompressionId::None);

  NativeColumnarFileFormat fmt(MakeSchema());
  auto reader = fmt.OpenReader(spy, "t.dbc", {1});  // the String column
  ASSERT_NE(reader, nullptr);

  spy.read_bytes = 0;
  Chunk c;
  ASSERT_TRUE(reader->ReadRange(500, 4, &c));  // still only 4 rows wanted
  EXPECT_EQ(c.columns[0].GetBytes(0).ToString(), "row500");

  // Whole-page fallback: reads far more than 4 short strings' worth.
  EXPECT_GT(spy.read_bytes, 1000u);
}

TEST(NativeColumnarFastPathTest, CompressedFixedWidthFallsBackButIsCorrect) {
  MemStorage mem;
  const int64_t kN = 500;
  WriteFile(mem, "t.dbc", kN, CompressionId::Zlib);  // compressed -> no direct path

  NativeColumnarFileFormat fmt(MakeSchema());
  auto reader = fmt.OpenReader(mem, "t.dbc", {0});  // Int64, but compressed
  ASSERT_NE(reader, nullptr);
  Chunk c;
  ASSERT_TRUE(reader->ReadRange(100, 5, &c));
  for (int i = 0; i < 5; ++i) EXPECT_EQ(c.columns[0].Get<int64_t>(i), 100 + i);
}

}  // namespace dbplay
