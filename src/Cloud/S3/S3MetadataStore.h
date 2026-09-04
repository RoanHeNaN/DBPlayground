//
// IMetadataStore backed by an S3-compatible object store, using S3 conditional
// writes for the CAS the cloud commit protocol depends on. The object ETag is
// the opaque MetadataVersion:
//
//   Get            -> GET            (ETag -> MetadataVersion)
//   PutIfAbsent    -> PUT If-None-Match: *      (create-if-absent)
//   CompareExchange-> PUT If-Match: "<etag>"    (CAS)
//
// Status mapping: 200 -> Applied, 412 -> PreconditionFailed, transport error /
// 5xx -> RetryableConflict, anything else -> hard error (throw). Verified
// against MinIO (see the step-0 conditional-write spike).
//

#ifndef DBPLAYGROUND_S3_S3METADATASTORE_H
#define DBPLAYGROUND_S3_S3METADATASTORE_H

#include <memory>
#include <optional>
#include <string>

#include "Cloud/S3/S3Client.h"
#include "Metadata/IMetadataStore.h"

namespace dbplay {

class S3MetadataStore : public IMetadataStore {
 public:
  S3MetadataStore(std::shared_ptr<s3::S3Client> client, std::string bucket);

  std::optional<VersionedValue> Get(const std::string &key) const override;
  ConditionalWriteResult PutIfAbsent(const std::string &key, const Slice &value, MetadataVersion *new_version) override;
  ConditionalWriteResult CompareExchange(const std::string &key, const MetadataVersion &expected, const Slice &value,
                                         MetadataVersion *new_version) override;

 private:
  std::string ObjectKey(const std::string &key) const { return bucket_ + "/" + key; }
  // Resolve the ETag after a successful write; PUT usually returns it, else HEAD.
  MetadataVersion ResolveVersion(const std::string &key, const s3::S3Response &put_response) const;

  std::shared_ptr<s3::S3Client> client_;
  std::string bucket_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_S3_S3METADATASTORE_H
