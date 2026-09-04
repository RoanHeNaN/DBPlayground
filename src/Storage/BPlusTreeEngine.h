//
// Phase 3-4 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// BPlusTreeEngine is the first concrete IStorageEngine. It ties the Phase 0-2
// pieces together behind the type-erased Slice boundary:
//
//   key   --(order-preserving)-->  EncodedKey  --> B+Tree index --> RID
//   value --------------------------------------> TupleStore  <----/
//
// The B+Tree and the TupleStore are siblings sharing one BufferPoolManager.
// Phase 4: the engine also owns the catalog (MetaPage at HEADER_PAGE_ID). On a
// new database it reserves page 0 and writes the meta; on an existing one it
// reads the meta and restores the tree root and TupleStore chain head. Flush()
// writes the current entry points back to the meta page and flushes all pages.
//

#ifndef DBPLAYGROUND_BPLUSTREEENGINE_H
#define DBPLAYGROUND_BPLUSTREEENGINE_H

#include <memory>
#include <string>

#include "Common/EncodedKey.h"
#include "Common/RID.h"
#include "Common/Type.h"
#include "Container/BPlusTree.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/IStorageEngine.h"
#include "Storage/TupleStore/TupleStore.h"

namespace dbplay {

class BPlusTreeEngine : public IStorageEngine {
 public:
  // `is_new` selects create (reserve + init page 0) vs open (read page 0). For
  // create, key_type/value_type are recorded in the meta; for open they are
  // read from it (the passed values are ignored).
  BPlusTreeEngine(std::shared_ptr<BufferPoolManager> buffer_pool_manager, bool is_new, Type key_type = Type::Invalid,
                  Type value_type = Type::Invalid);

  // key must be an order-preserving encoding of exactly kKeyLen bytes (produced
  // by encode_key<T>); value is opaque bytes.
  bool Insert(const Slice &key, const Slice &value) override;
  bool Get(const Slice &key, std::string *value) override;
  bool Remove(const Slice &key) override;
  void Flush() override;
  std::unique_ptr<IKvCursor> NewCursor() override;

  Type key_type() const { return key_type_; }
  Type value_type() const { return value_type_; }

 private:
  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  std::unique_ptr<BPlusTree> index_;    // EncodedKey -> RID
  std::unique_ptr<TupleStore> tuples_;  // RID -> value bytes
  Type key_type_;
  Type value_type_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_BPLUSTREEENGINE_H
