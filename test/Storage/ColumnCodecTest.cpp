//
// C2 acceptance (docs/design/StorageAbstraction.md): the encoding layer. A
// Column round-trips through ICodec (values <-> encoded bytes) and, end to end,
// through ICodec + ICompression (values <-> stored bytes), with both resolved
// from the CodecRegistry by their persisted ids -- proving the self-describing
// indirection.
//

#include <cstdint>
#include <string>

#include "Storage/Encoding/CodecRegistry.h"
#include "Storage/Encoding/PlainCodec.h"
#include "Table/Column.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {

// Encode `col`, decode into a fresh Column of the same type, return the copy.
Column RoundTrip(const ICodec &codec, const Column &col) {
  std::string encoded;
  codec.Encode(col, &encoded);
  Column out(col.type());
  codec.Decode(Slice(encoded), col.size(), &out);
  return out;
}

}  // namespace

TEST(ColumnCodecTest, PlainFixedWidthRoundTrip) {
  PlainCodec codec;

  Column i32(Type::Int32);
  for (int32_t v : {-3, 0, 7, 2147483647}) {
    i32.Append<int32_t>(v);
  }
  Column r = RoundTrip(codec, i32);
  ASSERT_EQ(r.size(), i32.size());
  for (size_t i = 0; i < i32.size(); ++i) {
    EXPECT_EQ(r.Get<int32_t>(i), i32.Get<int32_t>(i));
  }

  Column dbl(Type::Double);
  for (double v : {-1.5, 0.0, 3.25, 1e300}) {
    dbl.Append<double>(v);
  }
  Column rd = RoundTrip(codec, dbl);
  ASSERT_EQ(rd.size(), 4u);
  for (size_t i = 0; i < dbl.size(); ++i) {
    EXPECT_DOUBLE_EQ(rd.Get<double>(i), dbl.Get<double>(i));
  }

  Column b(Type::Bool);
  b.Append<bool>(true);
  b.Append<bool>(false);
  b.Append<bool>(true);
  Column rb = RoundTrip(codec, b);
  ASSERT_EQ(rb.size(), 3u);
  EXPECT_EQ(rb.Get<bool>(0), true);
  EXPECT_EQ(rb.Get<bool>(1), false);
  EXPECT_EQ(rb.Get<bool>(2), true);
}

TEST(ColumnCodecTest, PlainVarLengthRoundTrip) {
  PlainCodec codec;
  Column s(Type::String);
  s.AppendBytes(Slice("hello"));
  s.AppendBytes(Slice(""));  // empty value
  s.AppendBytes(Slice("a longer string value"));

  Column r = RoundTrip(codec, s);
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r.GetBytes(0).ToString(), "hello");
  EXPECT_EQ(r.GetBytes(1).ToString(), "");
  EXPECT_EQ(r.GetBytes(2).ToString(), "a longer string value");
}

TEST(ColumnCodecTest, RegistryResolvesById) {
  const CodecRegistry &reg = CodecRegistry::Instance();
  const ICodec &codec = reg.Get(EncodingId::Plain);
  const ICompression &comp = reg.Get(CompressionId::None);
  EXPECT_EQ(codec.id(), EncodingId::Plain);
  EXPECT_EQ(comp.id(), CompressionId::None);

  // Full page path: values -> encode -> compress -> stored bytes -> back.
  Column i64(Type::Int64);
  for (int64_t v : {10, -20, 30, 40, 50}) {
    i64.Append<int64_t>(v);
  }

  std::string encoded;
  codec.Encode(i64, &encoded);
  std::string stored;
  comp.Compress(Slice(encoded), &stored);

  std::string decompressed;
  comp.Decompress(Slice(stored), encoded.size(), &decompressed);
  Column out(Type::Int64);
  codec.Decode(Slice(decompressed), i64.size(), &out);

  ASSERT_EQ(out.size(), i64.size());
  for (size_t i = 0; i < i64.size(); ++i) {
    EXPECT_EQ(out.Get<int64_t>(i), i64.Get<int64_t>(i));
  }
}

TEST(ColumnCodecTest, UnknownIdThrows) {
  const CodecRegistry &reg = CodecRegistry::Instance();
  EXPECT_THROW(reg.Get(EncodingId::Invalid), std::invalid_argument);
  EXPECT_THROW(reg.Get(CompressionId::Invalid), std::invalid_argument);
}

}  // namespace dbplay
