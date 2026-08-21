#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Cloud/CloudServices.h"
#include "Cloud/CloudTable.h"
#include "Cloud/IBatchCommitResolver.h"
#include "Cloud/ICloudCatalog.h"
#include "Cloud/IManifestStore.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/IWriterRouting.h"
#include "Execution/ScanExecutor.h"
#include "Metadata/MemMetadataStore.h"
#include "Metadata/TableMetadataCodec.h"
#include "Storage/File/IStorage.h"
#include "Storage/File/MemStorage.h"
#include "Table/Format/NativeColumnarFileFormat.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

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

void WriteFile(IFileFormat &format, IStorage &storage, const std::string &path, const Chunk &chunk) {
  auto writer = format.OpenWriter(storage, path);
  writer->Write(chunk);
  writer->Close();
}

class NoListStorage : public IStorage {
 public:
  std::unique_ptr<IInputFile> OpenInput(const std::string &path) override { return storage_.OpenInput(path); }
  std::unique_ptr<IOutputStream> OpenOutput(const std::string &path) override { return storage_.OpenOutput(path); }
  bool Exists(const std::string &path) const override { return storage_.Exists(path); }
  std::vector<std::string> List(const std::string &) const override {
    throw std::logic_error("Cloud hot path must not call LIST");
  }
  void Delete(const std::string &path) override { storage_.Delete(path); }

 private:
  MemStorage storage_;
};

class EmptyManifestStore : public IManifestStore {
 public:
  std::optional<BaseManifest> Load(const TableDescriptor &, const std::string &) const override { return std::nullopt; }
};

class FixedManifestStore : public IManifestStore {
 public:
  FixedManifestStore(std::string key, BaseManifest manifest) : key_(std::move(key)), manifest_(std::move(manifest)) {}

  std::optional<BaseManifest> Load(const TableDescriptor &, const std::string &key) const override {
    return key == key_ ? std::optional<BaseManifest>(manifest_) : std::nullopt;
  }

 private:
  std::string key_;
  BaseManifest manifest_;
};

class SequenceKeys : public IObjectKeyGenerator {
 public:
  std::string NewWalKey(const std::string &) override { return "wal/" + std::to_string(next_wal_++) + ".dbc1"; }
  std::string NewCommitKey(const std::string &) override {
    return "commit/" + std::to_string(next_commit_++) + ".meta";
  }

 private:
  std::atomic<uint64_t> next_wal_{1};
  std::atomic<uint64_t> next_commit_{1};
};

class NeverCommittedBatches : public IBatchCommitResolver {
 public:
  BatchCommitStatus Lookup(const TableDescriptor &, const VersionedTableState &,
                           const std::vector<std::string> &) const override {
    return BatchCommitStatus::NotCommitted;
  }
};

class StaticCatalog : public ICloudCatalog {
 public:
  explicit StaticCatalog(TableDescriptor table) : table_(std::move(table)) {}

  std::optional<TableDescriptor> Resolve(const TableName &name) const override {
    if (name.database == "observability" && name.table == "events") {
      return table_;
    }
    return std::nullopt;
  }

 private:
  TableDescriptor table_;
};

class StaticRouter : public IWriterRouter {
 public:
  std::optional<WriterTarget> Route(const std::string &table_id) const override {
    if (table_id != "tenant-a/events") {
      return std::nullopt;
    }
    return WriterTarget{"writer-1", "in-process"};
  }
};

class WriterCoordinator : public ITableWriteCoordinator {
 public:
  explicit WriterCoordinator(std::unique_ptr<CloudTableWriter> writer) : writer_(std::move(writer)) {}

  CloudWriterStartResult Start() { return writer_->Start(); }
  CloudImportResult Import(const CloudImportBatch &batch) override { return writer_->Import(batch); }

 private:
  std::unique_ptr<CloudTableWriter> writer_;
};

class SingleCoordinatorProvider : public IWriteCoordinatorProvider {
 public:
  SingleCoordinatorProvider(std::string table_id, std::shared_ptr<ITableWriteCoordinator> coordinator)
      : table_id_(std::move(table_id)), coordinator_(std::move(coordinator)) {}

  std::shared_ptr<ITableWriteCoordinator> Get(const std::string &table_id) override {
    return table_id == table_id_ ? coordinator_ : nullptr;
  }

 private:
  std::string table_id_;
  std::shared_ptr<ITableWriteCoordinator> coordinator_;
};

class LocalWriterTransport : public IWriterTransport {
 public:
  explicit LocalWriterTransport(std::shared_ptr<CloudWriterService> service) : service_(std::move(service)) {}

  CloudImportResult Import(const WriterTarget &, const std::string &table_id, const CloudImportBatch &batch) override {
    return service_->Import(table_id, batch);
  }

 private:
  std::shared_ptr<CloudWriterService> service_;
};

class SingleTableProvider : public ICloudTableProvider {
 public:
  explicit SingleTableProvider(std::shared_ptr<CloudTable> table) : table_(std::move(table)) {}

  std::shared_ptr<CloudTable> Get(const TableDescriptor &descriptor) override {
    return descriptor.table_id == table_->descriptor().table_id ? table_ : nullptr;
  }

 private:
  std::shared_ptr<CloudTable> table_;
};

}  // namespace

TEST(CloudPathTest, ImportIsImmediatelyVisibleWithoutListAndSnapshotsAreImmutable) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();

  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  auto manifests = std::make_shared<EmptyManifestStore>();
  auto format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  auto keys = std::make_shared<SequenceKeys>();
  auto batches = std::make_shared<NeverCommittedBatches>();
  auto table = std::make_shared<CloudTable>(descriptor, files, metadata, manifests, format, format);

  auto coordinator = std::make_shared<WriterCoordinator>(table->NewWriter(keys, batches));
  ASSERT_EQ(coordinator->Start().code, CloudWriterStartCode::Started);
  auto coordinator_provider = std::make_shared<SingleCoordinatorProvider>(descriptor.table_id, coordinator);
  auto writer_service = std::make_shared<CloudWriterService>(coordinator_provider);

  auto catalog = std::make_shared<StaticCatalog>(descriptor);
  auto router = std::make_shared<StaticRouter>();
  auto transport = std::make_shared<LocalWriterTransport>(writer_service);
  CloudImportService importer(catalog, router, transport);
  CloudQueryService queries(catalog, std::make_shared<SingleTableProvider>(table));

  CloudImportBatch first;
  first.batch_ids = {"client-1"};
  first.chunks = {MakeChunk(0, 3)};
  const auto first_result = importer.Import({"observability", "events"}, first);
  ASSERT_EQ(first_result.code, CloudImportCode::Committed);
  EXPECT_EQ(first_result.committed_cursor, 1u);

  auto first_snapshot = queries.Open({"observability", "events"});
  ASSERT_NE(first_snapshot, nullptr);
  auto first_rows = CollectProjected(*first_snapshot, {0, 1});
  ASSERT_EQ(first_rows.size(), 3u);
  EXPECT_EQ(first_rows[2][0].AsInt64(), 2);
  EXPECT_EQ(first_rows[2][1].AsString(), "event-2");

  CloudImportBatch second;
  second.batch_ids = {"client-2", "client-3"};
  second.chunks = {MakeChunk(100, 2), MakeChunk(200, 1)};
  const auto second_result = importer.Import({"observability", "events"}, second);
  ASSERT_EQ(second_result.code, CloudImportCode::Committed);
  EXPECT_EQ(second_result.committed_cursor, 2u);

  // A source already opened remains pinned to the old commit head.
  EXPECT_EQ(CollectProjected(*first_snapshot, {0}).size(), 3u);

  auto latest = queries.Open({"observability", "events"});
  ASSERT_NE(latest, nullptr);
  auto latest_rows = CollectProjected(*latest, {0});
  ASSERT_EQ(latest_rows.size(), 6u);
  EXPECT_EQ(latest_rows[0][0].AsInt64(), 0);
  EXPECT_EQ(latest_rows[3][0].AsInt64(), 100);
  EXPECT_EQ(latest_rows[5][0].AsInt64(), 200);
}

TEST(CloudPathTest, MissingCatalogEntriesFailBeforeRoutingOrOpeningStorage) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();

  auto catalog = std::make_shared<StaticCatalog>(descriptor);
  auto router = std::make_shared<StaticRouter>();

  class RejectingTransport : public IWriterTransport {
   public:
    CloudImportResult Import(const WriterTarget &, const std::string &, const CloudImportBatch &) override {
      throw std::logic_error("transport must not be called");
    }
  };
  class EmptyTableProvider : public ICloudTableProvider {
   public:
    std::shared_ptr<CloudTable> Get(const TableDescriptor &) override {
      throw std::logic_error("table provider must not be called");
    }
  };

  CloudImportService importer(catalog, router, std::make_shared<RejectingTransport>());
  CloudQueryService queries(catalog, std::make_shared<EmptyTableProvider>());
  CloudImportBatch batch;
  EXPECT_EQ(importer.Import({"missing", "table"}, batch).code, CloudImportCode::TableNotFound);
  EXPECT_EQ(queries.Open({"missing", "table"}), nullptr);
}

TEST(CloudPathTest, QueryCombinesCompactedBaseWithCommittedWalTail) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();

  const std::string base_file = descriptor.file_prefix + "/data/base-1.dbc1";
  const std::string wal_file = descriptor.file_prefix + "/wal/2.dbc1";
  const std::string manifest_key = "manifest/base-1.meta";
  const std::string commit_key = "commit/2.meta";

  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  auto format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  WriteFile(*format, *files, base_file, MakeChunk(0, 2));
  WriteFile(*format, *files, wal_file, MakeChunk(100, 2));

  BaseManifest manifest;
  manifest.table_id = descriptor.table_id;
  manifest.indexed_cursor = 1;
  manifest.data_files = {base_file};
  auto manifests = std::make_shared<FixedManifestStore>(manifest_key, manifest);

  CommitRecord commit;
  commit.table_id = descriptor.table_id;
  commit.writer_epoch = 3;
  commit.first_cursor = 2;
  commit.last_cursor = 2;
  commit.wal_files = {wal_file};
  commit.batch_ids = {"batch-2"};
  const std::string encoded_commit = TableMetadataCodec::EncodeCommitRecord(commit);
  ASSERT_EQ(metadata->PutIfAbsent(descriptor.metadata_prefix + "/" + commit_key, Slice(encoded_commit), nullptr),
            ConditionalWriteResult::Applied);

  TableState state;
  state.table_id = descriptor.table_id;
  state.state_version = 7;
  state.writer_epoch = 3;
  state.indexed_cursor = 1;
  state.committed_cursor = 2;
  state.base_manifest = manifest_key;
  state.commit_head = commit_key;
  const std::string encoded_state = TableMetadataCodec::EncodeTableState(state);
  ASSERT_EQ(metadata->PutIfAbsent(descriptor.metadata_prefix + "/CURRENT", Slice(encoded_state), nullptr),
            ConditionalWriteResult::Applied);

  CloudTable table(descriptor, files, metadata, manifests, format, format);
  auto source = table.OpenSnapshot();
  ASSERT_NE(source, nullptr);
  auto rows = CollectProjected(*source, {0});
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0][0].AsInt64(), 0);
  EXPECT_EQ(rows[1][0].AsInt64(), 1);
  EXPECT_EQ(rows[2][0].AsInt64(), 100);
  EXPECT_EQ(rows[3][0].AsInt64(), 101);
}

TEST(CloudPathTest, OldCloudWriterStopsAfterAnotherWriterAcquiresTheTable) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();

  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  auto manifests = std::make_shared<EmptyManifestStore>();
  auto format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  auto batches = std::make_shared<NeverCommittedBatches>();
  CloudTable table(descriptor, files, metadata, manifests, format, format);

  auto old_writer = table.NewWriter(std::make_shared<SequenceKeys>(), batches);
  ASSERT_EQ(old_writer->Start().code, CloudWriterStartCode::Started);
  auto new_writer = table.NewWriter(std::make_shared<SequenceKeys>(), batches);
  ASSERT_EQ(new_writer->Start().code, CloudWriterStartCode::Started);
  EXPECT_GT(new_writer->writer_epoch(), old_writer->writer_epoch());

  CloudImportBatch batch;
  batch.batch_ids = {"stale-client"};
  batch.chunks = {MakeChunk(0, 1)};
  EXPECT_EQ(old_writer->Import(batch).code, CloudImportCode::Fenced);
  EXPECT_FALSE(old_writer->started());
}

}  // namespace dbplay
