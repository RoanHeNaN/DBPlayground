//
// Created by 何智强 on 2021/10/3.
//

#ifndef DBPLAYGROUND_LINEARPROBEHASHTABLE_H
#define DBPLAYGROUND_LINEARPROBEHASHTABLE_H

#include <functional>
#include <utility>

/// Cant allocate slots for all possible keys, so we have to deal with hash collision.
/// LinearProbeHashTable use fixed size of hash table, if we run out of all free slots, allocate a twice larger array.
/// When collision happens, new entry will be inserted into next free slots after position where it should be.

#include "Storage/BufferPool/BufferPoolManager.h"

namespace dbplay {
template <typename KeyType, typename ValueType>
class LinearProbeHashTable {
 public:
  explicit LinearProbeHashTable(std::shared_ptr<BufferPoolManager> buffer_pool_manager, size_t num_buckets)
      : buffer_pool_manager_(std::move(buffer_pool_manager)), num_buckets_(num_buckets) {
    auto header_page = buffer_pool_manager_->NewPage();
    header_page_id_ = header_page->GetPageId();
    hash_function_ = std::hash<KeyType>{};
  };

  bool GetValue(const KeyType &key, ValueType &value);
  bool Insert(const KeyType &key, const ValueType &value);
  bool Remove(const KeyType &key);
  void Resize(size_t size);
  size_t GetSize() const;

 private:
  page_id_t header_page_id_;

  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  size_t num_buckets_;
  std::function<KeyType(const KeyType &)> hash_function_;
};
}  // namespace dbplay

#endif  // DBPLAYGROUND_LINEARPROBEHASHTABLE_H
