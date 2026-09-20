# Cloud Mode Import and Query Framework

> Status: **framework implemented; production adapters pending**。
>
> 本文把 Cloud 模式的导入与查询路径落实到具体类和接口。当前已经实现内存端到端编排与协议测试；
> S3、Catalog、membership、RPC、group commit scheduler、manifest codec 和独立 WAL codec 仍是接口。

## 1. 目标与边界

Cloud 模式面向 high-cardinality multi-tenant observability：单 Table 写入频率有限，集群通过大量
Table 在多个 writer 上分布获得总吞吐。每个 Table 在正常路径上只有一个逻辑 writer，但最终正确性
仍来自对象存储上的 epoch + CURRENT CAS。

框架需要满足：

- SQL/frontend 不直接争抢 Table CAS；
- 导入成功前 WAL 必须 durable 且已被 CURRENT 引用；
- 成功返回后，新查询立刻能看到该 WAL；
- 查询只从已知 key 出发，不依赖 LIST；
- 查询打开后固定 immutable snapshot；
- `IStorage` 始终只是 file locator + byte-range IO；
- 路由、RPC 和 S3 SDK 不进入 Table 领域协议。

## 2. 类关系

```text
                         control / metadata path
ICloudCatalog ──resolve──> TableDescriptor
                              │
                              ├──────────────┐
                              ▼              ▼
                       CloudTable       CloudTableWriter
                              │              │
                              │              ├─ IObjectKeyGenerator
                              │              ├─ IBatchCommitResolver
                              │              ├─ IFileFormat(WAL)
                              │              ├─ IStorage(files)
                              │              └─ TableCurrentStateStore
                              │                       │
                              ▼                       ▼
                    TableSnapshotLoader        IMetadataStore CAS
                       ├─ CURRENT
                       ├─ ICompactedDataManifestStore
                       └─ CommitRecord chain
                              │
                              ▼
                     CloudTableSource
                       ├─ IFileFormat(compacted DBC1)
                       ├─ IFileFormat(WAL)
                       └─ IStorage(files)
```

`CloudTable` 是 node 缓存的长生命周期 handle。`CloudTableWriter` 是当前 owner 的 table-local 状态机。
`CloudTableSource` 是一次查询独占的 immutable snapshot view，不能在 manifest/CURRENT 推进后原地变化。

## 3. 集群导入路径

```text
SQL INSERT
  │
  ▼
CloudImportService
  │  ICloudCatalog.Resolve(database, table)
  │  IWriterRouter.Route(table_id)       // consistent/rendezvous hash adapter
  ▼
IWriterTransport                         // local call or RPC adapter
  │
  ▼
CloudWriterService
  │  IWriteCoordinatorProvider.Get(table_id)
  ▼
ITableWriteCoordinator                   // per-table serialization + group commit
  │  N client requests → one CloudImportBatch
  ▼
CloudTableWriter.Import
```

`ITableWriteCoordinator` 是并发收敛点。它后续负责：

- 同一 Table 的请求排队；
- group commit deadline / size threshold；
- 多个 client `batch_id` 合并进一个 `CloudImportBatch`；
- 同一时刻只调用一个 `CloudTableWriter`。

因此正常客户端并发不会变成 N 个 CURRENT CAS。

`IBatchCommitResolver` 在写 WAL 前以及 CAS 结果不确定时，根据固定的 CURRENT view 查询 batch ids。
生产实现从近期 commit chain 和 compacted manifest 中的 bounded dedupe index 得到结果，区分全部已提交、
全部未提交和非法的 partial match。它不能使用一个与 CURRENT 提交点无关的弱一致旁路表。

### 3.1 writer 启动

```text
GET CURRENT
  ├─ missing → PutIfAbsent(initial CURRENT)
  └─ exists
CAS CURRENT: writer_epoch += 1, current_state_version += 1
```

只有 CAS 成功的 `CloudTableWriter` 进入 started 状态。另一个候选 owner 收到 `Contended`，不能开始
写入。旧 owner 后续 publish 时看到 epoch 已改变，进入 `Fenced` 并丢弃本地 ownership 状态。

### 3.2 一个 group commit

```text
CloudImportBatch { batch_ids, chunks }
  │
  ├─ generate wal/<unique>.wal
  ├─ IFileFormat.OpenWriter(IStorage, wal_path)
  ├─ Write(chunks) + Close()                       durable
  │
  ├─ generate commit/<unique>.meta
  ├─ PutIfAbsent(immutable CommitRecord)
  └─ CAS CURRENT(latest_commit_key, committed_cursor)    publish / visibility point
```

返回 `Committed`/`AlreadyCommitted` 之前，CURRENT 必须已经引用 commit。只有 WAL PUT 成功不算导入
成功。CAS 之前失败留下的 WAL/commit 是不可见孤儿，未来由冷路径 GC 处理。

CAS 失败的处理不是 blind retry：

| 结果 | 行为 |
|---|---|
| `AlreadyCommitted` | CAS 响应不确定但相同 commit 已成为 head，返回成功 |
| `Fenced` | epoch 已改变，停止该 Table writer |
| `RebaseRequired` | 同 epoch 的 root 被推进；重新读取 CURRENT，为同一 WAL 生成新 commit parent/cursor |
| `CommitKeyCollision` | 换新的 immutable commit key |
| `RetryableConflict` | 以相同 key/record 重试，用 PutIfAbsent 保持幂等 |

## 4. 查询路径

查询不经过 writer：

```text
SQL query
  │
  ▼
CloudQueryService
  │  Catalog.Resolve
  │  ICloudTableProvider.Get(table_id)
  ▼
CloudTable.OpenSnapshot
  │
  ▼
TableSnapshotLoader
  1. GET CURRENT
  2. GET compacted_data_manifest_key（若非空）
  3. 从 latest_commit_key 向 parent_commit_key 反向读取
  4. 到达 compacted_cursor 后停止
  5. 反转 commit 列表，得到确定顺序的 WAL files
  │
  ▼
CloudTableSource
  ├─ scan compacted data files
  └─ scan committed wal_files
```

Snapshot loader 验证 commit cursor 连续、manifest 水位与 CURRENT 一致、所有对象属于同一 Table。
任一缺失或断链都作为 metadata corruption/hard error，不能静默跳过，否则会向查询返回缺行结果。

`CloudTableSource::Scan(projection)` 返回组合 cursor，先消费 compacted data files，再消费 snapshot 中的
WAL tail。compacted data 与 WAL 各自通过 `IFileFormat` 解释，因此首版测试可以复用 DBC1，未来替换成独立 WAL
编码不需要修改 query service 或执行器。

## 5. Snapshot 隔离与立即可见

查询开始时复制以下内容：

```text
CURRENT MetadataVersion
current state / epoch / compacted / committed cursor
compacted-data manifest and files
ordered commit records and WAL files
```

后续 CURRENT 推进不会修改已经打开的 `CloudTableSource`。因此：

- 导入 CAS 成功后，新打开的查询立即发现新 WAL；
- 旧查询继续使用旧 snapshot；
- compaction 可以发布新的 compacted-data manifest，而不改变在途 source 的文件集合。

当前测试使用一个 `List()` 会直接抛异常的 `IStorage` 跑通完整导入/查询路径，以固定“热路径零 LIST”
这一契约。

## 6. 尚未提供的具体实现

以下是有意保留的 adapter/scheduler 工作，不属于当前框架 commit：

- `S3MetadataStore`：ETag / If-Match / If-None-Match；
- `S3FileStorage` 和 cache decorator；
- durable Catalog 与 `ICloudTableProvider` cache；
- membership-aware rendezvous hash router；
- writer RPC transport；
- asynchronous per-table group commit coordinator；
- WAL 的生产编码、checksum 与 schema version；
- manifest codec/store 和 compaction publish；
- 持久化 batch-id dedupe window 与 GC。

这些实现必须接入现有接口，不能重新把 Table/CAS 语义塞回 `IStorage`。
