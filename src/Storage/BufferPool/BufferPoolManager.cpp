//
// Created by 何智强 on 2021/10/2.
//

#include "Storage/BufferPool/BufferPoolManager.h"

#include <memory>
#include <utility>

#include "Storage/BufferPool/LRUReplacer.h"

namespace dbplay {

BufferPoolManager::BufferPoolManager(size_t slot_num, std::shared_ptr<DiskManager> disk_manager)
    : slot_num_(slot_num), disk_manager_(std::move(disk_manager)) {
  for (size_t i = 0; i < slot_num_; ++i) {
    pages_.push_back(std::make_shared<Page>());
    free_list_.push_back(i);
  }

  replacer_ = std::make_unique<LRUReplacer>(slot_num_);
}

std::shared_ptr<Page> BufferPoolManager::FetchPage(page_id_t page_id) {
  // 1.     Search the page table for the requested page (P).
  // 1.1    If P exists, pin it and return it immediately.
  // 1.2    If P does not exist, find a replacement page (R) from either the
  // free list or the replacer_.
  //        Note that pages_ are always found from the free list first.
  // 2.     If R is dirty, write it back to the disk.
  // 3.     Delete R from the page table and insert P.
  // 4.     Update P's metadata, read in the page content from disk, and then
  // return a pointer to P.
  std::lock_guard<std::mutex> guard{latch_};

  if (page_table_.count(page_id) != 0) {
    frame_id_t frame_id = page_table_[page_id];
    auto page_ptr = pages_.at(frame_id);
    ++page_ptr->pin_count_;
    replacer_->Pin(frame_id);
    return page_ptr;
  }

  if (free_list_.empty()) {
    frame_id_t victim_frame_id;
    if (replacer_->Victim(&victim_frame_id)) {
      auto page_ptr = pages_.at(victim_frame_id);
      if (page_ptr->IsDirty()) {
        disk_manager_->WritePage(page_ptr->GetPageId(), page_ptr->GetData());
      }
      page_table_.erase(page_ptr->page_id_);
      page_ptr->ResetMemory();
      page_ptr->page_id_ = INVALID_PAGE_ID;
      page_ptr->is_dirty_ = false;
      free_list_.emplace_back(victim_frame_id);
    } else {
      // Can not find any victim frame in replacer_.
      return nullptr;
    }
  }

  frame_id_t free_frame_id = free_list_.back();
  free_list_.pop_back();
  auto page_ptr = pages_.at(free_frame_id);
  ++page_ptr->pin_count_;
  page_ptr->page_id_ = page_id;
  page_ptr->is_dirty_ = false;
  replacer_->Pin(free_frame_id);
  page_table_[page_id] = free_frame_id;
  try {
    disk_manager_->ReadPage(page_id, page_ptr->GetData());
  } catch (...) {
    ;
  }
  return page_ptr;
}  // namespace bustub

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> guard{latch_};
  if (page_table_.count(page_id) == 0) {
    return false;
  }

  frame_id_t frame_id = page_table_[page_id];
  auto page_ptr = pages_.at(frame_id);
  // If page is not dirty, set its state to new state
  // Or just skip new state
  if (!page_ptr->is_dirty_) {
    page_ptr->is_dirty_ = is_dirty;
  }

  if (page_ptr->pin_count_ <= 0) {
    return false;
  }

  if (--page_ptr->pin_count_ == 0) {
    replacer_->Unpin(frame_id);
  }
  return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> guard{latch_};
  // If page is not in buffer
  if (page_table_.count(page_id) == 0) {
    return false;
  }

  frame_id_t frame_id = page_table_[page_id];
  auto page_ptr = pages_.at(frame_id);
  disk_manager_->WritePage(page_id, page_ptr->GetData());
  page_ptr->is_dirty_ = false;
  return true;
}

std::shared_ptr<Page> BufferPoolManager::NewPage() {
  // 0.   Make sure you call DiskManager::AllocatePage!
  // 1.   If all the pages_ in the buffer pool are pinned, return nullptr.
  // 2.   Pick a victim page P from either the free list or the replacer_. Always
  // pick from the free list first.
  // 3.   Update P's metadata, zero out memory and add P to the page table.
  // 4.   Set the page ID output parameter. Return a pointer to P.
  std::lock_guard<std::mutex> guard{latch_};

  /*
   * Free list is empty.
   * Find a victim page from replacer_, flush it to disk
   */
  if (free_list_.empty()) {
    if (replacer_->Size() == 0) {
      // There is no unpinned pages_ in replacer_.
      return nullptr;
    }
    frame_id_t victim_frame;
    if (replacer_->Victim(&victim_frame)) {
      auto page_ptr = pages_.at(victim_frame);
      if (page_ptr->IsDirty()) {
        disk_manager_->WritePage(page_ptr->GetPageId(), page_ptr->GetData());
      }
      page_table_.erase(page_ptr->GetPageId());
      page_ptr->ResetMemory();
      page_ptr->page_id_ = INVALID_PAGE_ID;
      page_ptr->is_dirty_ = false;
      free_list_.push_back(victim_frame);
    } else {
      // Can not find any victim frame in replacer_.
      return nullptr;
    }
  }

  // At least one free frame in free_list_ here
  frame_id_t free_frame = free_list_.front();
  auto free_page = pages_.at(free_frame);
  page_id_t new_page_id = disk_manager_->AllocatePage();
  free_page->page_id_ = new_page_id;
  page_table_[new_page_id] = free_frame;
  replacer_->Pin(free_frame);
  free_page->pin_count_ = 1;
  free_page->is_dirty_ = false;
  free_list_.pop_front();
  //        LOG(INFO) << "Created a new page, page_id: " << free_page->page_id_ << std::endl ;
  return free_page;
}  // namespace bustub

bool BufferPoolManager::DeletePage(page_id_t page_id) {
  // 0.   Make sure you call DiskManager::DeallocatePage!
  // 1.   Search the page table for the requested page (P).
  // 1.   If P does not exist, return true.
  // 2.   If P exists, but has a non-zero pin-count, return false. Someone is
  // using the page.
  // 3.   Otherwise, P can be deleted. Remove P from the page table, reset its
  // metadata and return it to the free list.
  std::lock_guard<std::mutex> guard{latch_};

  if (page_table_.count(page_id) == 0) {
    return true;
  }

  frame_id_t frame_id = page_table_[page_id];
  auto page_ptr = pages_.at(frame_id);
  if (page_ptr->GetPinCount() != 0) {
  }

  disk_manager_->DeallocatePage(page_id);
  page_ptr->ResetMemory();
  page_ptr->is_dirty_ = false;
  page_ptr->page_id_ = INVALID_PAGE_ID;
  page_table_.erase(page_id);
  replacer_->Unpin(frame_id);
  free_list_.push_back(frame_id);
  return true;
}

void BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::mutex> guard{latch_};
  for (auto item : page_table_) {
    page_id_t page_id = item.first;
    frame_id_t frame_id = item.second;
    auto page = pages_.at(frame_id);
    disk_manager_->WritePage(page_id, page->GetData());
    page->is_dirty_ = false;
  }
}

}  // namespace dbplay
