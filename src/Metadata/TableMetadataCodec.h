#ifndef DBPLAYGROUND_TABLEMETADATACODEC_H
#define DBPLAYGROUND_TABLEMETADATACODEC_H

#include <string>

#include "Common/Slice.h"
#include "Metadata/TableState.h"

namespace dbplay {

// Stable, bounds-checked binary codecs for metadata objects. Decode throws
// std::invalid_argument for corrupt, truncated, unsupported, or semantically
// invalid input.
class TableMetadataCodec {
 public:
  static std::string EncodeTableState(const TableState &state);
  static TableState DecodeTableState(const Slice &bytes);

  static std::string EncodeCommitRecord(const CommitRecord &record);
  static CommitRecord DecodeCommitRecord(const Slice &bytes);

  static std::string EncodeBaseManifest(const BaseManifest &manifest);
  static BaseManifest DecodeBaseManifest(const Slice &bytes);
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLEMETADATACODEC_H
