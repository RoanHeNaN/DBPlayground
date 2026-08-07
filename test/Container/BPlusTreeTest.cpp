//
// Created by 何智强 on 2021/10/11.
//

#include <algorithm>

#include "Container/BPlusTree.h"
#include "gtest/gtest.h"

namespace dbplay {
TEST(BPlusTreeTest, InsertTest1) {
  auto disk_manager = std::make_shared<DiskManager>("test.db");
  auto bpm = std::make_shared<BufferPoolManager>(50, disk_manager);

  BPlusTree<key_t, value_t> tree{bpm, 2, 3};
  Transaction *transaction = new Transaction{0};

  key_t max_key = 5;
  uint32_t value_mask = 0xFFFFFFFF;
  for (key_t key = 0; key < max_key; ++key) {
    value_t value = key & value_mask;
    tree.Insert(key, value);
  }

  for (key_t key = 0; key < max_key; ++key) {
    value_t value;
    tree.GetValue(key, value, transaction);
    EXPECT_EQ(value, key & value_mask);
  }

  delete transaction;
  remove("test.db");
}
}  // namespace dbplay