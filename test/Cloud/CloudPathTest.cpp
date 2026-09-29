#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Cloud/CloudServices.h"
#include "Cloud/CloudTable.h"
#include "Cloud/ICloudCatalog.h"
#include "Cloud/IObjectKeyGenerator.h"
#include "Cloud/IWriterRouting.h"
#include "Cloud/SequentialProtocol.h"
#include "Execution/ScanExecutor.h"
#include "Metadata/MemMetadataStore.h"
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
  CreateFileResult CreateIfAbsent(const std::string &path, const Slice &bytes) override {
    return storage_.CreateIfAbsent(path, bytes);
  }
  bool Exists(const std::string &path) const override { return storage_.Exists(path); }
  std::vector<std::string> List(const std::string &) const override {
    throw std::logic_error("Cloud hot path must not call LIST");
  }
  void Delete(const std::string &path) override { storage_.Delete(path); }

 private:
  MemStorage storage_;
};

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
  auto data_format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  auto keys = std::make_shared<SequenceKeys>();
  auto table = std::make_shared<CloudTable>(descriptor, files, metadata, data_format);

  auto coordinator = std::make_shared<WriterCoordinator>(table->NewWriter());
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
  EXPECT_EQ(first_result.published_seq, 2u);  // seq 1 is the writer's fence

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
  EXPECT_EQ(second_result.published_seq, 3u);

  // A source already opened remains pinned to the old commit head.
  EXPECT_EQ(CollectProjected(*first_snapshot, {0}).size(), 3u);

  auto latest = queries.Open({"observability", "events"});
  ASSERT_NE(latest, nullptr);
  auto latest_rows = CollectProjected(*latest, {0});
  ASSERT_EQ(latest_rows.size(), 6u);
  EXPECT_EQ(latest_rows[0][0].AsInt64(), 0);
  EXPECT_EQ(latest_rows[3][0].AsInt64(), 100);
  EXPECT_EQ(latest_rows[5][0].AsInt64(), 200);

  const auto indexed = table->NewIndexer(keys)->Index();
  ASSERT_EQ(indexed.code, CloudIndexCode::Indexed);
  EXPECT_EQ(indexed.indexed_seq, 3u);
  EXPECT_EQ(indexed.published_seq, 3u);

  CloudImportBatch third;
  third.batch_ids = {"client-4"};
  third.chunks = {MakeChunk(300, 1)};
  ASSERT_EQ(importer.Import({"observability", "events"}, third).code, CloudImportCode::Committed);

  auto after_compact = queries.Open({"observability", "events"});
  ASSERT_NE(after_compact, nullptr);
  auto after_compact_rows = CollectProjected(*after_compact, {0});
  ASSERT_EQ(after_compact_rows.size(), 7u);
  EXPECT_EQ(after_compact_rows[0][0].AsInt64(), 0);
  EXPECT_EQ(after_compact_rows[5][0].AsInt64(), 200);
  EXPECT_EQ(after_compact_rows[6][0].AsInt64(), 300);

  const auto snapshot = table->LoadSnapshot();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->indexed_seq, 3u);
  EXPECT_EQ(snapshot->published_seq, 4u);
  EXPECT_EQ(snapshot->data_files.size(), 1u);
  EXPECT_EQ(snapshot->wal_files.size(), 1u);
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

TEST(CloudPathTest, QueryCombinesIndexedDataWithPublishedWalTail) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();

  const std::string compacted_data_file = descriptor.file_prefix + "/data/compacted-1.dbc1";
  const std::string compacted_data_manifest_key = "manifest/compacted-1.meta";

  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  auto data_format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  WriteFile(*data_format, *files, compacted_data_file, MakeChunk(0, 2));
  SequentialManifest manifest;
  manifest.table_id = descriptor.table_id;
  manifest.schema = descriptor.schema;
  manifest.covered_through_seq = 1;
  manifest.data_files = {{compacted_data_file, 1, 1, 2}};
  SequentialStateStore state_store(descriptor, metadata);
  ASSERT_EQ(state_store.WriteManifest(compacted_data_manifest_key, manifest), ConditionalWriteResult::Applied);

  SequentialWal wal;
  wal.table_id = descriptor.table_id;
  wal.seq = 2;
  wal.writer_epoch = 3;
  wal.operation_id = "batch-2";
  wal.batch_ids = {"batch-2"};
  wal.chunks = {MakeChunk(100, 2)};
  const std::string wal_bytes = EncodeSequentialWal(wal, descriptor.schema);
  ASSERT_EQ(files->CreateIfAbsent(SequentialWalKey(descriptor, 2), Slice(wal_bytes)), CreateFileResult::Created);

  SequentialState state;
  state.table_id = descriptor.table_id;
  state.state_version = 7;
  state.writer_epoch = 3;
  state.indexed_seq = 1;
  state.published_seq = 2;
  state.manifest_key = compacted_data_manifest_key;
  const std::string encoded_state = EncodeSequentialState(state);
  ASSERT_EQ(metadata->PutIfAbsent(descriptor.metadata_prefix + "/CURRENT", Slice(encoded_state), nullptr),
            ConditionalWriteResult::Applied);

  CloudTable table(descriptor, files, metadata, data_format);
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
  auto data_format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  CloudTable table(descriptor, files, metadata, data_format);

  auto old_writer = table.NewWriter();
  ASSERT_EQ(old_writer->Start().code, CloudWriterStartCode::Started);
  auto new_writer = table.NewWriter();
  ASSERT_EQ(new_writer->Start().code, CloudWriterStartCode::Started);
  EXPECT_GT(new_writer->writer_epoch(), old_writer->writer_epoch());

  CloudImportBatch batch;
  batch.batch_ids = {"stale-client"};
  batch.chunks = {MakeChunk(0, 1)};
  EXPECT_EQ(old_writer->Import(batch).code, CloudImportCode::Fenced);
  EXPECT_FALSE(old_writer->started());
}

TEST(CloudPathTest, DeferredWalIsOptionalForQueriesUntilIndexingPublishesIt) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();
  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  auto data_format = std::make_shared<NativeColumnarFileFormat>(descriptor.schema);
  auto keys = std::make_shared<SequenceKeys>();
  CloudTable table(descriptor, files, metadata, data_format);
  auto writer = table.NewWriter();
  ASSERT_EQ(writer->Start().code, CloudWriterStartCode::Started);

  CloudImportBatch batch;
  batch.batch_ids = {"deferred-1"};
  batch.chunks = {MakeChunk(10, 2)};
  batch.visibility = CloudImportBatch::Visibility::Deferred;
  const auto imported = writer->Import(batch);
  ASSERT_EQ(imported.code, CloudImportCode::Durable);
  EXPECT_EQ(imported.wal_seq, 2u);
  auto strict = table.OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  EXPECT_TRUE(CollectProjected(*strict, {0}).empty());
  auto speculative = table.OpenSnapshot(true);
  ASSERT_NE(speculative, nullptr);
  EXPECT_EQ(CollectProjected(*speculative, {0}).size(), 2u);
  EXPECT_EQ(writer->Import(batch).code, CloudImportCode::Durable);

  const auto indexed = table.NewIndexer(keys)->Index();
  ASSERT_EQ(indexed.code, CloudIndexCode::Indexed);
  EXPECT_EQ(indexed.indexed_seq, 2u);
  EXPECT_EQ(indexed.published_seq, 2u);
  strict = table.OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  EXPECT_EQ(CollectProjected(*strict, {0}).size(), 2u);
  EXPECT_EQ(writer->Import(batch).code, CloudImportCode::AlreadyCommitted);
}

TEST(CloudPathTest, ImmediateImportPublishesEarlierDeferredWal) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();
  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  CloudTable table(descriptor, files, metadata, std::make_shared<NativeColumnarFileFormat>(descriptor.schema));
  auto writer = table.NewWriter();
  ASSERT_EQ(writer->Start().code, CloudWriterStartCode::Started);
  CloudImportBatch first;
  first.batch_ids = {"first"};
  first.chunks = {MakeChunk(1, 1)};
  first.visibility = CloudImportBatch::Visibility::Deferred;
  EXPECT_EQ(writer->Import(first).code, CloudImportCode::Durable);
  CloudImportBatch second;
  second.batch_ids = {"second"};
  second.chunks = {MakeChunk(2, 1)};
  const auto imported = writer->Import(second);
  ASSERT_EQ(imported.code, CloudImportCode::Committed);
  EXPECT_EQ(imported.wal_seq, 3u);
  auto strict = table.OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  EXPECT_EQ(CollectProjected(*strict, {0}).size(), 2u);
}

TEST(CloudPathTest, ProbeWindowGrowsWhenQueriesReachItsLimit) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();
  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  CloudTable table(descriptor, files, metadata, std::make_shared<NativeColumnarFileFormat>(descriptor.schema));
  auto writer = table.NewWriter();
  ASSERT_EQ(writer->Start().code, CloudWriterStartCode::Started);
  for (int i = 0; i < 10; ++i) {
    CloudImportBatch batch;
    batch.batch_ids = {"deferred-" + std::to_string(i)};
    batch.chunks = {MakeChunk(i, 1)};
    batch.visibility = CloudImportBatch::Visibility::Deferred;
    ASSERT_EQ(writer->Import(batch).code, CloudImportCode::Durable);
  }
  const auto first = table.LoadSnapshot(true);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->wal_chunks.size(), 7u);  // one of the first eight slots is a fence
  EXPECT_TRUE(first->probe_limit_reached);
  const auto second = table.LoadSnapshot(true);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->wal_chunks.size(), 10u);
  EXPECT_EQ(second->published_seq, 0u);
}

TEST(CloudPathTest, SequentialWalCarriesIdentityAndRejectsCorruption) {
  SequentialWal wal;
  wal.table_id = "tenant-a/events";
  wal.seq = 42;
  wal.writer_epoch = 7;
  wal.operation_id = "request-42";
  wal.batch_ids = {"batch-42"};
  wal.chunks = {MakeChunk(100, 2)};
  std::string bytes = EncodeSequentialWal(wal, TestSchema());
  const auto decoded = DecodeSequentialWal(Slice(bytes), TestSchema());
  EXPECT_EQ(decoded.seq, 42u);
  EXPECT_EQ(decoded.writer_epoch, 7u);
  EXPECT_EQ(decoded.batch_ids, wal.batch_ids);
  EXPECT_EQ(decoded.chunks[0].row_count, 2u);
  bytes[bytes.size() - 9] ^= 1;
  EXPECT_THROW(DecodeSequentialWal(Slice(bytes), TestSchema()), std::invalid_argument);
}

TEST(CloudPathTest, SequentialStateFormattingIncludesVisibilityAndMetadataVersion) {
  SequentialState state;
  state.table_id = "tenant-a/events";
  state.state_version = 7;
  state.writer_epoch = 3;
  state.indexed_seq = 4;
  state.published_seq = 6;
  state.manifest_key = "manifest/7.meta";
  const VersionedSequentialState versioned{state, MetadataVersion("etag-7")};
  const auto text = versioned.toString();
  EXPECT_NE(text.find("table_id=tenant-a/events"), std::string::npos);
  EXPECT_NE(text.find("indexed_seq=4"), std::string::npos);
  EXPECT_NE(text.find("published_seq=6"), std::string::npos);
  EXPECT_NE(text.find("manifest_key=manifest/7.meta"), std::string::npos);
  EXPECT_NE(text.find("version=MetadataVersion{opaque=etag-7}"), std::string::npos);

  SequentialManifest manifest;
  manifest.table_id = state.table_id;
  manifest.covered_through_seq = state.indexed_seq;
  manifest.schema = TestSchema();
  manifest.data_files.push_back({"files/data/1.dbc1", 1, 4, 2});
  manifest.batch_index_files.push_back("dedup/1.idx");
  EXPECT_NE(manifest.toString().find("DataFile{path=files/data/1.dbc1"), std::string::npos);
  EXPECT_NE(manifest.toString().find("dedup/1.idx"), std::string::npos);

  SequentialWal wal;
  wal.table_id = state.table_id;
  wal.seq = 6;
  wal.writer_epoch = state.writer_epoch;
  wal.operation_id = "batch-6";
  wal.batch_ids = {"batch-6"};
  wal.chunks = {MakeChunk(10, 2)};
  EXPECT_NE(wal.toString().find("row_count=2"), std::string::npos);
}

TEST(CloudPathTest, IndexedPublishPreservesConcurrentWriterWatermark) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();
  auto metadata = std::make_shared<MemMetadataStore>();
  SequentialStateStore store(descriptor, metadata);
  ASSERT_EQ(store.Initialize(), ConditionalWriteResult::Applied);
  const auto initial = store.Load();
  ASSERT_TRUE(initial.has_value());

  VersionedSequentialState writer_published;
  ASSERT_EQ(store.Publish(*initial, SequentialPublishUpdate{3, std::nullopt}, &writer_published),
            ConditionalWriteResult::Applied);
  EXPECT_EQ(writer_published.state.published_seq, 3u);

  SequentialManifest manifest;
  manifest.table_id = descriptor.table_id;
  manifest.covered_through_seq = 2;
  manifest.schema = descriptor.schema;
  ASSERT_EQ(store.WriteManifest("manifest/indexed-2.meta", manifest), ConditionalWriteResult::Applied);
  EXPECT_EQ(store.Publish(*initial, SequentialPublishUpdate{2, "manifest/indexed-2.meta"}),
            ConditionalWriteResult::PreconditionFailed);

  VersionedSequentialState indexed;
  ASSERT_EQ(store.Publish(writer_published, SequentialPublishUpdate{2, "manifest/indexed-2.meta"}, &indexed),
            ConditionalWriteResult::Applied);
  EXPECT_EQ(indexed.state.indexed_seq, 2u);
  EXPECT_EQ(indexed.state.published_seq, 3u);
  EXPECT_EQ(indexed.state.manifest_key, "manifest/indexed-2.meta");
  EXPECT_EQ(store.LoadManifest(indexed.state).covered_through_seq, 2u);
}

TEST(CloudPathTest, DeferredWalSurvivesWriterReplacement) {
  TableDescriptor descriptor;
  descriptor.table_id = "tenant-a/events";
  descriptor.metadata_prefix = "tables/tenant-a/events/metadata";
  descriptor.file_prefix = "tables/tenant-a/events/files";
  descriptor.schema = TestSchema();
  auto files = std::make_shared<NoListStorage>();
  auto metadata = std::make_shared<MemMetadataStore>();
  auto keys = std::make_shared<SequenceKeys>();
  CloudTable table(descriptor, files, metadata, std::make_shared<NativeColumnarFileFormat>(descriptor.schema));
  auto old_writer = table.NewWriter();
  ASSERT_EQ(old_writer->Start().code, CloudWriterStartCode::Started);
  CloudImportBatch batch;
  batch.batch_ids = {"before-failover"};
  batch.chunks = {MakeChunk(25, 1)};
  batch.visibility = CloudImportBatch::Visibility::Deferred;
  ASSERT_EQ(old_writer->Import(batch).code, CloudImportCode::Durable);

  auto replacement = table.NewWriter();
  ASSERT_EQ(replacement->Start().code, CloudWriterStartCode::Started);
  EXPECT_EQ(old_writer->Import(batch).code, CloudImportCode::Fenced);
  EXPECT_TRUE(CollectProjected(*table.OpenSnapshot(false), {0}).empty());
  const auto indexed = table.NewIndexer(keys)->Index();
  ASSERT_EQ(indexed.code, CloudIndexCode::Indexed);
  EXPECT_EQ(indexed.indexed_seq, 3u);  // fence, data, replacement fence
  auto strict = table.OpenSnapshot(false);
  ASSERT_NE(strict, nullptr);
  EXPECT_EQ(CollectProjected(*strict, {0}).size(), 1u);
  EXPECT_EQ(replacement->Import(batch).code, CloudImportCode::AlreadyCommitted);
}

}  // namespace dbplay
