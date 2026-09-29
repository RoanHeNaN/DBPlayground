//
// End-to-end cloud path against a live MinIO: the same import -> query ->
// snapshot-isolation -> WAL indexing -> import scenario as CloudPathTest, but with
// S3FileStorage + S3MetadataStore instead of the in-memory fakes. Proves the
// whole CloudTable stack runs on real object storage and conditional-write CAS.
//
// Env-gated via MINIO_ENDPOINT:
//   MINIO_ENDPOINT=http://127.0.0.1:19900 MINIO_ACCESS_KEY=minioadmin \
//   MINIO_SECRET_KEY=minioadmin MINIO_BUCKET=dbplayground ./S3IntegrationTest_exe
//

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Cloud/CloudTable.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/S3/S3Client.h"
#include "Cloud/S3/S3FileStorage.h"
#include "Cloud/S3/S3MetadataStore.h"
#include "Execution/ScanExecutor.h"
#include "Table/Format/NativeColumnarFileFormat.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

const char *EnvOr(const char *name, const char *fallback) {
  const char *v = std::getenv(name);
  return (v != nullptr && *v != 0) ? v : fallback;
}

Schema TestSchema() { return {{"id", Type::Int64}, {"message", Type::String}}; }

Chunk MakeChunk(int64_t first, int64_t count) {
  Chunk chunk;
  chunk.column_ids = {0, 1};
  chunk.columns.emplace_back(Type::Int64);
  chunk.columns.emplace_back(Type::String);
  for (int64_t i = 0; i < count; ++i) {
    const int64_t id = first + i;
    chunk.columns[0].Append<int64_t>(id);
    chunk.columns[1].AppendBytes(Slice("event-" + std::to_string(id)));
  }
  chunk.row_count = count;
  return chunk;
}

class SequenceKeys : public IObjectKeyGenerator {
 public:
  std::string NewDataFileKey(const std::string &) override { return "data/" + std::to_string(next_data_++) + ".dbc1"; }
  std::string NewManifestKey(const std::string &) override {
    return "manifest/" + std::to_string(next_manifest_++) + ".meta";
  }

 private:
  std::atomic<uint64_t> next_data_{1};
  std::atomic<uint64_t> next_manifest_{1};
};

class S3IntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *endpoint = std::getenv("MINIO_ENDPOINT");
    if (endpoint == nullptr || *endpoint == 0) {
      GTEST_SKIP() << "MINIO_ENDPOINT not set; skipping live S3 integration test";
    }
    s3::S3Config cfg;
    cfg.endpoint = endpoint;
    cfg.region = EnvOr("MINIO_REGION", "us-east-1");
    cfg.access_key = EnvOr("MINIO_ACCESS_KEY", "minioadmin");
    cfg.secret_key = EnvOr("MINIO_SECRET_KEY", "minioadmin");
    const std::string bucket = EnvOr("MINIO_BUCKET", "dbplayground");
    client_ = std::make_shared<s3::S3Client>(cfg);

    // Unique prefix per test case (time+pid alone collides between two tests in
    // the same process/second) so no run ever reuses another's CURRENT / keys.
    static std::atomic<uint64_t> seq{0};
    const std::string run = "itest/run-" + std::to_string(std::time(nullptr)) + "-" +
                            std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(seq++);

    descriptor_.table_id = "tenant-a/events";
    descriptor_.metadata_prefix = run + "/metadata";
    descriptor_.file_prefix = run + "/files";
    descriptor_.schema = TestSchema();

    auto files = std::make_shared<S3FileStorage>(client_, bucket);
    auto metadata = std::make_shared<S3MetadataStore>(client_, bucket);
    table_ = std::make_shared<CloudTable>(descriptor_, files, metadata,
                                          std::make_shared<NativeColumnarFileFormat>(descriptor_.schema));
    keys_ = std::make_shared<SequenceKeys>();
  }

  size_t RowCount() {
    auto source = table_->OpenSnapshot();
    return source == nullptr ? 0 : CollectProjected(*source, {0}).size();
  }

  TableDescriptor descriptor_;
  std::shared_ptr<s3::S3Client> client_;
  std::shared_ptr<CloudTable> table_;
  std::shared_ptr<SequenceKeys> keys_;
};

TEST_F(S3IntegrationTest, FullImportQueryIndexCycleOnMinIO) {
  auto writer = table_->NewWriter();
  ASSERT_EQ(writer->Start().code, CloudWriterStartCode::Started);

  // First import is immediately visible.
  CloudImportBatch first;
  first.batch_ids = {"client-1"};
  first.chunks = {MakeChunk(0, 3)};
  ASSERT_EQ(writer->Import(first).code, CloudImportCode::Committed);

  auto pinned = table_->OpenSnapshot();
  ASSERT_NE(pinned, nullptr);
  auto pinned_rows = CollectProjected(*pinned, {0, 1});
  ASSERT_EQ(pinned_rows.size(), 3u);
  EXPECT_EQ(pinned_rows[2][0].AsInt64(), 2);
  EXPECT_EQ(pinned_rows[2][1].AsString(), "event-2");

  // Second import; the earlier snapshot stays pinned to its commit head.
  CloudImportBatch second;
  second.batch_ids = {"client-2", "client-3"};
  second.chunks = {MakeChunk(100, 2), MakeChunk(200, 1)};
  ASSERT_EQ(writer->Import(second).code, CloudImportCode::Committed);
  EXPECT_EQ(CollectProjected(*pinned, {0}).size(), 3u);  // immutable snapshot
  EXPECT_EQ(RowCount(), 6u);                             // fresh query sees all

  // Indexing publishes WAL as data plus manifest and advances indexed_seq.
  const auto indexed = table_->NewIndexer(keys_)->Index();
  ASSERT_EQ(indexed.code, CloudIndexCode::Indexed);
  EXPECT_EQ(indexed.indexed_seq, 3u);
  EXPECT_EQ(indexed.published_seq, 3u);

  // A post-indexing import lands on the WAL tail above the new compacted-data set.
  CloudImportBatch third;
  third.batch_ids = {"client-4"};
  third.chunks = {MakeChunk(300, 1)};
  ASSERT_EQ(writer->Import(third).code, CloudImportCode::Committed);

  auto after = table_->OpenSnapshot();
  ASSERT_NE(after, nullptr);
  auto after_rows = CollectProjected(*after, {0});
  ASSERT_EQ(after_rows.size(), 7u);
  EXPECT_EQ(after_rows[0][0].AsInt64(), 0);
  EXPECT_EQ(after_rows[5][0].AsInt64(), 200);
  EXPECT_EQ(after_rows[6][0].AsInt64(), 300);

  const auto snapshot = table_->LoadSnapshot();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->indexed_seq, 3u);
  EXPECT_EQ(snapshot->published_seq, 4u);
  EXPECT_EQ(snapshot->data_files.size(), 1u);
  EXPECT_EQ(snapshot->wal_files.size(), 1u);
}

TEST_F(S3IntegrationTest, SecondWriterFencesTheFirstOnMinIO) {
  auto old_writer = table_->NewWriter();
  ASSERT_EQ(old_writer->Start().code, CloudWriterStartCode::Started);
  auto new_writer = table_->NewWriter();
  ASSERT_EQ(new_writer->Start().code, CloudWriterStartCode::Started);
  EXPECT_GT(new_writer->writer_epoch(), old_writer->writer_epoch());

  CloudImportBatch batch;
  batch.batch_ids = {"stale-client"};
  batch.chunks = {MakeChunk(0, 1)};
  EXPECT_EQ(old_writer->Import(batch).code, CloudImportCode::Fenced);
  EXPECT_FALSE(old_writer->started());
}

TEST_F(S3IntegrationTest, DeferredWalBecomesVisibleAfterIndexingOnMinIO) {
  auto writer = table_->NewWriter();
  ASSERT_EQ(writer->Start().code, CloudWriterStartCode::Started);
  CloudImportBatch batch;
  batch.batch_ids = {"deferred-client"};
  batch.chunks = {MakeChunk(500, 2)};
  batch.visibility = CloudImportBatch::Visibility::Deferred;
  const auto result = writer->Import(batch);
  ASSERT_EQ(result.code, CloudImportCode::Durable);
  EXPECT_EQ(result.wal_seq, 2u);
  auto strict = table_->OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  EXPECT_TRUE(CollectProjected(*strict, {0}).empty());
  ASSERT_EQ(table_->NewIndexer(keys_)->Index().code, CloudIndexCode::Indexed);
  strict = table_->OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  EXPECT_EQ(CollectProjected(*strict, {0}).size(), 2u);
}

TEST_F(S3IntegrationTest, ConcurrentWritersAndStrictReaderOnMinIO) {
  constexpr int kWriters = 4;
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop_reader{false};
  std::atomic<size_t> reads{0};
  std::mutex errors_mutex;
  std::vector<std::string> errors;
  auto fail = [&](std::string message) {
    std::lock_guard<std::mutex> lock(errors_mutex);
    errors.push_back(std::move(message));
  };

  std::thread reader([&] {
    ++ready;
    while (!go.load()) std::this_thread::yield();
    std::set<int64_t> previously_visible;
    try {
      while (!stop_reader.load()) {
        auto source = table_->OpenSnapshot(false);
        if (source) {
          std::set<int64_t> visible;
          for (const auto &row : CollectProjected(*source, {0})) {
            const int64_t id = row[0].AsInt64();
            if (id < 1000 || id >= 1000 + kWriters || !visible.insert(id).second) {
              fail("strict read returned an unexpected or duplicate row");
              return;
            }
          }
          for (int64_t id : previously_visible) {
            if (visible.count(id) == 0) {
              fail("strict read lost a previously visible row");
              return;
            }
          }
          previously_visible = std::move(visible);
        }
        ++reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (const std::exception &e) {
      fail(std::string("reader: ") + e.what());
    }
  });

  std::vector<std::thread> writers;
  for (int i = 0; i < kWriters; ++i) {
    writers.emplace_back([&, i] {
      ++ready;
      while (!go.load()) std::this_thread::yield();
      try {
        CloudImportBatch batch;
        batch.batch_ids = {"concurrent-client-" + std::to_string(i)};
        batch.chunks = {MakeChunk(1000 + i, 1)};
        for (int attempt = 0; attempt < 40; ++attempt) {
          auto writer = table_->NewWriter(8);
          const auto start = writer->Start();
          if (start.code == CloudWriterStartCode::Started) {
            const auto result = writer->Import(batch);
            if (result.code == CloudImportCode::Committed || result.code == CloudImportCode::AlreadyCommitted) return;
            if (result.code != CloudImportCode::Fenced && result.code != CloudImportCode::RetryableConflict) {
              fail("writer " + std::to_string(i) + " returned unexpected import result " +
                   std::to_string(static_cast<int>(result.code)));
              return;
            }
          } else if (start.code != CloudWriterStartCode::Contended &&
                     start.code != CloudWriterStartCode::RetryableConflict) {
            fail("writer " + std::to_string(i) + " returned unexpected start result " +
                 std::to_string(static_cast<int>(start.code)));
            return;
          }
        }
        fail("writer " + std::to_string(i) + " exhausted contention retries");
      } catch (const std::exception &e) {
        fail("writer " + std::to_string(i) + ": " + e.what());
      }
    });
  }

  while (ready.load() != kWriters + 1) std::this_thread::yield();
  go = true;
  for (auto &writer : writers) writer.join();
  stop_reader = true;
  reader.join();
  ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
  EXPECT_GT(reads.load(), 0u);

  auto strict = table_->OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  auto rows = CollectProjected(*strict, {0});
  std::set<int64_t> ids;
  for (const auto &row : rows) ids.insert(row[0].AsInt64());
  EXPECT_EQ(rows.size(), kWriters);
  EXPECT_EQ(ids, (std::set<int64_t>{1000, 1001, 1002, 1003}));

  const auto indexed = table_->NewIndexer(keys_)->Index();
  ASSERT_EQ(indexed.code, CloudIndexCode::Indexed);
  S3FileStorage wal_files(client_, EnvOr("MINIO_BUCKET", "dbplayground"));
  const auto paths = wal_files.List(descriptor_.file_prefix + "/wal/");
  ASSERT_EQ(paths.size(), indexed.indexed_seq);
  for (uint64_t seq = 1; seq <= indexed.indexed_seq; ++seq) {
    const auto path = SequentialWalKey(descriptor_, seq);
    EXPECT_NE(std::find(paths.begin(), paths.end(), path), paths.end());
  }
  strict = table_->OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  rows = CollectProjected(*strict, {0});
  ids.clear();
  for (const auto &row : rows) ids.insert(row[0].AsInt64());
  EXPECT_EQ(rows.size(), kWriters);
  EXPECT_EQ(ids, (std::set<int64_t>{1000, 1001, 1002, 1003}));
}

}  // namespace
}  // namespace dbplay
