#ifndef DBPLAYGROUND_IMETADATASTORE_H
#define DBPLAYGROUND_IMETADATASTORE_H

#include <optional>
#include <string>
#include <utility>

#include "Common/Slice.h"

namespace dbplay {

class MetadataVersion {
 public:
  MetadataVersion() = default;
  explicit MetadataVersion(std::string opaque) : opaque_(std::move(opaque)) {}

  const std::string &opaque() const { return opaque_; }
  bool operator==(const MetadataVersion &other) const { return opaque_ == other.opaque_; }
  bool operator!=(const MetadataVersion &other) const { return !(*this == other); }

 private:
  std::string opaque_;
};

struct VersionedValue {
  std::string value;
  MetadataVersion version;
};

enum class ConditionalWriteResult { Applied, PreconditionFailed, RetryableConflict };

// Versioned key/value coordination seam used by TableMetadataStore. This is
// intentionally independent of the byte-range IStorage file abstraction.
class IMetadataStore {
 public:
  virtual ~IMetadataStore() = default;

  virtual std::optional<VersionedValue> Get(const std::string &key) const = 0;
  virtual ConditionalWriteResult PutIfAbsent(const std::string &key, const Slice &value,
                                             MetadataVersion *new_version) = 0;
  virtual ConditionalWriteResult CompareExchange(const std::string &key, const MetadataVersion &expected,
                                                 const Slice &value, MetadataVersion *new_version) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IMETADATASTORE_H
