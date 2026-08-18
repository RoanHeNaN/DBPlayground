//
// C2 of the composable storage model (docs/design/StorageAbstraction.md).
//
// PlainCodec: the identity encoding. Fixed-width values are written packed
// (value_count * sizeof(T)); variable-length values as (uint32 length + bytes)
// per value. The public entry points are virtual (the seam); the per-Type hot
// loops are templated kernels dispatched once on Column::type() (the core).
//

#ifndef DBPLAYGROUND_PLAINCODEC_H
#define DBPLAYGROUND_PLAINCODEC_H

#include <string>

#include "Storage/Encoding/Codec.h"

namespace dbplay {

class PlainCodec : public ICodec {
 public:
  EncodingId id() const override { return EncodingId::Plain; }
  void Encode(const Column &col, std::string *out) const override;
  void Decode(const Slice &bytes, size_t value_count, Column *out) const override;
  size_t FixedWidth(Type t) const override;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_PLAINCODEC_H
