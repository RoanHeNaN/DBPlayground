#ifndef DBPLAYGROUND_MEMMETADATASTORE_H
#define DBPLAYGROUND_MEMMETADATASTORE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Metadata/IMetadataStore.h"

namespace dbplay {

class MemMetadataStore : public IMetadataStore {
 public:
  std::optional<VersionedValue> Get(const std::string &key) const override;
  ConditionalWriteResult PutIfAbsent(const std::string &key, const Slice &value, MetadataVersion *new_version) override;
  ConditionalWriteResult CompareExchange(const std::string &key, const MetadataVersion &expected, const Slice &value,
                                         MetadataVersion *new_version) override;

  void DeleteForTest(const std::string &key);

 private:
  struct Entry {
    std::string value;
    MetadataVersion version;
  };

  MetadataVersion NextVersionLocked();

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  uint64_t next_version_ = 1;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_MEMMETADATASTORE_H
