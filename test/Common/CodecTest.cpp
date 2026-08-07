//
// Phase 0 acceptance tests: order-preserving key encoding.
// See docs/design/StorageEngineRefactor.md.
//
// The core invariant of the whole refactor: for every supported key type,
//   sign(memcmp(encode(a), encode(b))) == sign(a <=> b)
// so the tree can order keys with memcmp alone and never know their type.
//

#include <cfloat>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "Common/Codec.h"
#include "Common/Slice.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
int Sign(int x) { return (x > 0) - (x < 0); }

// Compare two encoded keys the way the tree will: via Slice::compare (memcmp).
template <typename T>
int CmpEncoded(T a, T b) {
  std::string ea = KeyEncoder<T>::Encode(a);
  std::string eb = KeyEncoder<T>::Encode(b);
  return Sign(Slice(ea).compare(Slice(eb)));
}

// Assert byte order matches value order across every ordered pair in `vals`,
// and that encodings are exactly kKeyLen bytes.
template <typename T>
void ExpectOrderPreserving(const std::vector<T> &vals) {
  for (const auto &v : vals) {
    EXPECT_EQ(KeyEncoder<T>::Encode(v).size(), kKeyLen);
  }
  for (size_t i = 0; i < vals.size(); ++i) {
    for (size_t j = 0; j < vals.size(); ++j) {
      T a = vals[i];
      T b = vals[j];
      int value_order = Sign((a > b) - (a < b));
      EXPECT_EQ(CmpEncoded<T>(a, b), value_order) << "i=" << i << " j=" << j;
    }
  }
}
}  // namespace

TEST(CodecTest, Int64OrderPreserving) {
  ExpectOrderPreserving<int64_t>({
      std::numeric_limits<int64_t>::min(),
      -1234567890,
      -1,
      0,
      1,
      1234567890,
      std::numeric_limits<int64_t>::max(),
  });
}

TEST(CodecTest, Int32OrderPreserving) {
  ExpectOrderPreserving<int32_t>({
      std::numeric_limits<int32_t>::min(),
      -100,
      -1,
      0,
      1,
      100,
      std::numeric_limits<int32_t>::max(),
  });
}

TEST(CodecTest, DoubleOrderPreserving) {
  ExpectOrderPreserving<double>({
      -std::numeric_limits<double>::max(),
      -3.14,
      -1.0,
      -DBL_MIN,
      0.0,
      DBL_MIN,
      1.0,
      3.14,
      std::numeric_limits<double>::max(),
  });
}

TEST(CodecTest, FloatOrderPreserving) {
  ExpectOrderPreserving<float>({
      -std::numeric_limits<float>::max(),
      -2.5f,
      -1.0f,
      0.0f,
      1.0f,
      2.5f,
      std::numeric_limits<float>::max(),
  });
}

TEST(CodecTest, BoolOrderPreserving) { ExpectOrderPreserving<bool>({false, true}); }

TEST(CodecTest, ValueCodecRoundTripScalar) {
  int64_t v = -9876543210LL;
  std::string bytes = encode_value<int64_t>(v);
  EXPECT_EQ(decode_value<int64_t>(Slice(bytes)), v);
}

TEST(CodecTest, ValueCodecRoundTripString) {
  const char raw[] = "hello\0world with embedded nul";
  std::string v(raw, sizeof(raw) - 1);
  std::string bytes = encode_value<std::string>(v);
  EXPECT_EQ(decode_value<std::string>(Slice(bytes)), v);
}

TEST(CodecTest, SliceCompareBasics) {
  EXPECT_LT(Slice("abc").compare(Slice("abd")), 0);
  EXPECT_GT(Slice("abd").compare(Slice("abc")), 0);
  EXPECT_EQ(Slice("abc").compare(Slice("abc")), 0);
  EXPECT_LT(Slice("ab").compare(Slice("abc")), 0);  // prefix is smaller
}

}  // namespace dbplay
