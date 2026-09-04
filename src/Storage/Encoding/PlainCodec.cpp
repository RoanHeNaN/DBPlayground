//
// C2 of the composable storage model (docs/design/StorageAbstraction.md).
//

#include "Storage/Encoding/PlainCodec.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace dbplay {

namespace {

// ---- templated kernels (the core): T is fixed, so these inline/vectorize ----

template <typename T>
void EncodeFixed(const Column &col, std::string *out) {
  for (size_t i = 0; i < col.size(); ++i) {
    const T v = col.Get<T>(i);
    out->append(reinterpret_cast<const char *>(&v), sizeof(T));
  }
}

template <typename T>
void DecodeFixed(const Slice &in, size_t n, Column *out) {
  if (in.size() < n * sizeof(T)) {
    throw std::runtime_error("PlainCodec: truncated fixed-width payload");
  }
  const char *p = in.data();
  for (size_t i = 0; i < n; ++i) {
    T v;
    std::memcpy(&v, p + i * sizeof(T), sizeof(T));
    out->Append<T>(v);
  }
}

void EncodeVar(const Column &col, std::string *out) {
  for (size_t i = 0; i < col.size(); ++i) {
    const Slice s = col.GetBytes(i);
    const uint32_t len = static_cast<uint32_t>(s.size());
    out->append(reinterpret_cast<const char *>(&len), sizeof(len));
    out->append(s.data(), s.size());
  }
}

void DecodeVar(const Slice &in, size_t n, Column *out) {
  const char *p = in.data();
  const char *end = p + in.size();
  for (size_t i = 0; i < n; ++i) {
    if (p + sizeof(uint32_t) > end) {
      throw std::runtime_error("PlainCodec: truncated var-length header");
    }
    uint32_t len;
    std::memcpy(&len, p, sizeof(len));
    p += sizeof(len);
    if (p + len > end) {
      throw std::runtime_error("PlainCodec: truncated var-length payload");
    }
    out->AppendBytes(Slice(p, len));
    p += len;
  }
}

}  // namespace

void PlainCodec::Encode(const Column &col, std::string *out) const {
  switch (col.type()) {
    case Type::Int32:
      EncodeFixed<int32_t>(col, out);
      break;
    case Type::Int64:
      EncodeFixed<int64_t>(col, out);
      break;
    case Type::Float:
      EncodeFixed<float>(col, out);
      break;
    case Type::Double:
      EncodeFixed<double>(col, out);
      break;
    case Type::Bool:
      EncodeFixed<bool>(col, out);
      break;
    case Type::String:
    case Type::Blob:
      EncodeVar(col, out);
      break;
    default:
      throw std::invalid_argument("PlainCodec::Encode: invalid column type");
  }
}

void PlainCodec::Decode(const Slice &bytes, size_t value_count, Column *out) const {
  switch (out->type()) {
    case Type::Int32:
      DecodeFixed<int32_t>(bytes, value_count, out);
      break;
    case Type::Int64:
      DecodeFixed<int64_t>(bytes, value_count, out);
      break;
    case Type::Float:
      DecodeFixed<float>(bytes, value_count, out);
      break;
    case Type::Double:
      DecodeFixed<double>(bytes, value_count, out);
      break;
    case Type::Bool:
      DecodeFixed<bool>(bytes, value_count, out);
      break;
    case Type::String:
    case Type::Blob:
      DecodeVar(bytes, value_count, out);
      break;
    default:
      throw std::invalid_argument("PlainCodec::Decode: invalid column type");
  }
}

size_t PlainCodec::FixedWidth(Type t) const {
  switch (t) {
    case Type::Int32:
      return sizeof(int32_t);
    case Type::Int64:
      return sizeof(int64_t);
    case Type::Float:
      return sizeof(float);
    case Type::Double:
      return sizeof(double);
    case Type::Bool:
      return sizeof(bool);
    default:
      return 0;  // String / Blob are variable-length
  }
}

}  // namespace dbplay
