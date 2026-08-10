//
// Created by 何智强 on 2021/10/2.
//

#ifndef DBPLAYGROUND_MINIKV_H
#define DBPLAYGROUND_MINIKV_H

#include <memory>
#include <string>

#include "Common/Slice.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Disk/DiskManager.h"
#include "Storage/IStorageEngine.h"

namespace dbplay {

// Phase 3: MiniKV is now a thin, type-erased facade over an IStorageEngine.
// Keys/values are opaque bytes; callers encode with encode_key<T>/encode_value<T>
// (see Common/Codec.h) since they know their own types at compile time.
class MiniKV {
 public:
  MiniKV();

  bool Insert(const Slice &key, const Slice &value);
  bool Get(const Slice &key, std::string *value);
  bool Remove(const Slice &key);

 private:
  std::shared_ptr<DiskManager> disk_manager_;
  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  std::unique_ptr<IStorageEngine> engine_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_MINIKV_H
