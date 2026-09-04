//
// A minimal S3 HTTP client over libcurl for the cloud path. Path-style
// addressing only (endpoint/bucket/key), which is what MinIO uses locally.
// Requests are signed with AWS Signature V4 (see Sigv4.h). The client is
// low-level: it returns HTTP status + body + ETag and leaves S3-semantic
// interpretation (not-found, precondition-failed, XML parsing) to the
// IStorage / IMetadataStore adapters built on top.
//

#ifndef DBPLAYGROUND_S3_S3CLIENT_H
#define DBPLAYGROUND_S3_S3CLIENT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Common/Slice.h"

namespace dbplay::s3 {

struct S3Config {
  std::string endpoint;    // "http://127.0.0.1:19900" (no trailing slash)
  std::string region;      // "us-east-1"
  std::string access_key;
  std::string secret_key;
};

// Precondition applied to a PUT.
enum class PutCondition {
  None,             // unconditional overwrite
  IfNoneMatchStar,  // create-if-absent (maps to PutIfAbsent)
  IfMatch,          // overwrite only if current ETag matches (maps to CAS)
};

struct S3Response {
  long status = 0;                // HTTP status code; 0 on transport error
  bool transport_error = false;   // libcurl failed before an HTTP reply
  std::string body;               // response payload (GET / List / error XML)
  std::string etag;               // response ETag with surrounding quotes stripped
  int64_t content_length = -1;    // Content-Length header, or -1 if absent
};

class S3Client {
 public:
  explicit S3Client(S3Config config);

  // `key` is the full object path WITHOUT a leading slash, e.g.
  // "bucket/prefix/object". The bucket is part of the key (path-style).
  S3Response Get(const std::string &key);
  S3Response GetRange(const std::string &key, uint64_t offset, uint64_t length);
  S3Response Head(const std::string &key);
  S3Response Put(const std::string &key, const Slice &body, PutCondition cond = PutCondition::None,
                 const std::string &if_match_etag = "");
  S3Response Delete(const std::string &key);
  // Raw ListObjectsV2 XML is returned in `body`; the caller parses it.
  S3Response ListV2(const std::string &bucket, const std::string &prefix, const std::string &continuation_token = "");

 private:
  S3Response Do(const std::string &method, const std::string &key,
                const std::vector<std::pair<std::string, std::string>> &query, const Slice *body,
                const std::vector<std::string> &extra_headers, bool head_only = false);

  S3Config config_;
  std::string host_;  // "127.0.0.1:19900", derived from endpoint
};

}  // namespace dbplay::s3

#endif  // DBPLAYGROUND_S3_S3CLIENT_H
