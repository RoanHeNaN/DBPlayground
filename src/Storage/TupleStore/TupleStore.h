//
// Phase 1 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// TupleStore is the "addressable persistent byte store" that holds variable-
// length values. The B+Tree stores only a fixed-size RID; the actual bytes live
// here. It is a SIBLING of the B+Tree: both sit directly on the shared
// BufferPoolManager and read/write their own pages in the same db file. The
// only link between them is the RID value the engine carries across.
//
// Storage model: a chain of slotted value pages.
//   page: [ header | slot[0] slot[1] ... ->   ...free...   <- ...v1 v0 ]
//   RID  : (page_id, slot_num) -> which page, which slot directory entry.
//
// Phase 1 scope (see the doc): single-page values only (asserted), free space
// from deletes is tombstoned but not yet reclaimed, and a growing Update
// relocates and returns a (possibly new) RID.
//

#ifndef DBPLAYGROUND_TUPLESTORE_H
#define DBPLAYGROUND_TUPLESTORE_H

#include <memory>
#include <string>

#include "Common/RID.h"
#include "Common/Slice.h"
#include "Storage/BufferPool/BufferPoolManager.h"
#include "Storage/Page/Page.h"

namespace dbplay {

class TupleStore {
 public:
  // `first_page_id` links to an existing value-page chain (e.g. restored from a
  // meta page in Phase 4); INVALID_PAGE_ID means an empty store.
  explicit TupleStore(std::shared_ptr<BufferPoolManager> buffer_pool_manager,
                      page_id_t first_page_id = INVALID_PAGE_ID);

  // Store `value`, returning a stable locator. Throws if `value` exceeds what a
  // single page can hold (single-page limitation of Phase 1).
  RID Put(const Slice &value);

  // Fetch the bytes for `rid` into *out. Returns false on a missing/deleted rid.
  // Copies out, so the result stays valid after the page is unpinned/evicted.
  bool Get(const RID &rid, std::string *out);

  // Tombstone `rid`. Returns true if a live value was removed.
  bool Delete(const RID &rid);

  // Overwrite the value at `rid`. If the new bytes fit in place the RID is
  // unchanged; otherwise the value is relocated and a NEW RID is returned (the
  // engine must then update the B+Tree entry).
  RID Update(const RID &rid, const Slice &value);

  // Head of the value-page chain, so a meta page can persist it (Phase 4).
  page_id_t FirstPageId() const { return first_page_id_; }

  // Largest value that fits in one freshly-allocated page.
  static size_t MaxValueSize();

 private:
  // Returns a page (still pinned) with at least `need` bytes free, allocating
  // and linking a fresh page if none in the chain qualifies.
  std::shared_ptr<Page> FindOrCreatePageWithSpace(size_t need, page_id_t *out_page_id);

  std::shared_ptr<BufferPoolManager> buffer_pool_manager_;
  page_id_t first_page_id_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TUPLESTORE_H
