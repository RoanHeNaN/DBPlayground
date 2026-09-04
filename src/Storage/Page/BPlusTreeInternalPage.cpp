//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/page/b_plus_tree_internal_page.cpp
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "Storage/Page/BPlusTreeInternalPage.h"

#include <iostream>
#include <sstream>

#include "Common/EncodedKey.h"
#include "Common/Utils.h"

namespace dbplay {
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
void BPlusTreeInternalPage::Init(page_id_t page_id, page_id_t parent_id, int max_size) {
  SetPageType(IndexPageType::InternalPage);
  SetSize(0);
  SetMaxSize(max_size);
  SetParentPageId(parent_id);
  SetPageId(page_id);
}

/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array_ offset)
 */
EncodedKey BPlusTreeInternalPage::KeyAt(int index) const { return array_[index].first; }

void BPlusTreeInternalPage::SetKeyAt(int index, const EncodedKey &key) {
  if (index < 0) {
    std::cout << "internal_page SetKeyAt: negative index: " << index << std::endl;
  } else if (index == 0) {
    std::cout << "internal_page SetKeyAt: index=0" << std::endl;
  }

  array_[index].first = key;
}

/*
 * Helper method to find and return array_ index(or offset), so that its value
 * equals to input "value"
 */
int BPlusTreeInternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); i++) {
    // We are comparing values here, of type page_id_t. So we just use ==.
    // Keys elsewhere are ordered by EncodedKey memcmp; values (page_ids) use ==.
    if (array_[i].second == value) {
      return i;
    }
  }

  // If no match, return -1
  return -1;
}

/*
 * Helper method to get the value associated with input "index"(a.k.a array_
 * offset)
 */
page_id_t BPlusTreeInternalPage::ValueAt(int index) const { return array_[index].second; }

//*****************************************************************************
//* LOOKUP
//*****************************************************************************

/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 */
page_id_t BPlusTreeInternalPage::Lookup(const EncodedKey &key) const {
  for (int i = 1; i < GetSize(); i++) {
    EncodedKey k = array_[i].first;
    if (key < k) {
      return array_[i - 1].second;
    }
  }
  return array_[GetSize() - 1].second;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 */
void BPlusTreeInternalPage::PopulateNewRoot(const page_id_t &old_value, const EncodedKey &new_key,
                                            const page_id_t &new_value) {
  // This is called when a new root page is created. It only contains one key, two values.
  SetKeyAt(1, new_key);
  if (old_value <= new_value) {
    array_[0].second = old_value;
    array_[1].second = new_value;
  } else {
    array_[0].second = new_value;
    array_[1].second = old_value;
  }
  SetSize(2);
}

/*
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
int BPlusTreeInternalPage::InsertNodeAfter(const page_id_t &old_value, const EncodedKey &new_key,
                                           const page_id_t &new_value) {
  int value_index = ValueIndex(old_value);
  if (value_index == -1) {
    LOG(WARNING) << "Could no find value " << old_value << " current node size " << GetSize();
  }

  int old_size = GetSize();

  // Shift existing values, if needed
  if (value_index + 1 < old_size) {
    for (int i = old_size - 1; i > value_index; i--) {
      array_[i + 1] = array_[i];
    }
  }

  // Insert new value
  array_[value_index + 1] = MappingType{new_key, new_value};

  // Update size
  IncreaseSize(1);

  return GetSize();
}

// Helper function
// Fetch page_id into buffer bool, update its parent page id, and unpin the page, marking it as dirty.
void UpdateParentPageId(page_id_t page_id, page_id_t parent_page_id,
                        std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  auto mem_page = buffer_pool_manager->FetchPage(page_id);
  BPlusTreePage *tree_page = reinterpret_cast<BPlusTreePage *>(mem_page->GetData());

  assert(tree_page != nullptr);
  tree_page->SetParentPageId(parent_page_id);
  buffer_pool_manager->UnpinPage(page_id, true);
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page.
 * The suffix is moved.
 */
void BPlusTreeInternalPage::MoveHalfTo(BPlusTreeInternalPage *recipient,
                                       std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  // Move array_[(size+1)/2 : size-1]
  // Number of elements moved: size-1 - (size+1)/2 + 1 = size-(size+1)/2 = size-ceil(size/2) = floor(size/2)
  // After move, this->GetSize() >= recipient->GetSize().
  int move_start = (GetSize() + 1) / 2;
  int num_moved = GetSize() - move_start;
  recipient->CopyNFrom(&array_[move_start], num_moved, buffer_pool_manager);

  recipient->SetSize(num_moved);
  SetSize(GetSize() - num_moved);
}

/* Copy entries into me, starting from {items} and copy {size} entries.
 * Since it is an internal page, for all entries (pages) moved, their parents page now changes to me.
 * So I need to 'adopt' them by changing their parent page id, which needs to be persisted with BufferPoolManger.
 *
 * The caller should update the size. This function doesn't.
 */
void BPlusTreeInternalPage::CopyNFrom(MappingType *items, int size,
                                      std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  int old_size = GetSize();
  for (int i = old_size; i < size; i++) {
    int offset = i - old_size;
    array_[i] = items[offset];

    page_id_t page_id = array_[i].second;
    page_id_t parent_page_id = GetPageId();
    UpdateParentPageId(page_id, parent_page_id, buffer_pool_manager);
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array_ offset)
 * NOTE: store key&value pair continuously after deletion
 */
void BPlusTreeInternalPage::Remove(int index) {
  for (int i = index + 1; i < GetSize(); i++) {
    array_[i - 1] = array_[i];
  }

  IncreaseSize(-1);
}

/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 */
page_id_t BPlusTreeInternalPage::RemoveAndReturnOnlyChild() {
  if (GetSize() != 1) {
    std::cout << "internal_page RemoveAndReturnOnlyChild called, size=" << GetSize() << std::endl;
  }

  SetSize(0);
  return array_[0].second;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all of key & value pairs from this page to "recipient" page.
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void BPlusTreeInternalPage::MoveAllTo(BPlusTreeInternalPage *recipient, const EncodedKey &middle_key,
                                      std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  // Assume recipient is the left sibling
  // This is only called by Coalesce() in b_plus_tree.cpp

  // Move key&value pairs
  for (int i = 0; i < GetSize(); i++) {
    recipient->array_[recipient->GetSize() + i] = array_[i];

    page_id_t page_id = array_[i].second;
    page_id_t parent_page_id = recipient->GetPageId();
    UpdateParentPageId(page_id, parent_page_id, buffer_pool_manager);
  }

  // Set middle_key
  recipient->array_[recipient->GetSize()].first = middle_key;

  // Update size for both nodes
  int sz = GetSize();
  IncreaseSize(-sz);
  recipient->IncreaseSize(sz);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to tail of "recipient" page.
 *
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void BPlusTreeInternalPage::MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const EncodedKey &middle_key,
                                             std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  recipient->array_[recipient->GetSize()] = array_[0];
  recipient->array_[recipient->GetSize()].first = middle_key;

  for (int i = 1; i < GetSize(); i++) {
    array_[i - 1] = array_[i];
  }

  page_id_t page_id = recipient->array_[recipient->GetSize()].second;
  page_id_t parent_page_id = recipient->GetPageId();
  UpdateParentPageId(page_id, parent_page_id, buffer_pool_manager);

  IncreaseSize(-1);
  recipient->IncreaseSize(1);
}

/* Append an entry at the end.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void BPlusTreeInternalPage::CopyLastFrom(const MappingType &pair,
                                         std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  array_[GetSize()] = pair;

  UpdateParentPageId(array_[GetSize()].second, GetPageId(), buffer_pool_manager);

  IncreaseSize(1);
}

/*
 * Remove the last key & value pair from this page to head of "recipient" page.
 * You need to handle the original dummy key properly, e.g. updating recipient’s array_ to position the middle_key at
 * the right place. You also need to use BufferPoolManager to persist changes to the parent page id for those pages that
 * are moved to the recipient
 */
void BPlusTreeInternalPage::MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const EncodedKey &middle_key,
                                              std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  // make room
  for (int i = recipient->GetSize() - 1; i >= 0; i--) {
    recipient->array_[i + 2] = recipient->array_[i];
  }

  // put middle_key
  recipient->array_[2].first = middle_key;

  // move key-value pair
  recipient->array_[1] = array_[GetSize() - 1];

  // invalidate pointer. TODO(fred): is this correct?
  recipient->array_[0].second = INVALID_PAGE_ID;

  // update parent page id
  UpdateParentPageId(recipient->array_[1].second, recipient->GetPageId(), buffer_pool_manager);

  // update size
  IncreaseSize(-1);
  recipient->IncreaseSize(1);
}

/* Append an entry at the beginning.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void BPlusTreeInternalPage::CopyFirstFrom(const MappingType &pair,
                                          std::shared_ptr<BufferPoolManager> buffer_pool_manager) {
  // make room
  for (int i = GetSize() - 1; i >= 0; i--) {
    array_[i + 1] = array_[i];
  }

  // put entry
  array_[1] = pair;

  // invalidate pointer. TODO(fred): is this correct?
  array_[0].second = INVALID_PAGE_ID;

  // update parent page id
  UpdateParentPageId(array_[1].second, GetPageId(), buffer_pool_manager);

  // update size
  IncreaseSize(1);
}

}  // namespace dbplay
