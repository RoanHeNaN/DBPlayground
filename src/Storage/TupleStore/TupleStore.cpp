//
// Phase 1 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md and TupleStore.h.
//

#include "Storage/TupleStore/TupleStore.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace dbplay {

namespace {

// On-disk layout of a slotted value page, overlaid on Page::GetData().
// All fields are 4 bytes so the slot directory stays naturally aligned.
struct ValuePageHeader {
  page_id_t next_page_id;  // next page in the chain, or INVALID_PAGE_ID
  uint32_t num_slots;      // slot-directory length (tombstones included)
  uint32_t free_ptr;       // top of the data region (grows down from PAGE_SIZE)
};

struct ValueSlot {
  uint32_t offset;  // byte offset of the value within the page
  uint32_t length;  // value length, or kTombstone if deleted
};

constexpr uint32_t kTombstone = 0xFFFFFFFFu;

ValuePageHeader *Header(Page *page) { return reinterpret_cast<ValuePageHeader *>(page->GetData()); }

ValueSlot *Slots(Page *page) { return reinterpret_cast<ValueSlot *>(page->GetData() + sizeof(ValuePageHeader)); }

// Bytes available for one more (slot entry + value) in this page.
size_t FreeSpace(const ValuePageHeader *hdr) {
  size_t slot_dir_end = sizeof(ValuePageHeader) + static_cast<size_t>(hdr->num_slots) * sizeof(ValueSlot);
  return hdr->free_ptr - slot_dir_end;
}

void InitValuePage(Page *page, page_id_t next) {
  ValuePageHeader *hdr = Header(page);
  hdr->next_page_id = next;
  hdr->num_slots = 0;
  hdr->free_ptr = PAGE_SIZE;
}

}  // namespace

TupleStore::TupleStore(std::shared_ptr<BufferPoolManager> buffer_pool_manager, page_id_t first_page_id)
    : buffer_pool_manager_(std::move(buffer_pool_manager)), first_page_id_(first_page_id) {}

size_t TupleStore::MaxValueSize() { return PAGE_SIZE - sizeof(ValuePageHeader) - sizeof(ValueSlot); }

std::shared_ptr<Page> TupleStore::FindOrCreatePageWithSpace(size_t need, page_id_t *out_page_id) {
  // Walk the existing chain looking for a page that can hold `need` bytes.
  page_id_t pid = first_page_id_;
  while (pid != INVALID_PAGE_ID) {
    auto page = buffer_pool_manager_->FetchPage(pid);
    if (FreeSpace(Header(page.get())) >= need) {
      *out_page_id = pid;
      return page;  // returned still pinned
    }
    page_id_t next = Header(page.get())->next_page_id;
    buffer_pool_manager_->UnpinPage(pid, false);
    pid = next;
  }

  // None fit: allocate a fresh page and link it at the head of the chain.
  auto page = buffer_pool_manager_->NewPage();
  if (page == nullptr) {
    throw std::runtime_error("TupleStore: buffer pool full, cannot allocate value page");
  }
  page_id_t new_pid = page->GetPageId();
  InitValuePage(page.get(), first_page_id_);
  first_page_id_ = new_pid;
  *out_page_id = new_pid;
  return page;
}

RID TupleStore::Put(const Slice &value) {
  if (value.size() > MaxValueSize()) {
    throw std::runtime_error("TupleStore: value too large for a single page");
  }

  const size_t need = sizeof(ValueSlot) + value.size();
  page_id_t pid;
  auto page = FindOrCreatePageWithSpace(need, &pid);

  ValuePageHeader *hdr = Header(page.get());
  const uint32_t data_off = hdr->free_ptr - static_cast<uint32_t>(value.size());
  std::memcpy(page->GetData() + data_off, value.data(), value.size());
  hdr->free_ptr = data_off;

  const uint32_t idx = hdr->num_slots;
  ValueSlot *slots = Slots(page.get());
  slots[idx].offset = data_off;
  slots[idx].length = static_cast<uint32_t>(value.size());
  hdr->num_slots++;

  buffer_pool_manager_->UnpinPage(pid, true);
  return RID(pid, idx);
}

bool TupleStore::Get(const RID &rid, std::string *out) {
  const page_id_t pid = rid.GetPageId();
  auto page = buffer_pool_manager_->FetchPage(pid);
  if (page == nullptr) {
    return false;
  }

  ValuePageHeader *hdr = Header(page.get());
  const uint32_t idx = rid.GetSlotNum();
  bool ok = false;
  if (idx < hdr->num_slots) {
    ValueSlot slot = Slots(page.get())[idx];
    if (slot.length != kTombstone) {
      // Copy out: after Unpin the page may be evicted, so we cannot hand back a
      // pointer into it.
      out->assign(page->GetData() + slot.offset, slot.length);
      ok = true;
    }
  }
  buffer_pool_manager_->UnpinPage(pid, false);
  return ok;
}

bool TupleStore::Delete(const RID &rid) {
  const page_id_t pid = rid.GetPageId();
  auto page = buffer_pool_manager_->FetchPage(pid);
  if (page == nullptr) {
    return false;
  }

  ValuePageHeader *hdr = Header(page.get());
  const uint32_t idx = rid.GetSlotNum();
  bool removed = false;
  if (idx < hdr->num_slots) {
    ValueSlot *slots = Slots(page.get());
    if (slots[idx].length != kTombstone) {
      slots[idx].length = kTombstone;
      removed = true;
    }
  }
  buffer_pool_manager_->UnpinPage(pid, removed);
  return removed;
}

RID TupleStore::Update(const RID &rid, const Slice &value) {
  const page_id_t pid = rid.GetPageId();
  auto page = buffer_pool_manager_->FetchPage(pid);
  if (page != nullptr) {
    ValuePageHeader *hdr = Header(page.get());
    const uint32_t idx = rid.GetSlotNum();
    if (idx < hdr->num_slots) {
      ValueSlot *slots = Slots(page.get());
      // In-place only when the new bytes fit within the current allocation.
      if (slots[idx].length != kTombstone && value.size() <= slots[idx].length) {
        std::memcpy(page->GetData() + slots[idx].offset, value.data(), value.size());
        slots[idx].length = static_cast<uint32_t>(value.size());
        buffer_pool_manager_->UnpinPage(pid, true);
        return rid;
      }
    }
    buffer_pool_manager_->UnpinPage(pid, false);
  }

  // Does not fit in place: relocate. RID changes; the engine updates the index.
  Delete(rid);
  return Put(value);
}

}  // namespace dbplay
