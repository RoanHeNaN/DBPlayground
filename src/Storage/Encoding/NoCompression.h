//
// C2 of the composable storage model (docs/design/StorageAbstraction.md).
//
// The identity compression: bytes pass through unchanged. The first ICompression
// so the file format has a valid CompressionId to persist; real codecs (zlib is
// already in contrib) drop in behind the same seam later.
//

#ifndef DBPLAYGROUND_NOCOMPRESSION_H
#define DBPLAYGROUND_NOCOMPRESSION_H

#include <string>

#include "Storage/Encoding/Codec.h"

namespace dbplay {

class NoCompression : public ICompression {
 public:
  CompressionId id() const override { return CompressionId::None; }
  void Compress(const Slice &in, std::string *out) const override { out->append(in.data(), in.size()); }
  void Decompress(const Slice &in, size_t /*raw_size*/, std::string *out) const override {
    out->append(in.data(), in.size());
  }
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_NOCOMPRESSION_H
