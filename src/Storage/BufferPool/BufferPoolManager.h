//
// Created by 何智强 on 2021/10/2.
//

#ifndef DBPLAYGROUND_BUFFERPOOLMANAGER_H
#define DBPLAYGROUND_BUFFERPOOLMANAGER_H

#include <list>
#include <unordered_map>

#include "Common/Config.h"
#include "Storage/BufferPool/IReplacer.h"
#include "Storage/Disk/DiskManager.h"
#include "Storage/Page/Page.h"

namespace dbplay {
class BufferPoolManager {
 public:
  BufferPoolManager() = default;
  /**
   * Create a new buffer pool manager.
   * @param slot_num Number of pages in buffer, page size is defiend in src/Common/Config.h.
   * @param disk_manager Disk manager used for page I/O.
   */
  BufferPoolManager(size_t slot_num, std::shared_ptr<DiskManager> disk_manager);

  std::shared_ptr<Page> FetchPage(page_id_t page_id);
  bool UnpinPage(page_id_t page_id, bool is_dirty);
  bool FlushPage(page_id_t page_id);
  std::shared_ptr<Page> NewPage();
  bool DeletePage(page_id_t page_id);
  void FlushAllPages();

 private:
  std::size_t slot_num_;
  std::shared_ptr<DiskManager> disk_manager_;
  std::unique_ptr<IReplacer> replacer_;
  std::vector<std::shared_ptr<Page>> pages_;
  std::list<frame_id_t> free_list_;
  std::mutex latch_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
};
}  // namespace dbplay

#endif  // DBPLAYGROUND_BUFFERPOOLMANAGER_H
