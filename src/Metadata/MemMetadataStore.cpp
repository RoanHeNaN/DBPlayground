#include "Metadata/MemMetadataStore.h"

#include <utility>

namespace dbplay {

std::optional<VersionedValue> MemMetadataStore::Get(const std::string &key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return VersionedValue{it->second.value, it->second.version};
}

ConditionalWriteResult MemMetadataStore::PutIfAbsent(const std::string &key, const Slice &value,
                                                     MetadataVersion *new_version) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.count(key) != 0) {
    return ConditionalWriteResult::PreconditionFailed;
  }
  MetadataVersion version = NextVersionLocked();
  entries_.emplace(key, Entry{std::string(value.data(), value.size()), version});
  if (new_version != nullptr) {
    *new_version = std::move(version);
  }
  return ConditionalWriteResult::Applied;
}

ConditionalWriteResult MemMetadataStore::CompareExchange(const std::string &key, const MetadataVersion &expected,
                                                         const Slice &value, MetadataVersion *new_version) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.version != expected) {
    return ConditionalWriteResult::PreconditionFailed;
  }
  MetadataVersion version = NextVersionLocked();
  it->second = Entry{std::string(value.data(), value.size()), version};
  if (new_version != nullptr) {
    *new_version = std::move(version);
  }
  return ConditionalWriteResult::Applied;
}

void MemMetadataStore::DeleteForTest(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(key);
}

MetadataVersion MemMetadataStore::NextVersionLocked() { return MetadataVersion(std::to_string(next_version_++)); }

}  // namespace dbplay
