Clickhouse 存储层设计目标优化场景：大批量写入 + 大规模数据扫描
根据这个目标，Clickhouse 的存储引擎遵守了严格的设计约束：
1. 数据 part 是不可变的
2. 所有的变更操作通过后台 merge 进行
3. 列式存储 + 按列压缩
4. 无主键索引，无MVCC事务
在这些约束下，Clickhouse 想要完成数据更新就只有一种方式：
后台异步重写整个受影响的 part，查询时默认重写未完成，扫描全量数据后在计算层完成数据更新，确保更新语意正确。

在两种具体的更新场景下解释 Clickhouse 的实现：
1. 所有列更新
```
CREATE TABLE user_profile (
      user_id   UInt64,
      name      String,
      city      String,
      level     UInt8,
      updated_at DateTime
  ) ENGINE = ReplacingMergeTree(updated_at)   -- 按 updated_at 取最新
  ORDER BY user_id;
```
一张用户画像表，记录哪位用户在什么时间做了什么事情，更新方式
```
 -- "更新" user 100，不是 UPDATE，而是插入一条完整新行
  INSERT INTO user_profile VALUES (100, 'Roan', 'Beijing', 5, now());
```
```
-- 后来又更新
INSERT INTO user_profile VALUES (100, 'Roan', 'Shanghai', 6, now());
```
两次写入，两个数据文件（part），如果后台 merge 没有执行，那么此时两行数据同时存储在，如果 merge 完成，那么新的 part 里面只有 1 行新数据。
对查询的影响：
```
SELECT * FROM user_profile;
```
用户期望看到的结果是最新的一行，Clickhouse 查询引擎是默认存储层已经完成了合并做的，因此如果实际上 merge 没有发生的话，那么用户将会看到两行数据。
如果用户要求更新语意完整，那么有两种方式实现这一点
```sql
SELECT * FROM user_profile FINAL;
```
FINAL 关键字强制存储层完成一次 merge。此时查询能够确保更新语意正确，但是代价很明显：每次查询都进行一次 merge 在生产上是不现实的。
因此比较现实的做法是在查询时在计算层进行数据合并，那么用户的sql就需要改成
```sql
SELECT user_id,name,argMax(upated_at) FROM user_profile;
```
此时查询层在扫描数据的时候必须扫描所有的行,后 GROUP BY user_id，对于每一个 user_id 取 updated_at 最大的那一行（这正是需要用户改写SQL的原因）。并且，由于更新语意的存在，Clickhouse无法完成谓词下推，必须扫描全量数据，在内存中完成数据更新后，才能进行谓词计算。因此更新模型下 Clickhouse 的查询性能也受到严重影响。
2. 部分列更新
订单状态表，记录订单随业务流程的变化，业务上只想更新 status 字段，amount 等其他列保持不变。沿用第一种场景相同的插入模型来建模：
```sql
CREATE TABLE orders (
      order_id   UInt64,
      user_id    UInt64,
      amount     Decimal(10,2),
      status     String,
      updated_at DateTime
  ) ENGINE = ReplacingMergeTree(updated_at)  -- 按 updated_at 取最新
  ORDER BY order_id;
```
初始数据：
```sql
INSERT INTO orders VALUES (123, 1, 99.00, 'pending', now());
```
直觉上，"只更新一列"应该只写 status 这一列：
```sql
-- 期望：只更新 status
INSERT INTO orders (order_id, status, updated_at) VALUES (123, 'shipped', now());
```
但这在 ReplacingMergeTree 下是错的。ReplacingMergeTree 的合并语意是**整行替换**：merge 时对相同 ORDER BY key 的多行，只保留 version（这里是 updated_at）最大的**那一整行**，并不会把不同版本里的列拼接合并。上面这条 INSERT 没有写 amount，amount 会取默认值 0，merge 完成后 order 123 的 amount 就被这一行的 0 覆盖丢掉了。

因此在 ClickHouse 里，想"只更新一列"，实际上必须做一次 read-modify-write：
1. 先把该行完整现值读出来（read）
2. 在应用层把 status 改成新值（modify）
3. 再把**整行**重新 INSERT 回去（write）
```sql
-- 必须提供完整行，只是把 status 换成新值
INSERT INTO orders VALUES (123, 1, 99.00, 'shipped', now());
```
也就是说：**ClickHouse 的 ReplacingMergeTree 并不真正支持"部分列更新"，它会退化成"整行更新"**。查询侧和第一种场景完全一样——不写 FINAL 就可能读到多行，要么用 `FINAL` 强制合并，要么用 `argMax(col, updated_at)` 在查询层按行取最新，同样无法谓词下推、必须扫全量。

这正是与 Doris 的关键差异：Doris 的 Unique Key（Merge-on-Write）模型**原生支持部分列更新**，业务只需写入变化的 status 一列，其余列由存储层自动保留原值，既不需要应用层 read-modify-write，也不会误覆盖其他列。
