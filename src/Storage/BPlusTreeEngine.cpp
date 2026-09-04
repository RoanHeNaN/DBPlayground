//
// Phase 3-4 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md and BPlusTreeEngine.h.
//

#include "Storage/BPlusTreeEngine.h"

#include <stdexcept>
#include <utility>
#include <vector>

#include "Storage/MetaPage.h"
#include "Storage/Page/BPlusTreeLeafPage.h"

namespace dbplay {

namespace {
EncodedKey ToKey(const Slice &key) {
  if (key.size() != kKeyLen) {
    throw std::invalid_argument("BPlusTreeEngine: key must be a kKeyLen-byte encoded key");
  }
  return EncodedKeyFromSlice(key);
}

MetaPage *Meta(Page *page) { return reinterpret_cast<MetaPage *>(page->GetData()); }

// Ordered cursor: walks the B+Tree leaf chain and fetches each value from the
// TupleStore. It buffers one leaf's (key, RID) entries at a time (bounded pin
// time), then materializes owned copies of the current key/value so the Slices
// stay valid across page eviction.
class BPlusTreeKvCursor : public IKvCursor {
 public:
  BPlusTreeKvCursor(std::shared_ptr<BufferPoolManager> bpm, BPlusTree *index, TupleStore *tuples)
      : bpm_(std::move(bpm)), tuples_(tuples) {
    LoadLeaf(index->FirstLeafPageId());
  }

  bool Valid() const override { return valid_; }
  Slice Key() const override { return Slice(key_); }
  Slice Value() const override { return Slice(value_); }

  void Next() override {
    if (!valid_) {
      return;
    }
    if (++pos_ >= entries_.size()) {
      LoadLeaf(next_leaf_);
      return;
    }
    Materialize();
  }

 private:
  // Buffer the entries of the leaf `pid` (skipping empty leaves), or mark the
  // cursor exhausted when the chain ends.
  void LoadLeaf(page_id_t pid) {
    entries_.clear();
    pos_ = 0;
    while (pid != INVALID_PAGE_ID) {
      auto page = bpm_->FetchPage(pid);
      auto *leaf = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
      const int n = leaf->GetSize();
      next_leaf_ = leaf->GetNextPageId();
      for (int i = 0; i < n; ++i) {
        entries_.push_back(leaf->GetItem(i));
      }
      bpm_->UnpinPage(pid, false);
      if (!entries_.empty()) {
        valid_ = true;
        Materialize();
        return;
      }
      pid = next_leaf_;
    }
    valid_ = false;
  }

  void Materialize() {
    key_.assign(entries_[pos_].first.bytes, kKeyLen);
    value_.clear();
    tuples_->Get(entries_[pos_].second, &value_);
  }

  std::shared_ptr<BufferPoolManager> bpm_;
  TupleStore *tuples_;
  std::vector<std::pair<EncodedKey, RID>> entries_;
  size_t pos_ = 0;
  page_id_t next_leaf_ = INVALID_PAGE_ID;
  std::string key_;
  std::string value_;
  bool valid_ = false;
};
}  // namespace

BPlusTreeEngine::BPlusTreeEngine(std::shared_ptr<BufferPoolManager> buffer_pool_manager, bool is_new, Type key_type,
                                 Type value_type)
    : buffer_pool_manager_(std::move(buffer_pool_manager)) {
  page_id_t root_page_id = INVALID_PAGE_ID;
  page_id_t tuple_first_page_id = INVALID_PAGE_ID;

  if (is_new) {
    // Reserve page 0 as the catalog; a fresh file hands out id 0 first.
    auto meta_page = buffer_pool_manager_->NewPage();
    if (meta_page == nullptr || meta_page->GetPageId() != HEADER_PAGE_ID) {
      throw std::runtime_error("BPlusTreeEngine: could not reserve the meta page (HEADER_PAGE_ID)");
    }
    MetaPage *m = Meta(meta_page.get());
    m->magic = kMetaMagic;
    m->version = kMetaVersion;
    m->key_type = static_cast<uint8_t>(key_type);
    m->value_type = static_cast<uint8_t>(value_type);
    m->root_page_id = INVALID_PAGE_ID;
    m->tuple_first_page_id = INVALID_PAGE_ID;
    key_type_ = key_type;
    value_type_ = value_type;
    buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
  } else {
    auto meta_page = buffer_pool_manager_->FetchPage(HEADER_PAGE_ID);
    if (meta_page == nullptr) {
      throw std::runtime_error("BPlusTreeEngine: could not read the meta page");
    }
    MetaPage *m = Meta(meta_page.get());
    if (m->magic != kMetaMagic) {
      buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, false);
      throw std::runtime_error("BPlusTreeEngine: not a dbplayground file (bad magic)");
    }
    root_page_id = m->root_page_id;
    tuple_first_page_id = m->tuple_first_page_id;
    key_type_ = static_cast<Type>(m->key_type);
    value_type_ = static_cast<Type>(m->value_type);
    buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, false);
  }

  index_ = std::make_unique<BPlusTree>(buffer_pool_manager_);  // default fanout
  index_->SetRootPageId(root_page_id);                         // restore (INVALID = empty)
  tuples_ = std::make_unique<TupleStore>(buffer_pool_manager_, tuple_first_page_id);
}

bool BPlusTreeEngine::Insert(const Slice &key, const Slice &value) {
  const EncodedKey ek = ToKey(key);

  RID existing;
  if (index_->GetValue(ek, existing)) {
    return false;  // insert-if-absent
  }

  RID rid = tuples_->Put(value);
  if (!index_->Insert(ek, rid)) {
    tuples_->Delete(rid);
    return false;
  }
  return true;
}

bool BPlusTreeEngine::Get(const Slice &key, std::string *value) {
  const EncodedKey ek = ToKey(key);
  RID rid;
  if (!index_->GetValue(ek, rid)) {
    return false;
  }
  return tuples_->Get(rid, value);
}

bool BPlusTreeEngine::Remove(const Slice &key) {
  const EncodedKey ek = ToKey(key);
  RID rid;
  if (!index_->GetValue(ek, rid)) {
    return false;
  }
  index_->Remove(ek);
  tuples_->Delete(rid);
  return true;
}

void BPlusTreeEngine::Flush() {
  // Persist the current entry points into the catalog, then flush every page.
  auto meta_page = buffer_pool_manager_->FetchPage(HEADER_PAGE_ID);
  if (meta_page != nullptr) {
    MetaPage *m = Meta(meta_page.get());
    m->root_page_id = index_->GetRootPageId();
    m->tuple_first_page_id = tuples_->FirstPageId();
    buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
  }
  buffer_pool_manager_->FlushAllPages();
}

std::unique_ptr<IKvCursor> BPlusTreeEngine::NewCursor() {
  return std::make_unique<BPlusTreeKvCursor>(buffer_pool_manager_, index_.get(), tuples_.get());
}

}  // namespace dbplay
