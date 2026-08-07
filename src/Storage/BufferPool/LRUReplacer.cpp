//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_replacer.cpp
//
// Identification: src/buffer/lru_replacer.cpp
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "Storage/BufferPool/LRUReplacer.h"

#include <cstddef>
#include <fstream>
#include <stack>

namespace dbplay {
// bool mocked = false;

LRUReplacer::LRUReplacer(size_t num_pages) : num_pages_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::Victim(frame_id_t *frame_id) {
  std::lock_guard<std::mutex> guard{lock_};
  if (unpinned_pages_.empty()) {
    return false;
  }

  *frame_id = unpinned_pages_.front();
  unpinned_pages_.pop_front();
  unpinned_iter_map_.erase(*frame_id);
  return true;
}

void LRUReplacer::Pin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> guard{lock_};
  if (unpinned_iter_map_.count(frame_id) == 0) {
    return;
  }
  /*
   * Remove a frame from unpinned pages.
   * Add it to pinned pages.
   */
  unpinned_pages_.erase(unpinned_iter_map_[frame_id]);
  unpinned_iter_map_.erase(frame_id);
  pinned_pages_.push_back(frame_id);
  pinned_iter_map_[frame_id] = --(pinned_pages_.end());
}

void LRUReplacer::Unpin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> guard{lock_};
  if (pinned_iter_map_.count(frame_id) != 0) {
    pinned_pages_.erase(pinned_iter_map_[frame_id]);
    pinned_iter_map_.erase(frame_id);
  }

  if (unpinned_iter_map_.count(frame_id) != 0 || unpinned_pages_.size() >= num_pages_) {  // Target frame is unpinned.
    return;
  }

  unpinned_pages_.push_back(frame_id);
  unpinned_iter_map_[frame_id] = --(unpinned_pages_.end());
}

size_t LRUReplacer::Size() {
  std::lock_guard<std::mutex> guard{lock_};
  return unpinned_pages_.size();
}

}  // namespace dbplay
