//
// Created by 何智强 on 2021/10/2.
//

#include "Core/MiniKV.h"

#include "Common/Config.h"
#include "Storage/BPlusTreeEngine.h"

namespace dbplay {

MiniKV::MiniKV()
    : disk_manager_(std::make_shared<DiskManager>("dbplayground.db")),
      buffer_pool_manager_(std::make_shared<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager_)),
      engine_(std::make_unique<BPlusTreeEngine>(buffer_pool_manager_)) {}

bool MiniKV::Insert(const Slice &key, const Slice &value) { return engine_->Insert(key, value); }

bool MiniKV::Get(const Slice &key, std::string *value) { return engine_->Get(key, value); }

bool MiniKV::Remove(const Slice &key) { return engine_->Remove(key); }

}  // namespace dbplay
