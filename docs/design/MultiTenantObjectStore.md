# 多租户对象存储：顺序 WAL、按需可见性与后台索引

> 状态：顺序 WAL、双水位、按需发布、自适应探测和 WAL → manifest + data 索引发布的核心路径已实现。后续 data → data compaction、WAL 回收与列统计仍属后续工作。
> 范围：一个 namespace 对应一张表和一组独立的对象。数据与元数据均存于对象存储，查询和写入的正常路径不依赖 LIST。

## 1. 目标与基本约束

每个 namespace 通过一个可条件更新的 `CURRENT` 对象维护元数据根状态。WAL 和列式数据文件不可变。一个 namespace 的 WAL 使用全局连续序号 `1, 2, 3, ...`，文件名只有序号，没有 writer ID。写者可以选择数据写完后立即对查询可见，也可以选择先完成持久化、由后台索引最终发布。

设计需要同时满足：

- **持久化与可见性分离**：WAL 对象写入成功表示数据已持久化；是否对严格查询可见由 `CURRENT` 的已发布水位决定。
- **单写者顺序**：同一 namespace 同一时刻只有一个有效 writer epoch；切换 writer 时必须阻止旧 writer 继续占用 WAL 序号。
- **连续发现**：读者和后台索引从已知水位开始按序号探测 WAL，遇到第一个不存在的序号便停止，不靠 LIST 建立写入顺序。
- **原子发布**：列式文件写完后，通过一次 `CURRENT` CAS 发布新的索引结果。任何未被发布的输出文件都不可作为严格读的依据。
- **两种读语义**：严格读必须包含读取 `CURRENT` 时已发布的全部数据；尽力读可额外探测尚未发布的 WAL，扫描数量由自适应参数控制。

S3 对成功 PUT 后的 GET 和 LIST 提供强一致性；这里避免在热路径使用 LIST 是为了控制请求量和建立明确的顺序协议，不是因为 S3 LIST 只有最终一致性。实现依赖目标对象存储对单对象写入的原子性和条件创建能力。

## 2. 对象布局

```text
<namespace>/
  metadata/
    CURRENT                         可变；只能通过条件写更新
    manifest/<unique-key>.meta      不可变；已索引数据文件集合与批次去重索引
    dedup/<unique-key>.idx          不可变；已索引批次 ID 到 WAL 序号的映射
  files/
    wal/00000000000000000001.wal    不可变；序号为 1 的 WAL 或 fencing 标记
    wal/00000000000000000002.wal
    data/<unique-key>.dbc1          不可变；后台索引与后续压缩的产物
```

路径中的零填充只方便人工检查和按名称排序；协议依据的是文件内的序号，不依赖对象列表的顺序。WAL 一旦成功创建不得覆盖，也不得在序号仍可能被读取时删除。`CURRENT` 是唯一需要原地更新的根对象。manifest 和数据文件采用防碰撞的唯一键。

## 3. `CURRENT` 与 manifest 的内容

以下 JSON 用来规定**逻辑字段和含义**。实际对象可以采用有版本号的二进制编码；编码方式不能改变字段的校验规则。所有路径均相对于 namespace，不能指向其他表的对象。

### 3.1 `CURRENT`：根状态与可见水位

```jsonc
{
  "format_version": 2,
  "table_id": "tenant-a/events",
  "state_version": 27,
  "schema_version": 1,
  "writer_epoch": 4,
  "indexed_seq": 12,
  "published_seq": 14,
  "manifest_key": "manifest/8f3c.meta"
}
```

| 字段 | 类型及含义 |
|---|---|
| `format_version` | 无符号整数；根状态的编码版本，当前为 2。 |
| `table_id` | 非空字符串；读取 manifest 或 WAL 时都要核对所属表。 |
| `state_version` | 无符号整数；每次成功 CAS 后加一，用于检测错误转移和排查问题。真正的 CAS 前提是对象存储返回的版本标识。 |
| `schema_version` | 无符号整数；当前写入使用的 schema 版本。对应 schema 定义由 manifest 保存。 |
| `writer_epoch` | 无符号整数；当前有效写者的任期。首次创建时为 0，取得写入权时递增。 |
| `indexed_seq = C` | 无符号整数；`≤ C` 的有效 WAL 已由 manifest 引用的 data 文件覆盖。fencing 标记占序号但不产生数据行。此水位由 WAL 索引阶段推进，与后续列式 compaction 无关。 |
| `published_seq = P` | 无符号整数；`≤ P` 的 WAL 对严格查询可见，包括尚未索引的尾部。 |
| `manifest_key` | 非空的 manifest 相对路径；首次创建表时即指向覆盖水位 0 的初始 manifest。 |

创建表时先写包含 schema、空 `data_files` 和空 `batch_index_files` 的初始 manifest，再条件创建 `CURRENT`。初始 `CURRENT` 的 `state_version = writer_epoch = C = P = 0`，`manifest_key` 指向该 manifest。始终满足 `0 ≤ C ≤ P`，各水位与 epoch 不得回退。`CURRENT` **不保存每个 WAL 的路径或已持久化的最大序号**：路径由序号确定，延迟可见的尾部可以满足 `seq > P`。对象存储返回的 ETag 或版本标识不写进 `CURRENT` 内容，只用于下一次 CAS。上例表示 manifest 覆盖至 12，严格查询还必须读取 WAL 13 和 14；WAL 15 即使已存在，也可能尚未发布。

### 3.2 manifest：已索引数据和去重索引

```jsonc
{
  "format_version": 2,
  "table_id": "tenant-a/events",
  "covered_through_seq": 12,
  "schema_version": 1,
  "schema": [
    { "column_id": 0, "name": "id", "type": "Int64" },
    { "column_id": 1, "name": "message", "type": "String" }
  ],
  "data_files": [
    {
      "path": "files/data/61ab.dbc1",
      "first_seq": 1,
      "last_seq": 12,
      "row_count": 4200,
      "column_stats": [
        { "column_id": 0, "min": 1, "max": 4200, "null_count": 0 }
      ]
    }
  ],
  "batch_index_files": ["dedup/4c9e.idx"]
}
```

`covered_through_seq` 必须等于引用它的 `CURRENT.indexed_seq`，`table_id` 和 `schema_version` 也必须相同。`data_files` 是截至该水位的**完整有效列式文件集合**，不是仅列出本轮新增文件；后续列式 compaction 可以替换物理文件，但不能改变逻辑数据。`first_seq`／`last_seq` 标记文件的 WAL 来源范围，供审计和恢复使用；一个范围可以含不产生行的 fencing 标记，不能据此单独推断该范围每个序号都有数据。`row_count` 与可选的 `column_stats` 用于读取和裁剪，不参与 WAL 发布判定。初始状态或只有 fencing 标记而无数据行时，`data_files` 可以为空。首个版本保持 schema 不变；未来若支持 schema 演进，manifest 还须保存能解释未索引 WAL 的历史 schema 定义。

`batch_index_files` 中的路径相对于 `metadata/`，指向不可变索引对象；`manifest_key` 也相对于 `metadata/`。每条索引至少保存 `batch_id → {seq, operation_id, payload_digest}`；写者查询这些索引和未索引 WAL，才能识别跨 writer 切换与列式 compaction 的重试。manifest 必须引用覆盖已索引有效批次的完整索引集合；合并索引时可以换新文件，但不能丢失仍在幂等保留期内的批次。索引保留期及过期后的重试语义应由 API 明确规定。

发布顺序是先写完 DBC1 和去重索引，再写不可变 manifest，最后 CAS 更新 `CURRENT.manifest_key` 与 `C`。读者固定一次 `CURRENT` 版本，按该版本加载 manifest；manifest 缺失、所属表不符或水位不符都是错误，不能退回到猜测对象列表。

## 4. WAL 文件内容

每个 `files/wal/<seq>.wal` 是一个完整、不可覆盖的对象，包含一个写入提交单元或一个 fencing 标记。它不是可以在 S3 上原地追加的共享文件。正常写入可以把多个客户端 batch 合成一个提交单元，并包含多个 Chunk。

目标二进制布局如下。整数使用小端编码，变长字符串与数组带长度；编码器对长度、条目数和总大小设置上限。

```text
magic = "DBW2", format_version: u32
entry_kind: u8                 Append 或 Fence
table_id: length + bytes
seq: u64, writer_epoch: u64, schema_version: u64
operation_id: length + bytes   一次提交单元的稳定幂等 ID
batch_ids: count + [length + bytes]*
chunk_count: u32
chunks:
  row_count: u32
  payload_size: u32
  payload: [row_length: u32 + RowCodec row bytes]*
  crc32: u32                    覆盖本 chunk 的行数、长度和 payload
footer:
  total_row_count: u64
  payload_digest: 32 bytes     schema 版本、batch_ids 与行数据的 SHA-256，供重试核对
  file_crc32: u32               覆盖 footer 之前的整个文件
  magic = "DBW2"              检测截断或不完整写入
```

例如 `files/wal/00000000000000000015.wal` 的**解码后逻辑内容**可以是：

```jsonc
{
  "entry_kind": "Append",
  "table_id": "tenant-a/events",
  "seq": 15,
  "writer_epoch": 4,
  "schema_version": 1,
  "operation_id": "import-request-72",
  "batch_ids": ["client-batch-72"],
  "chunks": [{ "rows": [[4201, "event-4201"], [4202, "event-4202"]] }],
  "total_row_count": 2
}
```

`Fence` 使用同一文件头，记录新 epoch 和唯一的 `operation_id`，但 `batch_ids`、`chunks` 均为空，`total_row_count = 0`。例如序号 16 的标记在解码后是 `{ "entry_kind": "Fence", "seq": 16, "writer_epoch": 5, "operation_id": "fence-epoch-5", "batch_ids": [], "chunks": [] }`。它占一个序号，不参与查询结果或批次去重，也不生成 DBC1 行。`Append` 必须有非空批次 ID 与有效数据，且 schema 与头部版本相符。读者核对文件名和头部 `seq`、表 ID、行数、CRC、digest 及结尾标记；任何不符都不能当作有效 WAL。

首个落地版本只支持 Append 和 Fence。Delete、Update 与墓碑需要另行定义编码、重放顺序和读时合并规则。`DBW2` WAL 存可重放的数据与幂等信息，不存已生成的 DBC1 文件。

## 5. writer 启动、序号与 fencing

写者先读取 `CURRENT`，用 CAS 将 `writer_epoch` 加一。取得新 epoch 后，从 `C+1` 开始按序检查现存 WAL，验证序号连续，直到找到第一个不存在的序号 `S`。已有 WAL 可能属于先前 writer 的延迟可见写入，不能因为它们尚未被 `CURRENT` 引用就忽略。若后台索引和 GC 在扫描期间推进了 `C`，写者须重读 `CURRENT`：候选槽位 `S ≤ C` 时重新从最新的 `C+1` 扫描，不能把已回收的旧序号重新用作 WAL 名称。

新 writer 用“仅当对象不存在才创建”的条件 PUT，在 `wal/S.wal` 写入自己的 fencing 标记：

- 创建成功：该标记之后，新 writer 才可接受写入。
- 槽位被旧 writer 抢先写入：验证已有文件，改试 `S+1`，直到自己的标记成功。
- 请求结果不明：读取 `wal/S.wal`，按 epoch、操作 ID 和校验判断是否为自己的标记，再决定继续或重试。

正常写入也只能**按序、单个在途**地条件创建下一个 WAL。不得预留后面的序号或并行上传同一 namespace 的多个新 WAL。旧 writer 若在新 epoch 的标记之前赢得一个槽位，新 writer 会先发现该 WAL 再继续争抢；标记一旦占住下一个槽位，旧 writer 的后续条件 PUT 就会失败，随后必须读取标记并停止。旧 writer 在每次写入前还应检查 `CURRENT.writer_epoch`，以减少无效写；真正的 fencing 仍由条件创建和连续槽位规则保证。

这些规则不允许“已分配但永远没有对象”的序号。写入失败时重试同一槽位或明确放弃且保持它为空；不能跳到 `S+1` 留下缺口。对已经存在的同名对象绝不能使用普通覆盖 PUT。S3 的 `If-None-Match: *` 是条件创建；本地和内存存储需要提供相同的原子语义。

## 6. 写入与可见性

每个写入请求声明 `visibility`：

| 模式 | 成功返回的条件 | 对严格查询的承诺 |
|---|---|---|
| `Immediate` | WAL 完整持久化，且 `CURRENT.published_seq` 已通过 CAS 推进到该 WAL 的序号或更高。 | 写入成功返回后启动的严格查询必须看到这批数据。 |
| `Deferred` | WAL 完整持久化，且写者确认序号与操作 ID；不要求更新 `CURRENT`。 | 查询可以提前看到，但必须在后台索引成功发布后稳定可见。 |

写入步骤为：检查批次幂等状态，确定下一个连续序号，条件创建 WAL，确认对象内容，然后按模式决定是否发布。`Immediate` 写入若得到序号 `S`，在 CAS 中把 `P` 推进到至少 `S`。这样也会一并发布该序号之前仍未发布的连续 WAL，这是水位语义的必然结果。CAS 与后台索引并发失败时，重新读取 `CURRENT`，核对 `writer_epoch`，保留较大的水位，并确认自己的序号已经被覆盖后再返回成功。若 epoch 已改变，旧 writer 不得替新 writer 更新 `CURRENT`；它可以在确认 `P ≥ S` 后报告成功，否则只能返回结果待确认状态。无法完成发布时，不能谎称满足立即可见承诺。

`Deferred` 的成功表示“已持久化”，不是“已被索引”或“已对所有查询可见”。写入结果应明确返回序号和持久化状态；客户端若随后需要严格读，可以等待该序号被发布，或发起带目标序号的读并等待系统完成发布。后台索引延迟或失败时，延迟可见写入不会自动得到时限保证，需要监控积压并重试索引任务。

客户端重试使用稳定的 `batch_id`／操作 ID。写者先检查未索引 WAL，再检查 manifest 中的持久化批次去重索引；同一批次不得因为换了 writer epoch 或完成了索引或列式 compaction 而再次导入。请求结果不明时先检查目标槽位内容。只有 WAL 内信息而没有跨索引与列式 compaction 的持久化去重索引，不能承诺长期 exactly-once。

## 7. 查询与自适应 WAL 探测

一次查询首先读取 `CURRENT` 和对应 manifest，固定 `C`、`P` 与列式文件集合。随后按序处理两段 WAL：

1. **必读段 `(C, P]`**：严格查询必须读取全部 WAL，跳过 fencing 标记并重放数据 WAL。若这个范围内缺文件、校验失败或读到不连续序号，不能把部分结果作为成功返回；应重读 `CURRENT` 后重试，仍异常则报错。
2. **探测段 `(P, P+N]`**：查询可按序探测最多 `N` 个未发布 WAL。遇到第一个不存在的文件即停止；存在的完整 WAL 可纳入这次尽力读。fencing 标记占用一个探测序号，但不产生行。探测段的结果不构成写后可见保证。

默认查询至少覆盖必读段。调用方可以选择只读已发布快照，或额外启用尽力探测。`Immediate` 的承诺由必读段保证，不依赖 `N`；`Deferred` 写入可能因 `N` 不足而暂时未被看到，即使对象已经存在。查询不得把“探测到第 `N` 个文件”解释为“尾部已读完”。

`N` 是**每个 namespace 在查询节点上的性能参数**，有配置的初值、最小值和最大值。实现可根据最近的命中数、是否用尽扫描额度、首个 404 的位置、WAL 字节数及查询延迟，用带滞后的规则调整它：反复用尽额度时增大，长期很早遇到 404 或成本超预算时减小。该参数无需写入 `CURRENT`，节点重启后可从初值重新学习。它只影响探测段；即使必读段长度超过预算，严格查询也不得静默截断。系统可以对过长的必读段触发同步索引、排队或显式失败，以控制查询延迟。

查询固定旧快照时，后台可能已经索引并回收其中所需的 WAL。遇到这种情况，查询重读 `CURRENT` 并从新 manifest 重试。GC 还应提供足够的保留期，照顾在途查询；不支持无限期持有旧快照而同时删除其 WAL。

## 8. 后台索引：WAL → manifest + data

后台任务从当前 `C+1` 开始依次读取 WAL，直到第一个不存在的序号或本轮资源上限。它验证每个对象，跳过 fencing 标记，将有效 Append 数据整理为 DBC1 文件，并把已处理批次 ID 写入持久化去重索引。即使这些 WAL 的序号大于 `P`，后台也必须发现并处理它们；这是 `Deferred` 写入的可见性兜底。

输出 DBC1、批次去重索引与新 manifest 持久化完成后，后台读取最新 `CURRENT`，用 CAS 原子发布新 manifest，并将 `C` 推进到本轮连续处理的末尾 `T`，同时设置 `P = max(P, T)`。**这次 CAS 成功就是延迟写入稳定可见的节点**：`≤ T` 的数据已有 data 文件和 manifest 支撑，严格查询从此必须能看到它们，无需等待后续列式 compaction。若 `CURRENT` 被 writer 或另一后台任务推进，重新验证输入范围与 manifest 基础，必要时重建输出再试；不能用旧快照盲目覆盖更晚的状态。

## 9. 列式 compaction：data → data

索引发布后的数据已稳定可见。列式 compaction 可独立合并小 DBC1 文件，仍采用“写不可变输出、写 manifest、CAS 切换 `CURRENT`”的流程。它不改变 `C` 或 `P`，只改变相同逻辑数据的物理布局；不能承担延迟 WAL 的首次发布职责。后台任务可以通过 `CURRENT` CAS 或独立租约减少重复工作，但租约不能代替发布时的 CAS 校验。

## 10. 故障、GC 与一致性边界

| 事件 | 处理规则 |
|---|---|
| WAL PUT 成功但回复丢失 | 读取目标槽位并核对操作 ID、epoch 和校验；确认相同内容后视为已持久化。 |
| WAL PUT 失败且槽位不存在 | 重试同一序号；不得跳号。 |
| WAL 写完但 writer 在发布前退出 | 新 writer 与后台任务按序号发现该 WAL；立即可见请求只有确认 `P` 已推进后才能报告成功。 |
| 两个 writer 竞争同一序号 | 条件创建只允许一个成功；输者读取已存在对象，按 fencing 规则继续或停止。 |
| 列式文件写完但 `CURRENT` CAS 失败 | 文件与 manifest 暂时不可见；后台基于新 `CURRENT` 重算或安全重试，遗留对象由 GC 处理。 |
| `CURRENT` 已发布但 WAL 读不到 | 严格查询重读 `CURRENT` 并重试；若仍缺失，则报告存储或协议错误。 |

只有在新 manifest 已经通过 `CURRENT` 发布、且在途读者保留策略允许时，GC 才能删除 `seq ≤ C` 的 WAL。GC 不能删除 `seq > C` 的未索引 WAL，即使其序号没有出现在 `CURRENT` 中。删除后序号永不复用。孤儿列式文件和旧 manifest 可在确认不被任何受支持的快照引用后清理。LIST 可用于后台 GC 和审计，不参与普通写入、查询或后台索引的连续 WAL 发现。

严格读的原子边界是一次 `CURRENT` 快照。尽力探测段允许看到快照之后完整写入的 WAL，因此不提供同一时刻的事务快照承诺；对需要固定版本的调用方应关闭探测。两个水位、manifest 与批次去重索引必须一起验证，以避免重复行或丢行。

## 11. 存储接口与实现分层

`IMetadataStore` 提供 `CURRENT` 的版本化读取、条件创建与 CAS。WAL 文件需要独立的**不可覆盖创建**接口，不能复用会截断同名对象的 `IStorage::OpenOutput()`。该接口应一次性写入完整对象，返回“已创建／已存在／结果待确认”；调用方可再读取对象以解决结果不明。S3 实现映射为条件 PUT，本地和内存实现也必须对同一路径原子排他。普通 `OpenOutput()` 仍可用于唯一命名的 DBC1 输出。

- writer 负责 epoch、fencing、连续序号、WAL 持久化和可选的 `P` 发布。
- snapshot loader 负责固定 `CURRENT`／manifest，生成必读段和可选探测段。
- query source 负责按序读取 WAL、校验并与列式数据合并。
- 后台索引负责发现未索引 WAL、生成 DBC1、维护批次去重索引、CAS 推进 `C` 和 `P`。
- GC 只依据已经发布的压缩水位和快照保留策略回收 WAL。

## 12. 落地顺序与验收

1. 定义 `CURRENT`、manifest、WAL 头部和写入结果的格式；实现不可覆盖的 WAL 条件创建。
2. 实现单 writer 连续序号、启动扫描、fencing 标记、结果不明时的读回确认，以及跨索引与列式 compaction 的批次去重。
3. 实现 `Immediate` 与 `Deferred` 写入；验证前者成功返回后严格读必见，后者不更新 `P`。
4. 实现严格必读段和可选探测段；先使用固定 `N`，验证缺号、校验失败、并发发布及旧快照重试。
5. 实现后台索引对 `P` 之后 WAL 的发现、DBC1 输出和 `C/P` 原子发布；再加入 `N` 的自适应控制与观测指标。
6. 完成 S3/MinIO 端到端验证：writer 切换、条件 PUT 竞争、崩溃恢复、延迟写入最终可见和 GC 与在途查询竞争。

关键验收不变量是：WAL 序号连续且不可覆盖；`C ≤ P`；严格查询完整覆盖 `(C, P]`；后台索引不遗漏 `P` 后的 WAL，并在 manifest + data 发布后使其稳定可见；一个批次在重试、writer 切换及索引后只生效一次。后续列式 compaction 必须保留这些语义。

## 参考

- [对象存储格式调研](ObjectStorageFormatResearch.md)
- [存储抽象](StorageAbstraction.md)
- [DBC1 文件格式](NativeColumnarFileFormat.md)
- [Amazon S3 数据一致性](https://docs.aws.amazon.com/AmazonS3/latest/userguide/Welcome.html#ConsistencyModel)
- [Amazon S3 条件写](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-writes.html)
- [SlateDB Manifest Design：writer epoch 与 WAL fencing](https://slatedb.io/rfcs/0001-manifest/)
