//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/include/page/b_plus_tree_internal_page.h
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#pragma once

#include <queue>

#include "Common/EncodedKey.h"
#include "Storage/Page/BPlusTreePage.h"

namespace dbplay {

#define INTERNAL_PAGE_HEADER_SIZE 24
#define INTERNAL_PAGE_SIZE ((PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (sizeof(MappingType)) - 1)
/**
 * Store n indexed keys and n+1 child pointers (page_id) within internal page.
 * Pointer PAGE_ID(i) points to a subtree in which all keys K satisfy:
 * K(i) <= K < K(i+1).
 * NOTE: since the number of keys does not equal to number of child pointers,
 * the first key always remains invalid. That is to say, any search/lookup
 * should ignore the first key.
 *
 * Internal page format (keys are stored in increasing order):
 *  --------------------------------------------------------------------------
 * | HEADER | KEY(1)+PAGE_ID(1) | KEY(2)+PAGE_ID(2) | ... | KEY(n)+PAGE_ID(n) |
 *  --------------------------------------------------------------------------
 */
class BPlusTreeInternalPage : public BPlusTreePage {
 public:
  using MappingType = std::pair<EncodedKey, page_id_t>;

  // must call initialize method after "create" a new node
  void Init(page_id_t page_id, page_id_t parent_id = INVALID_PAGE_ID, int max_size = INTERNAL_PAGE_SIZE);

  EncodedKey KeyAt(int index) const;
  void SetKeyAt(int index, const EncodedKey &key);
  int ValueIndex(const page_id_t &value) const;
  page_id_t ValueAt(int index) const;

  page_id_t Lookup(const EncodedKey &key) const;
  void PopulateNewRoot(const page_id_t &old_value, const EncodedKey &new_key, const page_id_t &new_value);
  int InsertNodeAfter(const page_id_t &old_value, const EncodedKey &new_key, const page_id_t &new_value);
  void Remove(int index);
  page_id_t RemoveAndReturnOnlyChild();

  // Split and Merge utility methods
  void MoveAllTo(BPlusTreeInternalPage *recipient, const EncodedKey &middle_key,
                 std::shared_ptr<BufferPoolManager> buffer_pool_manager);
  void MoveHalfTo(BPlusTreeInternalPage *recipient, std::shared_ptr<BufferPoolManager> buffer_pool_manager);
  void MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const EncodedKey &middle_key,
                        std::shared_ptr<BufferPoolManager> buffer_pool_manager);
  void MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const EncodedKey &middle_key,
                         std::shared_ptr<BufferPoolManager> buffer_pool_manager);

 private:
  void CopyNFrom(MappingType *items, int size, std::shared_ptr<BufferPoolManager> buffer_pool_manager);
  void CopyLastFrom(const MappingType &pair, std::shared_ptr<BufferPoolManager> buffer_pool_manager);
  void CopyFirstFrom(const MappingType &pair, std::shared_ptr<BufferPoolManager> buffer_pool_manager);
  MappingType array_[0];
};
}  // namespace dbplay
