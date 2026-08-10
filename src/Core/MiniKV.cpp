//
// Created by 何智强 on 2021/10/2.
//

#include "Core/MiniKV.h"

#include "Common/Config.h"
#include "Storage/BPlusTreeEngine.h"

namespace dbplay {

MiniKV::MiniKV(std::string path, Type key_type, Type value_type)
    : disk_manager_(std::make_shared<DiskManager>(std::move(path))),
      buffer_pool_manager_(std::make_shared<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager_)) {
  // A brand-new file hands out page id 0 first; anything already allocated
  // means an existing database to reopen.
  const bool is_new = (disk_manager_->GetNextPageId() == 0);
  engine_ = std::make_unique<BPlusTreeEngine>(buffer_pool_manager_, is_new, key_type, value_type);
}

MiniKV::~MiniKV() { Close(); }

void MiniKV::Close() {
  if (closed_) {
    return;
  }
  if (engine_ != nullptr) {
    engine_->Flush();
  }
  closed_ = true;
}

bool MiniKV::Insert(const Slice &key, const Slice &value) { return engine_->Insert(key, value); }

bool MiniKV::Get(const Slice &key, std::string *value) { return engine_->Get(key, value); }

bool MiniKV::Remove(const Slice &key) { return engine_->Remove(key); }

}  // namespace dbplay
