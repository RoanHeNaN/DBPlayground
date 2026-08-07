#include <iostream>
#include <vector>

#include "Core/MiniKV.h"

int main() {
  dbplay::MiniKV db;

  std::vector<std::pair<dbplay::key_t, dbplay::value_t>> entries;
  for (int i = 0; i < 100; ++i) {
    entries.emplace_back(i, i + 100);
  }

  for (const auto &entry : entries) {
    db.Insert(entry.first, entry.second);
  }

  for (const auto &entry : entries) {
    std::cout << entry.first << " : " << entry.second << " in database: " << entry.first << " : " << db.Get(entry.first)
              << std::endl;
  }

  return 0;
}
