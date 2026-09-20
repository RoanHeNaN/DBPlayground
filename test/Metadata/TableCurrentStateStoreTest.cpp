#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Metadata/MemMetadataStore.h"
#include "Metadata/TableCurrentStateStore.h"
#include "Metadata/TableMetadataCodec.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

CurrentTableState InitializedCurrentState(TableCurrentStateStore *tables) {
  VersionedCurrentTableState created;
  EXPECT_EQ(tables->Initialize(tables->NewCurrentTableState(), &created), InitializeTableResult::Created);
  return created.current_state;
}

}  // namespace

TEST(TableMetadataCodecTest, RoundTripsCurrentTableState) {
  CurrentTableState state;
  state.table_id = "tenant-a/events";
  state.current_state_version = 42;
  state.writer_epoch = 7;
  state.committed_cursor = 1042;
  state.compacted_cursor = 1038;
  state.compacted_data_manifest_key = "manifest/compacted-1038.meta";
  state.latest_commit_key = "commit/1042-uuid.meta";

  const std::string encoded = TableMetadataCodec::EncodeCurrentTableState(state);
  EXPECT_EQ(TableMetadataCodec::DecodeCurrentTableState(Slice(encoded)), state);
}

TEST(TableMetadataCodecTest, RoundTripsCommitRecord) {
  CommitRecord record;
  record.table_id = "tenant-a/events";
  record.writer_epoch = 7;
  record.first_cursor = 1039;
  record.last_cursor = 1042;
  record.parent_commit_key = "commit/1038-parent.meta";
  record.wal_files = {"wal/1039-1040.wal", "wal/1041-1042.wal"};
  record.batch_ids = {"client-batch-a", "client-batch-b"};

  const std::string encoded = TableMetadataCodec::EncodeCommitRecord(record);
  EXPECT_EQ(TableMetadataCodec::DecodeCommitRecord(Slice(encoded)), record);
}

TEST(TableMetadataCodecTest, RejectsCorruptionAndInvalidState) {
  CurrentTableState state;
  state.table_id = "table-a";
  std::string encoded = TableMetadataCodec::EncodeCurrentTableState(state);

  std::string truncated = encoded.substr(0, encoded.size() - 1);
  EXPECT_THROW(TableMetadataCodec::DecodeCurrentTableState(Slice(truncated)), std::invalid_argument);

  encoded.push_back('x');
  EXPECT_THROW(TableMetadataCodec::DecodeCurrentTableState(Slice(encoded)), std::invalid_argument);

  state.compacted_cursor = 2;
  state.committed_cursor = 1;
  EXPECT_THROW(TableMetadataCodec::EncodeCurrentTableState(state), std::invalid_argument);
}

TEST(TableMetadataCodecTest, RoundTripsCompactedDataManifest) {
  CompactedDataManifest manifest;
  manifest.table_id = "tenant-a/events";
  manifest.compacted_cursor = 1038;
  manifest.compacted_data_files = {"data/compacted-a.dbc1", "data/compacted-b.dbc1"};

  const std::string encoded = TableMetadataCodec::EncodeCompactedDataManifest(manifest);
  EXPECT_EQ(TableMetadataCodec::DecodeCompactedDataManifest(Slice(encoded)), manifest);
}

TEST(TableCurrentStateStoreTest, InitializesAndLoadsCurrentWithoutList) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a/", metadata);
  EXPECT_EQ(tables.current_key(), "tables/table-a/CURRENT");
  EXPECT_FALSE(tables.Load().has_value());

  VersionedCurrentTableState created;
  const CurrentTableState initial = tables.NewCurrentTableState();
  EXPECT_EQ(tables.Initialize(initial, &created), InitializeTableResult::Created);
  EXPECT_EQ(tables.Initialize(initial), InitializeTableResult::AlreadyExists);

  const auto loaded = tables.Load();
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->current_state, initial);
  EXPECT_EQ(loaded->current_metadata_version, created.current_metadata_version);
}

TEST(TableCurrentStateStoreTest, PublishesWithCurrentVersionAndRejectsStaleWriter) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);
  const VersionedCurrentTableState initial = *tables.Load();

  VersionedCurrentTableState owner;
  ASSERT_EQ(tables.AcquireWriter(initial, &owner), AcquireWriterResult::Acquired);
  EXPECT_EQ(tables.AcquireWriter(initial), AcquireWriterResult::StaleVersion);

  CommitRecord record;
  record.table_id = "table-a";
  record.writer_epoch = owner.current_state.writer_epoch;
  record.first_cursor = 1;
  record.last_cursor = 1;
  record.wal_files = {"wal/1.wal"};
  record.batch_ids = {"batch-1"};
  VersionedCurrentTableState committed;
  ASSERT_EQ(tables.PublishWal(owner, "commit/1.meta", record, &committed), PublishWalResult::Committed);
  EXPECT_EQ(committed.current_state.committed_cursor, 1u);
  EXPECT_EQ(committed.current_state.latest_commit_key, "commit/1.meta");
  ASSERT_TRUE(tables.LoadCommitRecord("commit/1.meta").has_value());
  EXPECT_EQ(*tables.LoadCommitRecord("commit/1.meta"), record);

  VersionedCurrentTableState recovered;
  EXPECT_EQ(tables.PublishWal(owner, "commit/1.meta", record, &recovered), PublishWalResult::AlreadyCommitted);
  EXPECT_EQ(recovered.current_state, committed.current_state);
}

TEST(TableCurrentStateStoreTest, RejectsInvalidCommitBeforeCas) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);
  VersionedCurrentTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  CommitRecord invalid;
  invalid.table_id = "table-a";
  invalid.writer_epoch = owner.current_state.writer_epoch;
  invalid.first_cursor = 2;
  invalid.last_cursor = 2;
  invalid.wal_files = {"wal/2.wal"};
  EXPECT_EQ(tables.PublishWal(owner, "commit/2.meta", invalid), PublishWalResult::InvalidCommit);

  invalid.first_cursor = 1;
  invalid.last_cursor = 1;
  invalid.table_id = "table-b";
  EXPECT_EQ(tables.PublishWal(owner, "commit/1.meta", invalid), PublishWalResult::InvalidCommit);
  EXPECT_FALSE(tables.LoadCommitRecord("commit/1.meta").has_value());
  EXPECT_EQ(tables.Load()->current_state, owner.current_state);
}

TEST(TableCurrentStateStoreTest, ConcurrentPublishHasExactlyOneWinner) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);
  VersionedCurrentTableState owner;
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
      record.writer_epoch = owner.current_state.writer_epoch;
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
  EXPECT_EQ(tables.Load()->current_state.committed_cursor, 1u);
}

TEST(TableCurrentStateStoreTest, OldWriterIsFencedAfterNewEpochIsAcquired) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);

  VersionedCurrentTableState old_writer;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &old_writer), AcquireWriterResult::Acquired);
  VersionedCurrentTableState new_writer;
  ASSERT_EQ(tables.AcquireWriter(old_writer, &new_writer), AcquireWriterResult::Acquired);

  CommitRecord stale_record;
  stale_record.table_id = "table-a";
  stale_record.writer_epoch = old_writer.current_state.writer_epoch;
  stale_record.first_cursor = 1;
  stale_record.last_cursor = 1;
  stale_record.wal_files = {"wal/stale.wal"};
  stale_record.batch_ids = {"stale-batch"};
  EXPECT_EQ(tables.PublishWal(old_writer, "commit/stale.meta", stale_record), PublishWalResult::Fenced);
  EXPECT_EQ(tables.Load()->current_state.writer_epoch, new_writer.current_state.writer_epoch);
}

TEST(TableCurrentStateStoreTest, CommittedResponseCanBeRecoveredAfterEpochAdvances) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);

  VersionedCurrentTableState writer;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &writer), AcquireWriterResult::Acquired);
  CommitRecord record;
  record.table_id = "table-a";
  record.writer_epoch = writer.current_state.writer_epoch;
  record.first_cursor = 1;
  record.last_cursor = 1;
  record.wal_files = {"wal/1.wal"};
  record.batch_ids = {"batch-1"};
  ASSERT_EQ(tables.PublishWal(writer, "commit/1.meta", record), PublishWalResult::Committed);

  VersionedCurrentTableState next_writer;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &next_writer), AcquireWriterResult::Acquired);
  EXPECT_GT(next_writer.current_state.writer_epoch, writer.current_state.writer_epoch);

  VersionedCurrentTableState recovered;
  EXPECT_EQ(tables.PublishWal(writer, "commit/1.meta", record, &recovered), PublishWalResult::AlreadyCommitted);
  EXPECT_EQ(recovered.current_state.writer_epoch, next_writer.current_state.writer_epoch);
}

TEST(TableCurrentStateStoreTest, ReusedCommitKeyMustContainIdenticalRecord) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);
  VersionedCurrentTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  CommitRecord first;
  first.table_id = "table-a";
  first.writer_epoch = owner.current_state.writer_epoch;
  first.first_cursor = 1;
  first.last_cursor = 1;
  first.wal_files = {"wal/first.wal"};
  first.batch_ids = {"batch-1"};
  ASSERT_EQ(tables.PublishWal(owner, "commit/fixed.meta", first), PublishWalResult::Committed);

  CommitRecord different = first;
  different.wal_files = {"wal/different.wal"};
  EXPECT_EQ(tables.PublishWal(owner, "commit/fixed.meta", different), PublishWalResult::CommitKeyCollision);
}

TEST(TableCurrentStateStoreTest, TablePrefixesAreIndependent) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore a("table-a", "tables/table-a", metadata);
  TableCurrentStateStore b("table-b", "tables/table-b", metadata);

  EXPECT_EQ(a.Initialize(a.NewCurrentTableState()), InitializeTableResult::Created);
  EXPECT_EQ(b.Initialize(b.NewCurrentTableState()), InitializeTableResult::Created);
  EXPECT_EQ(a.Load()->current_state.table_id, "table-a");
  EXPECT_EQ(b.Load()->current_state.table_id, "table-b");
}

TEST(TableCurrentStateStoreTest, PublishesCompactionWithoutChangingCommitHead) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);
  VersionedCurrentTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  CommitRecord record;
  record.table_id = "table-a";
  record.writer_epoch = owner.current_state.writer_epoch;
  record.first_cursor = 1;
  record.last_cursor = 1;
  record.wal_files = {"wal/1.wal"};
  record.batch_ids = {"batch-1"};
  VersionedCurrentTableState committed;
  ASSERT_EQ(tables.PublishWal(owner, "commit/1.meta", record, &committed), PublishWalResult::Committed);

  CompactedDataManifest manifest;
  manifest.table_id = "table-a";
  manifest.compacted_cursor = 1;
  manifest.compacted_data_files = {"data/1.dbc1"};
  VersionedCurrentTableState compacted;
  ASSERT_EQ(tables.PublishCompaction(committed, "manifest/1.meta", manifest, &compacted),
            PublishCompactionResult::Published);
  EXPECT_EQ(compacted.current_state.compacted_cursor, 1u);
  EXPECT_EQ(compacted.current_state.committed_cursor, 1u);
  EXPECT_EQ(compacted.current_state.latest_commit_key, "commit/1.meta");
  EXPECT_EQ(compacted.current_state.writer_epoch, committed.current_state.writer_epoch);
  EXPECT_EQ(compacted.current_state.compacted_data_manifest_key, "manifest/1.meta");
  ASSERT_TRUE(tables.LoadCompactedDataManifest("manifest/1.meta").has_value());
  EXPECT_EQ(*tables.LoadCompactedDataManifest("manifest/1.meta"), manifest);

  EXPECT_EQ(tables.PublishCompaction(committed, "manifest/1.meta", manifest),
            PublishCompactionResult::AlreadyPublished);
}

TEST(TableCurrentStateStoreTest, CompactionDoesNotFenceAConcurrentWriterAdvance) {
  auto metadata = std::make_shared<MemMetadataStore>();
  TableCurrentStateStore tables("table-a", "tables/table-a", metadata);
  ASSERT_EQ(tables.Initialize(tables.NewCurrentTableState()), InitializeTableResult::Created);
  VersionedCurrentTableState owner;
  ASSERT_EQ(tables.AcquireWriter(*tables.Load(), &owner), AcquireWriterResult::Acquired);

  CommitRecord first;
  first.table_id = "table-a";
  first.writer_epoch = owner.current_state.writer_epoch;
  first.first_cursor = 1;
  first.last_cursor = 1;
  first.wal_files = {"wal/1.wal"};
  first.batch_ids = {"batch-1"};
  VersionedCurrentTableState after_first;
  ASSERT_EQ(tables.PublishWal(owner, "commit/1.meta", first, &after_first), PublishWalResult::Committed);

  CommitRecord second = first;
  second.first_cursor = 2;
  second.last_cursor = 2;
  second.parent_commit_key = "commit/1.meta";
  second.wal_files = {"wal/2.wal"};
  second.batch_ids = {"batch-2"};
  VersionedCurrentTableState after_second;
  ASSERT_EQ(tables.PublishWal(after_first, "commit/2.meta", second, &after_second), PublishWalResult::Committed);

  CompactedDataManifest manifest;
  manifest.table_id = "table-a";
  manifest.compacted_cursor = 1;
  manifest.compacted_data_files = {"data/1.dbc1"};
  EXPECT_EQ(tables.PublishCompaction(after_first, "manifest/1.meta", manifest), PublishCompactionResult::StaleVersion);

  VersionedCurrentTableState compacted;
  ASSERT_EQ(tables.PublishCompaction(after_second, "manifest/1.meta", manifest, &compacted),
            PublishCompactionResult::Published);
  EXPECT_EQ(compacted.current_state.compacted_cursor, 1u);
  EXPECT_EQ(compacted.current_state.committed_cursor, 2u);
  EXPECT_EQ(compacted.current_state.latest_commit_key, "commit/2.meta");
}

}  // namespace dbplay
