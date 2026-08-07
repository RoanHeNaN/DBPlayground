//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/include/page/b_plus_tree_leaf_page.h
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Storage/Page/BPlusTreePage.h"

namespace dbplay {

#define B_PLUS_TREE_LEAF_PAGE BPlusTreeLeafPage<KeyType, ValueType>
#define LEAF_PAGE_HEADER_SIZE 24
#define LEAF_PAGE_SIZE ((PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / sizeof(MappingType))

/**
 * Store indexed key and record id(record id = page id combined with slot id,
 * see include/common/rid.h for detailed implementation) together within leaf
 * page. Only support unique key.
 *
 * Leaf page format (keys are stored in order):
 *  ----------------------------------------------------------------------
 * | HEADER | KEY(1) + RID(1) | KEY(2) + RID(2) | ... | KEY(n) + RID(n)
 *  ----------------------------------------------------------------------
 *
 *  Header format (size in byte, 24 bytes in total):
 *  ---------------------------------------------------------------------
 * | PageType (4) |  CurrentSize (4) | MaxSize (4) |
 *  ---------------------------------------------------------------------
 *  -------------------------------------------------
 * | ParentPageId (4) | PageId (4) | NextPageId (4) ｜
 *  -------------------------------------------------
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTreeLeafPage : public BPlusTreePage {
 public:
  using MappingType = std::pair<KeyType, ValueType>;

  // After creating a new leaf page from buffer pool, must call initialize
  // method to set default values
  void Init(page_id_t page_id, page_id_t parent_id = INVALID_PAGE_ID, int max_size = LEAF_PAGE_SIZE);
  // helper methods
  page_id_t GetNextPageId() const;
  void SetNextPageId(page_id_t next_page_id);
  KeyType KeyAt(int index) const;
  int KeyIndex(const KeyType &key) const;
  const MappingType &GetItem(int index);

  // insert and delete methods
  int Insert(const KeyType &key, const ValueType &value);
  bool Lookup(const KeyType &key, ValueType *value) const;
  int RemoveAndDeleteRecord(const KeyType &key);

  // Split and Merge utility methods
  void MoveHalfTo(BPlusTreeLeafPage *recipient);
  void MoveAllTo(BPlusTreeLeafPage *recipient);
  void MoveFirstToEndOf(BPlusTreeLeafPage *recipient);
  void MoveLastToFrontOf(BPlusTreeLeafPage *recipient);

  std::string ToString() const;

 private:
  void CopyNFrom(MappingType *items, int size);
  void CopyLastFrom(const MappingType &item);
  void CopyFirstFrom(const MappingType &item);
  page_id_t next_page_id_;
  // `array_` is the name of the variable. The syntax is the same as `MappingType arr[0]` or `int arr[0]`.
  // To assign `array_` to another variable, you must use `const`, since `array_` is constant. Example:
  // `const MappingType *ptr = array_;`.
  MappingType array_[0];
};
}  // namespace dbplay
