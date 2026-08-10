//
// Phase 4 of the storage-engine type-system refactor.
// See docs/design/StorageEngineRefactor.md.
//
// MetaPage is the catalog overlaid on page 0 (HEADER_PAGE_ID) of the db file.
// It records the schema and the entry points that let a database be reopened:
// the B+Tree root and the TupleStore chain head. (The page allocator resumes
// from the file size, so next_page_id need not be stored here.)
//

#ifndef DBPLAYGROUND_METAPAGE_H
#define DBPLAYGROUND_METAPAGE_H

#include <cstdint>

#include "Common/Config.h"
#include "Common/Type.h"

namespace dbplay {

constexpr uint32_t kMetaMagic = 0x44425047;  // 'DBPG'
constexpr uint32_t kMetaVersion = 1;

// Overlaid on the raw bytes of page 0 via reinterpret_cast<MetaPage*>(data).
struct MetaPage {
  uint32_t magic;                 // kMetaMagic, identifies a dbplayground file
  uint32_t version;               // kMetaVersion / encoding version
  uint8_t key_type;               // Type (declared key type)
  uint8_t value_type;             // Type (declared value type)
  uint8_t pad[2];                 // keep the following fields 4-byte aligned
  page_id_t root_page_id;         // B+Tree root, or INVALID_PAGE_ID if empty
  page_id_t tuple_first_page_id;  // TupleStore chain head, or INVALID_PAGE_ID
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_METAPAGE_H
