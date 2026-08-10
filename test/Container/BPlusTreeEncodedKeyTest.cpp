//
// Phase 2 acceptance tests: the B+Tree used as a pure index over
// (EncodedKey, RID). See docs/design/StorageEngineRefactor.md.
//
// The point of these tests is that the tree orders keys purely by memcmp over
// the order-preserving EncodedKey encoding -- it never knows the keys are
// really int64 -- and stores an opaque RID as the value.
//

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include "Common/EncodedKey.h"
#include "Common/RID.h"
#include "Concurrency/Transaction.h"
#include "Container/BPlusTree.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "gtest/gtest.h"

namespace dbplay {

namespace {
constexpr char kTestDb[] = "bplustree_encoded_test.db";

std::shared_ptr<BufferPoolManager> MakeBpm() {
  auto disk_manager = std::make_shared<DiskManager>(kTestDb);
  // Large pool: keep these functional tests off the (pre-existing, slow and
  // possibly buggy) eviction path so they isolate the EncodedKey behaviour.
  return std::make_shared<BufferPoolManager>(1000, disk_manager);
}
}  // namespace

TEST(BPlusTreeEncodedKeyTest, InsertGetSequential) {
  auto bpm = MakeBpm();
  BPlusTree<EncodedKey, RID> tree{bpm, 3, 4};  // small fanout to force splits
  auto txn = std::make_unique<Transaction>(0);

  const int n = 300;
  for (int i = 0; i < n; ++i) {
    ASSERT_TRUE(tree.Insert(MakeEncodedKey<int64_t>(i), RID(i, i), txn.get())) << "i=" << i;
  }
  for (int i = 0; i < n; ++i) {
    RID out;
    ASSERT_TRUE(tree.GetValue(MakeEncodedKey<int64_t>(i), out, txn.get())) << "i=" << i;
    EXPECT_EQ(out, RID(i, i)) << "i=" << i;
  }

  txn.release();
  remove(kTestDb);
}

TEST(BPlusTreeEncodedKeyTest, ShuffledInsertWithNegatives) {
  auto bpm = MakeBpm();
  BPlusTree<EncodedKey, RID> tree{bpm, 3, 4};
  auto txn = std::make_unique<Transaction>(0);

  std::vector<int64_t> keys;
  for (int64_t k = -150; k < 150; ++k) {
    keys.push_back(k);
  }
  std::mt19937 rng(12345);  // fixed seed: deterministic
  std::shuffle(keys.begin(), keys.end(), rng);

  for (size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(tree.Insert(MakeEncodedKey<int64_t>(keys[i]), RID(static_cast<page_id_t>(i), static_cast<uint32_t>(i)),
                            txn.get()))
        << "key=" << keys[i];
  }

  // Every inserted key resolves to the RID it was stored with, regardless of
  // insertion order or sign.
  for (size_t i = 0; i < keys.size(); ++i) {
    RID out;
    ASSERT_TRUE(tree.GetValue(MakeEncodedKey<int64_t>(keys[i]), out, txn.get())) << "key=" << keys[i];
    EXPECT_EQ(out, RID(static_cast<page_id_t>(i), static_cast<uint32_t>(i))) << "key=" << keys[i];
  }

  // A key that was never inserted must miss.
  RID missing;
  EXPECT_FALSE(tree.GetValue(MakeEncodedKey<int64_t>(9999), missing, txn.get()));

  txn.release();
  remove(kTestDb);
}

// CONTROL: same shuffled pattern on the original int64/int32 tree. If this
// also fails, the bug is pre-existing in the B+Tree (not the EncodedKey work).
TEST(BPlusTreeEncodedKeyTest, ControlShuffledIntKeys) {
  auto bpm = MakeBpm();
  BPlusTree<key_t, value_t> tree{bpm, 3, 4};
  auto txn = std::make_unique<Transaction>(0);

  std::vector<key_t> keys;
  for (key_t k = -30; k < 30; ++k) {
    keys.push_back(k);
  }
  std::mt19937 rng(12345);
  std::shuffle(keys.begin(), keys.end(), rng);

  for (size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(tree.Insert(keys[i], static_cast<value_t>(keys[i]), txn.get())) << "key=" << keys[i];
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    value_t out;
    ASSERT_TRUE(tree.GetValue(keys[i], out, txn.get())) << "key=" << keys[i];
    EXPECT_EQ(out, static_cast<value_t>(keys[i])) << "key=" << keys[i];
  }

  txn.release();
  remove(kTestDb);
}

TEST(BPlusTreeEncodedKeyTest, DuplicateInsertRejected) {
  auto bpm = MakeBpm();
  BPlusTree<EncodedKey, RID> tree{bpm, 3, 4};
  auto txn = std::make_unique<Transaction>(0);

  ASSERT_TRUE(tree.Insert(MakeEncodedKey<int64_t>(42), RID(1, 1), txn.get()));
  EXPECT_FALSE(tree.Insert(MakeEncodedKey<int64_t>(42), RID(2, 2), txn.get()));

  RID out;
  ASSERT_TRUE(tree.GetValue(MakeEncodedKey<int64_t>(42), out, txn.get()));
  EXPECT_EQ(out, RID(1, 1));  // original value retained

  txn.release();
  remove(kTestDb);
}

}  // namespace dbplay
