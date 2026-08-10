//
// T0 acceptance: IStorageEngine::NewCursor yields the whole key space in
// ascending encoded-key order. See docs/design/ColumnarTableSource.md.
//

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Common/Codec.h"
#include "Common/Slice.h"
#include "Storage/BPlusTreeEngine.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
constexpr char kDb[] = "kvcursor_test.db";

std::shared_ptr<BufferPoolManager> MakeBpm() {
  remove(kDb);
  auto disk_manager = std::make_shared<DiskManager>(kDb);
  return std::make_shared<BufferPoolManager>(500, disk_manager);
}
}  // namespace

TEST(KvCursorTest, EmptyEngineYieldsNothing) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm, /*is_new=*/true);
  auto cursor = engine.NewCursor();
  EXPECT_FALSE(cursor->Valid());
  remove(kDb);
}

TEST(KvCursorTest, ScanReturnsAllInAscendingKeyOrder) {
  auto bpm = MakeBpm();
  BPlusTreeEngine engine(bpm, /*is_new=*/true);

  // Enough keys (incl. negatives) to build several leaves; store value == key
  // so we can recover the logical key from the value without a key decoder.
  const int64_t lo = -1500;
  const int64_t hi = 1500;
  std::vector<int64_t> keys;
  for (int64_t k = lo; k < hi; ++k) {
    keys.push_back(k);
  }
  std::mt19937 rng(2024);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (int64_t k : keys) {
    ASSERT_TRUE(engine.Insert(encode_key<int64_t>(k), encode_value<int64_t>(k))) << "k=" << k;
  }

  auto cursor = engine.NewCursor();
  std::string prev_key;
  bool first = true;
  int64_t expected = lo;
  size_t count = 0;
  for (; cursor->Valid(); cursor->Next()) {
    // Keys come out in strictly ascending encoded (memcmp) order.
    std::string cur_key = cursor->Key().ToString();
    if (!first) {
      EXPECT_LT(Slice(prev_key).compare(Slice(cur_key)), 0) << "at count=" << count;
    }
    prev_key = cur_key;
    first = false;

    // value == key, and since keys are ascending the decoded values must be lo, lo+1, ...
    int64_t v = decode_value<int64_t>(cursor->Value());
    EXPECT_EQ(v, expected) << "at count=" << count;
    ++expected;
    ++count;
  }
  EXPECT_EQ(count, keys.size());

  remove(kDb);
}

}  // namespace dbplay
