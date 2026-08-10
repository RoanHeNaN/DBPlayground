#include <cstdint>
#include <iostream>
#include <string>

#include "Common/Codec.h"
#include "Common/Slice.h"
#include "Core/MiniKV.h"

int main() {
  dbplay::MiniKV db;

  // Callers know their own types at compile time and encode to bytes; MiniKV
  // itself is type-erased (Slice in, bytes out).
  for (int64_t i = 0; i < 100; ++i) {
    db.Insert(dbplay::encode_key<int64_t>(i), dbplay::encode_value<int32_t>(static_cast<int32_t>(i + 100)));
  }

  for (int64_t i = 0; i < 100; ++i) {
    std::string raw;
    if (db.Get(dbplay::encode_key<int64_t>(i), &raw)) {
      std::cout << i << " : " << dbplay::decode_value<int32_t>(dbplay::Slice(raw)) << std::endl;
    }
  }

  return 0;
}
