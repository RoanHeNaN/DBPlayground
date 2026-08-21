# Cloud Table Layering: Catalog, Metadata, Files, and Cache

> Status: **分阶段实现中**。`IMetadataStore`、内存 CAS、`TableState` / `CommitRecord` codec、
> `TableMetadataStore`、WAL publish 编排、snapshot loader 与 `CloudTableSource` 框架已经落地；
> 生产级 WAL/manifest/S3/Catalog/router/RPC adapter 尚未实现。完整调用路径见
> [`CloudImportQueryFramework.md`](CloudImportQueryFramework.md)。
>
> 本文在 [`HighCardinalityMultiTenantObservability.md`](HighCardinalityMultiTenantObservability.md)
> 的 workload 与一致性模型之上，确定 Cloud Table 的软件分层。核心原则是：
> **按上层需要的语义划分接口，而不是按 S3 恰好提供的 API 划分接口。**

---

## 1. 问题

S3 同时提供对象读写、range GET、LIST、ETag 和条件写。如果直接照搬这些 API，容易把几种不同
职责塞进一个 `CloudStorage`：

- SQL 名称解析；
- Table CURRENT / manifest / writer epoch；
- DBC1 与 WAL 的 byte-range IO；
- RAM/NVMe cache；
- S3 CAS。

这种设计会让物理 IO 层理解 Table、snapshot 和 Catalog，使 `IFileFormat × IStorage` 的正交性
失效。正确的分层依据应当是寻址模型与一致性语义。

---

## 2. 总体分层

```text
SQL Layer
   │
   ▼
Catalog                         name → TableDescriptor
   │
   ▼
CloudTable / TableHandle        long-lived per-table handle
   │
   ├── TableMetadataStore       CURRENT / commit / epoch / rebase protocol
   │       │
   │       ▼
   │   IMetadataStore           versioned key/value + CAS
   │       └── S3MetadataStore  ETag / If-Match adapter
   │
   └── OpenSnapshot()
           │
           ▼
       CloudTableSource         immutable query snapshot
           ├── TableSource      base DBC1 files
           ├── WalTableSource   committed WAL tail
           └── IStorage         file locator + byte-range IO
                   │
                   ▼
              CachedStorage
               ├── IFileCache   RAM / NVMe
               └── S3FileStorage
```

metadata path 与 data path 是两条独立路径：

```text
TableMetadataStore → IMetadataStore → S3
TableSource         → IStorage       → cache → S3
```

它们可以共享 S3 client 和 bucket，但不共享抽象接口。

---

## 3. Catalog：名字到 Table

Catalog 回答：

```text
(database_name, table_name) → TableDescriptor
```

它负责 CREATE/DROP、名称解析、权限和 Table properties。Catalog 不读取 DBC1，也不属于
`CloudStorage`。如果第一阶段可以由 SQL 名称确定性生成 `table_id/prefix`，Catalog 可以暂缓实现，
但其逻辑位置仍在 CloudTable 之上。

```cpp
struct TableDescriptor {
  TableId table_id;
  std::string metadata_prefix;
  // engine type, properties, ownership, ...
};
```

---

## 4. CloudTable：长生命周期 Table handle

node 可以按 `table_id` 缓存 `CloudTable`，但不应长期缓存一个不断修改文件列表的 `TableSource`。

```cpp
class CloudTable {
 public:
  std::unique_ptr<ITableSource> OpenSnapshot();

 private:
  TableDescriptor descriptor_;
  std::shared_ptr<IStorage> files_;
  std::shared_ptr<IMetadataStore> metadata_;
  TableMetadataStore table_metadata_;
};
```

`CloudTable` 是 composition root：它同时引用 metadata 和 file storage，但不把两种接口混合。

---

## 5. TableMetadataStore：Table 领域协议

`TableMetadataStore` 理解：

- CURRENT；
- immutable CommitRecord；
- writer epoch / fencing；
- committed/indexed cursor；
- manifest；
- batch-id 幂等；
- CAS 失败后的分类与 rebase；
- compaction 发布。

逻辑接口示意：

```cpp
class TableMetadataStore {
 public:
  TableSnapshot LoadSnapshot(TableId table);
  AcquireWriterResult AcquireWriter(TableId table, WriterId writer);
  CommitResult PublishWal(TableId table, WriterEpoch epoch,
                          BatchId batch, WalFileRef wal);
  CompactionResult PublishCompaction(TableId table,
                                     const CompactionUpdate &update);
};
```

它不执行 DBC1 range read，也不暴露 S3 ETag。它把 Table 协议建立在 `IMetadataStore` 的单 key
原子操作之上。

---

## 6. IMetadataStore：版本化 KV + CAS

metadata 的寻址模型是 key/value，不是 byte-range file：

```cpp
struct VersionedValue {
  std::string value;
  MetadataVersion version;
};

class IMetadataStore {
 public:
  virtual std::optional<VersionedValue> Get(const std::string &key) const = 0;
  virtual ConditionalWriteResult PutIfAbsent(
      const std::string &key, const Slice &value,
      MetadataVersion *new_version) = 0;
  virtual ConditionalWriteResult CompareExchange(
      const std::string &key, const MetadataVersion &expected,
      const Slice &value, MetadataVersion *new_version) = 0;
};
```

实现：

- `MemMetadataStore`：线程安全、单调 generation，用于协议与故障注入测试；
- `S3MetadataStore`：`Get → ETag`、`PutIfAbsent → If-None-Match:*`、
  `CompareExchange → If-Match`；
- 未来可增加 GCS/Azure 实现。

`IMetadataStore` 与 `IStorage` 没有继承关系。

---

## 7. IStorage：文件与 byte-range

`IStorage` 保持现有契约：

```text
file locator → IInputFile → Size / ReadAt
file locator → IOutputStream → Append / Close
```

它不知道 Table、CURRENT、manifest、epoch、CAS 或 ETag。`S3FileStorage` 内部可以把 file locator
映射为 S3 object key，但 object 只是实现细节。

`IFileFormat` 继续只依赖 `IStorage`，因此 DBC1 可以不变地运行在 Mem、Local、Cached S3 上。

---

## 8. CachedStorage 与 IFileCache

`CachedStorage` 是 `IStorage` decorator：

```cpp
class CachedStorage : public IStorage {
 private:
  std::shared_ptr<IFileCache> cache_;
  std::shared_ptr<IStorage> remote_;
};
```

它只缓存 file bytes/ranges，不解析 Table metadata。长期应使用独立 `IFileCache`，因为 cache 具有
capacity、eviction、miss 和 range/page 等语义；当前 `MemStorage` 是权威内存文件系统，只能作为
第一阶段 whole-file cache 的临时 backend。

immutable WAL、commit、manifest、DBC1 很容易按不可变 identity 缓存。mutable CURRENT 走
metadata path，强一致读取不能被普通 data cache 静默遮蔽。

---

## 9. TableSnapshot 与 CloudTableSource

查询开始时固定一次 immutable snapshot：

```cpp
struct TableSnapshot {
  MetadataVersion metadata_version;
  Schema schema;
  std::vector<std::string> data_files;
  std::vector<std::string> wal_files;
  uint64_t indexed_cursor;
  uint64_t committed_cursor;
};
```

`CloudTableSource` 组合 base DBC1 和 committed WAL tail：

```text
CloudTableSource
  ├── TableSource(format, files, data_files)
  └── WalTableSource(files, wal_files)
```

现有 `TableSource(format, store, files)` 天然是 snapshot-level object。node 长期缓存的是
`CloudTable` handle；每次查询创建短生命周期 source，避免 manifest 切换污染在途查询。

---

## 10. Node 级资源共享

所有 Table 应共享 node 级 backend，而不是每张 Table 创建一套 S3 client/cache：

```cpp
struct CloudRuntime {
  std::shared_ptr<S3Client> s3_client;
  std::shared_ptr<S3FileStorage> remote_files;
  std::shared_ptr<IFileCache> file_cache;
  std::shared_ptr<CachedStorage> cached_files;
  std::shared_ptr<S3MetadataStore> metadata;
};
```

每个 `CloudTable` 只保存 shared handles 和自己的 descriptor/state manager。

---

## 11. 命名与职责

| 名称 | 职责 |
|---|---|
| `Catalog` | SQL 名称到 TableDescriptor |
| `CloudTable` | 长生命周期 Table handle |
| `TableMetadataStore` | CURRENT/commit/epoch/rebase 领域协议 |
| `IMetadataStore` | versioned KV + CAS |
| `S3MetadataStore` | metadata CAS 到 S3 条件请求的 adapter |
| `IStorage` | file locator + byte-range IO |
| `S3FileStorage` | file IO 到 S3 的 adapter |
| `CachedStorage` | read-through file cache decorator |
| `IFileCache` | RAM/NVMe cache policy |
| `TableSnapshot` | 一次查询固定的 metadata view |
| `CloudTableSource` | base DBC1 + WAL tail |

核心职责链：

```text
Catalog 找到 Table
TableMetadataStore 找到 Snapshot
CloudTableSource 组合 base + WAL
IFileFormat 解释文件 bytes
IStorage 提供 byte ranges
IMetadataStore 提供单 key CAS
```

---

## 12. 第一阶段实施顺序

1. 独立 `IMetadataStore` + `MemMetadataStore`，验证 CAS、stale generation 和 ABA；
2. `TableState` / immutable `CommitRecord` codec；
3. `TableMetadataStore` 创建、writer epoch、publish、rebase；
4. WAL file codec 与 `CloudTableSource` 强一致读取；
5. WAL → DBC1 compaction；
6. `CachedStorage` 与 file cache；
7. `S3FileStorage` / `S3MetadataStore`；
8. Catalog 与集群级 CloudTable lifecycle。

---

## 13. CURRENT 首版协议

首版 `CURRENT` 直接保存完整且有界的 `TableState`，而不是只保存 manifest 文件名：

```text
TableState {
  format_version
  table_id
  state_version
  writer_epoch
  committed_cursor
  indexed_cursor
  base_manifest
  commit_head
}
```

这里有两种不同的版本，不能混淆：

- `MetadataVersion` 是 `IMetadataStore` 返回的不透明 CAS token；S3 实现中通常对应 ETag；
- `state_version` 是 Table 领域中可读、单调加一的状态版本，用于诊断和验证状态迁移。

CURRENT key 由 `metadata_prefix + "/CURRENT"` 确定。创建使用 `PutIfAbsent`，发布使用读取时得到的
`MetadataVersion` 做 `CompareExchange`，因此读取和写入都不需要 LIST。

首版 codec 是明确 little-endian、带 magic、codec version 和 length prefix 的二进制格式。解码必须
拒绝截断、尾随字节、未知版本和不合法 cursor。它是内部格式，不把 S3 ETag 或对象 API 编进内容。

每次 publish 必须满足：

- `state_version` 恰好加一；
- `writer_epoch`、`committed_cursor`、`indexed_cursor` 不倒退；
- `indexed_cursor <= committed_cursor`；
- `table_id` 和 format version 不变。

上层不直接调用通用的 `Publish(TableState)`。当前公开的领域操作是：

- `AcquireWriter`：只把 `writer_epoch` 和 `state_version` 各加一；
- `PublishWal`：验证 epoch、连续 cursor 和 parent commit，先幂等创建 immutable commit record，
  再 CAS CURRENT。

`PublishWal` 遇到 CURRENT CAS 失败后读取最新状态并分类：epoch 改变是 `Fenced`；同 epoch、同
commit head 是响应不确定场景下的 `AlreadyCommitted`；同 epoch 但 root 已由其他操作推进则是
`RebaseRequired`。这些结果不能被上层折叠成统一 blind retry。

`CommitRecord` 是 immutable metadata，记录一个连续 cursor 区间、WAL 文件、batch ids 和父 commit。
后续 WAL publish 会先 `PutIfAbsent` 写 commit record，再通过一次 CURRENT CAS 使它可见；CAS 前产生的
WAL/commit 都只是不可见孤儿。
