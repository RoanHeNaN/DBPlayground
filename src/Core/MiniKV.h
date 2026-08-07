//
// Created by 何智强 on 2021/10/2.
//

#ifndef DBPLAYGROUND_MINIKV_H
#define DBPLAYGROUND_MINIKV_H

#include <vector>

#include "Common/Config.h"
#include "Container/BPlusTree.h"
#include "Storage/BufferPool/BufferPoolManager.h"

namespace dbplay {

class MiniKV {
 public:
  MiniKV();

  bool Insert(key_t key, value_t value);
  bool Update(key_t key, value_t value);
  bool Remove(key_t key);
  value_t Get(key_t key);
  values Range(key_t key);

 private:
  std::shared_ptr<DiskManager> disk_manager_;
  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  BPlusTree<key_t, value_t> container_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_MINIKV_H
