//
// AWS Signature V4 signing for S3-compatible object stores (used against
// MinIO in the cloud path). Kept deliberately small: only the request forms
// the S3 adapters issue (GET/PUT/HEAD/DELETE/ListObjectsV2) are supported.
//
// The signer produces the three headers S3 requires on every request:
// x-amz-date, x-amz-content-sha256, and Authorization. Only host,
// x-amz-content-sha256 and x-amz-date are signed headers; anything else
// (Range, If-Match, If-None-Match, Content-Length) rides unsigned, which S3
// permits. Crypto (SHA256/HMAC) comes from OpenSSL.
//

#ifndef DBPLAYGROUND_S3_SIGV4_H
#define DBPLAYGROUND_S3_SIGV4_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbplay::s3 {

// Lowercase hex SHA-256 of `data`.
std::string Sha256Hex(std::string_view data);

// Raw (binary) HMAC-SHA256 of `data` under `key`.
std::string HmacSha256(std::string_view key, std::string_view data);

// Lowercase hex of arbitrary bytes.
std::string HexEncode(std::string_view bytes);

// RFC 3986 percent-encoding of the AWS "unreserved" set. When `encode_slash`
// is false, '/' is left literal (used for the canonical URI path).
std::string UriEncode(std::string_view value, bool encode_slash);

// Everything the canonical request needs. `canonical_uri` is the raw,
// undecoded object path beginning with '/', e.g. "/bucket/my key" -- the
// signer percent-encodes each segment. `query` holds raw (undecoded)
// key/value pairs; the signer encodes and sorts them.
struct SigningInput {
  std::string method;                                       // "GET", "PUT", ...
  std::string canonical_uri;                                // "/bucket/key"
  std::vector<std::pair<std::string, std::string>> query;   // raw pairs
  std::string host;                                         // "127.0.0.1:19900"
  std::string amz_date;                                     // "20240101T000000Z"
  std::string payload_sha256_hex;                          // hex sha256 of body
  std::string region;
  std::string service;                                     // "s3"
  std::string access_key;
  std::string secret_key;
};

// Returns the value for the Authorization header.
std::string BuildAuthorization(const SigningInput &in);

// "20240101T000000Z" (amz date) and "20240101" (scope date) for a UTC time.
struct AmzTimestamp {
  std::string date_time;  // YYYYMMDDT HHMMSSZ (no space)
  std::string date;       // YYYYMMDD
};
AmzTimestamp FormatAmzTime(std::time_t utc_seconds);

}  // namespace dbplay::s3

#endif  // DBPLAYGROUND_S3_SIGV4_H
