//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_replacer.h
//
// Identification: src/include/buffer/lru_replacer.h
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <list>
#include <mutex>  // NOLINT
#include <unordered_map>
#include <vector>

#include "Common/Config.h"
#include "Storage/BufferPool/IReplacer.h"

namespace dbplay {

/**
 * LRUReplacer implements the lru replacement policy, which approximates the
 * Least Recently Used policy.
 */
class LRUReplacer : public IReplacer {
 public:
  /**
   * Create a new LRUReplacer.
   * @param num_pages the maximum number of pages the LRUReplacer will be
   * required to store
   */
  explicit LRUReplacer(size_t num_pages);

  /**
   * Destroys the LRUReplacer.
   */
  ~LRUReplacer() override;

  bool Victim(frame_id_t *frame_id) override;

  void Pin(frame_id_t frame_id) override;

  void Unpin(frame_id_t frame_id) override;

  size_t Size() override;

 private:
  std::mutex lock_;
  size_t num_pages_;
  std::list<frame_id_t> pinned_pages_;
  std::list<frame_id_t> unpinned_pages_;
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> pinned_iter_map_;
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> unpinned_iter_map_;
};

}  // namespace dbplay
