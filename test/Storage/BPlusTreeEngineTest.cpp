//
// Phase 3 acceptance tests: BPlusTreeEngine behind the type-erased Slice API.
// See docs/design/StorageEngineRefactor.md.
//

#include <cstdio>
#include <memory>
#include <string>

#include "Common/Codec.h"
#include "Common/Slice.h"
#include "Storage/BPlusTreeEngine.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
constexpr char kTestDb[] = "engine_test.db";

std::shared_ptr<BufferPoolManager> MakeBpm() {
  auto disk_manager = std::make_shared<DiskManager>(kTestDb);
  return std::make_shared<BufferPoolManager>(100, disk_manager);
}
}  // namespace

TEST(BPlusTreeEngineTest, TypedKeyValueRoundTrip) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm);

  for (int64_t k = -50; k < 50; ++k) {
    ASSERT_TRUE(engine.Insert(encode_key<int64_t>(k), encode_value<int32_t>(static_cast<int32_t>(k * 3)))) << "k=" << k;
  }
  for (int64_t k = -50; k < 50; ++k) {
    std::string raw;
    ASSERT_TRUE(engine.Get(encode_key<int64_t>(k), &raw)) << "k=" << k;
    EXPECT_EQ(decode_value<int32_t>(Slice(raw)), static_cast<int32_t>(k * 3)) << "k=" << k;
  }

  remove(kTestDb);
}

TEST(BPlusTreeEngineTest, VariableLengthStringValues) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm);

  ASSERT_TRUE(engine.Insert(encode_key<int64_t>(1), Slice("hello")));
  ASSERT_TRUE(engine.Insert(encode_key<int64_t>(2), Slice("a considerably longer opaque value with bytes")));
  std::string embedded("a\0b\0c", 5);
  ASSERT_TRUE(engine.Insert(encode_key<int64_t>(3), Slice(embedded)));

  std::string out;
  ASSERT_TRUE(engine.Get(encode_key<int64_t>(1), &out));
  EXPECT_EQ(out, "hello");
  ASSERT_TRUE(engine.Get(encode_key<int64_t>(2), &out));
  EXPECT_EQ(out, "a considerably longer opaque value with bytes");
  ASSERT_TRUE(engine.Get(encode_key<int64_t>(3), &out));
  EXPECT_EQ(out, embedded);

  remove(kTestDb);
}

TEST(BPlusTreeEngineTest, InsertIsAbsentOnly) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm);

  ASSERT_TRUE(engine.Insert(encode_key<int64_t>(5), encode_value<int32_t>(1)));
  EXPECT_FALSE(engine.Insert(encode_key<int64_t>(5), encode_value<int32_t>(2)));  // no overwrite

  std::string raw;
  ASSERT_TRUE(engine.Get(encode_key<int64_t>(5), &raw));
  EXPECT_EQ(decode_value<int32_t>(Slice(raw)), 1);  // original retained

  remove(kTestDb);
}

TEST(BPlusTreeEngineTest, RemoveThenMiss) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm);

  ASSERT_TRUE(engine.Insert(encode_key<int64_t>(9), Slice("gone")));
  std::string out;
  ASSERT_TRUE(engine.Remove(encode_key<int64_t>(9)));
  EXPECT_FALSE(engine.Get(encode_key<int64_t>(9), &out));
  EXPECT_FALSE(engine.Remove(encode_key<int64_t>(9)));  // second remove is a no-op

  remove(kTestDb);
}

TEST(BPlusTreeEngineTest, RejectsWrongSizedKey) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm);
  EXPECT_THROW(engine.Insert(Slice("short"), Slice("v")), std::invalid_argument);
  remove(kTestDb);
}

}  // namespace dbplay
