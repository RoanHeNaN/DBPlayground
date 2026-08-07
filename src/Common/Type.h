//
// Phase 0 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// The set of scalar types a DBPlayground database can declare for its key and
// value. This lives ABOVE the tree: it is used for encode/decode and for
// runtime validation, never for comparison inside the tree.
//

#ifndef DBPLAYGROUND_TYPE_H
#define DBPLAYGROUND_TYPE_H

#include <cstdint>

namespace dbplay {

enum class Type : uint8_t {
  Invalid = 0,
  Int32,
  Int64,
  Float,
  Double,
  Bool,
  String,
  Blob,
};

// Fixed-width types are the only ones currently allowed as KEYS (they encode to
// a fixed-width EncodedKey). String/Blob are allowed as VALUES (opaque bytes in
// the TupleStore) but not yet as keys — that needs variable-length keys.
inline bool IsFixedWidthType(Type t) {
  switch (t) {
    case Type::Int32:
    case Type::Int64:
    case Type::Float:
    case Type::Double:
    case Type::Bool:
      return true;
    default:
      return false;
  }
}

}  // namespace dbplay

#endif  // DBPLAYGROUND_TYPE_H
