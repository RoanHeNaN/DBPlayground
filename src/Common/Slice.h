//
// Phase 0 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// A Slice is a lightweight, non-owning view over a byte range: just a pointer
// and a length. It is the type-erased currency at the storage-engine boundary
// (see docs/template/TypeSystem.md).
//

#ifndef DBPLAYGROUND_SLICE_H
#define DBPLAYGROUND_SLICE_H

#include <cstring>
#include <string>

namespace dbplay {

class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char *data, size_t size) : data_(data), size_(size) {}
  // NOLINTNEXTLINE implicit conversions are intentional for ergonomics.
  Slice(const std::string &s) : data_(s.data()), size_(s.size()) {}
  // NOLINTNEXTLINE
  Slice(const char *s) : data_(s), size_(std::strlen(s)) {}

  const char *data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  char operator[](size_t n) const { return data_[n]; }

  std::string ToString() const { return std::string(data_, size_); }

  // Three-way byte comparison: <0, 0, >0. Because keys are stored with an
  // order-preserving encoding, this memcmp is the ONLY ordering the tree needs.
  int compare(const Slice &b) const {
    const size_t min_len = size_ < b.size_ ? size_ : b.size_;
    int r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) {
        r = -1;
      } else if (size_ > b.size_) {
        r = 1;
      }
    }
    return r;
  }

 private:
  const char *data_;
  size_t size_;
};

inline bool operator==(const Slice &x, const Slice &y) {
  return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size()) == 0;
}
inline bool operator!=(const Slice &x, const Slice &y) { return !(x == y); }

}  // namespace dbplay

#endif  // DBPLAYGROUND_SLICE_H
