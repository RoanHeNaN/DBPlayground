#include "Cloud/S3/S3FileStorage.h"

#include <stdexcept>
#include <utility>

namespace dbplay {
namespace {

// A single S3 object opened for random reads. Its size is captured at open
// time (via HEAD) so ReadAt can reject out-of-range requests without a round
// trip, matching the IInputFile contract.
class S3InputFile : public IInputFile {
 public:
  S3InputFile(std::shared_ptr<s3::S3Client> client, std::string key, uint64_t size)
      : client_(std::move(client)), key_(std::move(key)), size_(size) {}

  uint64_t Size() const override { return size_; }

  bool ReadAt(uint64_t offset, size_t len, std::string *out) const override {
    if (offset > size_ || offset + len > size_) return false;
    if (len == 0) {
      out->clear();
      return true;
    }
    const s3::S3Response r = client_->GetRange(key_, offset, len);
    if (r.transport_error) throw std::runtime_error("S3InputFile: transport error reading " + key_);
    if (r.status == 206 || r.status == 200) {
      *out = r.body;
      return true;
    }
    if (r.status == 416 || r.status == 404) return false;
    throw std::runtime_error("S3InputFile: unexpected status " + std::to_string(r.status) + " reading " + key_);
  }

 private:
  std::shared_ptr<s3::S3Client> client_;
  std::string key_;
  uint64_t size_;
};

// Buffers all appended bytes and PUTs them as one object on Close (truncating
// any prior content), matching the OpenOutput contract. Fine for
// WAL/compacted-data files written whole; multipart upload is a future optimization for large
// objects.
class S3OutputStream : public IOutputStream {
 public:
  S3OutputStream(std::shared_ptr<s3::S3Client> client, std::string key)
      : client_(std::move(client)), key_(std::move(key)) {}

  void Append(const Slice &data) override {
    if (closed_) throw std::runtime_error("S3OutputStream: append after close on " + key_);
    buffer_.append(data.data(), data.size());
  }

  void Close() override {
    if (closed_) return;
    const s3::S3Response r = client_->Put(key_, Slice(buffer_));
    if (r.transport_error) throw std::runtime_error("S3OutputStream: transport error writing " + key_);
    if (r.status != 200) {
      throw std::runtime_error("S3OutputStream: PUT " + key_ + " failed with status " + std::to_string(r.status));
    }
    closed_ = true;
  }

 private:
  std::shared_ptr<s3::S3Client> client_;
  std::string key_;
  std::string buffer_;
  bool closed_ = false;
};

// Pull the text between the first <tag> and </tag> after `from`; returns
// npos-terminated {value, next_search_pos}. Used for the flat XML MinIO/S3
// return from ListObjectsV2 (keys are safe, no entity-escaping to undo here).
bool ExtractTag(const std::string &xml, const std::string &tag, size_t *pos, std::string *value) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const size_t a = xml.find(open, *pos);
  if (a == std::string::npos) return false;
  const size_t b = xml.find(close, a + open.size());
  if (b == std::string::npos) return false;
  *value = xml.substr(a + open.size(), b - (a + open.size()));
  *pos = b + close.size();
  return true;
}

}  // namespace

S3FileStorage::S3FileStorage(std::shared_ptr<s3::S3Client> client, std::string bucket)
    : client_(std::move(client)), bucket_(std::move(bucket)) {
  if (client_ == nullptr || bucket_.empty()) {
    throw std::invalid_argument("S3FileStorage: null client or empty bucket");
  }
}

std::unique_ptr<IInputFile> S3FileStorage::OpenInput(const std::string &path) {
  const s3::S3Response r = client_->Head(ObjectKey(path));
  if (r.transport_error) throw std::runtime_error("S3FileStorage: transport error on HEAD " + path);
  if (r.status == 404) return nullptr;
  if (r.status != 200) {
    throw std::runtime_error("S3FileStorage: HEAD " + path + " status " + std::to_string(r.status));
  }
  const uint64_t size = r.content_length >= 0 ? static_cast<uint64_t>(r.content_length) : 0;
  return std::make_unique<S3InputFile>(client_, ObjectKey(path), size);
}

std::unique_ptr<IOutputStream> S3FileStorage::OpenOutput(const std::string &path) {
  return std::make_unique<S3OutputStream>(client_, ObjectKey(path));
}

bool S3FileStorage::Exists(const std::string &path) const {
  const s3::S3Response r = client_->Head(ObjectKey(path));
  if (r.transport_error) throw std::runtime_error("S3FileStorage: transport error on HEAD " + path);
  return r.status == 200;
}

std::vector<std::string> S3FileStorage::List(const std::string &prefix) const {
  std::vector<std::string> keys;
  std::string token;
  do {
    const s3::S3Response r = client_->ListV2(bucket_, prefix, token);
    if (r.transport_error) throw std::runtime_error("S3FileStorage: transport error on LIST " + prefix);
    if (r.status != 200) {
      throw std::runtime_error("S3FileStorage: LIST " + prefix + " status " + std::to_string(r.status));
    }
    size_t pos = 0;
    std::string key;
    while (ExtractTag(r.body, "Key", &pos, &key)) keys.push_back(key);

    token.clear();
    size_t tpos = 0;
    std::string truncated;
    if (ExtractTag(r.body, "IsTruncated", &tpos, &truncated) && truncated == "true") {
      size_t cpos = 0;
      ExtractTag(r.body, "NextContinuationToken", &cpos, &token);
    }
  } while (!token.empty());
  return keys;
}

void S3FileStorage::Delete(const std::string &path) {
  const s3::S3Response r = client_->Delete(ObjectKey(path));
  if (r.transport_error) throw std::runtime_error("S3FileStorage: transport error on DELETE " + path);
  if (r.status != 204 && r.status != 200 && r.status != 404) {
    throw std::runtime_error("S3FileStorage: DELETE " + path + " status " + std::to_string(r.status));
  }
}

}  // namespace dbplay
