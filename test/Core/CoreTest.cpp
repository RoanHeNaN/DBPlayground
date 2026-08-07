//
// Created by 何智强 on 2021/10/17.
//

#include <cstddef>
#include <random>
#include <thread>
#include <vector>

#include "Common/Config.h"
#include "Container/BPlusTree.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "glog/logging.h"
#include "gtest/gtest.h"

namespace dbplay {

constexpr size_t NUM_TRIES = 1;
const constexpr size_t BUFFER_POOL_SLOT_NUM = 128000;

template <typename... Args>
void LaunchParallelTest(std::vector<std::thread> &threads, uint32_t num_threads, Args &&...args) {
  for (uint32_t thread_itr = 0; thread_itr < num_threads; ++thread_itr) {
    threads.push_back(std::thread(args..., thread_itr));
  }
}

void WaitThreadsFinished(std::vector<std::thread> &threads) {
  for (auto &thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void InsertHelper(std::shared_ptr<BPlusTree<key_t, value_t>> tree, const std::vector<key_t> keys, uint64_t tid) {
  Transaction *transaction = new Transaction(tid);
  for (const auto &key : keys) {
    value_t value = key & 0xFFFFFFFF;
    tree->Insert(key, value, transaction);
  }
  delete transaction;
}

void DeleteHelper(std::shared_ptr<BPlusTree<key_t, value_t>> tree, const std::vector<key_t> keys, uint64_t tid) {
  Transaction *transaction = new Transaction(tid);
  for (const auto &key : keys) {
    tree->Remove(key, transaction);
  }
  delete transaction;
}

TEST(CoreTest, ConcurrentInsert) {
  for (size_t iter = 0; iter < NUM_TRIES; ++iter) {
    auto disk_manager = std::make_shared<DiskManager>("test.db");
    auto buffer_pool_manager = std::make_shared<BufferPoolManager>(BUFFER_POOL_SLOT_NUM, disk_manager);
    auto container = std::make_shared<BPlusTree<key_t, value_t>>(buffer_pool_manager);

    std::vector<key_t> keys;
    constexpr size_t NUM_KEYS = 20000000;  // 20 million

    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<key_t> dist;
    for (size_t iter = 0; iter < NUM_KEYS; ++iter) {
      keys.push_back(dist(mt));
    }

    constexpr size_t NUM_THREADS = 10;
    std::vector<std::thread> insert_threads;
    for (size_t iter = 0; iter < NUM_KEYS;) {
      const std::vector<key_t> keys_interval{keys.begin() + iter, keys.begin() + iter + NUM_KEYS / NUM_THREADS};
      LaunchParallelTest(insert_threads, 1, InsertHelper, container, keys_interval);
      iter += NUM_KEYS / NUM_THREADS;
    }

    LOG(INFO) << "Waiting for " << insert_threads.size() << " insert threads to finish";

    WaitThreadsFinished(insert_threads);

    for (size_t iter = 0; iter < NUM_KEYS; ++iter) {
      key_t key = keys[iter];
      value_t value;
      container->GetValue(key, value);
      EXPECT_EQ((value_t)(key & 0xFFFFFFFF), value);
    }

    remove("test.db");
  }
}

TEST(CoreTest, DISABLED_ConcurrentRemove) {
  for (size_t iter = 0; iter < NUM_TRIES; ++iter) {
    auto disk_manager = std::make_shared<DiskManager>("test.db");
    auto buffer_pool_manager = std::make_shared<BufferPoolManager>(50, disk_manager);
    auto container = std::make_shared<BPlusTree<key_t, value_t>>(buffer_pool_manager);

    std::vector<key_t> keys;
    constexpr size_t NUM_KEYS = 20000;

    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<key_t> dist;
    for (size_t iter = 0; iter < NUM_KEYS; ++iter) {
      keys.push_back(dist(mt));
    }

    InsertHelper(container, keys, 1);

    constexpr size_t NUM_THREADS = 10;
    constexpr size_t NUM_PER_THREAD = 10;
    std::vector<std::thread> delete_threads;
    for (size_t iter = 0; iter < NUM_THREADS; ++iter) {
      size_t current_index = iter * NUM_PER_THREAD;
      std::vector<key_t> key_interval{keys.begin() + current_index, keys.begin() + current_index + NUM_PER_THREAD};
      LaunchParallelTest(delete_threads, 1, DeleteHelper, container, key_interval);
    }

    LOG(INFO) << "Waiting for " << delete_threads.size() << " delete threads to finish";
    WaitThreadsFinished(delete_threads);

    for (size_t iter = 0; iter < NUM_KEYS; ++iter) {
      key_t key = keys[iter];
      value_t value;
      bool found = container->GetValue(key, value);
      if (iter < (NUM_PER_THREAD * NUM_THREADS))
        EXPECT_EQ(found, false);
      else {
        EXPECT_EQ(found, true);
        EXPECT_EQ((value_t)(key & 0xFFFFFFFF), value);
      }
    }

    remove("test.db");
  }
}

TEST(CoreTest, DISABLED_ConcurrentRead) {
  for (size_t iter = 0; iter < NUM_TRIES; ++iter) {
    auto disk_manager = std::make_shared<DiskManager>("test.db");
    auto buffer_pool_manager = std::make_shared<BufferPoolManager>(50, disk_manager);
    auto container = std::make_shared<BPlusTree<key_t, value_t>>(buffer_pool_manager);

    std::vector<key_t> keys;
    constexpr size_t NUM_KEYS = 20000;

    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<key_t> dist;
    for (size_t iter = 0; iter < NUM_KEYS; ++iter) {
      keys.push_back(dist(mt));
    }

    constexpr size_t NUM_INSERT_THREADS = 1;
    std::vector<std::thread> insert_threads;
    for (size_t iter = 0; iter < NUM_KEYS;) {
      const std::vector<key_t> keys_interval{keys.begin() + iter, keys.begin() + iter + NUM_KEYS / NUM_INSERT_THREADS};
      LaunchParallelTest(insert_threads, 1, InsertHelper, container, keys_interval);
      iter += NUM_KEYS / NUM_INSERT_THREADS;
    }

    LOG(INFO) << "Waiting for " << insert_threads.size() << " insert threads to finish";

    WaitThreadsFinished(insert_threads);

    LOG(INFO) << insert_threads.size() << " insert threads finished";

    auto read_func = [&](uint32_t tid) {
      for (const auto &key : keys) {
        Transaction *transaction = new Transaction(tid);
        value_t value;
        container->GetValue(key, value, transaction);
        EXPECT_EQ((value_t)(key & 0xFFFFFFFF), value);
        delete transaction;
      }
    };

    std::vector<std::thread> read_threads;

    constexpr size_t NUM_THREADS = 4;
    LaunchParallelTest(read_threads, NUM_THREADS, read_func);

    LOG(INFO) << "Waiting " << read_threads.size() << " read threads to finish";
    WaitThreadsFinished(read_threads);

    remove("test.db");
  }
}
}  // namespace dbplay
