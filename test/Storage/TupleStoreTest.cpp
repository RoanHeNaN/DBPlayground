//
// Phase 1 acceptance tests for TupleStore.
// See docs/design/StorageEngineRefactor.md.
//

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "Storage/TupleStore/TupleStore.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
constexpr char kTestDb[] = "tuplestore_test.db";

std::shared_ptr<BufferPoolManager> MakeBpm() {
  auto disk_manager = std::make_shared<DiskManager>(kTestDb);
  return std::make_shared<BufferPoolManager>(50, disk_manager);
}
}  // namespace

TEST(TupleStoreTest, PutGetRoundTrip) {
  auto bpm = MakeBpm();
  TupleStore ts(bpm);

  RID a = ts.Put(Slice("hello"));
  RID b = ts.Put(Slice("a much longer value with some bytes"));
  std::string with_nul("x\0y\0z", 5);
  RID c = ts.Put(Slice(with_nul));

  std::string out;
  ASSERT_TRUE(ts.Get(a, &out));
  EXPECT_EQ(out, "hello");
  ASSERT_TRUE(ts.Get(b, &out));
  EXPECT_EQ(out, "a much longer value with some bytes");
  ASSERT_TRUE(ts.Get(c, &out));
  EXPECT_EQ(out, with_nul);

  remove(kTestDb);
}

TEST(TupleStoreTest, SpansMultiplePages) {
  auto bpm = MakeBpm();
  TupleStore ts(bpm);

  // ~10KB each; enough of them to overflow a single 160KB page.
  const int n = 40;
  const std::string chunk(10 * 1024, 'z');
  std::vector<RID> rids;
  std::vector<std::string> expected;
  rids.reserve(n);
  for (int i = 0; i < n; ++i) {
    std::string v = std::to_string(i) + ":" + chunk;
    rids.push_back(ts.Put(Slice(v)));
    expected.push_back(v);
  }

  // Confirm the values really landed on more than one page.
  std::vector<page_id_t> distinct;
  for (const auto &r : rids) {
    if (std::find(distinct.begin(), distinct.end(), r.GetPageId()) == distinct.end()) {
      distinct.push_back(r.GetPageId());
    }
  }
  EXPECT_GT(distinct.size(), 1u);

  for (int i = 0; i < n; ++i) {
    std::string out;
    ASSERT_TRUE(ts.Get(rids[i], &out)) << "i=" << i;
    EXPECT_EQ(out, expected[i]) << "i=" << i;
  }

  remove(kTestDb);
}

TEST(TupleStoreTest, DeleteThenGetMisses) {
  auto bpm = MakeBpm();
  TupleStore ts(bpm);

  RID a = ts.Put(Slice("gone soon"));
  RID b = ts.Put(Slice("stays"));

  std::string out;
  ASSERT_TRUE(ts.Delete(a));
  EXPECT_FALSE(ts.Get(a, &out));
  EXPECT_FALSE(ts.Delete(a));  // second delete is a no-op

  ASSERT_TRUE(ts.Get(b, &out));
  EXPECT_EQ(out, "stays");

  remove(kTestDb);
}

TEST(TupleStoreTest, UpdateInPlaceKeepsRid) {
  auto bpm = MakeBpm();
  TupleStore ts(bpm);

  RID a = ts.Put(Slice("aaaaaaaaaa"));  // 10 bytes allocated
  RID a2 = ts.Update(a, Slice("bbb"));  // shrinks -> fits in place

  EXPECT_EQ(a2.GetPageId(), a.GetPageId());
  EXPECT_EQ(a2.GetSlotNum(), a.GetSlotNum());
  std::string out;
  ASSERT_TRUE(ts.Get(a, &out));
  EXPECT_EQ(out, "bbb");

  remove(kTestDb);
}

TEST(TupleStoreTest, UpdateGrowRelocates) {
  auto bpm = MakeBpm();
  TupleStore ts(bpm);

  RID a = ts.Put(Slice("small"));
  std::string big(500, 'q');
  RID a2 = ts.Update(a, Slice(big));  // grows -> relocate

  EXPECT_FALSE(a2.GetPageId() == a.GetPageId() && a2.GetSlotNum() == a.GetSlotNum());
  std::string out;
  ASSERT_TRUE(ts.Get(a2, &out));
  EXPECT_EQ(out, big);
  EXPECT_FALSE(ts.Get(a, &out));  // old locator is now tombstoned

  remove(kTestDb);
}

TEST(TupleStoreTest, ValueTooLargeThrows) {
  auto bpm = MakeBpm();
  TupleStore ts(bpm);

  std::string too_big(TupleStore::MaxValueSize() + 1, 'x');
  EXPECT_THROW(ts.Put(Slice(too_big)), std::runtime_error);

  remove(kTestDb);
}

}  // namespace dbplay
