//===----------------------------------------------------------------------===//
//
//                         CMU-DB Project (15-445/645)
//                         ***DO NO SHARE PUBLICLY***
//
// Identification: src/page/b_plus_tree_leaf_page.cpp
//
// Copyright (c) 2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "Storage/Page/BPlusTreeLeafPage.h"

#include <algorithm>
#include <sstream>

#include "Common/EncodedKey.h"
#include "Common/RID.h"

#include "Common/Utils.h"
#include "Storage/Page/BPlusTreePage.h"

namespace dbplay {

/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * Init method after creating a new leaf page
 * Including set page type, set current size to zero, set page id/parent id, set
 * next page id and set max size
 */
void BPlusTreeLeafPage::Init(page_id_t page_id, page_id_t parent_id, int max_size) {
  // Default values:
  // parent_id = INVALID_PAGE_ID;
  // max_size = LEAF_PAGE_SIZE.

  SetPageType(IndexPageType::LeafPage);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetNextPageId(INVALID_PAGE_ID);
  SetMaxSize(max_size);
}

/**
 * Helper methods to set/get next page id
 */
page_id_t BPlusTreeLeafPage::GetNextPageId() const { return next_page_id_; }

void BPlusTreeLeafPage::SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

/**
 * Helper method to find the first index i so that array_[i].first >= key
 * NOTE: This method is only used when generating index iterator
 */
int BPlusTreeLeafPage::KeyIndex(const EncodedKey &key) const {
  const MappingType *p = std::lower_bound(array_, array_ + GetSize(), key,
                                          [&](const MappingType &item, const EncodedKey &k) { return item.first < k; });

  return std::distance(array_, p);
}

/*
 * Helper method to find and return the key associated with input "index"(a.k.a
 * array_ offset)
 */
EncodedKey BPlusTreeLeafPage::KeyAt(int index) const { return array_[index].first; }

/*
 * Helper method to find and return the key & value pair associated with input
 * "index"(a.k.a array_ offset)
 */
auto BPlusTreeLeafPage::GetItem(int index) -> const MappingType & { return array_[index]; }

//*****************************************************************************
//* INSERTION
//*****************************************************************************

/*
 * Insert key & value pair into leaf page ordered by key
 * @return  page size after insertion
 */
int BPlusTreeLeafPage::Insert(const EncodedKey &key, const RID &value) {
  /* find insert position [binary search] */
  int insert_position = KeyIndex(key);

  // make room
  for (int i = GetSize() - 1; i >= insert_position; i--) {
    array_[i + 1] = array_[i];
  }

  // insert key-value pair
  array_[insert_position] = MappingType{key, value};

  // update size
  IncreaseSize(1);

  return GetSize();
}

//*****************************************************************************
// SPLIT
//*****************************************************************************

/**
 * Remove half of key & value pairs from this page to "recipient" page
 */
void BPlusTreeLeafPage::MoveHalfTo(BPlusTreeLeafPage *recipient) {
  // Currently, this is only called when recipient is empty (in Split)

  // Move array_[(size+1)/2 : size-1].
  // Number of elements moved: size-1 - (size+1)/2 + 1 = size-(size+1)/2 = size-ceil(size/2) = floor(size/2)
  // After move, this->GetSize() >= recipient->GetSize().
  int move_start = (GetSize() + 1) / 2;
  int num_moved = GetSize() - move_start;
  recipient->CopyNFrom(&array_[move_start], num_moved);

  IncreaseSize(-num_moved);
  recipient->IncreaseSize(num_moved);
}

/*
 * Copy starting from items, and copy {size} number of elements into me.
 * [Attention] The caller should update size. This function doesn't.
 */
void BPlusTreeLeafPage::CopyNFrom(MappingType *items, int size) {
  if (GetSize() + size > GetMaxSize()) {
    throw std::runtime_error("CopyNFrom: will overflow page");
  }

  int old_size = GetSize();
  for (int i = 0; i < size; i++) {
    array_[old_size + i] = items[i];
  }
}

//*****************************************************************************
//* LOOKUP
//*****************************************************************************

/*
 * For the given key, check to see whether it exists in the leaf page. If it
 * does, then store its corresponding value in input "value" and return true.
 * If the key does not exist, then return false
 */
bool BPlusTreeLeafPage::Lookup(const EncodedKey &key, RID *value) const {
  /* Linear search */
  // for (int i = 0; i < GetSize(); i++) {
  //   if (comparator(array_[i].first, key) == 0) {
  //     if (value != nullptr) {
  //       *value = array_[i].second;
  //     }
  //     return true;
  //   }
  // }
  // return false;

  /* Binary search */
  int pos = KeyIndex(key);
  if (pos < GetSize() && (array_[pos].first == key)) {
    if (value != nullptr) {
      *value = array_[pos].second;
    }
    return true;
  }
  return false;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * First look through leaf page to see whether delete key exist or not. If
 * exist, perform deletion, otherwise return immediately.
 * NOTE: store key&value pair continuously after deletion
 * @return   page size after deletion
 */
int BPlusTreeLeafPage::RemoveAndDeleteRecord(const EncodedKey &key) {
  /* Binary search */
  int pos = KeyIndex(key);
  if (pos < GetSize() && (array_[pos].first == key)) {
    for (int j = pos + 1; j < GetSize(); j++) {
      array_[j - 1] = array_[j];
    }
    IncreaseSize(-1);
  }
  return GetSize();
}

//*****************************************************************************
//* MERGE
//*****************************************************************************

/*
 * Remove all of key & value pairs from this page to "recipient" page. Don't forget
 * to update the next_page id in the sibling page.
 *
 * This function updates the size of two nodes.
 */
void BPlusTreeLeafPage::MoveAllTo(BPlusTreeLeafPage *recipient) {
  // Assume we are moving to the left sibling
  // If we are moving to the right sibling (recipient), we need to update the left sibling
  int sz = GetSize();

  recipient->CopyNFrom(array_, sz);
  recipient->SetNextPageId(GetNextPageId());

  recipient->IncreaseSize(sz);
  IncreaseSize(-sz);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to "recipient" page.
 * This function updates the size for both nodes.
 */
void BPlusTreeLeafPage::MoveFirstToEndOf(BPlusTreeLeafPage *recipient) {
  // Copy element to recipient's last position
  recipient->CopyLastFrom(array_[0]);

  // Remove first element from my array_
  for (int i = 1; i < GetSize(); i++) {
    array_[i - 1] = array_[i];
  }

  // update size for both nodes
  IncreaseSize(-1);
  recipient->IncreaseSize(1);
}

/*
 * Remove the last key & value pair from this page to "recipient" page.
 * This function updates the size for both nodes.
 */
void BPlusTreeLeafPage::MoveLastToFrontOf(BPlusTreeLeafPage *recipient) {
  // Copy element to recipient's front position
  recipient->CopyFirstFrom(array_[GetSize() - 1]);

  // No need to remove the last element. The caller can just update the size.

  // update size for both nodes
  IncreaseSize(-1);
  recipient->IncreaseSize(1);
}

/*
 * Copy the item into the end of my item list. (Append item to my array_)
 * [Attention] This function doesn't update size.
 */
void BPlusTreeLeafPage::CopyLastFrom(const MappingType &item) { array_[GetSize()] = item; }

/*
 * Insert item at the front of my items. Move items accordingly.
 * [Attention] this function doesn't update size.
 */
void BPlusTreeLeafPage::CopyFirstFrom(const MappingType &item) {
  // make space
  for (int i = GetSize() - 1; i >= 0; i--) {
    array_[i + 1] = array_[i];
  }

  // copy element
  array_[0] = item;
}

// Phase 2: leaf stores (order-preserving key, RID into the TupleStore).
}  // namespace dbplay
