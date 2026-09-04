#include "Cloud/S3/S3Client.h"

#include <curl/curl.h>

#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>

#include "Cloud/S3/Sigv4.h"

namespace dbplay::s3 {
namespace {

// libcurl requires a one-time global init before any easy handle is used.
void EnsureCurlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("S3Client: curl_global_init failed");
    }
  });
}

std::string HostFromEndpoint(const std::string &endpoint) {
  std::string s = endpoint;
  const auto scheme = s.find("://");
  if (scheme != std::string::npos) s = s.substr(scheme + 3);
  const auto slash = s.find('/');
  if (slash != std::string::npos) s = s.substr(0, slash);
  return s;
}

size_t WriteToString(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string TrimHeaderValue(const char *p, size_t n) {
  std::string value(p, n);
  const size_t begin = value.find_first_not_of(" \t");
  const size_t end = value.find_last_not_of(" \t\r\n");
  if (begin == std::string::npos || end == std::string::npos) return {};
  return value.substr(begin, end - begin + 1);
}

// Header callback: capture ETag (quotes stripped) and Content-Length.
size_t CaptureHeaders(char *buffer, size_t size, size_t nitems, void *userdata) {
  const size_t len = size * nitems;
  auto *resp = static_cast<S3Response *>(userdata);
  if (len >= 5 && strncasecmp(buffer, "etag:", 5) == 0) {
    std::string value = TrimHeaderValue(buffer + 5, len - 5);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    resp->etag = value;
  } else if (len >= 15 && strncasecmp(buffer, "content-length:", 15) == 0) {
    const std::string value = TrimHeaderValue(buffer + 15, len - 15);
    if (!value.empty()) resp->content_length = std::strtoll(value.c_str(), nullptr, 10);
  }
  return len;
}

}  // namespace

S3Client::S3Client(S3Config config) : config_(std::move(config)), host_(HostFromEndpoint(config_.endpoint)) {
  EnsureCurlGlobalInit();
}

S3Response S3Client::Do(const std::string &method, const std::string &key,
                        const std::vector<std::pair<std::string, std::string>> &query, const Slice *body,
                        const std::vector<std::string> &extra_headers, bool head_only) {
  CURL *curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("S3Client: curl_easy_init failed");

  const std::string canonical_uri = "/" + key;
  const std::string payload = body != nullptr ? std::string(body->data(), body->size()) : std::string();
  const std::string payload_hash = Sha256Hex(payload);
  const auto ts = FormatAmzTime(std::time(nullptr));

  SigningInput sign;
  sign.method = method;
  sign.canonical_uri = canonical_uri;
  sign.query = query;
  sign.host = host_;
  sign.amz_date = ts.date_time;
  sign.payload_sha256_hex = payload_hash;
  sign.region = config_.region;
  sign.service = "s3";
  sign.access_key = config_.access_key;
  sign.secret_key = config_.secret_key;
  const std::string authorization = BuildAuthorization(sign);

  // Build the URL from the same encoding the signature used.
  std::string url = config_.endpoint + UriEncode(canonical_uri, /*encode_slash=*/false);
  if (!query.empty()) {
    std::string qs;
    for (size_t i = 0; i < query.size(); ++i) {
      if (i != 0) qs.push_back('&');
      qs += UriEncode(query[i].first, true) + "=" + UriEncode(query[i].second, true);
    }
    url += "?" + qs;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, ("x-amz-date: " + ts.date_time).c_str());
  headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
  headers = curl_slist_append(headers, ("Authorization: " + authorization).c_str());
  // Suppress the default "Expect: 100-continue" and "Content-Type" curl adds.
  headers = curl_slist_append(headers, "Expect:");
  for (const auto &h : extra_headers) headers = curl_slist_append(headers, h.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  if (method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
  } else if (head_only) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  S3Response resp;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CaptureHeaders);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    resp.transport_error = true;
    resp.body = curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return resp;
}

S3Response S3Client::Get(const std::string &key) { return Do("GET", key, {}, nullptr, {}); }

S3Response S3Client::GetRange(const std::string &key, uint64_t offset, uint64_t length) {
  if (length == 0) return Do("GET", key, {}, nullptr, {});
  const std::string range = "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
  return Do("GET", key, {}, nullptr, {range});
}

S3Response S3Client::Head(const std::string &key) { return Do("HEAD", key, {}, nullptr, {}, /*head_only=*/true); }

S3Response S3Client::Put(const std::string &key, const Slice &body, PutCondition cond,
                         const std::string &if_match_etag) {
  std::vector<std::string> headers;
  if (cond == PutCondition::IfNoneMatchStar) {
    headers.push_back("If-None-Match: *");
  } else if (cond == PutCondition::IfMatch) {
    headers.push_back("If-Match: \"" + if_match_etag + "\"");
  }
  return Do("PUT", key, {}, &body, headers);
}

S3Response S3Client::Delete(const std::string &key) { return Do("DELETE", key, {}, nullptr, {}); }

S3Response S3Client::ListV2(const std::string &bucket, const std::string &prefix,
                            const std::string &continuation_token) {
  std::vector<std::pair<std::string, std::string>> query;
  query.emplace_back("list-type", "2");
  query.emplace_back("prefix", prefix);
  if (!continuation_token.empty()) query.emplace_back("continuation-token", continuation_token);
  return Do("GET", bucket, query, nullptr, {});
}

}  // namespace dbplay::s3
