//
// Created by 何智强 on 2021/10/3.
//

#include "LinearProbeHashTable.h"

#include "Storage/Page/HashTableHeaderPage.h"
#include "Storage/Page/HashTablePage.h"

namespace dbplay {

template <typename KeyType, typename ValueType>
bool LinearProbeHashTable<KeyType, ValueType>::GetValue(const KeyType &key, ValueType &value) {
  /// 1. Calculate bucket index
  /// 2. Fetch block page
  /// 3. Return value
  std::size_t hash_value = hash_function_(key);
  std::size_t bucket_num = hash_value % num_buckets_;
  std::size_t slot_num = hash_value - bucket_num * dbplay::PAGE_SIZE;
  std::shared_ptr<Page> page_ptr = buffer_pool_manager_->FetchPage(bucket_num);
  if (!page_ptr) {
  }
  page_ptr->RLatch();
  std::shared_ptr<HashTablePage<KeyType, ValueType>> table_page(
      reinterpret_cast<HashTablePage<KeyType, ValueType> *>(page_ptr->GetData()));
  value = table_page->ValueAt(slot_num);
  return true;
}

template class LinearProbeHashTable<dbplay::key_t, dbplay::value_t>;
}  // namespace dbplay
