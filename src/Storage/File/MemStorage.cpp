//
// C1 of the composable storage model (docs/design/StorageAbstraction.md).
//

#include "Storage/File/MemStorage.h"

#include <cstring>
#include <utility>

namespace dbplay {

namespace {

// Owns a private snapshot of the file's bytes, taken at OpenInput time.
class MemInputFile : public IInputFile {
 public:
  explicit MemInputFile(std::string bytes) : bytes_(std::move(bytes)) {}

  uint64_t Size() const override { return bytes_.size(); }

  bool ReadAt(uint64_t offset, size_t len, std::string *out) const override {
    if (offset > bytes_.size() || len > bytes_.size() - offset) {
      return false;  // range outside the file
    }
    out->assign(bytes_.data() + offset, len);
    return true;
  }

 private:
  std::string bytes_;
};

// Accumulates appended bytes; publishes them into the store on Close().
class MemOutputStream : public IOutputStream {
 public:
  MemOutputStream(MemStorage *store, std::string path) : store_(store), path_(std::move(path)) {}
  ~MemOutputStream() override { Close(); }

  void Append(const Slice &data) override { buf_.append(data.data(), data.size()); }

  void Close() override {
    if (closed_) {
      return;
    }
    closed_ = true;
    store_->Put(path_, std::move(buf_));
  }

 private:
  MemStorage *store_;
  std::string path_;
  std::string buf_;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<IInputFile> MemStorage::OpenInput(const std::string &path) {
  auto it = files_.find(path);
  if (it == files_.end()) {
    return nullptr;
  }
  return std::make_unique<MemInputFile>(it->second);
}

std::unique_ptr<IOutputStream> MemStorage::OpenOutput(const std::string &path) {
  return std::make_unique<MemOutputStream>(this, path);
}

bool MemStorage::Exists(const std::string &path) const { return files_.count(path) != 0; }

std::vector<std::string> MemStorage::List(const std::string &prefix) const {
  std::vector<std::string> out;
  for (const auto &kv : files_) {
    if (kv.first.compare(0, prefix.size(), prefix) == 0) {
      out.push_back(kv.first);
    }
  }
  return out;
}

void MemStorage::Delete(const std::string &path) { files_.erase(path); }

void MemStorage::Put(const std::string &path, std::string bytes) { files_[path] = std::move(bytes); }

}  // namespace dbplay
