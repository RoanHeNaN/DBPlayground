# High-Cardinality Multi-Tenant Observability Storage

> Status: **design direction**（问题定义与架构原则，尚未实现）。
>
> 本文确定 DBPlayground 面向可观测场景时的核心工作负载与扩展模型，并收敛
> [`MultiTenantObjectStore.md`](MultiTenantObjectStore.md) 中尚未定型的并发写方向：
> **Table 是租户级 namespace；正常情况下每张 Table 只有一个逻辑 writer；集群通过承载更多
> Table 横向扩展；S3 CAS 是最终提交与故障裁决机制。**
>
> 本文主要回答“为什么选择这个模型”。对象布局、WAL 编码和 manifest 的字节级格式仍由后续
> implementation spec 确定。

---

## 1. 背景：我们解决的不是单表极限写入

DBPlayground 的目标场景是 **high-cardinality multi-tenant observability**：

- 系统承载大量租户；
- 每个租户有独立的数据、生命周期、查询热度与成本边界；
- 对单个租户，写入通常是周期性批量上报，例如约每秒一次 `INSERT`，每次 K 行；
- 对整个集群，大量租户同时写入，形成很高的聚合吞吐；
- 数据以 append 为主，event time 已包含在记录中，不依赖跨租户或全局写入顺序；
- 冷租户很多，活跃租户只占一部分，不能为每个租户常驻一个有状态服务。

这与“少量超大表，每张表都要求无限横向扩展写入”的数据仓库场景不同。我们的核心扩展维度是：

```text
更多 Table / tenant
        ×
每张 Table 中等、可批处理的写入
        =
很高的集群聚合吞吐
```

因此，系统不需要首先解决“一张表由许多节点同时无协调写入”。相比之下，更重要的是：

1. 百万级 Table 如何以接近零常驻成本存在；
2. 如何把 Table 均匀分散到 writer 集群；
3. 写入成功后如何立刻可查，且查询热路径不依赖对象存储 `LIST`；
4. 节点故障或扩缩容时，如何避免双写者破坏数据；
5. 如何把 WAL 异步 compact 成适合分析查询的列存文件。

---

## 2. 核心映射：Table = tenant namespace

在本设计中，一张 DBPlayground `Table` 同时是：

- 租户隔离单元；
- 一致性哈希路由单元；
- writer ownership 单元；
- WAL 与提交顺序单元；
- manifest / snapshot 单元；
- compaction 与 backpressure 单元；
- 缓存与故障隔离单元。

即：

```text
DBPlayground Table ≈ turbopuffer namespace
```

Table 内部暂不引入用户可见的 bucket。集群扩展依靠 Table 之间的天然并行，而不是先把每张 Table
拆成多个独立写分片。

这个映射也明确了隔离边界：某个租户写入突增、WAL 积压或 compaction 落后时，应只对该 Table
限流，而不是拖慢全局。

---

## 3. 目标与非目标

### 3.1 目标

1. **对象存储是唯一持久状态**：writer、reader、compactor 节点可以丢失并重建。
2. **成功写入立刻可见**：`INSERT` 成功返回后，随后开始的强一致查询必须看到它。
3. **热路径零 LIST**：查询通过已知根指针和确定性元数据发现 WAL / data files。
4. **集群级横向扩展**：增加 writer 节点即可承载更多 Table 和更高聚合写入吞吐。
5. **正常路径低冲突**：同一 Table 的并发请求被路由到同一个逻辑 writer 并 group commit。
6. **故障时仍正确**：扩缩容、网络分区、进程暂停和 zombie writer 由 S3 CAS + fencing 裁决。
7. **异步列存化**：写路径先提交 WAL，后台再生成 DBC1 与统计信息。

### 3.2 非目标

- 单张 Table 的无限写入扩展；
- 正常情况下多个 active writer 同时写同一 Table；
- 跨 Table 事务或跨 Table 原子快照；
- 为避免一次元数据 CAS 而牺牲写后立即可见；
- 让无限增长的 uncompacted WAL 永远由查询重放；
- 第一阶段支持复杂 UPDATE/DELETE、通用 Serializable 事务或在线 resharding。

极热租户若超过单 Table 上限，第一阶段应显式 backpressure；未来可通过租户拆表或透明 table
sharding 扩展，但不应提前把该复杂度放进基础协议。

---

## 4. 为什么“不 LIST + 立刻可见”必然需要提交点

writer 可以把新数据写到任意唯一对象：

```text
wal/<random-uuid>.wal
```

但如果查询既不 `LIST`，又没有任何共享元数据告诉它这个 UUID，查询就不可能发现该对象。

因此，成功写入必须包含两个阶段：

```text
1. durable：数据对象已经持久化到 S3
2. publish：数据对象已经进入读者可确定发现的 committed state
```

只有第二步完成后才能向用户返回成功。

publish 需要一个逻辑串行化点，例如 `CURRENT` 的条件更新、连续 commit cursor，或由 CURRENT
引用的 immutable commit record。具体编码可以变化，但必须满足：

- 读者从已知 key 出发即可得到全部 committed WAL 路径；
- 提交是一次 S3 CAS；
- CAS 成功前数据不可见；
- CAS 成功后新查询必然可发现数据；
- CAS 失败者不能把同一逻辑 batch 重复发布。

这不是在给所有观测事件建立业务全序，而是在给“哪些 batch 已经成功返回”建立可发现的提交边界。

---

## 5. 集群路由：每 Table 一个逻辑 writer

writer 集群使用 consistent hashing，可采用 rendezvous hashing，或带虚拟节点的一致性哈希环：

```text
owner = RendezvousHash(table_id, writer_membership)
```

```text
Table A ─┐
Table C ─┼──> Writer 1
Table G ─┘

Table B ─┐
Table D ─┼──> Writer 2
Table H ─┘
```

一个 writer 同时服务大量 Table，并为每张活跃 Table 维护轻量的内存状态：

```text
TableWriteState {
  writer_epoch
  committed_cursor
  pending_requests
  group_commit_deadline
  unindexed_bytes
}
```

实现必须是异步多路复用，而不是每张 Table 一个线程。空闲 Table 的内存状态可以淘汰，重新活跃时
从 S3 的 CURRENT / manifest 恢复。

### 5.1 为什么不让客户端直接争抢 CAS

同一 Table 的 N 个并发 `INSERT` 不应变成 N 个独立节点争抢 CURRENT：

```text
错误的正常路径：
N requests → N CAS attempts → N-1 conflicts → rebase storm
```

一致性哈希把请求收敛到同一个 writer，writer 再做 group commit：

```text
目标路径：
N requests → one table-local buffer → one WAL batch → one CAS
```

因此客户端并发增加时，单行的 CAS 成本下降，而不是冲突数量随并发恶化。

---

## 6. 写入与写后立即可见

一个 Table 的逻辑写入流程为：

```text
Client(s)
   │  concurrent INSERTs
   ▼
Table Writer
   │  buffer / group commit
   ▼
PutIfAbsent(WAL batch)
   │  durable but not visible
   ▼
CAS(table CURRENT / commit point)
   │  committed and visible
   ▼
return success to all requests in the batch
```

group commit 可以按以下条件触发：

- 时间窗口到期，例如最多等待一秒；
- 行数或字节数达到阈值；
- 显式 flush；
- 内存或 per-table backpressure 要求立即提交。

一个 WAL batch 中必须保存：

- table / schema version；
- writer epoch；
- 一个或多个 client batch id；
- row count；
- append payload；
- checksum；
- 为响应不确定结果准备的幂等标识。

写请求只有在 publish CAS 成功后才返回。仅仅完成 WAL PUT 不代表用户可见。

### 6.1 提交元数据的逻辑模型

下面是逻辑字段示意，不限定最终序列化格式：

```jsonc
{
  "format_version": 1,
  "table_id": "tenant-a-events",
  "writer_epoch": 7,
  "committed_cursor": 1042,
  "indexed_cursor": 1038,
  "base_manifest": "manifest/base-1038.json",
  "commit_head": "commit/commit-<uuid>.json"
}
```

物理实现可以选择连续 WAL key、immutable commit chain，或 CURRENT 中的有界 pending list。
无论选择哪一种，都必须让 reader 从 `indexed_cursor + 1` 精确发现到 `committed_cursor` 的数据，
不依赖 LIST。

---

## 7. 强一致读：base files + committed WAL tail

查询读取的是一个 Table snapshot：

```text
committed table state
  = base DBC1 files through indexed_cursor
  + committed WAL after indexed_cursor
```

逻辑读路径：

```text
1. GET CURRENT / commit metadata
2. GET base manifest
3. 读取已经 compact 的 DBC1 files
4. 精确 GET (indexed_cursor, committed_cursor] 的 WAL
5. 合并结果
```

如果：

```text
indexed_cursor   = 1038
committed_cursor = 1042
```

查询就读取 base snapshot，再重放 1039–1042。后台 compaction 是否已经运行不影响新写入的
可见性，只影响查询需要重放多少 WAL。

查询在开始时固定 CURRENT 版本；GC 不能删除该 snapshot 仍可能引用的数据。具体实现需要
snapshot lease 或足够保守的 retention/grace period，并在对象意外 404 时从新 CURRENT 整体重试。

---

## 8. CAS + rebase：仍然需要，但不作为吞吐机制

一致性哈希只保证正常路由，不能覆盖以下情况：

- writer 集群扩缩容时成员视图不一致；
- 旧 writer 长时间暂停后恢复；
- 网络分区导致旧、新 owner 同时存在；
- compactor 与 writer 同时更新 Table metadata；
- CAS 成功但响应丢失。

因此 S3 CAS 仍然是最终正确性原语：

```text
Routing / single writer：让 CAS 通常不冲突
CAS：决定哪个状态真正提交
Rebase：合并仍然合法的同 epoch metadata 变更
Fencing：阻止旧 epoch writer 继续提交
```

### 8.1 CAS 失败不能统一重试

```text
CAS failed
   │
   ├─ writer_epoch 已变
   │     → 当前 writer 已被 fence，停止处理该 Table
   │
   ├─ 同 epoch，操作前置条件仍成立
   │     → 基于最新状态 rebase 后重试
   │
   └─ 输入已经被消费或替换
         → 丢弃输出，必要时重新计算
```

writer 不得在发现更高 epoch 后继续追赶 rebase，否则 zombie writer 会重新参与正常写入。

对于超时导致的结果未知，使用 client `batch_id` 读取提交元数据判断“已提交”还是“需要重试”，
不能盲目重新追加一份相同数据。

### 8.2 冲突来自故障和后台工作，不来自正常导入并发

该模型下，CAS 冲突主要随以下事件增长：

- table ownership rebalance；
- writer failover；
- compaction / checkpoint 提交频率；
- schema metadata 更新。

它不随同一 Table 的客户端 `INSERT` 并发线性增长，因为这些请求已在 active writer 内合批。

---

## 9. Writer ownership 与 fencing

一致性哈希决定“请求通常发给谁”，但它本身不是一致性协议。Table CURRENT 中必须保存持久化的
`writer_epoch` 或等价 generation。

新 writer 接管 Table 时：

```text
1. GET CURRENT
2. CAS bump writer_epoch
3. 从 committed state 恢复
4. 开始接收新写入
```

旧 writer 即使恢复，也只能：

- 产生无法发布的孤儿对象；
- 在 CURRENT CAS 时因为旧 tag / epoch 被拒绝；
- 停止接受该 Table 的后续请求。

要让 CURRENT tag 充当 fencing token，所有会改变 Table 逻辑可见状态的路径都必须经过 CURRENT
或等价的 epoch-checked commit point。任何绕过该点、且会被 reader 直接发现的 WAL 写入都会破坏
fencing。

lease 可以减少两个节点同时工作造成的浪费，但正确性来自 epoch + CAS，而不是 lease 的时间判断。

---

## 10. Compaction：异步扩展，按 Table 隔离

compaction 不在同步写入路径中：

```text
WAL batches
    ↓
L0 flush / indexing
    ↓
small DBC1 files
    ↓
size-tiered compaction
    ↓
larger DBC1 files + updated stats
```

compactor 集群与 writer 集群独立扩展。不同 Table 天然可以并行 compact；单 Table 内也可以并行
计算互不重叠的输入范围，但最终发布仍需 CAS。

一次 WAL compaction 的正确提交必须原子表达：

```text
add new DBC1 files
advance indexed_cursor over exactly the covered WAL range
update base_manifest
```

一次 data-file compaction 必须表达：

```text
remove exact input file IDs
add exact output file IDs
```

CAS 失败后不能盲目把输出 union 到最新 manifest。只有输入范围或输入文件仍未被其他提交消费时，
该输出才允许 rebase；否则必须丢弃。

WAL 与旧 data files 只能在新 manifest 提交后由幂等 GC 删除，并需要保护仍持有旧 snapshot 的读者。

---

## 11. Backpressure：持续吞吐最终受 compaction 限制

group commit 可以吸收突发写入，但如果：

```text
ingestion rate > compaction rate
```

那么：

```text
committed_cursor - indexed_cursor
```

会持续增长，强一致查询必须重放越来越长的 WAL tail。最终问题从写入吞吐转化为查询延迟、GET
成本和内存压力。

因此必须按 Table 维护：

- `unindexed_bytes`；
- `unindexed_rows`；
- WAL segment count；
- oldest unindexed age；
- compaction lag。

超过阈值时优先：

1. 调度更多 compaction 资源；
2. 对该 Table 限流或返回 retryable error；
3. 允许显式 bulk-import 模式暂时牺牲强一致查询；
4. 避免让单个 hot tenant 拖垮共享 writer。

这一上限是产品语义的一部分，不应通过无限 WAL replay 隐藏。

---

## 12. 集群扩缩容

增加 writer 节点时，consistent hash 只迁移一部分 Table ownership。由于持久状态在 S3，迁移的
核心不是复制数据，而是：

```text
old owner stops / is fenced
        ↓
new owner CAS bumps epoch
        ↓
new owner reconstructs lightweight TableWriteState from S3
```

迁移成本主要是：

- ownership epoch 更新；
- 内存 buffer 中未提交请求的客户端重试；
- cache 重新 hydrate；
- 少量孤儿 WAL/manifest 的后续 GC。

路由层应携带 membership version，并在发现 owner 不匹配时返回可重试的重定向信息。即使不同节点
短暂持有不同 membership view，S3 CAS 仍保证只有一个 epoch 可以发布新状态。

---

## 13. 扩展模型与容量边界

该架构的容量模型是：

```text
单 Table 吞吐
  ≈ 单 logical writer 的 group-commit 能力
  且长期受该 Table compaction 能力限制

集群聚合吞吐
  ≈ writer 节点数 × 每节点可承载的活跃 Table 写入
```

因此增加 writer 节点主要提高：

- 活跃 Table 数量；
- 集群 WAL PUT 并发；
- 集群编码与 group-commit 能力；
- 租户级故障隔离与公平性。

它不会自动提高单张极热 Table 的上限。这是有意的 workload 选择，而不是遗漏。

---

## 14. 建议的不变量

实现与测试必须守住以下不变量：

1. 一个成功返回的 batch 必须已经 durable 且 published。
2. 未经 committed metadata 引用的对象永远不能被 reader 当作可见数据。
3. 强一致 reader 能从已知 key 精确发现全部 committed WAL，不使用 LIST。
4. 正常情况下每张 Table 只有一个 active writer epoch。
5. 更低 epoch 的 writer 永远不能发布新状态。
6. CAS 失败后必须根据 epoch 和 operation precondition 分类处理，不能统一 blind retry。
7. 每个 client batch id 在承诺的幂等窗口内最多发布一次。
8. compaction 输出只能替换其明确覆盖的输入。
9. GC 只能删除已被 committed snapshot 取代且不再受 reader retention 保护的对象。
10. compaction lag 超限时对单 Table backpressure，不能无限放大强一致读成本。

---

## 15. 与现有设计文档的关系

- [`MultiTenantObjectStore.md`](MultiTenantObjectStore.md)：继续提供 namespace、CURRENT、manifest、
  CAS、WAL 与 GC 的总体对象存储布局；其中“每个 writer 各写一个 WAL 前缀”的 leaderless 收敛
  方案不再作为可观测场景的首选正常写路径。
- [`ObjectStorageFormatResearch.md`](ObjectStorageFormatResearch.md)：提供 turbopuffer / Lance 的外部
  调研背景。
- [`StorageAbstraction.md`](StorageAbstraction.md)：定义 Object Store / File Format / Table Format
  的分层。
- [`NativeColumnarFileFormat.md`](NativeColumnarFileFormat.md)：定义 compaction 目标文件 DBC1。

后续 implementation spec 需要进一步确定：

1. CURRENT、commit record 与 WAL key 的精确布局；
2. group-commit 的时间/大小策略；
3. writer epoch 的获取和 failover 状态机；
4. client batch id 的去重保留窗口；
5. snapshot retention 与 GC 协议；
6. writer 与 compactor 同时提交时各操作的 rebase precondition；
7. per-table backpressure 阈值与错误语义。

---

## 16. 参考

- turbopuffer Architecture：<https://turbopuffer.com/docs/architecture>
- turbopuffer Concepts / WAL：<https://turbopuffer.com/docs/concepts>
- turbopuffer Guarantees：<https://turbopuffer.com/docs/guarantees>
- turbopuffer Ingestion：<https://turbopuffer.com/docs/ingestion>
- turbopuffer Limits：<https://turbopuffer.com/docs/limits>
- turbopuffer Namespace Sharding：<https://turbopuffer.com/docs/sharding>
- SlateDB Manifest / writer fencing RFC：<https://slatedb.io/rfcs/0001-manifest/>
- SlateDB Compaction RFC：<https://slatedb.io/rfcs/0002-compaction/>
