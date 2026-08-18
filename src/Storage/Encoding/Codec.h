//
// C2 of the composable storage model (docs/design/StorageAbstraction.md).
//
// The encoding layer, which lives INSIDE a file format (it is not a top-level
// peer of the table source). Two self-describing levels, as in Parquet:
//
//   values  --Encode-->  encoded bytes  --Compress-->  stored bytes
//   values  <-Decode--   encoded bytes  <-Decompress-  stored bytes
//
// Both are chosen at runtime by an id read back from a page header, so these are
// virtual seams (a codec cannot be selected from a persisted id at compile
// time). The per-Type hot loop inside a codec uses templates -- see PlainCodec.
//

#ifndef DBPLAYGROUND_CODEC_H
#define DBPLAYGROUND_CODEC_H

#include <cstdint>
#include <string>

#include "Common/Slice.h"
#include "Table/Column.h"

namespace dbplay {

// Persisted in a page header to name the encoding (level 1).
enum class EncodingId : uint8_t {
  Invalid = 0,
  Plain = 1,  // raw values, no compression of the value stream itself
};

// Persisted in a page header to name the compression (level 2).
enum class CompressionId : uint8_t {
  Invalid = 0,
  None = 1,  // identity
  Zlib = 2,  // zlib/deflate (contrib zlib-ng, ZLIB_COMPAT)
};

// Level 1: typed values <-> encoded bytes. One codec instance handles all Types;
// it dispatches on Column::type() internally.
class ICodec {
 public:
  virtual ~ICodec() = default;
  virtual EncodingId id() const = 0;

  // Append the encoding of every value in `col` to *out.
  virtual void Encode(const Column &col, std::string *out) const = 0;

  // Decode `value_count` values from `bytes` and append them to *out (out must
  // already carry the target Type; matches RowCodec::DecodeInto's convention).
  virtual void Decode(const Slice &bytes, size_t value_count, Column *out) const = 0;

  // Bytes per value if this encoding lays values out fixed-width and directly
  // addressable (value i at base + i*width), else 0. Combined with an
  // uncompressed page, a non-zero width lets a reader fetch/decode only the
  // requested rows instead of the whole page (the direct-offset fast path).
  virtual size_t FixedWidth(Type t) const = 0;
};

// Level 2: encoded bytes <-> stored bytes. Byte-blind (does not know Types).
class ICompression {
 public:
  virtual ~ICompression() = default;
  virtual CompressionId id() const = 0;

  virtual void Compress(const Slice &in, std::string *out) const = 0;
  // `raw_size` is the decompressed length (from the page header) so the codec
  // can size the output up front.
  virtual void Decompress(const Slice &in, size_t raw_size, std::string *out) const = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CODEC_H
