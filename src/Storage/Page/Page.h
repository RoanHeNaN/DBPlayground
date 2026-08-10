//
// Created by 何智强 on 2021/10/4.
//

#ifndef DBPLAYGROUND_PAGE_H
#define DBPLAYGROUND_PAGE_H

#include <cstring>
#include <iostream>

#include "Base/ReaderWriterLatch.h"
#include "Common/Config.h"

namespace dbplay {

class Page {
  friend class BufferPoolManager;

 public:
  /** Constructor. Zeros out the page data. */
  Page() { ResetMemory(); }

  /** Default destructor. */
  ~Page() = default;

  /** @return the actual data contained within this page */
  inline char *GetData() { return data_; }

  /** @return the page id of this page */
  inline page_id_t GetPageId() { return page_id_; }

  /** @return the pin count of this page */
  inline int GetPinCount() { return pin_count_; }

  /** @return true if the page in memory has been modified from the page on disk, false otherwise */
  inline bool IsDirty() { return is_dirty_; }

  /** Acquire the page write latch. */
  inline void WLatch() { rwlatch_.WLock(); }

  /** Release the page write latch. */
  inline void WUnlatch() { rwlatch_.WUnlock(); }

  /** Acquire the page read latch. */
  inline void RLatch() { rwlatch_.RLock(); }

  /** Release the page read latch. */
  inline void RUnlatch() { rwlatch_.RUnlock(); }

 protected:
  static_assert(sizeof(page_id_t) == 4);

  static constexpr size_t SIZE_PAGE_HEADER = 8;
  static constexpr size_t OFFSET_PAGE_START = 0;
  static constexpr size_t OFFSET_LSN = 4;

 private:
  inline void ResetMemory() { memset(data_, 0, PAGE_SIZE); }

  char data_[PAGE_SIZE];
  page_id_t page_id_ = INVALID_PAGE_ID;
  int pin_count_ = 0;
  bool is_dirty_ = false;
  ReaderWriterLatch rwlatch_;
};

}  // namespace dbplay
#endif  // DBPLAYGROUND_PAGE_H
