#ifndef DBPLAYGROUND_TABLEMETADATACODEC_H
#define DBPLAYGROUND_TABLEMETADATACODEC_H

#include <string>

#include "Common/Slice.h"
#include "Metadata/TableMetadataTypes.h"

namespace dbplay {

// Stable, bounds-checked binary codecs for metadata objects. Decode throws
// std::invalid_argument for corrupt, truncated, unsupported, or semantically
// invalid input.
class TableMetadataCodec {
 public:
  static std::string EncodeCurrentTableState(const CurrentTableState &state);
  static CurrentTableState DecodeCurrentTableState(const Slice &bytes);

  static std::string EncodeCommitRecord(const CommitRecord &record);
  static CommitRecord DecodeCommitRecord(const Slice &bytes);

  static std::string EncodeCompactedDataManifest(const CompactedDataManifest &manifest);
  static CompactedDataManifest DecodeCompactedDataManifest(const Slice &bytes);
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLEMETADATACODEC_H
