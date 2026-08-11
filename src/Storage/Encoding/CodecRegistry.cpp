//
// C2 of the composable storage model (docs/design/StorageAbstraction.md).
//

#include "Storage/Encoding/CodecRegistry.h"

#include <stdexcept>

#include "Storage/Encoding/NoCompression.h"
#include "Storage/Encoding/PlainCodec.h"
#include "Storage/Encoding/ZlibCompression.h"

namespace dbplay {

const CodecRegistry &CodecRegistry::Instance() {
  static const CodecRegistry kRegistry;
  return kRegistry;
}

const ICodec &CodecRegistry::Get(EncodingId id) const {
  static const PlainCodec kPlain;
  switch (id) {
    case EncodingId::Plain:
      return kPlain;
    default:
      throw std::invalid_argument("CodecRegistry: unknown EncodingId");
  }
}

const ICompression &CodecRegistry::Get(CompressionId id) const {
  static const NoCompression kNone;
  static const ZlibCompression kZlib;
  switch (id) {
    case CompressionId::None:
      return kNone;
    case CompressionId::Zlib:
      return kZlib;
    default:
      throw std::invalid_argument("CodecRegistry: unknown CompressionId");
  }
}

}  // namespace dbplay
