//
// Created by 何智强 on 2021/10/2.
//

#ifndef DBPLAYGROUND_MINIKV_H
#define DBPLAYGROUND_MINIKV_H

#include <memory>
#include <string>

#include "Common/Slice.h"
#include "Common/Type.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "Storage/IStorageEngine.h"

namespace dbplay {

// Phase 3-4: MiniKV is a thin, type-erased facade over an IStorageEngine.
// Keys/values are opaque bytes; callers encode with encode_key<T>/encode_value<T>
// (see Common/Codec.h) since they know their own types at compile time.
//
// The database persists to `path`: constructing on an existing file reopens it;
// on a new file it is created with the given declared types. Close() (also run
// by the destructor) flushes the catalog and pages so it can be reopened.
class MiniKV {
 public:
  explicit MiniKV(std::string path = "dbplayground.db", Type key_type = Type::Int64, Type value_type = Type::Int64);
  ~MiniKV();

  bool Insert(const Slice &key, const Slice &value);
  bool Get(const Slice &key, std::string *value);
  bool Remove(const Slice &key);

  void Close();  // idempotent: persist catalog + flush pages

 private:
  std::shared_ptr<DiskManager> disk_manager_;
  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  std::unique_ptr<IStorageEngine> engine_;
  bool closed_ = false;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_MINIKV_H
