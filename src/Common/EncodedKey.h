//
// Phase 2 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// EncodedKey is the fixed-width, order-preserving key the B+Tree stores. Keys
// of any (fixed-width) type are encoded to kKeyLen bytes such that byte order
// == value order (see Codec.h / KeyEncoder). The tree then orders keys with a
// plain memcmp and never needs to know the original key type or a comparator.
//
// It is a trivially-copyable POD so a leaf/internal page can hold it inline in
// its flexible array and memcpy the whole page to disk, exactly like the old
// scalar key did.
//

#ifndef DBPLAYGROUND_ENCODEDKEY_H
#define DBPLAYGROUND_ENCODEDKEY_H

#include <cstring>
#include <type_traits>

#include "Common/Codec.h"
#include "Common/Slice.h"

namespace dbplay {

struct EncodedKey {
  char bytes[kKeyLen];

  // The only two operators the tree/pages require of a key (see the page code:
  // binary search uses `<`, exact match uses `==`). memcmp gives value order
  // because the encoding is order-preserving.
  bool operator<(const EncodedKey &o) const { return std::memcmp(bytes, o.bytes, kKeyLen) < 0; }
  bool operator==(const EncodedKey &o) const { return std::memcmp(bytes, o.bytes, kKeyLen) == 0; }
};

static_assert(std::is_trivially_copyable<EncodedKey>::value, "EncodedKey must be trivially copyable (stored inline in pages)");
static_assert(sizeof(EncodedKey) == kKeyLen, "EncodedKey must be exactly kKeyLen bytes");

// Build an EncodedKey from a typed key (caller knows T at compile time).
template <typename T>
inline EncodedKey MakeEncodedKey(const T &v) {
  EncodedKey k;
  KeyEncoder<T>::Encode(v, k.bytes);
  return k;
}

// Build an EncodedKey from already-encoded bytes (must be exactly kKeyLen).
inline EncodedKey EncodedKeyFromSlice(const Slice &s) {
  EncodedKey k;
  std::memcpy(k.bytes, s.data(), kKeyLen);
  return k;
}

}  // namespace dbplay

#endif  // DBPLAYGROUND_ENCODEDKEY_H
