//
// Created by 何智强 on 2021/10/3.
//

#include "Storage/Disk/DiskManager.h"

#include <sys/stat.h>

namespace dbplay {

page_id_t DiskManager::AllocatePage() { return next_page_id_++; }

void DiskManager::ReadPage(page_id_t page_id, char *page_data) {
  int offset = page_id * PAGE_SIZE;
  if (offset > GetFileSize()) {
    throw std::runtime_error("page id out of range");
  }

  db_io_.seekp(page_id * PAGE_SIZE);
  db_io_.read(page_data, PAGE_SIZE);

  int read_count = db_io_.gcount();
  if (read_count < PAGE_SIZE) {
    db_io_.clear();
    memset(page_data + read_count, 0, PAGE_SIZE - read_count);
  }
}

void DiskManager::WritePage(page_id_t page_id, char *page_data) {
  size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);

  /// Flush to disk SYNC
  db_io_.flush();
}

int DiskManager::GetFileSize() const {
  struct stat stat_buf;
  int result = stat(db_file_name_.c_str(), &stat_buf);
  return result == 0 ? static_cast<int>(stat_buf.st_size) : -1;
}

void DiskManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {}

}  // namespace dbplay
