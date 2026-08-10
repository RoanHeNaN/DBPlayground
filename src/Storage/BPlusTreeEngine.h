//
// Phase 3 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// BPlusTreeEngine is the first concrete IStorageEngine. It ties the three
// Phase 0-2 pieces together behind the type-erased Slice boundary:
//
//   key   --(order-preserving)-->  EncodedKey  --> B+Tree index --> RID
//   value --------------------------------------> TupleStore  <----/
//
// The B+Tree and the TupleStore are siblings sharing one BufferPoolManager;
// the engine just carries the RID between them.
//

#ifndef DBPLAYGROUND_BPLUSTREEENGINE_H
#define DBPLAYGROUND_BPLUSTREEENGINE_H

#include <memory>
#include <string>

#include "Common/EncodedKey.h"
#include "Common/RID.h"
#include "Container/BPlusTree.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/IStorageEngine.h"
#include "Storage/TupleStore/TupleStore.h"

namespace dbplay {

class BPlusTreeEngine : public IStorageEngine {
 public:
  explicit BPlusTreeEngine(std::shared_ptr<BufferPoolManager> buffer_pool_manager);

  // key must be an order-preserving encoding of exactly kKeyLen bytes (produced
  // by encode_key<T>); value is opaque bytes.
  bool Insert(const Slice &key, const Slice &value) override;
  bool Get(const Slice &key, std::string *value) override;
  bool Remove(const Slice &key) override;

 private:
  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  BPlusTree index_;  // EncodedKey -> RID
  TupleStore tuples_;                 // RID -> value bytes
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_BPLUSTREEENGINE_H
