# 对象存储上的检索/列存格式调研 — turbopuffer & Lance

> Status: **research note**(调研 + 设计取舍),非实现规格。
> 目的:为 `DBC1`([`NativeColumnarFileFormat.md`](NativeColumnarFileFormat.md))
> 演进到对象存储(S3/GCS/Azure Blob)提供外部参考与决策依据。
> 关联:[`ColumnarReadPath.md`](ColumnarReadPath.md)、[`StorageAbstraction.md`](StorageAbstraction.md)。

---

## 0. 背景:Agent 时代对数据 infra 的需求

查询发起者从"人 + BI 工具"变成"LLM Agent",带来几条贯穿性需求:

- **Hybrid 检索**:向量 + 全文(BM25)+ 结构化过滤,最好在一个引擎、一次查询里完成。
- **自描述 + 语义层**:Agent 不了解数据,靠 `DESCRIBE` / 采样 / 元数据探索来理解 schema,格式必须自描述。
- **Agent 友好的交互契约**:结构化报错、成本预估(EXPLAIN)、省 token 的结果(TopN/聚合/近似)、MCP 工具化。
- **成本可控 + 突发并发**:Agent 打出人类 10~100× 的探索查询,且多为"浪费的探索"。

典型场景 → 数据需求映射:

| 场景 | 核心数据需求 | 引擎形态 |
|------|-------------|---------|
| RAG 问答(客服/文档) | Hybrid 检索 + 强过滤 + 多租户隔离 | 搜索/向量引擎 |
| Coding Agent | 倒排 + 向量 + 图,省 token 的 `file:line` 粒度 | 代码专用混合索引 |
| 对话式 BI / 问数 | **语义层** + 自描述 + 成本预估 + 容错报错 | 分析引擎 + semantic layer |
| Deep Research | 联邦查询 + 高扇出并发 + 溯源(citation) | 检索抽象层 / MCP |
| Agentic 运维 | 时序/日志/trace + **新鲜度** + 写事务 + 审计 | HTAP / 可观测性栈 |
| Agent 记忆 | Hybrid + 时间衰减 + 淘汰 | 向量 + 元数据组合 |

其中 `DBC1` 列存最直接对应 **问数 BI** 与 **运维分析** 的底座。

---

## 1. turbopuffer:对象存储优先的 serverless 搜索

**一句话**:把搜索索引直接建在对象存储上,用 NVMe SSD + RAM 做多级缓存,做成 serverless 的向量 + 全文检索引擎。支持 dense vector、BM25、带过滤的聚合。生产规模 2.5T+ 文档、10M+ writes/s、10k+ QPS(Cursor、Notion 在用)。

### 1.1 存储层级的倒置

传统搜索引擎:内存/本地盘是主存储,越多副本越快越可靠,对象存储只是冷备。
turbopuffer 反过来:

```
        查询
         │
   ┌─────▼─────┐   热数据,sub-10ms p50(warm)
   │  RAM 缓存  │
   ├───────────┤
   │ NVMe SSD  │   温数据
   ├───────────┤
   │  对象存储  │   ← 唯一 source of truth(冷/全量)
   │  (S3/GCS) │
   └───────────┘
```

对象存储是唯一权威副本,上面全是缓存。心智模型:**像 JIT 编译器——查得越多越热,越热越快**("pufferfish 膨胀":数据按热度从 S3 充气到 SSD、RAM)。

### 1.2 向量索引:不用 HNSW,用聚类(SPFresh / centroid / IVF 类)

- HNSW 图索引要多跳随机小读,**每跳 = 一次对象存储 GET(几十 ms)** → 灾难。
- 聚类索引"先定位少数几个簇 → 一次拉一大块连续数据",把一次向量搜索压到极少 round-trip,并降低写放大。
- **核心信条:索引结构必须服从底层存储的物理特性。**

### 1.3 写路径 / WAL / 一致性

核心:**对象存储本身就是 WAL,节点无状态,靠"查询时重放 WAL"拿强一致。**

写路径:
```
client write
   │
   ▼
query/write 节点 ──①直接写──► s3://bucket/<ns>/wal/1.bin
   │                                       2.bin  ← 只 append,永不原地改
   │  ②S3 确认持久化(~200ms)               3.bin
   ▼
返回 success ──③(若划算)顺手写进本节点 write-through cache
```

- 每次写 = 往该 namespace 的 `wal/` append 一个新对象;S3 确认 = 已持久化才返回。持久性外包给对象存储,节点不需要副本。
- 单次写 ~200ms(S3 PUT 延迟),靠 **group commit 批量提交** 把吞吐拉到每 ns 数千 writes/s、全局 10M+/s。**写吞吐与写延迟解耦**。
- **算写分离**:query 节点只处理查询 + 写 WAL + 写 cache;独立的 indexing 节点(auto-scaled)异步消费 WAL 建索引(向量/BM25/attribute),结果写回对象存储。

一致性(**默认强一致 / read-your-writes**,反 ES 的最终一致):
```
查询到达
   ├─ 读已建好的 index(可能落后于最新写)
   └─ 把 index 之后那段未索引 WAL 在查询时"重放"叠加上去
   ▼
返回 = index 结果 + WAL 增量  → 立刻看到刚写的数据
```
动机(创始人原话):用过 Elastic,"绕最终一致性太痛苦",没信心让出去。

ACID:有 **A/C/D**(conditional write 原子求值、atomic batch 同时生效),**无 I**(不支持通用读写事务)。

强一致的边界(会"塌陷"):
- >99.8% 查询返回一致数据。
- **单 namespace 未索引写 >128MiB** → 后续写在"索引完 + 装进 cache"前不可见(WAL 太长,重放不划算)。小 ns 几十秒,大 ns 几十分钟。
- scaling/failover 时 ~100ms staleness。
- 想 sub-10ms → 主动降级最终一致(worst case 最多 1 小时 staleness)。

> 模型本质:**WAL 短时重放拿强一致,WAL 长到重放不划算就退化成最终一致。**

### 1.4 减少 S3 GET 的手段(分层堵截)

1. **三级缓存**(RAM→SSD→S3):命中缓存 0 次 GET,是 95% 降本主力。
2. **聚类索引少跳**:见 1.2。
3. **冷查询 ranged read**:query planner 与存储层协同,提前算好 byte-range,合并/并发 range GET,目标 sub-second 冷查询。
4. **group commit**:并发写按 ns 批量成一次提交(代价:单 ns 写吞吐有天花板,历史 ~1 WAL entry/s/ns → 偏爱多 namespace)。
5. **LSM compaction**:size-tiered(指数分层)合并 sorted run,压低每查要读的 run 数(即上文"exponential batching",官方未命名)。
6. **S3 CAS 取代 Raft/ZK**:低频协调(indexing 派发、metadata 更新、leader 选举)用对象存储 compare-and-swap,无 Raft/Paxos/ZooKeeper → 节点完全无状态。

### 1.5 元数据策略:不自建元数据系统

- **一致性检查所需的"最新写"元数据** → 复用 S3 条件请求(`GET IF-NONE-MATCH`)。押注对象存储 metadata 延迟低(S3 p50=10ms/p90=17ms,GCS p50=12-18ms)且会随云厂商进步继续降。
- **索引/表元数据** → 落对象存储,靠三级缓存常驻,更新走 S3 CAS。无独立 catalog/coordinator。
- 代价:一致性延迟被绑在 S3 metadata 延迟上。

### 1.6 权衡

| 优点 | 代价 |
|------|------|
| 成本比内存型向量库低约 10× | 冷 namespace 查询有**延迟地板**(首查从 S3 拉,几百 ms) |
| 无限扩展、极高持久性(靠 S3) | **尾延迟不可预测**,尤其 Agent 激进并行扇出下 |
| Serverless,不用管节点类型/分片 | 写到可查有**索引延迟**(异步),实时靠合并 WAL 兜底 |
| 多租户成本模型极优(冷 ns ≈ 零成本) | 不适合"全量热、要求稳定个位数 ms"的场景 |

**多租户是杀手级特性**:每个租户/对象 = 一个 namespace = S3 上一组独立对象,设计目标百万级 namespace,**不活跃 ns 几乎零成本**。
- Cursor:每个 repo = 一个 ns(同时几千万个),打开 repo 时 hydrate 缓存,成本砍 95%。
- Notion:100 亿+ 向量散在百万 ns。

**注意**:对象存储路线正在被抄(Pinecone、Amazon OpenSearch Serverless、LanceDB 都转向 object-storage-native),turbopuffer 的技术独特性在稀释,护城河更多在多租户成本模型的工程成熟度。

---

## 2. Lance:为随机访问 + 对象存储设计的列存格式

**一句话**:Arrow-native 的列存容器,为云对象存储、随机访问优化;**刻意把 statistics / 搜索结构留在文件格式之外**,让它们作为独立索引演进。

### 2.1 关键设计决策:文件格式与索引解耦

- Parquet/ORC:zone map / stats **焊死在 footer 的 row-group metadata** 里。列多时 footer 巨大,打开文件就要读一大坨;统计结构想演进就得改格式。
- Lance:**文件 footer 只描述"page 在哪、怎么编码"**;zone map / stats 做成**表级的一等索引对象**,独立存储、独立版本化、事务协调。
- 好处:① 统计信息独立演进,不改文件格式;② 不建就不占 footer;③ 索引可用不同粒度/结构,不被文件布局绑死。

### 2.2 Page 布局:为少 IO、随机读设计(减 GET 核心)

- **不用 row group**,改用 **page**:每列自己决定 page 数,列数据保持大块连续,scanner 分区不与物理布局耦合。
- **page 目标几 MB**:"大到值得为它单独发一次 IO,即便在云存储上"(与 turbopuffer"一次拉一大簇"同一原则)。page 越大 IO 越少,但写入内存占用越高。
- 结果:拉连续行区间只需少量、可预测的 IO 次数。论文:100M 数据、NVMe 上随机访问比 Parquet 快最多 **60×**,顺序扫描持平。

### 2.3 Zone Map:独立 scalar index(即"稀疏索引")

- 切成固定大小 zone,每 zone 存 **min/max/null_count**,查询时排除不可能命中的 zone(谓词下推 / scan pruning)。
- **非精确(inexact)过滤**:能确定性排除,但可能假阳性,需回读校验。
- 与传统列存稀疏索引本质一致(只裁剪、不定位精确行),差别在 **Lance 把它抽出文件格式做成可插拔索引层**。

### 2.4 其它随机访问优化

- **Struct packing**:把 struct 的多个子列打包成一列存,取整包一次 IO 拿到多列,随机访问 IOPS 下降。代价:失去 struct 内部列投影(要么整取要么不取)——**用读放大换请求数**。
- **Page 内编码元数据内联**:如字符串 FSST 压缩 + 长度 bitpack,符号表放在该 page 的 metadata 里,解码自包含,无额外 round-trip。

---

## 3. 三者对照

| | turbopuffer | Lance | `DBC1`(现状) |
|---|---|---|---|
| 范式 | 搜索引擎(topK+过滤) | 列存文件格式(扫描+随机点查) | 列存文件格式(扫描) |
| 减 GET 主力 | 三级缓存 + 聚类索引少跳 + ranged read 合并 | 大 page(几 MB)+ struct packing + 少 IO 布局 | 尚未面向对象存储 |
| 元数据 | 不自建,外包给 S3 metadata + CAS + 缓存 | footer 只描述 page 位置/编码,**stats 抽到独立索引** | footer 存 ColumnMeta,**无 stats、schema 不入文件** |
| 稀疏索引 | ❌(靠 centroid/attribute index) | ✅ Zone map,**独立 scalar index** | ❌(设计文档标注 TODO) |
| 哲学 | 存储物理特性决定索引结构 | 文件格式与索引解耦,各自独立演进 | — |

---

## 4. 对 `DBC1` 的改造建议

S3 三条铁律 → 改造出发点:
1. 每次 GET 有固定延迟(~20-100ms)且按请求收费 → **请求数要少,单次读一大块**。
2. 对象不可变、不能 append → **写模型必须变**。
3. 没有廉价随机小读 → **顺序大块读才快**。

现状问题:一次 scan 的读尾序列(`ReadAt(size-4)` magic → `ReadAt(size-12)` footer_size → 读 footer → 每投影列一次 `ReadAt`)在本地盘几乎免费,**在 S3 上是 3+N 次 GET,每次几十 ms**。

改造清单(按性价比排序):

| 优先级 | 改动 | 解决的 S3 问题 |
|--------|------|---------------|
| ★★★ | **加 page/column stats(min/max/null_count)** | 让整段数据"不用读" = 省 GET |
| ★★★ | **schema 持久化进 footer** | 冷节点自描述,无状态前提 |
| ★★★ | **一次尾读拿回整个 footer**(如 `GET Range: bytes=-65536`) | 消灭"打开文件就 3 次 GET" |
| ★★ | **row group + page index,page 对齐 ~512KB-1MB** | 平衡请求数 vs 读放大 |
| ★★ | **不可变文件 + manifest + 原子提交(S3 CAS/If-None-Match)** | 适配 S3 不可变、支持写、后台 compaction |
| ★★ | **coalesced + 并行 range read** | 相邻列 page 合并成一次 GET;不相邻列并发,把 N 跳压成 1 跳延迟 |
| ★ | **page cache + prewarm 接口**(footer/page-index 优先常驻) | 对冲冷启动尾延迟 |

写模型(对应 turbopuffer WAL):**表 = 一堆不可变 `DBC1` 文件 + 一个 manifest**。每次写产出新文件(永不改旧文件),manifest 列出当前文件集合 + 文件级 stats;查询先读 manifest 用文件级 stats prune;原子提交靠切换 manifest 指针;后台 compaction 合并小文件。

**贯穿哲学**:在对象存储上,**减少请求数 > 减少读字节数**,而**"根本不发请求"(靠 stats 裁剪 + 缓存 footer)> 一切**。

### 4.1 待决策的架构分叉:stats 放哪

| | Parquet 路线(stats 进 footer) | Lance 路线(stats 独立索引) |
|---|---|---|
| 位置 | min/max/null 焊进文件 footer / page-index | footer 只放 page 位置/编码;zone map 做成独立索引对象 |
| 优点 | 简单、自包含、单文件即可裁剪 | footer 极简;索引可独立加/删/换粒度;格式可先稳定 |
| 缺点 | 列多时 footer 变大;统计结构演进要改格式 | 多一层索引对象管理;需 manifest/catalog 协调 |

**倾向**:考虑到 `DBC1` 已有 catalog TODO 和 manifest 规划,**Lance 路线与本项目架构更契合**——文件格式管字节布局,manifest/catalog 层管 zone map 和文件级 stats。文件格式得以稳定,而"是否建稀疏索引、建多细"成为上层可插拔决策。

---

## 5. 参考来源

- turbopuffer: fast search on object storage — <https://turbopuffer.com/blog/turbopuffer>
- turbopuffer Architecture / Tradeoffs / Guarantees docs — <https://turbopuffer.com/docs/architecture>
- Object Storage-First Vector Database Architecture (Jason Liu) — <https://jxnl.co/writing/2025/09/11/turbopuffer-object-storage-first-vector-database-architecture/>
- How Turbopuffer Serves 2.5 Trillion Vectors on S3 (Ajay Edupuganti) — <https://ajay-edupuganti.medium.com/how-turbopuffer-serves-2-5-trillion-vectors-on-s3-7d7ab7f9a7fa>
- What I learned building a vector database on object storage — <https://blog.karanjanthe.me/posts/vecpuff/>
- Lance file format 官方文档 — <https://lance.org/format/file/>
- Lance — Zone Map scalar index — <http://lance.org/format/index/scalar/zonemap/>
- Lance: Efficient Random Access in Columnar Storage through Adaptive Structural Encodings(论文) — <https://maxnilz.com/papers/Lance%20Efficient%20Random%20Access%20in%20Columnar%20Storage%20through%20Adaptive%20Structural%20Encodings.pdf>
