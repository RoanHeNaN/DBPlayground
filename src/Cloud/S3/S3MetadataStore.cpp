#include "Cloud/S3/S3MetadataStore.h"

#include <stdexcept>
#include <utility>

namespace dbplay {

S3MetadataStore::S3MetadataStore(std::shared_ptr<s3::S3Client> client, std::string bucket)
    : client_(std::move(client)), bucket_(std::move(bucket)) {
  if (client_ == nullptr || bucket_.empty()) {
    throw std::invalid_argument("S3MetadataStore: null client or empty bucket");
  }
}

std::optional<VersionedValue> S3MetadataStore::Get(const std::string &key) const {
  const s3::S3Response r = client_->Get(ObjectKey(key));
  if (r.transport_error) throw std::runtime_error("S3MetadataStore: transport error on GET " + key);
  if (r.status == 404) return std::nullopt;
  if (r.status != 200) {
    throw std::runtime_error("S3MetadataStore: GET " + key + " status " + std::to_string(r.status));
  }
  if (r.etag.empty()) throw std::runtime_error("S3MetadataStore: GET " + key + " returned no ETag");
  return VersionedValue{r.body, MetadataVersion(r.etag)};
}

MetadataVersion S3MetadataStore::ResolveVersion(const std::string &key, const s3::S3Response &put_response) const {
  if (!put_response.etag.empty()) return MetadataVersion(put_response.etag);
  // Some servers omit the ETag on a conditional PUT reply; fall back to HEAD.
  const s3::S3Response head = client_->Head(ObjectKey(key));
  if (head.status == 200 && !head.etag.empty()) return MetadataVersion(head.etag);
  throw std::runtime_error("S3MetadataStore: could not resolve ETag after writing " + key);
}

ConditionalWriteResult S3MetadataStore::PutIfAbsent(const std::string &key, const Slice &value,
                                                    MetadataVersion *new_version) {
  const s3::S3Response r = client_->Put(ObjectKey(key), value, s3::PutCondition::IfNoneMatchStar);
  if (r.transport_error) return ConditionalWriteResult::RetryableConflict;
  if (r.status == 200) {
    if (new_version != nullptr) *new_version = ResolveVersion(key, r);
    return ConditionalWriteResult::Applied;
  }
  if (r.status == 412) return ConditionalWriteResult::PreconditionFailed;
  if (r.status >= 500) return ConditionalWriteResult::RetryableConflict;
  throw std::runtime_error("S3MetadataStore: PutIfAbsent " + key + " status " + std::to_string(r.status));
}

ConditionalWriteResult S3MetadataStore::CompareExchange(const std::string &key, const MetadataVersion &expected,
                                                        const Slice &value, MetadataVersion *new_version) {
  const s3::S3Response r = client_->Put(ObjectKey(key), value, s3::PutCondition::IfMatch, expected.opaque());
  if (r.transport_error) return ConditionalWriteResult::RetryableConflict;
  if (r.status == 200) {
    if (new_version != nullptr) *new_version = ResolveVersion(key, r);
    return ConditionalWriteResult::Applied;
  }
  // 412: ETag mismatch. 404: the object is gone, so the expected version can
  // no longer match -- both are precondition failures for a CAS.
  if (r.status == 412 || r.status == 404) return ConditionalWriteResult::PreconditionFailed;
  if (r.status >= 500) return ConditionalWriteResult::RetryableConflict;
  throw std::runtime_error("S3MetadataStore: CompareExchange " + key + " status " + std::to_string(r.status));
}

}  // namespace dbplay
