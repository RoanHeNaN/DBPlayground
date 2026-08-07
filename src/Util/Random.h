//
// Created by 何智强 on 2021/10/22.
//

#ifndef DBPLAYGROUND_RANDOM_H
#define DBPLAYGROUND_RANDOM_H

#include <cstdint>
#include <vector>

namespace dbplay {

class Random {
 public:
  int32_t GetValue();
  std::vector<int32_t> GetSequence();
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_RANDOM_H
