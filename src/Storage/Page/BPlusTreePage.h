//
// Created by 何智强 on 2021/10/5.
//

#ifndef DBPLAYGROUND_BPLUSTREEPAGE_H
#define DBPLAYGROUND_BPLUSTREEPAGE_H

#include <cassert>
#include <climits>
#include <cstdlib>
#include <string>

#include "Common/Config.h"
#include "Concurrency/Transaction.h"
#include "Storage/BufferPool/BufferPoolManager.h"

namespace dbplay {

// define page type enum

enum class IndexPageType { InvalidIndexPage = 0, LeafPage, InternalPage };

#define INDEX_TEMPLATE_ARGUMENTS template <typename KeyType, typename ValueType>

/**
 * Both internal and leaf page are inherited from this page.
 *
 * It actually serves as a header part for each B+ tree page and
 * contains information shared by both leaf page and internal page.
 *
 * Header format (size in byte, 20 bytes in total):
 * ----------------------------------------------------------------------------
 * | PageType (4) | CurrentSize (4) | MaxSize (4) |
 * ----------------------------------------------------------------------------
 * | ParentPageId (4) | PageId(4) |
 * ----------------------------------------------------------------------------
 */
class BPlusTreePage {
 public:
  bool IsLeafPage() const;
  bool IsRootPage() const;
  void SetPageType(IndexPageType page_type);

  int GetSize() const;
  void SetSize(int size);
  void IncreaseSize(int amount);

  int GetMaxSize() const;
  void SetMaxSize(int max_size);
  int GetMinSize() const;

  page_id_t GetParentPageId() const;
  void SetParentPageId(page_id_t parent_page_id);

  page_id_t GetPageId() const;
  void SetPageId(page_id_t page_id);

 private:
  // member variable, attributes that both internal and leaf page share
  IndexPageType page_type_;
  int size_;
  int max_size_;
  page_id_t parent_page_id_;
  page_id_t page_id_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_BPLUSTREEPAGE_H
