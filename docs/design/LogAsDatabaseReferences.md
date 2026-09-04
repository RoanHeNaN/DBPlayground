# Log-as-Database：思想源流与本项目对应

> Status: **reference note**（外部文献 + 与本项目设计的映射），非实现规格。
> 目的：为 Cloud 模式"不可变 WAL/commit + CAS 切 CURRENT + 后台 compaction 物化 base"这套设计
> 提供思想出处。关联：[`CloudImportQueryFramework.md`](CloudImportQueryFramework.md)、
> [`CloudTableLayering.md`](CloudTableLayering.md)、[`CloudGarbageCollection.md`](CloudGarbageCollection.md)、
> [`ObjectStorageFormatResearch.md`](ObjectStorageFormatResearch.md)。

## 阅读顺序

1. 建立直觉 → Jay Kreps《The Log》
2. 世界观（不可变 + 物化视图）→ Kleppmann《Turning the DB Inside Out》+ Helland《Immutability Changes Everything》
3. 系统化 → DDIA 第 3 / 5 / 11 章
4. 极致工程落地 → Amazon Aurora 论文

## 文献

### 1. Jay Kreps — *The Log: What every software engineer should know about real-time data's unifying abstraction*（LinkedIn Engineering, 2013）
Kafka 作者，"log 作为统一抽象"思潮的源头。核心：log 是最简单的存储原语（append-only + 全序）；
**State Machine Replication 原理**——相同初始状态 + 相同顺序的相同输入 → 相同结果。
- <https://engineering.linkedin.com/distributed-systems/log-what-every-software-engineer-should-know-about-real-time-datas-unifying>
- **对应本项目**：WAL + `first_cursor/last_cursor` 严格 +1 全序、commit 链定序，就是 SMR 的落地。

### 2. Pat Helland — *Immutability Changes Everything*（CIDR 2015 / ACM Queue）
"The truth is the log. The database is a cache of a subset of the log." 不可变数据 + append-only。
- <https://queue.acm.org/detail.cfm?id=2884038>
- **对应本项目**：base（DBC1）是 WAL 的物化缓存；不可变 WAL/CommitRecord + CAS 切指针。

### 3. Pat Helland — *Life beyond Distributed Transactions: an Apostate's Opinion*（2007 / 2016 重刊）
大规模系统放弃分布式事务，拥抱幂等 + 至少一次投递。
- <https://queue.acm.org/detail.cfm?id=3025012>
- **对应本项目**：`batch_ids` 幂等去重、`AlreadyCommitted`/`RetryableConflict` 重试语义。

### 4. Martin Kleppmann — *Turning the Database Inside Out*（2015 talk + 长文）
把数据库从"全局共享可变状态"翻转成"不断增长的不可变事实流 + 物化视图"。
- <https://martin.kleppmann.com/2015/03/04/turning-the-database-inside-out.html>
- **对应本项目**：base = WAL 的物化视图，compaction = 重新物化；CURRENT 只是可变指针。

### 5. Martin Kleppmann — *Designing Data-Intensive Applications (DDIA)*
第 3 章（LSM/存储引擎）、第 5 章（复制/log）、第 11 章（流处理）。直接引用 Helland "log 即真相"。
若只读一本，读这本。
- **对应本项目**：LSM 的 memtable-flush-to-immutable-file 模型 ≈ WAL→DBC1；快照隔离；compaction。

### 6. Martin Kleppmann — *Is Kafka a Database?*（2019 talk / 辩论）
直接辩 log 能否当数据库用，讲清 log-as-db 的能力边界（缺什么、需要补什么）。
- **对应本项目**：判断"WAL 本身能承担多少数据库语义"、为什么还需要 CommitRecord/CURRENT/catalog。

### 7. Amazon Aurora — *Amazon Aurora: Design Considerations…*（SIGMOD 2017）与 *…Avoiding Distributed Consensus*（SIGMOD 2018）
"the log is the database" 最激进的生产实现：只把 redo log 写进存储，数据页按需从 log 惰性物化。
- SIGMOD'17: <https://www.cs.purdue.edu/homes/csjgwang/CS592DisaggregatedDB/AuroraSIGMOD17.pdf>
- **对应本项目**：写路径"log 先行、数据后物化"的极致形态（对照见提交协议调研）。

## 一句话

本项目"不可变 WAL/commit + CAS 切 CURRENT + 后台 compaction 物化成 base + 幂等重试"的每一处，
都能在上述文献里找到思想出处：log 是真相，表是 log 的物化缓存，提交是对一个小指针的原子切换。
