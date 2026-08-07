//
// Created by 何智强 on 2021/10/4.
//

#ifndef DBPLAYGROUND_HASHTABLEPAGE_H
#define DBPLAYGROUND_HASHTABLEPAGE_H

#include <atomic>
#include <cassert>
#include <utility>

#include "Common/Config.h"

namespace dbplay {

#define HASH_TABLE_BLOCK_TYPE HashTableBlockPage<KeyType, ValueType, KeyComparator>

template <typename KeyType, typename ValueType>
class HashTablePage {
 public:
  using MappingType = std::pair<KeyType, ValueType>;

/** BLOCK_ARRAY_SIZE is the number of (key, value) pairs that can be stored in   * a block page. It is an approximate
 * calculation based on the size of MappingType (which is a std::pair of KeyType and ValueType). For each key/value
 * pair, we need two additional bits for occupied_ and readable_. 4 * PAGE_SIZE / (4 * sizeof (MappingType) + 1) =
 * PAGE_SIZE/(sizeof (MappingType) + 0.25) because 0.25 bytes = 2 bits is the space required to maintain the occupied
 * and readable flags for a key value pair.*/
#define BLOCK_ARRAY_SIZE (4 * PAGE_SIZE / (4 * sizeof(MappingType) + 1))

  HashTablePage();

  KeyType KeyAt(size_t slot_offset) const;
  ValueType ValueAt(size_t slot_offset) const;
  bool Insert(size_t slot_offset, const KeyType &key, const ValueType &value);
  void Remove(size_t slot_offset);

  /**
   * Returns whether or not an index is occupied (key/value pair or tombstone)
   *
   * @param slot_offset index to look at
   * @return true if the index is occupied, false otherwise
   */
  bool IsOccupied(size_t slot_offset) const;

  /**
   * Returns whether or not an index is readable (valid key/value pair)
   *
   * @param slot_offset index to look at
   * @return true if the index is readable, false otherwise
   */
  bool IsReadable(size_t slot_offset) const;

  inline size_t Size() const { return size_; }

  inline page_id_t GetPageId() const { return page_id_; }

  inline page_id_t GetNextPageId() const { return next_page_id_; }

  inline bool IsFull() const {
    assert(size_ <= BLOCK_ARRAY_SIZE);
    return size_ == BLOCK_ARRAY_SIZE;
  }

  void SetPageId(page_id_t page_id) { page_id_ = page_id; }

  void SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

 private:
  page_id_t page_id_;
  page_id_t next_page_id_;
  size_t size_;

  std::atomic_char occupied_[(BLOCK_ARRAY_SIZE - 1) / 8 + 1];
  std::atomic_char readable_[(BLOCK_ARRAY_SIZE - 1) / 8 + 1];
  MappingType array_[0];
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_HASHTABLEPAGE_H
