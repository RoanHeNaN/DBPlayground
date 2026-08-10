//
// Created by 何智强 on 2021/10/3.
//

#ifndef DBPLAYGROUND_DISKMANAGER_H
#define DBPLAYGROUND_DISKMANAGER_H

#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include "Common/Config.h"

namespace dbplay {

class DiskManager {
 public:
  DiskManager() = delete;
  explicit DiskManager(std::string db_file) : db_file_name_(std::move(db_file)), next_page_id_(0) {
    std::string::size_type extension_position = db_file_name_.rfind('.');
    if (extension_position == std::string::npos) {
      std::cout << "wrong file format";
      return;
    }

    db_io_.open(db_file_name_, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_io_.is_open()) {
      db_io_.clear();
      // create a new file
      db_io_.open(db_file_name_, std::ios::binary | std::ios::trunc | std::ios::out);
      db_io_.close();
      // reopen with original mode
      db_io_.open(db_file_name_, std::ios::binary | std::ios::in | std::ios::out);
      if (!db_io_.is_open()) {
        throw std::runtime_error("can't open db file");
      }
    }

    // Resume page allocation past the pages already on disk, so reopening an
    // existing database does not hand out ids that overwrite live pages.
    int file_size = GetFileSize();
    if (file_size > 0) {
      next_page_id_ = file_size / PAGE_SIZE;
    }
  }

  void ReadPage(page_id_t page_id, char *page_data);
  void WritePage(page_id_t page_id, char *page_data);

  page_id_t AllocatePage();
  void DeallocatePage(page_id_t page_id);

  // Next id AllocatePage would hand out. 0 means an empty (new) database file.
  page_id_t GetNextPageId() const { return next_page_id_; }

 private:
  const std::string db_file_name_;
  std::fstream db_io_;
  std::atomic<page_id_t> next_page_id_;

  int GetFileSize() const;
};
}  // namespace dbplay

#endif  // DBPLAYGROUND_DISKMANAGER_H
