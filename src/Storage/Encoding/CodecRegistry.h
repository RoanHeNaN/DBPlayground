//
// C2 of the composable storage model (docs/design/StorageAbstraction.md).
//
// CodecRegistry maps a persisted id (read from a page header) back to the codec
// instance. This is the indirection that makes a file self-describing: the
// reader learns "encoding=Plain, compression=None" from the bytes, then asks the
// registry for the implementations -- it does not hard-code them. New encodings
// register here without touching the read/write path or the file layout.
//

#ifndef DBPLAYGROUND_CODECREGISTRY_H
#define DBPLAYGROUND_CODECREGISTRY_H

#include "Storage/Encoding/Codec.h"

namespace dbplay {

class CodecRegistry {
 public:
  // The process-wide registry (codecs are stateless singletons).
  static const CodecRegistry &Instance();

  // Throw std::invalid_argument if the id is unknown.
  const ICodec &Get(EncodingId id) const;
  const ICompression &Get(CompressionId id) const;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_CODECREGISTRY_H
