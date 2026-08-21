# DBPlayground

一个用 C++17 编写的数据库内核实验场。项目从 CMU 15-445 的课程项目出发，持续探索存储引擎、页式文件、列式读取和可组合存储抽象的实现方式。

它适合用来阅读、验证和演进数据库内核设计，不是面向生产环境的数据库产品。目前的主线是：一个可持久化的 B+Tree KV 引擎，以及围绕 `ITableSource` 建立的行式/列式批量读取路径。

## 当前能力

### 持久化 KV 引擎

- `MiniKV` 提供 `Insert`、`Get`、`Remove` 和 `Close`。
- `BPlusTreeEngine` 将 B+Tree、`TupleStore`、`BufferPoolManager` 和 `DiskManager` 组合起来。
- 引擎边界使用 `Slice` 和字节串；调用方通过 `encode_key<T>`、`encode_value<T>` 和 `decode_value<T>` 完成类型编码。
- 支持 `int32`、`int64`、`float`、`double`、`bool` 作为定长 key；`String`/`Blob` 可以作为不参与排序的 value。
- key 使用保序编码，树内部只需要按字节序比较；value 通过 RID 间接存放在 `TupleStore` 中。
- 数据库文件包含元数据页，关闭后可以重新打开并继续读写。

### 表与列式读取

- `Schema`、`Column`、`Chunk`、`Value` 构成轻量的批量数据模型。
- `ITableSource` 是查询侧的统一读取接口，支持按列投影并以 `Chunk` 逐批返回。
- `RowTableSource` 将 KV 引擎中的行记录解码成列式批次。
- `NativeColumnarFileFormat` 提供自有的 `DBC1` 列式文件格式：一个文件对应一个 row group，每列一个 page，footer 保存列的偏移、编码、压缩和行数信息。
- 当前列式格式支持 Plain 编码、None/Zlib 压缩、投影下推、`ReadRange`、批量 cursor，以及定长未压缩列的按范围直接读取路径。
- `MemStorage` 和 `LocalStorage` 实现可替换的字节范围文件存储；`TableSource` 负责将 `IFileFormat` 与 `IStorage` 组合起来。

### IO 实验

`src/Io` 提供一个单线程 Reactor 的最小实现：`EventLoop`、`Channel` 和基于 `poll` 的 `Poller`。它目前是独立的运行时实验，还没有连接到网络服务或存储 IO 调度。

## 架构概览

```text
调用方
  │ encode_key<T> / encode_value<T>
  ▼
MiniKV / IStorageEngine                         ITableSource
  │                                             ├─ RowTableSource
  ▼                                             │    KV cursor → RowCodec → Chunk
BPlusTreeEngine                                 └─ TableSource
  ├─ BPlusTree (EncodedKey → RID)                    IFileFormat + IStorage
  ├─ TupleStore (RID → value bytes)                     └─ NativeColumnarFileFormat
  └─ BufferPoolManager → DiskManager                         DBC1 / Plain / Zlib
                                                              └─ MemStorage / LocalStorage

EventLoop → Poller → Channel
```

其中，B+Tree 是按 key 访问的存储引擎；`IStorage` 是按 byte range 访问文件的抽象，两者刻意保持为不同的层次。

## 快速开始

### 环境要求

- CMake 3.20 或更高版本
- 支持 C++17 的编译器（Clang 或 GCC）
- Git
- 主要面向 macOS/Linux 环境

第三方依赖通过 submodule 提供，包括 glog、GoogleTest、Boost 和 zlib-ng。

### 获取并构建

```bash
git clone https://github.com/RoanHeNaN/DBPlayground.git
cd DBPlayground
git submodule update --init --recursive

cmake -S . -B build -DDBPLAYGROUND_BUILD_TESTS=ON
cmake --build build --target dbplayground -j
```

运行示例程序：

```bash
./build/dbplayground
```

示例会在当前目录创建 `dbplayground.db`，写入一组 `int64 -> int32` 数据后再读取出来。想从干净数据库重新运行时，可以先删除这个示例文件。

### 运行测试

项目没有使用 CTest 的默认 `test` 目标，而是由 CMake 自定义了会“构建并运行”的测试目标：

```bash
# 构建并依次运行全部测试
cmake --build build --target test -j

# 只运行一个测试目标
cmake --build build --target MiniKVReopenTest -j
cmake --build build --target NativeColumnarFileFormatTest -j
cmake --build build --target EventLoopTest -j

# 只构建测试，不运行
cmake --build build --target build-tests -j
```

每个 `test/<Module>/<Name>Test.cpp` 都会生成一个同名的便捷目标，例如 `BPlusTreeEngineTest`、`RowTableSourceTest` 和 `NativeColumnarFastPathTest`。

### 构建选项

```bash
# 不编译测试
cmake -S . -B build-no-tests -DDBPLAYGROUND_BUILD_TESTS=OFF

# 构建静态库而不是默认的共享库
cmake -S . -B build-static -DDBPLAYGROUND_BUILD_SHARED=OFF
```

主要目标如下：

| 目标 | 作用 |
| --- | --- |
| `dbplayground` | 构建顶层示例程序 |
| `dbplayground_lib` | 构建核心库，默认是共享库 |
| `test` | 构建并运行全部测试 |
| `build-tests` | 构建全部测试但不运行 |
| `format` | 使用 clang-format 格式化 `src/` 和 `test/` |

## 最小使用示例

`MiniKV` 的 key/value 在存储层都是字节串，类型由调用方负责编码：

```cpp
#include <cstdint>
#include <iostream>
#include <string>

#include "Common/Codec.h"
#include "Core/MiniKV.h"

int main() {
  dbplay::MiniKV db("example.db", dbplay::Type::Int64, dbplay::Type::Int32);

  const auto key = dbplay::encode_key<int64_t>(42);
  const auto value = dbplay::encode_value<int32_t>(100);

  if (!db.Insert(key, value)) {
    return 1;  // 当前 Insert 不覆盖已经存在的 key
  }

  std::string raw;
  if (db.Get(key, &raw)) {
    std::cout << dbplay::decode_value<int32_t>(dbplay::Slice(raw)) << "\n";
  }

  db.Close();  // 析构时也会幂等地 flush
}
```

当前 `DiskManager` 的数据库路径需要带文件扩展名，例如 `example.db`。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `src/Common` | `Slice`、类型、编码、常量和通用工具 |
| `src/Container` | B+Tree 等底层容器 |
| `src/Storage` | 磁盘、页、Buffer Pool、TupleStore、KV 引擎、文件存储和列编码 |
| `src/Table` | Schema、Column/Chunk、RowTableSource、文件格式和 TableSource |
| `src/Execution` | 面向 `ITableSource` 的最小 scan/project 执行逻辑 |
| `src/Io` | EventLoop、Channel、Poller |
| `test` | 按模块组织的 GoogleTest 测试 |
| `docs/design` | 架构、格式和演进设计文档 |
| `docs/blog` | 列式存储、更新机制等专题文章 |
| `contrib` | 通过 submodule 引入的第三方依赖及其 CMake 封装 |

## 设计文档导航

- [Storage Engine Refactor](docs/design/StorageEngineRefactor.md)：类型擦除的 KV 引擎、保序 key 编码和 RID 间接存储。
- [Columnar Table Source](docs/design/ColumnarTableSource.md)：`ITableSource`、行式 source、Schema/Chunk 以及批量读取路径。
- [Native Columnar File Format](docs/design/NativeColumnarFileFormat.md)：`DBC1` 文件的 byte-exact 布局和读写流程。
- [Columnar Read Path](docs/design/ColumnarReadPath.md)：`ReadRange`、批量 cursor 和 direct-offset fast path 的设计与边界。
- [Storage Abstraction](docs/design/StorageAbstraction.md)：`IStorage`、`IFileFormat`、Codec 和未来可组合存储模型。
- [Expression Engine](docs/design/ExpressionEngine.md)：尚未实现的表达式、过滤和统计信息下推设计。
- [Object Storage Format Research](docs/design/ObjectStorageFormatResearch.md)：对象存储场景下的列式格式调研与取舍。

## 当前边界与后续方向

下面这些内容在设计文档中已有讨论，但不应被理解为当前已经完成的功能：

- String/Blob 作为可排序 key 的可变长 key 支持。
- 将 Schema、manifest、快照等信息持久化为完整的 catalog/table format。
- 单个文件内的多 row group、多 page、page index 和统计信息下推。
- Dictionary/RLE/Delta 等更多编码，以及 S3 等远程对象存储实现。
- Null/validity bitmap、完整表达式引擎、过滤下推和更完整的查询执行器。
- SQL、优化器、服务端协议，以及完整的事务、日志和恢复体系。

因此，仓库中的“已实现”和“设计中”会明确写在各设计文档的状态说明里；阅读某个模块时，建议同时查看对应的 `docs/design` 文档和测试。

## 代码风格

```bash
cmake --build build --target format -j
```

该目标会直接更新 `src/` 和 `test/` 下的源码格式。
