//
// Phase 0 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// Two encoders, both living BELOW the tree (the doc's Column<T> layer):
//
//   * KeyEncoder<T>  -- ORDER-PRESERVING, fixed-width. Encodes a key into 8
//                       bytes such that byte order == value order, so the tree
//                       only ever memcmp()s and never knows the key type.
//   * Codec<T>       -- value bytes, NO ordering required (values are opaque).
//
// Templates are confined here on purpose: the caller, who knows its own type at
// compile time, instantiates these; everything above the seam is type-erased.
//

#ifndef DBPLAYGROUND_CODEC_H
#define DBPLAYGROUND_CODEC_H

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "Common/Slice.h"

namespace dbplay {

// ---------------------------------------------------------------------------
// Order-preserving key encoding
// ---------------------------------------------------------------------------
//
// Every fixed-width key encodes to exactly kKeyLen bytes.
static constexpr size_t kKeyLen = 8;

namespace codec_detail {

inline void PutBigEndian64(char *dst, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    dst[i] = static_cast<char>(v & 0xFF);
    v >>= 8;
  }
}

// Signed integers: flip the sign bit, then store big-endian. This maps the
// signed range monotonically onto the unsigned range, so memcmp == value order.
inline void EncodeInt(int64_t v, char out[kKeyLen]) {
  uint64_t u = static_cast<uint64_t>(v) ^ (1ULL << 63);
  PutBigEndian64(out, u);
}

// IEEE-754 order-preserving trick: for positive numbers flip only the sign bit;
// for negative numbers flip all bits. Then big-endian memcmp == value order.
inline void EncodeDouble(double d, char out[kKeyLen]) {
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof(bits));
  if (bits & (1ULL << 63)) {
    bits = ~bits;
  } else {
    bits |= (1ULL << 63);
  }
  PutBigEndian64(out, bits);
}

}  // namespace codec_detail

// KeyEncoder<T>::Encode writes kKeyLen order-preserving bytes for a key of type
// T. Only fixed-width types are supported as keys for now.
template <typename T>
struct KeyEncoder {
  static_assert(sizeof(T) == 0, "KeyEncoder: unsupported key type");
};

template <>
struct KeyEncoder<int64_t> {
  static void Encode(int64_t v, char out[kKeyLen]) { codec_detail::EncodeInt(v, out); }
  static std::string Encode(int64_t v) {
    char buf[kKeyLen];
    Encode(v, buf);
    return std::string(buf, kKeyLen);
  }
};

template <>
struct KeyEncoder<int32_t> {
  // Sign-extend to int64 (order- and value-preserving), then reuse.
  static void Encode(int32_t v, char out[kKeyLen]) { codec_detail::EncodeInt(static_cast<int64_t>(v), out); }
  static std::string Encode(int32_t v) {
    char buf[kKeyLen];
    Encode(v, buf);
    return std::string(buf, kKeyLen);
  }
};

template <>
struct KeyEncoder<bool> {
  static void Encode(bool v, char out[kKeyLen]) { codec_detail::EncodeInt(v ? 1 : 0, out); }
  static std::string Encode(bool v) {
    char buf[kKeyLen];
    Encode(v, buf);
    return std::string(buf, kKeyLen);
  }
};

template <>
struct KeyEncoder<double> {
  static void Encode(double v, char out[kKeyLen]) { codec_detail::EncodeDouble(v, out); }
  static std::string Encode(double v) {
    char buf[kKeyLen];
    Encode(v, buf);
    return std::string(buf, kKeyLen);
  }
};

template <>
struct KeyEncoder<float> {
  // float -> double is exact and monotonic, then reuse the double encoding.
  static void Encode(float v, char out[kKeyLen]) { codec_detail::EncodeDouble(static_cast<double>(v), out); }
  static std::string Encode(float v) {
    char buf[kKeyLen];
    Encode(v, buf);
    return std::string(buf, kKeyLen);
  }
};

// ---------------------------------------------------------------------------
// Value codec (no ordering; values are opaque bytes)
// ---------------------------------------------------------------------------
//
// Scalars are stored as their raw in-memory bytes (host endianness is fine
// because values are never compared). Strings/blobs are stored verbatim.
template <typename T>
struct Codec {
  static_assert(std::is_trivially_copyable<T>::value, "Codec<T>: use a specialization for non-trivial T");
  static std::string Encode(const T &v) { return std::string(reinterpret_cast<const char *>(&v), sizeof(T)); }
  static T Decode(const Slice &s) {
    T v;
    std::memcpy(&v, s.data(), sizeof(T));
    return v;
  }
};

template <>
struct Codec<std::string> {
  static std::string Encode(const std::string &v) { return v; }
  static std::string Decode(const Slice &s) { return s.ToString(); }
};

// ---------------------------------------------------------------------------
// Free helpers for callers (they know their own types at compile time)
// ---------------------------------------------------------------------------
template <typename T>
std::string encode_key(const T &k) {
  return KeyEncoder<T>::Encode(k);
}

template <typename T>
std::string encode_value(const T &v) {
  return Codec<T>::Encode(v);
}

template <typename T>
T decode_value(const Slice &s) {
  return Codec<T>::Decode(s);
}

}  // namespace dbplay

#endif  // DBPLAYGROUND_CODEC_H
