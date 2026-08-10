//
// T1 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// Column holds one column's worth of data for a batch of rows, in the Arrow-lite
// layout: fixed-width types are packed contiguously; variable-length types
// (String/Blob) use an offsets array + a shared bytes buffer. (Nulls / validity
// bitmap are intentionally omitted in this first cut.)
//
// One Column is one Type; a Chunk bundles several Columns for the same rows.
//

#ifndef DBPLAYGROUND_COLUMN_H
#define DBPLAYGROUND_COLUMN_H

#include <cstdint>
#include <cstring>
#include <vector>

#include "Common/Slice.h"
#include "Common/Type.h"

namespace dbplay {

class Column {
 public:
  explicit Column(Type type) : type_(type) { offsets_.push_back(0); }

  Type type() const { return type_; }
  size_t size() const { return size_; }

  // ---- build (append one value; caller matches T to type_) ----

  // Fixed-width types (Int32/Int64/Float/Double/Bool): append the raw value.
  template <typename T>
  void Append(const T &v) {
    const char *p = reinterpret_cast<const char *>(&v);
    fixed_.insert(fixed_.end(), p, p + sizeof(T));
    ++size_;
  }

  // Variable-length types (String/Blob): append raw bytes.
  void AppendBytes(const Slice &s) {
    var_bytes_.insert(var_bytes_.end(), s.data(), s.data() + s.size());
    offsets_.push_back(static_cast<uint32_t>(var_bytes_.size()));
    ++size_;
  }

  // ---- read ----

  template <typename T>
  T Get(size_t i) const {
    T v;
    std::memcpy(&v, fixed_.data() + i * sizeof(T), sizeof(T));
    return v;
  }

  Slice GetBytes(size_t i) const {
    const uint32_t start = offsets_[i];
    const uint32_t end = offsets_[i + 1];
    return Slice(var_bytes_.data() + start, end - start);
  }

 private:
  Type type_;
  size_t size_ = 0;
  std::vector<char> fixed_;        // fixed-width: packed values
  std::vector<uint32_t> offsets_;  // var-len: size_+1 offsets, offsets_[0] == 0
  std::vector<char> var_bytes_;    // var-len: concatenated payload
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_COLUMN_H
