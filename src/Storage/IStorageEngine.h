//
// Phase 0 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// IStorageEngine is the type-erased boundary between the query side (MiniKV /
// future query engine) and any concrete storage engine. It is deliberately
// NON-templated: a single unique_ptr<IStorageEngine> can point at an engine
// storing any key/value type combination, because all types are encoded to
// bytes before they cross this seam.
//
// Declaration only in Phase 0; the first implementation (BPlusTreeEngine)
// arrives in Phase 3.
//

#ifndef DBPLAYGROUND_ISTORAGEENGINE_H
#define DBPLAYGROUND_ISTORAGEENGINE_H

#include <string>

#include "Common/Slice.h"

namespace dbplay {

class IStorageEngine {
 public:
  virtual ~IStorageEngine() = default;

  // Insert `value` for `key` if the key is absent. Returns false if the key
  // already exists (no overwrite).
  virtual bool Insert(const Slice &key, const Slice &value) = 0;

  // Look up `key`. On hit, writes the raw value bytes into *value and returns
  // true; on miss returns false.
  virtual bool Get(const Slice &key, std::string *value) = 0;

  // Remove `key`. Returns true if a key was removed.
  virtual bool Remove(const Slice &key) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ISTORAGEENGINE_H
