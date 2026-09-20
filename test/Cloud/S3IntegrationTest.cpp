//
// End-to-end cloud path against a live MinIO: the same import -> query ->
// snapshot-isolation -> compaction -> import scenario as CloudPathTest, but with
// S3FileStorage + S3MetadataStore instead of the in-memory fakes. Proves the
// whole CloudTable stack runs on real object storage and conditional-write CAS.
//
// Env-gated via MINIO_ENDPOINT:
//   MINIO_ENDPOINT=http://127.0.0.1:19900 MINIO_ACCESS_KEY=minioadmin \
//   MINIO_SECRET_KEY=minioadmin MINIO_BUCKET=dbplayground ./S3IntegrationTest_exe
//

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "Cloud/CloudTable.h"
#include "Cloud/IBatchCommitResolver.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/MetadataCompactedDataManifestStore.h"
#include "Cloud/S3/S3Client.h"
#include "Cloud/S3/S3FileStorage.h"
#include "Cloud/S3/S3MetadataStore.h"
#include "Execution/ScanExecutor.h"
#include "Table/Format/NativeColumnarFileFormat.h"
#include "Table/Format/WalFileFormat.h"
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
  std::string NewWalKey(const std::string &) override { return "wal/" + std::to_string(next_wal_++) + ".wal"; }
  std::string NewCommitKey(const std::string &) override {
    return "commit/" + std::to_string(next_commit_++) + ".meta";
  }
  std::string NewCompactedDataFileKey(const std::string &) override {
    return "data/" + std::to_string(next_data_++) + ".dbc1";
  }
  std::string NewCompactedDataManifestKey(const std::string &) override {
    return "manifest/" + std::to_string(next_manifest_++) + ".meta";
  }

 private:
  std::atomic<uint64_t> next_wal_{1};
  std::atomic<uint64_t> next_commit_{1};
  std::atomic<uint64_t> next_data_{1};
  std::atomic<uint64_t> next_manifest_{1};
};

class NeverCommittedBatches : public IBatchCommitResolver {
 public:
  BatchCommitStatus Lookup(const TableDescriptor &, const VersionedCurrentTableState &,
                           const std::vector<std::string> &) const override {
    return BatchCommitStatus::NotCommitted;
  }
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
    auto compacted_data_manifest_store = std::make_shared<MetadataCompactedDataManifestStore>(
        descriptor_.table_id, descriptor_.metadata_prefix, metadata);
    table_ = std::make_shared<CloudTable>(descriptor_, files, metadata, compacted_data_manifest_store,
                                          std::make_shared<NativeColumnarFileFormat>(descriptor_.schema),
                                          std::make_shared<WalFileFormat>(descriptor_.schema));
    keys_ = std::make_shared<SequenceKeys>();
    batches_ = std::make_shared<NeverCommittedBatches>();
  }

  size_t RowCount() {
    auto source = table_->OpenSnapshot();
    return source == nullptr ? 0 : CollectProjected(*source, {0}).size();
  }

  TableDescriptor descriptor_;
  std::shared_ptr<s3::S3Client> client_;
  std::shared_ptr<CloudTable> table_;
  std::shared_ptr<SequenceKeys> keys_;
  std::shared_ptr<NeverCommittedBatches> batches_;
};

TEST_F(S3IntegrationTest, FullImportQueryCompactCycleOnMinIO) {
  auto writer = table_->NewWriter(keys_, batches_);
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

  // Compaction folds the WAL tail into a compacted-data file and advances compacted_cursor.
  const auto compacted = table_->NewCompactor(keys_)->Compact();
  ASSERT_EQ(compacted.code, CloudCompactCode::Compacted);
  EXPECT_EQ(compacted.compacted_cursor, 2u);
  EXPECT_EQ(compacted.committed_cursor, 2u);

  // A post-compaction import lands on the WAL tail above the new compacted-data set.
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
  EXPECT_EQ(snapshot->compacted_cursor, 2u);
  EXPECT_EQ(snapshot->committed_cursor, 3u);
  EXPECT_EQ(snapshot->compacted_data_files.size(), 1u);
  EXPECT_EQ(snapshot->wal_files.size(), 1u);
}

TEST_F(S3IntegrationTest, SecondWriterFencesTheFirstOnMinIO) {
  auto old_writer = table_->NewWriter(std::make_shared<SequenceKeys>(), batches_);
  ASSERT_EQ(old_writer->Start().code, CloudWriterStartCode::Started);
  auto new_writer = table_->NewWriter(std::make_shared<SequenceKeys>(), batches_);
  ASSERT_EQ(new_writer->Start().code, CloudWriterStartCode::Started);
  EXPECT_GT(new_writer->writer_epoch(), old_writer->writer_epoch());

  CloudImportBatch batch;
  batch.batch_ids = {"stale-client"};
  batch.chunks = {MakeChunk(0, 1)};
  EXPECT_EQ(old_writer->Import(batch).code, CloudImportCode::Fenced);
  EXPECT_FALSE(old_writer->started());
}

}  // namespace
}  // namespace dbplay
