#include "Cloud/S3/Sigv4.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <ctime>

namespace dbplay::s3 {
namespace {

const char kHex[] = "0123456789abcdef";
const char kHexUpper[] = "0123456789ABCDEF";

bool IsUnreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~';
}

}  // namespace

std::string HexEncode(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

std::string Sha256Hex(std::string_view data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest);
  return HexEncode(std::string_view(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH));
}

std::string HmacSha256(std::string_view key, std::string_view data) {
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char *>(data.data()), data.size(), out, &out_len);
  return std::string(reinterpret_cast<const char *>(out), out_len);
}

std::string UriEncode(std::string_view value, bool encode_slash) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (IsUnreserved(c) || (c == '/' && !encode_slash)) {
      out.push_back(static_cast<char>(c));
    } else {
      // AWS requires uppercase hex in percent-encoding.
      out.push_back('%');
      out.push_back(kHexUpper[c >> 4]);
      out.push_back(kHexUpper[c & 0x0f]);
    }
  }
  return out;
}

AmzTimestamp FormatAmzTime(std::time_t utc_seconds) {
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &utc_seconds);
#else
  gmtime_r(&utc_seconds, &tm_utc);
#endif
  char dt[32];
  std::strftime(dt, sizeof(dt), "%Y%m%dT%H%M%SZ", &tm_utc);
  char d[16];
  std::strftime(d, sizeof(d), "%Y%m%d", &tm_utc);
  return AmzTimestamp{std::string(dt), std::string(d)};
}

std::string BuildAuthorization(const SigningInput &in) {
  // Scope date is the leading YYYYMMDD of the amz timestamp.
  const std::string scope_date = in.amz_date.substr(0, 8);

  // --- Canonical URI: percent-encode each path segment, keep '/'. ---
  const std::string canonical_uri = UriEncode(in.canonical_uri, /*encode_slash=*/false);

  // --- Canonical query string: encode, then sort by encoded key. ---
  std::vector<std::pair<std::string, std::string>> encoded;
  encoded.reserve(in.query.size());
  for (const auto &kv : in.query) {
    encoded.emplace_back(UriEncode(kv.first, true), UriEncode(kv.second, true));
  }
  std::sort(encoded.begin(), encoded.end());
  std::string canonical_query;
  for (size_t i = 0; i < encoded.size(); ++i) {
    if (i != 0) canonical_query.push_back('&');
    canonical_query += encoded[i].first;
    canonical_query.push_back('=');
    canonical_query += encoded[i].second;
  }

  // --- Signed headers: host, x-amz-content-sha256, x-amz-date (sorted). ---
  const std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  std::string canonical_headers;
  canonical_headers += "host:" + in.host + "\n";
  canonical_headers += "x-amz-content-sha256:" + in.payload_sha256_hex + "\n";
  canonical_headers += "x-amz-date:" + in.amz_date + "\n";

  std::string canonical_request;
  canonical_request += in.method + "\n";
  canonical_request += canonical_uri + "\n";
  canonical_request += canonical_query + "\n";
  canonical_request += canonical_headers + "\n";
  canonical_request += signed_headers + "\n";
  canonical_request += in.payload_sha256_hex;

  // --- String to sign. ---
  const std::string scope = scope_date + "/" + in.region + "/" + in.service + "/aws4_request";
  std::string string_to_sign;
  string_to_sign += "AWS4-HMAC-SHA256\n";
  string_to_sign += in.amz_date + "\n";
  string_to_sign += scope + "\n";
  string_to_sign += Sha256Hex(canonical_request);

  // --- Derive the signing key and sign. ---
  const std::string k_date = HmacSha256("AWS4" + in.secret_key, scope_date);
  const std::string k_region = HmacSha256(k_date, in.region);
  const std::string k_service = HmacSha256(k_region, in.service);
  const std::string k_signing = HmacSha256(k_service, "aws4_request");
  const std::string signature = HexEncode(HmacSha256(k_signing, string_to_sign));

  return "AWS4-HMAC-SHA256 Credential=" + in.access_key + "/" + scope + ", SignedHeaders=" + signed_headers +
         ", Signature=" + signature;
}

}  // namespace dbplay::s3
