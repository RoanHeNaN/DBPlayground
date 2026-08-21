#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "Metadata/MemMetadataStore.h"
#include "gtest/gtest.h"

namespace dbplay {

TEST(MetadataStoreTest, PutIfAbsentAndCompareExchange) {
  MemMetadataStore store;
  MetadataVersion first;
  ASSERT_EQ(store.PutIfAbsent("CURRENT", Slice("v1"), &first), ConditionalWriteResult::Applied);

  MetadataVersion second;
  ASSERT_EQ(store.CompareExchange("CURRENT", first, Slice("v2"), &second), ConditionalWriteResult::Applied);
  EXPECT_NE(first, second);

  MetadataVersion untouched("sentinel");
  EXPECT_EQ(store.CompareExchange("CURRENT", first, Slice("stale"), &untouched),
            ConditionalWriteResult::PreconditionFailed);
  EXPECT_EQ(untouched.opaque(), "sentinel");
  ASSERT_TRUE(store.Get("CURRENT").has_value());
  EXPECT_EQ(store.Get("CURRENT")->value, "v2");
}

TEST(MetadataStoreTest, VersionsPreventAbaAcrossDeleteAndRecreate) {
  MemMetadataStore store;
  MetadataVersion original;
  ASSERT_EQ(store.PutIfAbsent("CURRENT", Slice("A"), &original), ConditionalWriteResult::Applied);

  MetadataVersion middle;
  ASSERT_EQ(store.CompareExchange("CURRENT", original, Slice("B"), &middle), ConditionalWriteResult::Applied);
  MetadataVersion restored;
  ASSERT_EQ(store.CompareExchange("CURRENT", middle, Slice("A"), &restored), ConditionalWriteResult::Applied);
  EXPECT_NE(original, restored);

  store.DeleteForTest("CURRENT");
  MetadataVersion recreated;
  ASSERT_EQ(store.PutIfAbsent("CURRENT", Slice("A"), &recreated), ConditionalWriteResult::Applied);
  EXPECT_NE(original, recreated);
  EXPECT_EQ(store.CompareExchange("CURRENT", original, Slice("stale"), nullptr),
            ConditionalWriteResult::PreconditionFailed);
}

TEST(MetadataStoreTest, ConcurrentCompareExchangeHasOneWinner) {
  MemMetadataStore store;
  MetadataVersion initial;
  ASSERT_EQ(store.PutIfAbsent("CURRENT", Slice("initial"), &initial), ConditionalWriteResult::Applied);

  constexpr size_t kThreads = 64;
  std::atomic<bool> start{false};
  std::atomic<size_t> applied{0};
  std::vector<std::thread> threads;
  for (size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      std::string value = "writer-" + std::to_string(i);
      if (store.CompareExchange("CURRENT", initial, Slice(value), nullptr) == ConditionalWriteResult::Applied) {
        applied.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(applied.load(), 1u);
}

TEST(MetadataStoreTest, ConcurrentPutIfAbsentHasOneWinner) {
  MemMetadataStore store;
  constexpr size_t kThreads = 64;
  std::atomic<bool> start{false};
  std::atomic<size_t> applied{0};
  std::vector<std::thread> threads;
  for (size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      std::string value = "writer-" + std::to_string(i);
      if (store.PutIfAbsent("CURRENT", Slice(value), nullptr) == ConditionalWriteResult::Applied) {
        applied.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(applied.load(), 1u);
}

}  // namespace dbplay
