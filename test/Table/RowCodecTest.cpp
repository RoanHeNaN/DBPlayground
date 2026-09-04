//
// T2 acceptance: RowCodec encodes/decodes rows and materializes projected
// fields into columns. See docs/design/ColumnarTableSource.md.
//

#include <string>
#include <vector>

#include "Common/Slice.h"
#include "Common/Type.h"
#include "Table/Column.h"
#include "Table/RowCodec.h"
#include "Table/Schema.h"
#include "Table/Value.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
// (id INT64, name STRING, score DOUBLE, active BOOL)
Schema MakeSchema() {
  return {{"id", Type::Int64}, {"name", Type::String}, {"score", Type::Double}, {"active", Type::Bool}};
}
}  // namespace

TEST(RowCodecTest, FullRowRoundTrip) {
  RowCodec codec(MakeSchema());

  std::vector<Value> row{Value::Int64(42), Value::String("alice"), Value::Double(9.5), Value::Bool(true)};
  std::string blob = codec.Encode(row);
  std::vector<Value> back = codec.Decode(Slice(blob));

  ASSERT_EQ(back.size(), 4u);
  EXPECT_EQ(back[0].AsInt64(), 42);
  EXPECT_EQ(back[1].AsString(), "alice");
  EXPECT_DOUBLE_EQ(back[2].AsDouble(), 9.5);
  EXPECT_EQ(back[3].AsBool(), true);
}

TEST(RowCodecTest, EmptyAndNegativeAndEmbeddedNul) {
  RowCodec codec(MakeSchema());
  std::string weird("a\0b", 3);
  std::vector<Value> row{Value::Int64(-1000000), Value::String(weird), Value::Double(-0.25), Value::Bool(false)};
  auto back = codec.Decode(Slice(codec.Encode(row)));
  EXPECT_EQ(back[0].AsInt64(), -1000000);
  EXPECT_EQ(back[1].AsString(), weird);
  EXPECT_DOUBLE_EQ(back[2].AsDouble(), -0.25);
  EXPECT_EQ(back[3].AsBool(), false);
}

TEST(RowCodecTest, DecodeIntoProjectedColumns) {
  RowCodec codec(MakeSchema());

  // Project {id(0), score(2)} -- name and active must be skipped.
  std::vector<int> projection{0, 2};
  std::vector<Column> cols;
  cols.emplace_back(Type::Int64);
  cols.emplace_back(Type::Double);

  std::vector<std::vector<Value>> rows{
      {Value::Int64(1), Value::String("aaa"), Value::Double(1.5), Value::Bool(true)},
      {Value::Int64(2), Value::String("bb"), Value::Double(2.5), Value::Bool(false)},
      {Value::Int64(3), Value::String("cccc"), Value::Double(3.5), Value::Bool(true)},
  };
  for (const auto &r : rows) {
    codec.DecodeInto(Slice(codec.Encode(r)), projection, &cols);
  }

  ASSERT_EQ(cols[0].size(), 3u);
  ASSERT_EQ(cols[1].size(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(cols[0].Get<int64_t>(i), static_cast<int64_t>(i + 1));
    EXPECT_DOUBLE_EQ(cols[1].Get<double>(i), static_cast<double>(i) + 1.5);
  }
}

// A blob truncated mid-field must fault, not read out of bounds. Row blobs
// arrive from object storage in the cloud path, so a short or crafted length
// prefix has to be rejected rather than trusted.
TEST(RowCodecTest, DecodeRejectsTruncatedFixedField) {
  RowCodec codec(MakeSchema());
  std::string blob = codec.Encode({Value::Int64(7), Value::String("x"), Value::Double(1.0), Value::Bool(true)});
  blob.resize(4);  // cut off inside the leading Int64
  EXPECT_THROW(codec.Decode(Slice(blob)), std::runtime_error);
}

TEST(RowCodecTest, DecodeRejectsStringLengthPastEnd) {
  // (s STRING) only. A huge length prefix with no payload behind it must throw.
  RowCodec codec(Schema{{"s", Type::String}});
  std::string blob;
  const uint32_t huge = 0xFFFFFFFFu;
  blob.append(reinterpret_cast<const char *>(&huge), sizeof(huge));
  EXPECT_THROW(codec.Decode(Slice(blob)), std::runtime_error);

  std::vector<Column> cols;
  cols.emplace_back(Type::String);
  EXPECT_THROW(codec.DecodeInto(Slice(blob), {0}, &cols), std::runtime_error);
}

}  // namespace dbplay
