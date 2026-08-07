//
// Created by 何智强 on 2021/10/2.
//

#include "Core/MiniKV.h"
#include "Common/Config.h"

namespace dbplay {

MiniKV::MiniKV()
    : disk_manager_(new DiskManager("dbplayground.db")),
      buffer_pool_manager_(new BufferPoolManager(BUFFER_POOL_SIZE, disk_manager_)),
      container_(buffer_pool_manager_) {}

value_t MiniKV::Get(key_t key) {
  value_t value;
  if (container_.GetValue(key, value)) return value;

  return -1;
}

bool MiniKV::Insert(key_t key, value_t value) { return container_.Insert(key, value); }

bool MiniKV::Update(key_t key, value_t value) { return container_.Insert(key, value); }

bool MiniKV::Remove(key_t key) {
  container_.Remove(key);
  return true;
}

values MiniKV::Range(key_t key) {
  values res;
  return res;
};

}  // namespace dbplay
