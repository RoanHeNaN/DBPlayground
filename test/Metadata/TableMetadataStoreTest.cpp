#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Metadata/MemMetadataStore.h"
#include "Metadata/TableMetadataCodec.h"
#include "Metadata/TableMetadataStore.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

TableState InitializedState(TableMetadataStore *tables) {
  VersionedTableState created;
  EXPECT_EQ(tables->Initialize(tables->NewTableState(), &created), InitializeTableResult::Created);
  return created.state;
}

}  // namespace

TEST(TableMetadataCodecTest, RoundTripsTableState) {
  TableState state;
  state.table_id = "tenant-a/events";
  state.state_version = 42;
  state.writer_epoch = 7;
  state.committed_cursor = 1042;
  state.indexed_cursor = 1038;
  state.base_manifest = "manifest/base-1038.meta";
  state.commit_head = "commit/1042-uuid.meta";

  const std::string encoded = TableMetadataCodec::EncodeTableState(state);
  EXPECT_EQ(TableMetadataCodec::DecodeTableState(Slice(encoded)), state);
}

TEST(TableMetadataCodecTest, RoundTripsCommitRecord) {
  CommitRecord record;
  record.table_id = "tenant-a/events";
  record.writer_epoch = 7;
  record.first_cursor = 1039;
  record.last_cursor = 1042;
  record.parent_commit = "commit/1038-parent.meta";
  record.wal_files = {"wal/1039-1040.wal", "wal/1041-1042.wal"};
  record.batch_ids = {"client-batch-a", "client-batch-b"};

  const std::string encoded = TableMetadataCodec::EncodeCommitRecord(record);
  EXPECT_EQ(TableMetadataCodec::DecodeCommitRecord(Slice(encoded)), record);
}

TEST(TableMetadataCodecTest, RejectsCorruptionAndInvalidState) {
  TableState state;
  state.table_id = "table-a";
  std::string encoded = TableMetadataCodec::EncodeTableState(state);

  std::string truncated = encoded.substr(0, encoded.size() - 1);
  EXPECT_THROW(TableMetadataCodec::DecodeTableState(Slice(truncated)), std::invalid_argument);

  encoded.push_back('x');
  EXPECT_THROW(TableMetadataCodec::DecodeTableState(Slice(encoded)), std::invalid_argument);

  state.indexed_cursor = 2;
  state.committed_cursor = 1;
  EXPECT_THROW(TableMetadataCodec::EncodeTableState(state), std::invalid_argument);
}

TEST(TableMetadataStoreTest, InitializesAndLoadsCurrentWithoutList) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore tables("table-a", "tables/table-a/", metadata);
  EXPECT_EQ(tables.current_key(), "tables/table-a/CURRENT");
  EXPECT_FALSE(tables.Load().has_value());

  VersionedTableState created;
  const TableState initial = tables.NewTableState();
  EXPECT_EQ(tables.Initialize(initial, &created), InitializeTableResult::Created);
  EXPECT_EQ(tables.Initialize(initial), InitializeTableResult::AlreadyExists);

  const auto loaded = tables.Load();
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->state, initial);
  EXPECT_EQ(loaded->metadata_version, created.metadata_version);
}

TEST(TableMetadataStoreTest, PublishesWithCurrentVersionAndRejectsStaleWriter) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewTableState()), InitializeTableResult::Created);
  const VersionedTableState initial = *tables.Load();

  VersionedTableState owner;
  ASSERT_EQ(tables.AcquireWriter(initial, &owner), AcquireWriterResult::Acquired);
  EXPECT_EQ(tables.AcquireWriter(initial), AcquireWriterResult::StaleVersion);

  CommitRecord record;
  record.table_id = "table-a";
  record.writer_epoch = owner.state.writer_epoch;
  record.first_cursor = 1;
  record.last_cursor = 1;
  record.wal_files = {"wal/1.wal"};
  record.batch_ids = {"batch-1"};
  VersionedTableState committed;
  ASSERT_EQ(tables.PublishWal(owner, "commit/1.meta", record, &committed), PublishWalResult::Committed);
  EXPECT_EQ(committed.state.committed_cursor, 1u);
  EXPECT_EQ(committed.state.commit_head, "commit/1.meta");
  ASSERT_TRUE(tables.LoadCommitRecord("commit/1.meta").has_value());
  EXPECT_EQ(*tables.LoadCommitRecord("commit/1.meta"), record);

  VersionedTableState recovered;
  EXPECT_EQ(tables.PublishWal(owner, "commit/1.meta", record, &recovered), PublishWalResult::AlreadyCommitted);
  EXPECT_EQ(recovered.state, committed.state);
}

TEST(TableMetadataStoreTest, RejectsInvalidCommitBeforeCas) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewTableState()), InitializeTableResult::Created);
  VersionedTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  CommitRecord invalid;
  invalid.table_id = "table-a";
  invalid.writer_epoch = owner.state.writer_epoch;
  invalid.first_cursor = 2;
  invalid.last_cursor = 2;
  invalid.wal_files = {"wal/2.wal"};
  EXPECT_EQ(tables.PublishWal(owner, "commit/2.meta", invalid), PublishWalResult::InvalidCommit);

  invalid.first_cursor = 1;
  invalid.last_cursor = 1;
  invalid.table_id = "table-b";
  EXPECT_EQ(tables.PublishWal(owner, "commit/1.meta", invalid), PublishWalResult::InvalidCommit);
  EXPECT_FALSE(tables.LoadCommitRecord("commit/1.meta").has_value());
  EXPECT_EQ(tables.Load()->state, owner.state);
}

TEST(TableMetadataStoreTest, ConcurrentPublishHasExactlyOneWinner) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewTableState()), InitializeTableResult::Created);
  VersionedTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  constexpr size_t kThreads = 64;
  std::atomic<bool> start{false};
  std::atomic<size_t> applied{0};
  std::atomic<size_t> rebase_required{0};
  std::vector<std::thread> threads;
  for (size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      CommitRecord record;
      record.table_id = "table-a";
      record.writer_epoch = owner.state.writer_epoch;
      record.first_cursor = 1;
      record.last_cursor = 1;
      record.wal_files = {"wal/writer-" + std::to_string(i) + ".wal"};
      record.batch_ids = {"batch-" + std::to_string(i)};
      const PublishWalResult result = tables.PublishWal(owner, "commit/writer-" + std::to_string(i) + ".meta", record);
      if (result == PublishWalResult::Committed) {
        applied.fetch_add(1, std::memory_order_relaxed);
      } else if (result == PublishWalResult::RebaseRequired) {
        rebase_required.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(applied.load(), 1u);
  EXPECT_EQ(rebase_required.load(), kThreads - 1);
  EXPECT_EQ(tables.Load()->state.committed_cursor, 1u);
}

TEST(TableMetadataStoreTest, OldWriterIsFencedAfterNewEpochIsAcquired) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewTableState()), InitializeTableResult::Created);

  VersionedTableState old_writer;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &old_writer), AcquireWriterResult::Acquired);
  VersionedTableState new_writer;
  ASSERT_EQ(tables.AcquireWriter(old_writer, &new_writer), AcquireWriterResult::Acquired);

  CommitRecord stale_record;
  stale_record.table_id = "table-a";
  stale_record.writer_epoch = old_writer.state.writer_epoch;
  stale_record.first_cursor = 1;
  stale_record.last_cursor = 1;
  stale_record.wal_files = {"wal/stale.wal"};
  stale_record.batch_ids = {"stale-batch"};
  EXPECT_EQ(tables.PublishWal(old_writer, "commit/stale.meta", stale_record), PublishWalResult::Fenced);
  EXPECT_EQ(tables.Load()->state.writer_epoch, new_writer.state.writer_epoch);
}

TEST(TableMetadataStoreTest, ReusedCommitKeyMustContainIdenticalRecord) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewTableState()), InitializeTableResult::Created);
  VersionedTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  CommitRecord first;
  first.table_id = "table-a";
  first.writer_epoch = owner.state.writer_epoch;
  first.first_cursor = 1;
  first.last_cursor = 1;
  first.wal_files = {"wal/first.wal"};
  first.batch_ids = {"batch-1"};
  ASSERT_EQ(tables.PublishWal(owner, "commit/fixed.meta", first), PublishWalResult::Committed);

  CommitRecord different = first;
  different.wal_files = {"wal/different.wal"};
  EXPECT_EQ(tables.PublishWal(owner, "commit/fixed.meta", different), PublishWalResult::CommitKeyCollision);
}

TEST(TableMetadataStoreTest, TablePrefixesAreIndependent) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableMetadataStore a("table-a", "tables/table-a", metadata);
  TableMetadataStore b("table-b", "tables/table-b", metadata);

  EXPECT_EQ(a.Initialize(a.NewTableState()), InitializeTableResult::Created);
  EXPECT_EQ(b.Initialize(b.NewTableState()), InitializeTableResult::Created);
  EXPECT_EQ(a.Load()->state.table_id, "table-a");
  EXPECT_EQ(b.Load()->state.table_id, "table-b");
}

}  // namespace dbplay
