# 多租户对象存储布局 — namespace + manifest + CAS(turbopuffer 式)

> Status: **design proposal**(未实现)。
> 目标:让 DBPlayground 往 turbopuffer 方向看齐——**对象存储优先、节点无状态、
> 按 namespace 隔离多租户**,且**热路径不依赖 S3 LIST**。
> 这篇补齐 [`StorageAbstraction.md`](StorageAbstraction.md) 里标注为 TODO 的
> **Table Format 层**(Iceberg 式多文件元数据/快照)与 **Catalog** 职责——但用
> "每 namespace 一个 manifest 对象 + CAS"取代中心化 catalog 服务。
> 数据文件沿用 [`NativeColumnarFileFormat.md`](NativeColumnarFileFormat.md) 的 `DBC1`。

---

## 0. 设计立场:为什么不用中心化 catalog

真正的对立不是 "prefix vs catalog",而是**元数据放哪、谁来协调原子提交**:

| 方案 | 元数据在哪 | 原子提交 | 无状态? | 结论 |
|------|-----------|---------|---------|------|
| 纯 prefix + LIST | 无,靠枚举 | 做不到 | 是 | ❌ 热路径要 LIST,不可用 |
| **本设计(turbopuffer 式)** | 每 ns 一个 S3 对象 | S3 CAS | **是** | ✅ 无状态 + 不靠 LIST |
| 中心化 catalog | 独立服务/DB | catalog 事务 | 否 | 功能全,但百万 ns 下是瓶颈/单点 |

**关键洞察:不需要中心化 catalog 也能不靠 LIST。** 把"每 namespace 的元数据"
物化成对象存储里的一个 `CURRENT` 指针 + 不可变 manifest,用**条件写(CAS)**做
原子切换,用**确定性命名**做发现。它等于一个分布式的、无状态的 per-namespace
mini-catalog。这正是 turbopuffer 的做法:无 Raft / Paxos / ZooKeeper。

热路径为什么不能用 LIST:分页每次最多 1000 key、单次几十~几百 ms、比 GET 贵。
放进 "sub-second 冷查询" 目标里是灾难。LIST 只在**维护/GC/灾难恢复**这种冷路径用。

---

## 1. 对象存储布局(每 namespace 一个前缀)

```
bucket/
  <namespace>/                         ← 租户隔离边界 = 前缀。冷 ns ≈ 几个对象,近零成本
    CURRENT                            ← 极小指针对象,CAS 的唯一目标(见 §3)
    manifest/
      manifest-<uuid>.json             ← 不可变 manifest 快照(每次提交产生一个新的)
      manifest-<uuid>.json
    wal/
      00000001.wal                     ← append-only,确定性顺序命名(见 §5)
      00000002.wal
    data/
      data-<uuid>.dbc1                 ← 不可变 DBC1 列文件(flush/compaction 产物)
      data-<uuid>.dbc1
    index/                             ← 可选,未来 zone map / 向量索引(Lance 式独立索引层)
      zonemap-<uuid>.idx
```

原则:
- **一切数据文件不可变**(对象存储不能原地改、不能 append)。写 = 产新文件。
- **`CURRENT` 是唯一可变对象**,且只通过 CAS 改。它是这个 namespace 的"根指针"。
- **文件用 uuid 命名**(除 WAL),这样并发写各自产文件永不撞名;谁的提交生效由
  CURRENT 的 CAS 裁决,没胜出的文件成为孤儿,由 GC 清理。

---

## 2. Manifest 格式(per-namespace 的 mini-catalog)

一个不可变 JSON 对象,描述"当前这个版本由哪些文件组成 + schema + 裁剪所需 stats"。
读一次 manifest 就知道要读哪些 `DBC1` 文件——**不需要 LIST**。

```jsonc
{
  "format_version": 1,
  "namespace": "acme",
  "version": 43,                       // 单调递增,便于人读/排错;权威性由 CAS 保证
  "parent": "manifest-<prev-uuid>.json",
  "schema": {                          // ← schema 持久化进元数据(DBC1 现状缺,这里补上)
    "fields": [
      { "name": "id",  "type": "Int64"  },
      { "name": "ts",  "type": "Int64"  },
      { "name": "msg", "type": "String" }
    ]
  },
  "data_files": [
    {
      "path": "data/data-<uuid>.dbc1",
      "row_count": 100000,
      "row_range": [0, 100000],        // 用于按位置定位(配合 ColumnarReadPath 的 ReadRange)
      "column_stats": [                // ← 文件级 stats,先 prune 掉整个文件(见 StorageAbstraction 的 stats 分叉)
        { "field": "id", "min": "1",          "max": "100000",     "null_count": 0 },
        { "field": "ts", "min": "1699999999",  "max": "1700086399", "null_count": 0 }
      ]
    }
  ],
  "wal_applied_seq": 17,               // WAL 已折叠进 data_files 的水位(见 §5 一致性)
  "created_at_hint": "..."             // 仅信息用(不参与正确性;时间戳由外部传入)
}
```

设计要点:
- **stats 走 Lance 路线**:放在 manifest(表元数据层),不焊进 `DBC1` footer。
  文件格式保持稳定,"要不要建 zone map、建多细"是上层可插拔决策。文件级 stats
  先 prune 整个文件;更细的 zone map 未来放 `index/`。
- manifest 只增不改。版本 43 引用版本 42 的大部分文件 + 新增文件,是**增量快照**。

---

## 3. `CURRENT` 指针 + CAS(原子提交的核心)

`CURRENT` 是个极小对象,内容就是"当前生效的 manifest 是谁":

```jsonc
{ "version": 43, "manifest": "manifest/manifest-<uuid>.json" }
```

- **读**:一次 `GET CURRENT` → 拿到 manifest 路径 + 该对象的 **version tag**(S3 ETag /
  GCS generation)→ 再一次 `GET manifest-<uuid>.json`。**打开一个 namespace = 2 次 GET**,
  零 LIST。
- **写(原子提交)**:`PutIfMatch(CURRENT, 新内容, expected_tag)`——仅当 `CURRENT`
  的当前 tag 等于我读到的 tag 时才写成功。这就是乐观并发:两个写者只有一个能赢。

CAS 落在不同对象存储上的原语(抽象成 §6 的 `IStorage` 接口):
- **S3**:`If-Match: <etag>` 条件 PUT(2024 起支持)、`If-None-Match: *` 创建即写。
- **GCS**:`x-goog-if-generation-match`(长期支持)。
- **Azure Blob**:`If-Match` ETag 条件。
- **Local/Mem**(测试用):进程内锁 + 版本号模拟 CAS。

---

## 4. 提交流程(乐观并发 + 重试)

```
写者:
 1. GET CURRENT              → (version V, manifest M_v, tag T)
 2. GET M_v                  → 当前文件集合 + schema + wal_applied_seq
 3. 产出新 data 文件         → PutIfAbsent("data/data-<uuid>.dbc1", ...)   (uuid,不撞名)
 4. 构造 manifest V+1        → 引用(旧文件 ∪ 新文件),写:
                               PutIfAbsent("manifest/manifest-<uuid2>.json", ...)
 5. CAS 切换根指针:
        PutIfMatch(CURRENT, {version:V+1, manifest:M_{v+1}}, expected_tag=T)
      ├─ 成功 → 提交完成,V+1 生效
      └─ 失败(别人先提交了)→ 回到步骤 1,把本次变更 rebase 到更新的 manifest 上,
                              重新写 manifest-<uuid3> 再 CAS。
                              步骤 4 写的孤儿 manifest / 步骤 3 的孤儿 data 文件留给 GC。
```

性质:
- **原子性**:生效与否只取决于 CURRENT 的一次 CAS;要么整批文件可见,要么完全不可见。
- **无锁、无协调服务**:并发靠 CAS 裁决,失败者重试(rebase)。对应 turbopuffer 的
  "S3 CAS 取代 Raft/ZK"。
- **孤儿文件**:CAS 失败者写出的文件无人引用 → 后台 GC 用 LIST(冷路径,可容忍慢)
  找出"不被任何存活 manifest 引用"的对象删除。

---

## 5. WAL + 强一致读(可选,turbopuffer 写路径)

> 若本轮只做"存储布局 + manifest + 原子提交",可跳过本节;WAL 是加上"高频写 +
> read-your-writes"时的增量。放在这里让布局一次性设计到位。

### 5.1 WAL 里装什么:变更操作,不是索引结果

关键区分:**WAL 存逻辑变更(mutation),不是建好的列存/索引**。这样写入只需 append,
把"整理成列式 + 建 stats"这件贵活儿推给后台。一个 WAL 文件 = 若干自描述 entry:

```
wal/<seq>.wal  (append-only,一个文件可含多个 batch)
┌───────────────────────────────────────────────────────────┐
│ WalEntry:                                                    │
│   seq            u64   全局递增序号                            │
│   op             u8    Append / Delete                       │
│   schema_version u32   指向 manifest 里的 schema(演进用)       │
│   row_count      u64                                         │
│   payload:                                                   │
│     Append → 一个序列化的 Chunk(复用现有 Chunk/Column/Value)   │
│     Delete → 受影响的 key 列表 / 谓词(墓碑 tombstone)          │
│   crc32          u32   末尾校验(截断/半写检测)                 │
└───────────────────────────────────────────────────────────┘
```

设计取舍:
- **payload 用行式,复用现有 `RowCodec`**(不用 `DBC1` 列式)。WAL 目标是"写得快、
  条目小、易 append",列式批量压缩优势在小 batch 上用不上;重放/compaction 时再转列。
  → **写走行式 log,读走列式,后台把行转列**,正好呼应 repo 里 row/columnar 两个
  `IFileFormat` 并存的设计。
- **update = delete + append**;delete 落**墓碑**,不原地删(data 文件不可变)。读时
  做 merge-on-read(§5.4)。

写路径(高频写,不每次都提交 manifest):
```
写者 append:PutIfAbsent("wal/<seq+1>.wal", batch)   ← seq 确定性递增,S3 确认=已持久化
```
WAL 写比 manifest 提交频繁得多;用确定性顺序命名,不用每条都改 CURRENT。

### 5.2 后台合并层级 1:WAL → DBC1(flush / 异步索引)

```
后台 indexing 节点(无状态,任意节点可做):
  读 wal/<wal_applied_seq+1 .. N>.wal
  → 行式 batch 转列式,建文件级 stats(min/max/null)
  → 写 data/data-<uuid>.dbc1
  → 提交新 manifest(§4 的 CAS):data_files += 新文件,wal_applied_seq = N
  → 被折叠的 WAL 文件之后可 GC
```
把"行式增量日志"变成"列式可分析文件" = turbopuffer 的异步索引。做完后查询重放的
WAL 长度归零。

### 5.3 后台合并层级 2:DBC1 → 更大的 DBC1(compaction 本体)

flush 会产出很多**小 DBC1 文件**(每次 flush 一个)。小文件多 = 每次查询打开的文件多
= **GET 请求数爆炸**(对象存储头号成本)。size-tiered 合并:
```
  挑一批小 data 文件 + 相关墓碑
  → 归并、应用 delete(墓碑对应行真正丢弃)、重排、重新分 row group、更新 stats
  → 写一个大 DBC1 文件
  → 提交新 manifest:data_files 用大文件替换那批小文件
  → 旧小文件成孤儿,GC
```
干三件事:**① 减少文件数(降 GET)② 物化删除(merge-on-read → copy-on-write)
③ 重组 row group / 更新 stats 让 pruning 更有效**。即 LSM 的 size-tiered compaction。

**并发安全**:compaction 产出也走 §4 的 CAS 提交;它只是"换一批文件的等价表示",
不改变逻辑数据,所以 CAS 失败时**直接丢弃重来永远安全**——这是它能无锁并发的根本。
触发条件:未折叠 WAL 字节超阈值 / 小文件数超阈值 / 定时。

### 5.4 读路径:merge-on-read

任意时刻一个 namespace 的完整数据 =
```
  已 compact 的大 DBC1  +  未 compact 的小 DBC1  +  未 flush 的 WAL  −  生效中的墓碑
```
查询做 **merge-on-read**:三部分合并后扣掉墓碑命中的行。compaction 就是把"读时合并"
的代价逐步搬到后台"写时合并",让读越来越便宜。

强一致读(read-your-writes,不用 LIST):
```
查询:
 1. GET CURRENT → GET manifest → 拿到 data_files + wal_applied_seq = A
 2. 读 data_files(用 column_stats 先 prune)
 3. 重放尚未折叠的 WAL:从 seq = A+1 开始,GET wal/<A+1>.wal, <A+2>.wal ...
    直到 404 为止(确定性命名 → 逐个 GET,零 LIST)
 4. 结果 = data 文件结果 ⊕ WAL 增量  → 立刻看到刚写的数据
```
- 未折叠 WAL 数量有上界(compaction 跟进);超过阈值(turbopuffer 是 128MiB)就退化
  为"必须等索引完才可见",对应它的强一致塌陷边界。
- 想要 sub-10ms 可跳过步骤 3,降级最终一致(worst case 落后一个 compaction 周期)。

### 5.5 并发写:两个流派 + 收敛方案

一个 namespace 被多节点并发写时,"给写建立全局顺序"必然需要一个**串行化点**,冲突
不可消除,只能选择放在哪。业界有两派现成实现(都在纯 S3 + CAS 上做到无中心节点):

| | **SlateDB 派**(WAL + fencing) | **WarpStream 派**(先落后排) |
|---|---|---|
| 写模型 | **单写者**,epoch fencing 排他 | **无 leader**,任意节点并发写 |
| 顺序 | WAL 顺序写(SST ID 递增) | 先落 S3,后 sequencing(land first, sequence later) |
| 协调点 | manifest CAS + `writer_epoch` | commit 时的 CURRENT CAS / 元数据 store |
| 冲突 | 抢 SST ID,epoch 低者被 fence halt | append 无冲突,冲突推迟到 sequencing |

三个反直觉但关键的结论:

1. **正确性来自 CURRENT 的 CAS,不是来自锁/leader。** 多个节点可同时以为自己该
   compaction:各读 `CURRENT(V,tag T)` → 干活 → `PutIfMatch(CURRENT, V+1, expected=T)`,
   **只有一个 CAS 成功,其余丢弃重来**。因 compaction 产出与输入语义等价,丢弃永远安全。
   ⇒ **无需选举一个"正确的 leader",只需一次"正确的 commit"。** CAS 本身就是分布式选举
   原语,替代 Raft/ZK。租约(`<ns>/LOCK`,带 TTL,best-effort CAS)只是**省重活的优化**,
   选错也不影响正确性。

2. **Fencing 可以免费。** 若每次 commit 都 condition 在 CURRENT 的 tag 上,stale 写者
   (GC 暂停后苏醒)的 CAS 必然失败(CURRENT 已推进)→ **CURRENT 的 tag 就是 fencing
   token**,不需额外机制。只有当写者绕过 CURRENT 直接抢 WAL 槽位(SlateDB 那样)时,才
   需要显式 `writer_epoch`:新写者启动 bump epoch 并往 WAL 写空 SST 把旧写者 fence 掉,
   旧写者下次写时发现更高 epoch 便 halt。

3. **纯导入不需要定序(可交换)。** append-only 的并发写结果是集合并,谁先谁后不影响
   最终数据;只有对同一主键的 delete/update 才需定序。

**收敛方案(DBPlayground 采用)——导入走 WarpStream 派:**
- 每个 writer 写**自己的前缀** `wal/<writer_id>/<seq>.wal`:append **零冲突、零重试、
  每 batch 恰好 1 次 PUT**(直接消除乐观并发的重试风暴与成本)。
- compaction 时抢 `merge.lock` 所有权(CAS);赢家做 compaction + 走 CURRENT-CAS 提交。
- `WalEntry` 带 **`batch_id`**(客户端幂等键):CAS 重试或"PUT 超时但其实成功"这类模糊
  失败下,定序/合并时按 `batch_id` 去重,保证 exactly-once。
- **caveat — 发现与读可见性**:writer 各写各前缀后没有全局顺序。
  - compaction 是**后台冷路径 → 允许 `List(wal/)`** 找齐所有段(热路径才禁 LIST)。
  - 但强一致读要看到"刚导入、未 compact"的数据就得 LIST 所有 writer 前缀 → LIST 上了
    热路径。取舍:**① 导入期接受最终一致**(bulk load 不边导边精确查,推荐);或
    **② 维护轻量 open-segments 注册**(writer 写完段后 CAS append 一条 `{writer,seg,batch_id}`
    到共享列表,读它而非 LIST)——把 WarpStream 的 sequencing 落成一个小共享结构。

### 5.6 compaction 提交与 WAL 删除的原子性

问题:compaction 要"①提交新 manifest(抬高 `wal_applied_seq`)②删除被折叠的 WAL",
这两步怎么原子?**答案:不让它俩原子,降成"一次原子提交 + 一个幂等 GC"。**

- **唯一原子点 = CURRENT 的 CAS 提交。** 一旦成功,`seq ≤ wal_applied_seq` 的 WAL 逻辑
  上已死(数据已在 DBC1 且被 committed manifest 引用),**正确性不再依赖它们存在**。
- **删 WAL = 提交之后独立的、幂等的 GC**,可失败可重来:删晚 = 白占存储;提交后 crash
  没删成 = 孤儿,下轮 GC 收掉;重复删 = no-op。
- **强制顺序:先提交,后删。** 绝不能在数据进 DBC1 + 被 committed manifest 引用之前删
  WAL,否则 crash = 丢数据。
- **GC 只删 `seq ≤ 当前 committed wal_applied_seq` 的 WAL** → 保证任何读者都不会需要一个
  已删的 WAL。

**在途读者竞态**:读者持旧 manifest(`wal_applied_seq=A`)要重放 `A+1`,而 GC 依据新
manifest(`B>A`)已删 `A+1..B` → 读者 GET 404。两种解法(可组合):
- **读者重放遇 404 → 重读 CURRENT**(已推进,数据在 DBC1 里),用新 manifest 重试,自愈;
- GC 留 **grace period / tail buffer**,只删远落后于当前水位的 WAL(= Iceberg expire-snapshot)。

> 这是 Iceberg / Delta / SlateDB 的通用套路:**把两阶段原子难题,降成"一次 CAS 提交
> (权威)+ 幂等 GC(可失败、可重来、由 manifest 推导出可删集合)"。** data 文件的孤儿
> (§4 CAS 失败者产出的)同样走这个 GC。

---

## 6. `IStorage` 需要新增的原语(CAS)

现有接口(见 [`StorageAbstraction.md`](StorageAbstraction.md) §Layer A)只有
`OpenInput / OpenOutput / Exists / List / Delete`,**没有条件写和读回 tag 的能力**。
补上一个最小 CAS 面(沿用仓库错误约定:`bool` + out-pointer 表示预期失败;
硬 IO 错误 `throw std::runtime_error`):

```cpp
namespace dbplay {

// 对象存储的版本标识:S3 ETag / GCS generation / Azure ETag 的不透明封装。
// 空 tag 约定为"对象不存在"。
using StorageTag = std::string;

class IStorage {  // 在现有接口上追加以下方法
 public:
  // ... 现有:OpenInput / OpenOutput / Exists / List / Delete ...

  // 一把读回内容 + 版本 tag(供后续 CAS)。false = 对象不存在(*out/*tag 不动)。
  // 用于 GET CURRENT / GET manifest。硬 IO 错误抛异常。
  virtual bool GetWithTag(const std::string &path,
                          std::string *out, StorageTag *tag) const = 0;

  // 条件写:仅当对象当前 tag == expected_tag 时覆盖写。
  //   expected_tag 为空  → 语义为"仅当对象不存在时创建"(PutIfAbsent)。
  // 成功:写入并把新 tag 回填 *new_tag,返回 true。
  // 失败(tag 不匹配 / 对象已存在):返回 false,不写。硬 IO 错误抛异常。
  // 映射:S3 If-Match/If-None-Match、GCS x-goog-if-generation-match、Azure If-Match。
  virtual bool PutIfMatch(const std::string &path, const Slice &data,
                          const StorageTag &expected_tag,
                          StorageTag *new_tag) = 0;
};

}  // namespace dbplay
```

实现要点:
- **S3Storage**:`PutObject` 带 `If-Match`(覆盖)或 `If-None-Match: *`(创建);
  412 Precondition Failed → 返回 false。`GetWithTag` 读 `ETag` 头。
- **LocalStorage / MemStorage**:进程内 `mutex` + 每路径一个版本号模拟 CAS,供单测。
  这样 §4 的并发提交逻辑在本地就能测(两个线程抢 CAS,验证只有一个成功)。

其余 append-only 写(WAL、data、manifest)用现有 `OpenOutput` 或
`PutIfMatch(path, data, /*expected=*/"" , ...)`(即 PutIfAbsent)即可,不需要新接口。

---

## 7. 与现有分层如何衔接

```
Catalog(逻辑)          ← 不再是独立服务:namespace 前缀 + CURRENT 就是"catalog"
   │
Table Format 层  [新]   ← 本设计:Namespace = { CURRENT, manifest, WAL, data }
   │   Manifest 读写 + 原子提交 + WAL 重放;产出 data_files 列表给下层
   ▼
ITableSource / TableSource(现有)
   │   TableSource(IFileFormat, IStorage, files) —— files 现在由 manifest 提供
   ▼
IFileFormat = NativeColumnarFileFormat(DBC1)(现有)
   ▼
IStorage(现有 + §6 的 CAS 原语)   MemStorage / LocalStorage / (新) S3Storage
```

改动清单:
1. **`IStorage` 加 `GetWithTag` / `PutIfMatch`**(§6),给 Mem/Local 加 CAS 模拟。
2. **新增 `Namespace`(Table Format 层)**:封装 CURRENT/manifest 读写、提交、WAL 重放;
   对上暴露 "打开某版本 → 得到 `TableSource`(files 来自 manifest)"。
3. **schema 持久化进 manifest**(补 `DBC1` 现状缺口:reader 不再依赖进程内 Schema)。
4. **文件级 stats 放 manifest**(Lance 路线),供打开时 prune。
5.(可选,加 WAL 时)**WAL 追加 + 异步 compaction + 查询重放**。
6. **S3Storage 实现**(现在是 stub)。

多租户性质(达成的目标):
- **无状态节点**:任意节点打开任意 namespace = `GET CURRENT` + `GET manifest`,不需
  本地状态、不需中心 catalog。
- **冷租户近零成本**:不活跃 namespace = 几个躺在 S3 上的对象。
- **热路径零 LIST**:发现靠 CURRENT+manifest+确定性命名,LIST 仅用于 GC/恢复。
- **原子提交**:每 namespace 一次 CAS,无跨租户锁。

---

## 8. 建议的落地阶段(每步可测)

- **M1 — CAS 原语**:`IStorage::GetWithTag/PutIfMatch` + Mem/Local 模拟;并发 CAS 单测
  (两线程抢写 CURRENT,断言只有一个成功)。
- **M2 — Namespace + manifest**:写 manifest / `GET CURRENT`→打开 → 产出 `TableSource`;
  schema + 文件级 stats 入 manifest;打开时按 stats prune 掉整文件。
- **M3 — 原子提交**:§4 的提交流程 + rebase 重试;孤儿/WAL 幂等 GC(§5.6,LIST 冷路径)。
- **M4 —(可选)WAL + 并发写 + 强一致读**:per-writer 前缀 append(§5.5)、`merge.lock`
  CAS 抢占 compaction、异步 compaction 抬 `wal_applied_seq`、查询重放 + 遇 404 重读
  CURRENT(§5.6);`batch_id` 幂等;强一致塌陷阈值。
- **M5 — S3Storage**:把 CAS 映射到 S3 If-Match/If-None-Match,端到端多租户跑通。

---

## 9. 参考

- turbopuffer 写路径 / WAL / 一致性 / S3 CAS 取代 Raft:见
  [`ObjectStorageFormatResearch.md`](ObjectStorageFormatResearch.md) §1。
- Lance "stats 抽出文件格式做独立索引"路线:同上 §2、§4.1。
- 现有分层与 `IStorage` 接口:[`StorageAbstraction.md`](StorageAbstraction.md)。
- 数据文件字节布局:[`NativeColumnarFileFormat.md`](NativeColumnarFileFormat.md)。
- **SlateDB 派**(WAL + `writer_epoch` fencing + manifest CAS,形式化验证的单写者协议):
  Manifest Design RFC <https://slatedb.io/rfcs/0001-manifest/>、
  Compaction RFC(`compactor_epoch`)<https://slatedb.io/rfcs/0002-compaction/>。
- **WarpStream 派**(diskless / leaderless Kafka on S3,"先落后排"):
  <https://www.warpstream.com/blog/kafka-is-dead-long-live-kafka>、
  架构 <https://docs.warpstream.com/warpstream/overview/architecture>;
  Kafka KIP-1150 Diskless Topics 背书 <https://jack-vanlightly.com/blog/2025/10/22/a-fork-in-the-road-deciding-kafkas-diskless-future>。
