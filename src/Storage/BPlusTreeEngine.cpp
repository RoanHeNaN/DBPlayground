//
// Phase 3 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md and BPlusTreeEngine.h.
//

#include "Storage/BPlusTreeEngine.h"

#include <stdexcept>

namespace dbplay {

namespace {
EncodedKey ToKey(const Slice &key) {
  if (key.size() != kKeyLen) {
    throw std::invalid_argument("BPlusTreeEngine: key must be a kKeyLen-byte encoded key");
  }
  return EncodedKeyFromSlice(key);
}
}  // namespace

BPlusTreeEngine::BPlusTreeEngine(std::shared_ptr<BufferPoolManager> buffer_pool_manager)
    : buffer_pool_manager_(std::move(buffer_pool_manager)), index_(buffer_pool_manager_), tuples_(buffer_pool_manager_) {}

bool BPlusTreeEngine::Insert(const Slice &key, const Slice &value) {
  const EncodedKey ek = ToKey(key);

  // Insert-if-absent: check first so a duplicate does not orphan a tuple.
  RID existing;
  if (index_.GetValue(ek, existing)) {
    return false;
  }

  RID rid = tuples_.Put(value);
  if (!index_.Insert(ek, rid)) {
    // Lost a race / unexpected duplicate: reclaim the tuple we just wrote.
    tuples_.Delete(rid);
    return false;
  }
  return true;
}

bool BPlusTreeEngine::Get(const Slice &key, std::string *value) {
  const EncodedKey ek = ToKey(key);
  RID rid;
  if (!index_.GetValue(ek, rid)) {
    return false;
  }
  return tuples_.Get(rid, value);
}

bool BPlusTreeEngine::Remove(const Slice &key) {
  const EncodedKey ek = ToKey(key);
  RID rid;
  if (!index_.GetValue(ek, rid)) {
    return false;
  }
  index_.Remove(ek);
  tuples_.Delete(rid);
  return true;
}

}  // namespace dbplay
