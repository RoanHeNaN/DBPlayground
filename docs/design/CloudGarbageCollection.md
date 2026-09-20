# Cloud Mode Object Garbage Collection

> Status: **design note; not implemented**。
>
> 本文记录 Cloud 模式下 WAL / commit / compacted-data 对象的回收设计。GC 目前完全未实现（见
> [`CloudImportQueryFramework.md`](CloudImportQueryFramework.md) §6 与
> [`CloudTableLayering.md`](CloudTableLayering.md) §12）。热路径遵守零 LIST 契约，GC 是可用 LIST
> 的冷路径任务。关联：导入/查询路径见 `CloudImportQueryFramework.md`，epoch/CAS/fencing 语义见
> `CloudTableLayering.md` 与 `src/Metadata/TableCurrentStateStore.cpp`。

## 1. 两类垃圾

对象存储上"写模型 = 不可变对象 + CAS 切 CURRENT"必然产生两类垃圾，回收逻辑完全不同：

| 类型 | 产生原因 | 可达性 | 判定难点 |
|---|---|---|---|
| **① 过时 WAL** | 正常提交、已被 compaction 吸收到 compacted data | 曾在 commit 链上，现已在 `compacted_cursor` 之外 | 易判：cursor 水位 + 谁还 pin |
| **② 孤儿 WAL/commit** | 写了对象但 CURRENT CAS 未成功（被 fence、`RebaseRequired` 换 key、崩溃） | **从不可达**，任何 `latest_commit_key` 都到不了 | 难判：孤儿与"在途未 CAS 的合法对象"在存储上无法区分 |

**过时 WAL** 用 `compacted_cursor` 水位 + 最老存活快照低水位（或时间窗口）做引用计数式回收，风险可控，不是本文重点。

**孤儿**才是难点：面对一个未被引用的 `wal/<uuid>.wal`，无法从存储状态区分

```
情况 X（孤儿）  某被 fence 的写者留下，永不会被引用          → 该删
情况 Y（在途）  某活写者刚 PUT 完 WAL、正准备 CAS，还没引用   → 绝不能删
```

若 GC 按"没人引用就删"处理，会误删一个正在提交中的合法 WAL → CURRENT 指向不存在的对象 →
查询缺行，`TableSnapshotLoader` 视为 hard corruption。

## 2. 反模式：把时间宽限期当正确性证明

一个诱人但**危险**的做法：规定"对象老于 `T_grace` 就判定为孤儿并删除"。它成立的前提是

```
T_grace  >  任何一次 "PUT WAL → CAS 完成" 的最大可能耗时（含重试）
```

而右边**没有上界**：GC 停顿 / STW、网络分区、对象存储长尾、进程被换出，都能让一个合法写者
PUT 完 WAL 后卡任意久才 CAS。若某写者卡过 `T_grace` 再复活并完成 CAS，GC 已经删掉它的 WAL →
CURRENT 引用消失对象 → corruption。叠加 GC 节点与写者的时钟漂移会进一步提前误删。

**本质问题**：fencing/CAS 花大力气做到"正确性不依赖时钟"，纯时间宽限期又把"假死 + 时钟"问题
从写路径偷偷放回 GC 路径。

**原则**：
- 时间可以当"更保守地晚点删"的余量，**不能**当"可以删"的唯一证明；
- 时间背后必须垫一个非时间的硬条件（epoch 落差、快照低水位、显式墓碑）作正确性地板；
- GC 宁可漏删（费空间），绝不错删（丢数据）。

下面两节是取代"纯时间推断"的两条因果判据。

## 3. 方案一：把 epoch 编进对象 key（因果判据取代时间）

现状：`writer_epoch` 只记在 **CommitRecord**（`src/Metadata/TableMetadataTypes.h`）里；**WAL 数据文件本身
不带任何 epoch**。因此"有 WAL 但没有 commit 记录"的裸孤儿（写者 PUT 完 WAL、还没
`PutIfAbsent` commit 就崩溃）无法读出 epoch，只能退回时间宽限期——正是第 2 节要避免的。

**改动**：把 epoch 编进 WAL（及 commit）的对象 key：

```
wal/<epoch>/<uuid>.wal
commit/<epoch>/<uuid>.meta
```

这样即使裸孤儿也能从 **key** 读出所属 epoch，无需读内容。回收判据变为因果判据：

```
可回收孤儿 = 不可达（从 CURRENT.latest_commit_key 沿 parent_commit_key walk 不到）
             且 key.epoch < CURRENT.writer_epoch        // 属于已被顶替的死任期
```

**为什么安全**：epoch 单调递增，每次 `AcquireWriter` 都 +1，一个 epoch 只归一个写者任期
（`TableCurrentStateStore.cpp:88-93`）。一旦 CURRENT 的 epoch 前进过某个值，那个更老 epoch 的写者
**再也不可能合法 CAS**（会被判 `Fenced`）——这是 fencing 已经保证的不可逆逻辑序，与时钟无关。

**天然避开"活写者还没接管"**：若老写者 epoch=5 卡住、期间无人接管（CURRENT 仍是 epoch=5），
则 `key.epoch(5) < CURRENT.writer_epoch(5)` 不成立，GC 不删。它复活后仍能合法 CAS 引用自己的 WAL，
不会缺行。只有当确有新任期上位（epoch 前进）时，老 epoch 的未引用对象才被判死。

这是 fencing 机制的一个副产品：**epoch 落差直接给了 GC 一个不依赖时间的安全下界。**

## 4. 方案二：只回收有显式墓碑的对象（零推断）

更保守的路子：**GC 永不主动"推断"孤儿，只回收被显式登记为失败/作废的对象。**

墓碑（tombstone / 作废登记）的两个来源：

- **换 key 时登记**：写者在 `RebaseRequired` / `CommitKeyCollision` 分支换新 commit key 时
  （`CloudTableWriter.cpp:167`、`:194`），把被放弃的旧 commit key（及其 WAL）写入一个"作废列表"。
- **接管时登记**：新写者 `AcquireWriter` 成功后，把它观察到的、属于旧 epoch 的可疑遗留对象显式
  登记为待回收。

GC 只删这个列表里的对象。删的都是**有据可查的死对象**，零推断、零误删风险。

代价：写路径多一步登记；崩溃在"PUT WAL 后、登记前"仍会漏登记，留下无墓碑裸孤儿——这类只能靠
方案一（key epoch）或最保守的超长时间兜底。因此方案一与方案二互补，通常叠加使用。

## 5. 与 dedupe window 的耦合

`batch_ids` 幂等去重（`IBatchCommitResolver`）依赖能查到"最近提交过哪些 batch"，其窗口有界
（`CloudImportQueryFramework.md` §3.1）。这与回收 commit record 冲突：

- dedupe 窗口须覆盖客户端最长重试延迟；
- 因此 commit record（至少其 `batch_ids`）的保留期 **≥ dedupe 窗口 ≥ 客户端最大重试窗口**；
- 生产做法：把老 commit 的 `batch_ids` compact 进 manifest 的 bounded dedupe index，之后才删
  commit record 本身。

这就是路线图把"GC"与"dedupe window"列为同一项的原因——两者共享 commit record 生命周期。

## 6. 执行者与不变量

- GC 是**独立冷路径**任务（可与 compaction 节点合并），不在查询/写入关键路径上；
- 冷路径**可用 LIST**（热路径不可），扫 `wal/`、`commit/` 前缀枚举候选；
- 读 CURRENT 拿 `compacted_cursor` 与 `writer_epoch` 作安全下界；
- 全程**只删对象、不改语义状态**：GC 完全不跑，系统正确性也不受影响，只是费空间。这让 GC 可以
  做得极保守、极懒。

## 7. 汇总

| 垃圾类型 | 删除条件 | 依据（非时间地板） |
|---|---|---|
| 过时 WAL | `last_cursor ≤ compacted_cursor` 且无在途 snapshot pin | `compacted_cursor` + 最老存活快照低水位 |
| 孤儿（有 commit / key 带 epoch） | 不可达 且 `epoch < CURRENT.writer_epoch` | epoch 单调落差（方案一） |
| 孤儿（显式登记） | 出现在作废/遗留列表中 | 墓碑（方案二） |
| 无墓碑裸孤儿 | 仅剩超长保守时间兜底 | ——（应通过方案一尽量消除此类） |
| 老 commit record | cursor 段 < compacted_cursor 且 batch_ids 已进 dedupe index | `compacted_cursor` + dedupe 窗口 |

## 8. 业界调研：其他对象存储系统怎么做

调研了九个生产系统解决"孤儿 vs 在途写者"竞争的安全机制。结论先行：

> **绝大多数系统对"孤儿 vs 在途"这个硬问题最终都退回到时间宽限期**——因为在对象存储上
> "未被引用" ≠ "已废弃",可达性无法区分一个孤儿和一个"马上就要 commit"的在途对象，时间成了
> 唯一的裁决者。**只有自带强一致控制面 / 更强提交原语的系统才逃开时间**：用显式 marker（Hudi）、
> generation/epoch（Neon）、或 CAS 提交点（turbopuffer）。

这正好印证第 2、3、4 节：纯时间危险且是无奈之选；本项目的 epoch/CAS 属于"逃开时间"的少数派路线。

| 系统 | 机制 | 孤儿安全判据分类 | 默认宽限期 | 读者保护 |
|---|---|---|---|---|
| **Iceberg** `remove_orphan_files` | list vs snapshot 可达性 | **可达性 + 时间宽限** | **3 天**（孤儿）/ 5 天快照 | 保留被引用快照 |
| **Delta Lake** `VACUUM` | tombstone(remove action) + 可达性 | **墓碑/可达性 + 时间宽限** | **7 天** `deletedFileRetentionDuration` | 惰性删；<7 天被 check 拦截 |
| **Hudi** cleaner + markers | marker(孤儿) / 保留 commit 数(旧版本) | **marker/墓碑**（+ 计数） | 计数 **10 commits**；marker 无时间 | 保留 commit + savepoint |
| **turbopuffer** | CAS 串行化 WAL 提交 + compaction | **epoch/CAS** + 可达性 | 无（CAS 使未提交天然不可达） | CAS 提交点强一致 |
| **Neon** | generation 编进 key + 删除队列校验 | **generation/epoch** + 可达性 | 无（7 天 PITR 是读者窗口） | PITR + 子分支可达性 |
| **WarpStream / Freight** | 元数据存储可达性 + 延迟删除 | **可达性 + 时间延迟删除** | 延迟窗口（未公布默认值） | 物理删除前的时间延迟 |
| **Snowflake** | Time Travel → Fail-safe → purge | **时间**（两段窗口） | **8 天**(Std)–**97 天**(Ent) | 元数据版本快照隔离 |
| **Doris 存算分离** | Recycler + FDB(TxnKv) mark-for-delete | **committed: 可达性/墓碑；tmp: 时间宽限** | tmp rowset = txn 超时 + `retention_seconds` **3 天** | `compacted_rowset_retention_seconds` **30 分钟** |
| **Lance/LanceDB** | manifest MVCC + `cleanup_old_versions` | 旧版本: 可达性+时间；partial write: **时间** | **7 天** `older_than` | 保留旧 manifest + 当前/tag 版本 |

四个独立系统的默认宽限期收敛到 **7 天**（Delta / Databricks / Snowflake Fail-safe / Lance），Iceberg/Doris 是 **3 天**——可作为本项目"最保守时间兜底"取值的参考。

### 与本项目最像的两个：Doris 与 Neon

**Doris 存算分离**(3.0 GA)和本项目架构几乎同构,值得重点对照:
- **元数据源** = FoundationDB(TxnKv),对应本项目的 `IMetadataStore`;MS(Meta Service)对应
  `TableCurrentStateStore`;**Recycler** 就是这里要设计的冷路径 GC。
- **committed 数据**:rowset 是否存活由 FDB 里的可达性决定;失效时 MS 写一条 **recycle KV(=墓碑)**,
  且"写墓碑"和"改状态"在**同一个 FDB 事务**里原子完成——这正是本项目方案二(显式墓碑)的成熟形态,
  FDB 的多 key 事务让它比我们单 key CAS 更省心。
- **tmp rowset(在途/中止的导入)**:两阶段提交(prepare 写 tmp rowset → commit 提升为正式 rowset),
  tmp rowset 带 `txn_expiration`。Recycler 判据 = `current < txn_expiration + retention_seconds`
  (默认 **3 天**)——**这就是时间宽限期**,和 Iceberg/Delta 同构。
  **说明:Doris 用 FDB 拿到了 committed 状态的可达性保证,但对"还可能 commit 的 tmp 对象"依然退回时间。**
- **删除顺序**:先删对象、再删 KV(与本项目 §6 一致,崩溃可重试、不留悬空指针)。
- **读者保护**:MVCC 版本 + `compacted_rowset_retention_seconds`(默认 **1800s/30 分钟**);查询超过此
  窗口会 "404 file not found"——即纯时间保护,非引用计数 pin,对应本项目"过时 WAL + 快照低水位"那节。
- 教训:PR #47324 的孤儿泄漏 bug 说明——**GC 正确性靠精心协调 marker KV,而非自纠正的引用计数**。

**Neon** 是"逃开时间"最彻底的样本,和本项目 fencing 思路完全一致:
- **generation number** 由控制面单调递增(只有控制面能加),编进每个对象 key(`index_part.json-<gen>`)
  ——**和本项目方案一(epoch 编进 key)是同一招**。
- 删除走**删除队列 + 强制校验**:删前回控制面确认该 generation 仍是最新,过期 pageserver 的删除被拒。
  这样僵尸 pageserver 既不能写坏、也不能删坏别人依赖的对象——**孤儿安全判据里零时间**。
- 它的 7 天只用在 PITR(读者保留窗口),不用于孤儿竞争。

**启示**:本项目 epoch/CAS 路线可以做到 Neon 级别的"孤儿安全无时间";而对"没有 commit 记录的裸孤儿",
连 Neon 也是靠 key 里的 generation(方案一)——再次印证方案一的必要性。时间只应作为方案一/二覆盖不到
的最后兜底,取 3–7 天量级。

citations:Iceberg maintenance/spark-procedures docs;Delta VACUUM/table-properties + VLDB 2020;
Hudi markers/hoodie_cleaner;turbopuffer architecture/object-storage-queue;Neon RFC 025 generation-numbers;
WarpStream GC blog;Snowflake time-travel/fail-safe;Doris compute-storage-decoupled/recycler + PR#47324;
Lance table/transaction docs。

## 9. 待决策 / 后续

- 接口草案：`ICloudGarbageCollector` + 安全水位计算，接入 `TableCurrentStateStore` 与 snapshot 机制；
- key 是否统一编码 epoch（方案一；参考 Neon 的 generation-in-key 与 Doris resource-id）；
- 作废列表 / recycle 标记的存储形态（独立对象？manifest 的一部分？参考 Doris 的 recycle KV）与其自身回收；
- 时间兜底取值：参考业界 3–7 天收敛值，且仅用于方案一/二覆盖不到的裸孤儿。
