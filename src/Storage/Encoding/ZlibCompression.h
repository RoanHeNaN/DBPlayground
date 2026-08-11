//
// C4 of the composable storage model (docs/design/StorageAbstraction.md).
//
// ZlibCompression is the first real ICompression: it plugs the vendored zlib-ng
// (contrib, built with ZLIB_COMPAT) behind the same seam as NoCompression. A
// file written with CompressionId::Zlib self-describes that in its page footer,
// so the reader resolves this codec through the registry with no format change
// -- exactly the extensibility the two-level codec design promised.
//

#ifndef DBPLAYGROUND_ZLIBCOMPRESSION_H
#define DBPLAYGROUND_ZLIBCOMPRESSION_H

#include <string>

#include "Storage/Encoding/Codec.h"

namespace dbplay {

class ZlibCompression : public ICompression {
 public:
  CompressionId id() const override { return CompressionId::Zlib; }
  void Compress(const Slice &in, std::string *out) const override;
  void Decompress(const Slice &in, size_t raw_size, std::string *out) const override;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ZLIBCOMPRESSION_H
