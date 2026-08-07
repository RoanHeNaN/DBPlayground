//
// Created by 何智强 on 2021/10/5.
//

#ifndef DBPLAYGROUND_HASHTABLEHEADERPAGE_H
#define DBPLAYGROUND_HASHTABLEHEADERPAGE_H

#include "Common/Config.h"

namespace dbplay {
class HashTableHeaderPage {
 private:
  page_id_t page_id_;
  size_t size_;

  page_id_t bucket_page_ids_[0];
};
}  // namespace dbplay

#endif  // DBPLAYGROUND_HASHTABLEHEADERPAGE_H
