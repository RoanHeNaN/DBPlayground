//
// C4 of the composable storage model (docs/design/StorageAbstraction.md).
//

#include "Storage/Encoding/ZlibCompression.h"

#include <zlib.h>  // contrib zlib-ng, ZLIB_COMPAT -> standard zlib API

#include <stdexcept>

namespace dbplay {

// Both directions append to *out (matching NoCompression), so a codec can build
// a page incrementally. raw_size (the pre-compression length, kept in the page
// footer) sizes the inflate buffer exactly, so no growth loop is needed.

void ZlibCompression::Compress(const Slice &in, std::string *out) const {
  const uLong bound = compressBound(static_cast<uLong>(in.size()));
  const size_t start = out->size();
  out->resize(start + bound);

  uLongf dest_len = bound;
  const int rc =
      compress2(reinterpret_cast<Bytef *>(out->data() + start), &dest_len, reinterpret_cast<const Bytef *>(in.data()),
                static_cast<uLong>(in.size()), Z_DEFAULT_COMPRESSION);
  if (rc != Z_OK) {
    throw std::runtime_error("ZlibCompression: compress2 failed");
  }
  out->resize(start + dest_len);
}

void ZlibCompression::Decompress(const Slice &in, size_t raw_size, std::string *out) const {
  const size_t start = out->size();
  if (raw_size == 0) {
    return;  // nothing to inflate
  }
  out->resize(start + raw_size);

  uLongf dest_len = static_cast<uLongf>(raw_size);
  const int rc = uncompress(reinterpret_cast<Bytef *>(out->data() + start), &dest_len,
                            reinterpret_cast<const Bytef *>(in.data()), static_cast<uLong>(in.size()));
  if (rc != Z_OK || dest_len != raw_size) {
    throw std::runtime_error("ZlibCompression: uncompress failed or size mismatch");
  }
}

}  // namespace dbplay
