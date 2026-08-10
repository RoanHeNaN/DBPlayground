#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "Storage/BufferPool/BufferPoolManager.h"
#include "gtest/gtest.h"

namespace dbplay {

// NOLINTNEXTLINE
// Check whether pages containing terminal characters can be recovered
TEST(BufferPoolManagerTest, BinaryDataTest) {
  const std::string db_name = "test.db";
  const size_t buffer_pool_size = 10;

  std::random_device r;
  std::default_random_engine rng(r());
  std::uniform_int_distribution<unsigned int> uniform_dist(0, 255);

  auto disk_manager = std::make_shared<DiskManager>(db_name);
  auto bpm = std::make_shared<BufferPoolManager>(buffer_pool_size, disk_manager);

  auto page0 = bpm->NewPage();
  page_id_t page_id_temp = page0->GetPageId();

  // Scenario: The buffer pool is empty. We should be able to create a new page.
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(0, page_id_temp);

  char random_binary_data[PAGE_SIZE];
  // Generate random binary data
  for (char &i : random_binary_data) {
    i = static_cast<char>(uniform_dist(rng));
  }

  // Insert terminal characters both in the middle and at end
  random_binary_data[PAGE_SIZE / 2] = '\0';
  random_binary_data[PAGE_SIZE - 1] = '\0';

  // Scenario: Once we have a page, we should be able to read and write content.
  std::memcpy(page0->GetData(), random_binary_data, PAGE_SIZE);
  EXPECT_EQ(0, std::memcmp(page0->GetData(), random_binary_data, PAGE_SIZE));

  // Scenario: We should be able to create new pages until we fill up the buffer
  // pool.
  for (size_t i = 1; i < buffer_pool_size; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage());
  }

  // Scenario: Once the buffer pool is full, we should not be able to create any
  // new pages.
  for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
    EXPECT_EQ(nullptr, bpm->NewPage());
  }

  // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new
  // pages, there would still be one cache frame left for reading page 0.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(true, bpm->UnpinPage(i, true));
    bpm->FlushPage(i);
  }
  for (int i = 0; i < 5; ++i) {
    auto new_page = bpm->NewPage();
    EXPECT_NE(nullptr, new_page);
    bpm->UnpinPage(new_page->GetPageId(), false);
  }
  // Scenario: We should be able to fetch the data we wrote a while ago.
  page0 = bpm->FetchPage(0);
  EXPECT_EQ(0, std::memcmp(page0->GetData(), random_binary_data, PAGE_SIZE));
  EXPECT_EQ(true, bpm->UnpinPage(0, true));

  // Shutdown the disk manager and remove the temporary file we created.
  // disk_manager->ShutDown();
  remove("test.db");
}

// NOLINTNEXTLINE
TEST(BufferPoolManagerTest, SampleTest) {
  const std::string db_name = "test.db";
  const size_t buffer_pool_size = 10;

  auto disk_manager = std::make_shared<DiskManager>(db_name);
  auto bpm = std::make_shared<BufferPoolManager>(buffer_pool_size, disk_manager);

  auto page0 = bpm->NewPage();
  page_id_t page_id_temp = page0->GetPageId();

  // Scenario: The buffer pool is empty. We should be able to create a new page.
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(0, page_id_temp);

  // Scenario: Once we have a page, we should be able to read and write content.
  snprintf(page0->GetData(), PAGE_SIZE, "Hello");
  EXPECT_EQ(0, strcmp(page0->GetData(), "Hello"));

  // Scenario: We should be able to create new pages until we fill up the buffer
  // pool.
  for (size_t i = 1; i < buffer_pool_size; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage());
  }

  // Scenario: Once the buffer pool is full, we should not be able to create any
  // new pages.
  for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
    EXPECT_EQ(nullptr, bpm->NewPage());
  }

  // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new
  // pages, there would still be one buffer page left for reading page 0.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(true, bpm->UnpinPage(i, true));
  }
  for (int i = 0; i < 4; ++i) {
    EXPECT_NE(nullptr, bpm->NewPage());
  }

  // Scenario: We should be able to fetch the data we wrote a while ago.
  page0 = bpm->FetchPage(0);
  EXPECT_EQ(0, strcmp(page0->GetData(), "Hello"));

  // Scenario: If we unpin page 0 and then make a new page, all the buffer pages
  // should now be pinned. Fetching page 0 should fail.
  EXPECT_EQ(true, bpm->UnpinPage(0, true));
  EXPECT_NE(nullptr, bpm->NewPage());
  EXPECT_EQ(nullptr, bpm->FetchPage(0));

  // Shutdown the disk manager and remove the temporary file we created.
  // disk_manager->ShutDown();
  remove("test.db");
}

TEST(BufferPoolManagerTest, IsDirty) {
  auto disk_manager = std::make_shared<DiskManager>("test.db");
  auto bpm = std::make_shared<BufferPoolManager>(1, disk_manager);

  auto page0 = bpm->NewPage();
  page_id_t page_id_0 = page0->GetPageId();

  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(0, page0->IsDirty());
  strcpy(page0->GetData(), "page0");  // NOLINT
  EXPECT_EQ(1, bpm->UnpinPage(page_id_0, true));

  // Fetch again but don't write. Assert it is still marked as dirty
  page0 = bpm->FetchPage(page_id_0);
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(1, page0->IsDirty());
  EXPECT_EQ(1, bpm->UnpinPage(page_id_0, false));

  // Fetch and assert it is still dirty
  page0 = bpm->FetchPage(page_id_0);
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(1, page0->IsDirty());
  EXPECT_EQ(1, bpm->UnpinPage(page_id_0, false));

  // Create a new page, assert it's not dirty

  auto page1 = bpm->NewPage();
  page_id_t page_id_1 = page1->GetPageId();
  ASSERT_NE(nullptr, page1);
  EXPECT_EQ(0, page1->IsDirty());

  // Write to the page, and then delete it
  strcpy(page1->GetData(), "page1");  // NOLINT
  EXPECT_EQ(1, bpm->UnpinPage(page_id_1, true));
  EXPECT_EQ(1, page1->IsDirty());
  EXPECT_EQ(1, bpm->DeletePage(page_id_1));

  // Fetch page 0 again, and confirm its not dirty
  page0 = bpm->FetchPage(page_id_0);
  ASSERT_NE(nullptr, page0);
  EXPECT_EQ(0, page0->IsDirty());

  remove("test.db");
}

// Disabled by default: this is an I/O-bound concurrency stress test (~40s per run
// due to eviction thrashing against synchronous disk writes), so 500 runs would take
// hours. Run explicitly with:
//   ./test/BufferPoolManagerTest_exe --gtest_also_run_disabled_tests \
//       --gtest_filter='BufferPoolManagerTest.DISABLED_HardTest4'
TEST(BufferPoolManagerTest, DISABLED_HardTest4) {
  const int num_threads = 3;
  const int num_runs = 500;
  for (int run = 0; run < num_runs; run++) {
    auto disk_manager = std::make_shared<DiskManager>("test.db");
    auto bpm = std::make_shared<BufferPoolManager>(50, disk_manager);
    std::vector<std::thread> threads;

    page_id_t temp_page_id;
    std::vector<page_id_t> page_ids;
    /*
     * 创建 50 个 page，page data 为 page_id
     */
    for (int i = 0; i < 50; i++) {
      auto new_page = bpm->NewPage();
      temp_page_id = new_page->GetPageId();

      EXPECT_NE(nullptr, new_page);
      ASSERT_NE(nullptr, new_page);
      strcpy(new_page->GetData(), std::to_string(temp_page_id).c_str());  // NOLINT
      page_ids.push_back(temp_page_id);
    }

    // 所有 page unpin
    for (int i = 0; i < 50; i++) {
      if (i % 2 == 0) {
        EXPECT_EQ(1, bpm->UnpinPage(page_ids[i], true));
      } else {
        EXPECT_EQ(1, bpm->UnpinPage(page_ids[i], false));
      }
    }

    /*
     * 再创建 50 个新 page
     * 由于奇数 page unpin 时 dirty 设置为 false，所以它们在被替换时内容丢失
     */
    for (int i = 0; i < 50; i++) {
      auto new_page = bpm->NewPage();
      temp_page_id = new_page->GetPageId();

      EXPECT_NE(nullptr, new_page);
      ASSERT_NE(nullptr, new_page);
      EXPECT_EQ(1, bpm->UnpinPage(temp_page_id, true));
    }

    // 重新 fetch 最早创建的 50 个 page
    // data 前增加 Hard 字符串
    // 偶数 page：Hard${page_id}
    // 奇数 page：Hard
    for (int j = 0; j < 50; j++) {
      auto page = bpm->FetchPage(page_ids[j]);
      EXPECT_NE(nullptr, page);
      ASSERT_NE(nullptr, page);
      strcpy(page->GetData(), (std::string("Hard") + std::to_string(page_ids[j])).c_str());  // NOLINT
    }

    /*
     * 再将前 50 个 page unpin
     * 偶数 page：Hard
     * 奇数 page：Hard
     */

    for (int i = 0; i < 50; i++) {
      if (i % 2 == 0) {
        EXPECT_EQ(1, bpm->UnpinPage(page_ids[i], false));
      } else {
        EXPECT_EQ(1, bpm->UnpinPage(page_ids[i], true));
      }
    }

    // 再创建 50 个新 page
    for (int i = 0; i < 50; i++) {
      auto new_page = bpm->NewPage();
      temp_page_id = new_page->GetPageId();
      EXPECT_NE(nullptr, new_page);
      ASSERT_NE(nullptr, new_page);
      EXPECT_EQ(1, bpm->UnpinPage(temp_page_id, true));
    }

    for (int tid = 0; tid < num_threads; tid++) {
      threads.push_back(std::thread([&bpm, tid, page_ids]() {  // NOLINT
        page_id_t temp_page_id;
        int j = (tid * 10);
        while (j < 50) {
          if (j != tid * 10) {
            auto page_local = bpm->FetchPage(temp_page_id);
            while (page_local == nullptr) {
              page_local = bpm->FetchPage(temp_page_id);
            }
            EXPECT_NE(nullptr, page_local);
            ASSERT_NE(nullptr, page_local);
            EXPECT_EQ(0, std::strcmp(std::to_string(temp_page_id).c_str(), (page_local->GetData())));
            EXPECT_EQ(1, bpm->UnpinPage(temp_page_id, false));
            // If the page is still in buffer pool then put it in free list,
            // else also we are happy
            EXPECT_EQ(1, bpm->DeletePage(temp_page_id));
          }

          auto page = bpm->FetchPage(page_ids[j]);
          while (page == nullptr) {
            page = bpm->FetchPage(page_ids[j]);
          }
          EXPECT_NE(nullptr, page);
          ASSERT_NE(nullptr, page);
          if (j % 2 == 0) {
            // 偶数 page：内容为数字
            EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j]).c_str(), (page->GetData())));
            EXPECT_EQ(1, bpm->UnpinPage(page_ids[j], false));
          } else {
            // 奇数 page：内容为 Hard${page_id}
            EXPECT_EQ(0, std::strcmp((std::string("Hard") + std::to_string(page_ids[j])).c_str(), (page->GetData())));
            EXPECT_EQ(1, bpm->UnpinPage(page_ids[j], false));
          }
          j = (j + 1);

          page = bpm->NewPage();
          temp_page_id = page->GetPageId();
          while (page == nullptr) {
            page = bpm->NewPage();
            temp_page_id = page->GetPageId();
          }
          EXPECT_NE(nullptr, page);
          ASSERT_NE(nullptr, page);
          strcpy(page->GetData(), std::to_string(temp_page_id).c_str());  // NOLINT
          // Flush page instead of unpining with true
          EXPECT_EQ(1, bpm->FlushPage(temp_page_id));
          EXPECT_EQ(1, bpm->UnpinPage(temp_page_id, false));

          // Flood with new pages
          for (int k = 0; k < 10; k++) {
            auto flood_page = bpm->NewPage();
            page_id_t flood_page_id = flood_page->GetPageId();
            while (flood_page == nullptr) {
              flood_page = bpm->NewPage();
              flood_page_id = flood_page->GetPageId();
            }
            EXPECT_NE(nullptr, flood_page);
            ASSERT_NE(nullptr, flood_page);
            EXPECT_EQ(1, bpm->UnpinPage(flood_page_id, false));
            // If the page is still in buffer pool then put it in free list,
            // else also we are happy
            EXPECT_EQ(1, bpm->DeletePage(flood_page_id));
          }
        }
      }));
    }

    for (int i = 0; i < num_threads; i++) {
      threads[i].join();
    }

    for (int j = 0; j < 50; j++) {
      EXPECT_EQ(1, bpm->DeletePage(page_ids[j]));
    }

    remove("test.db");
  }
}
}  // namespace dbplay
