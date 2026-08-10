//
// Created by 何智强 on 2021/10/11.
//

#include <cstdint>
#include <memory>

#include "Common/EncodedKey.h"
#include "Common/RID.h"
#include "Concurrency/Transaction.h"
#include "Container/BPlusTree.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "gtest/gtest.h"

namespace dbplay {
TEST(BPlusTreeTest, InsertTest1) {
  auto disk_manager = std::make_shared<DiskManager>("test.db");
  auto bpm = std::make_shared<BufferPoolManager>(50, disk_manager);

  BPlusTree tree{bpm, 2, 3};
  std::unique_ptr<Transaction> transaction = std::make_unique<Transaction>(0);

  const int64_t max_key = 5;
  for (int64_t key = 0; key < max_key; ++key) {
    tree.Insert(MakeEncodedKey<int64_t>(key), RID(static_cast<page_id_t>(key), static_cast<uint32_t>(key)));
  }

  for (int64_t key = 0; key < max_key; ++key) {
    RID value;
    tree.GetValue(MakeEncodedKey<int64_t>(key), value, transaction.get());
    EXPECT_EQ(value, RID(static_cast<page_id_t>(key), static_cast<uint32_t>(key)));
  }
  static_cast<void>(transaction.release());
  remove("test.db");
}
}  // namespace dbplay
