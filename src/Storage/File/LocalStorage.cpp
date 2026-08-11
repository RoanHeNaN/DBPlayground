//
// C1 of the composable storage model (docs/design/StorageAbstraction.md).
//

#include "Storage/File/LocalStorage.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace dbplay {

namespace {

// Random-access reader backed by an open ifstream (seek + read per ReadAt).
class LocalInputFile : public IInputFile {
 public:
  LocalInputFile(std::ifstream in, uint64_t size) : in_(std::move(in)), size_(size) {}

  uint64_t Size() const override { return size_; }

  bool ReadAt(uint64_t offset, size_t len, std::string *out) const override {
    if (offset > size_ || len > size_ - offset) {
      return false;  // range outside the file
    }
    out->resize(len);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(out->data(), static_cast<std::streamsize>(len));
    if (in_.gcount() != static_cast<std::streamsize>(len)) {
      throw std::runtime_error("LocalStorage: short read");
    }
    return true;
  }

 private:
  mutable std::ifstream in_;  // ReadAt is logically const but moves the get pointer
  uint64_t size_;
};

class LocalOutputStream : public IOutputStream {
 public:
  explicit LocalOutputStream(std::ofstream out) : out_(std::move(out)) {}
  ~LocalOutputStream() override { Close(); }

  void Append(const Slice &data) override {
    out_.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out_) {
      throw std::runtime_error("LocalStorage: write failed");
    }
  }

  void Close() override {
    if (out_.is_open()) {
      out_.close();
    }
  }

 private:
  std::ofstream out_;
};

}  // namespace

LocalStorage::LocalStorage(std::string root) : root_(std::move(root)) { fs::create_directories(root_); }

std::string LocalStorage::FullPath(const std::string &path) const { return (fs::path(root_) / path).string(); }

std::unique_ptr<IInputFile> LocalStorage::OpenInput(const std::string &path) {
  const std::string full = FullPath(path);
  if (!fs::is_regular_file(full)) {
    return nullptr;
  }
  std::ifstream in(full, std::ios::binary);
  if (!in) {
    throw std::runtime_error("LocalStorage: cannot open " + full);
  }
  return std::make_unique<LocalInputFile>(std::move(in), static_cast<uint64_t>(fs::file_size(full)));
}

std::unique_ptr<IOutputStream> LocalStorage::OpenOutput(const std::string &path) {
  const std::string full = FullPath(path);
  fs::create_directories(fs::path(full).parent_path());
  std::ofstream out(full, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("LocalStorage: cannot create " + full);
  }
  return std::make_unique<LocalOutputStream>(std::move(out));
}

bool LocalStorage::Exists(const std::string &path) const { return fs::is_regular_file(FullPath(path)); }

std::vector<std::string> LocalStorage::List(const std::string &prefix) const {
  std::vector<std::string> out;
  if (!fs::is_directory(root_)) {
    return out;
  }
  const fs::path root(root_);
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string rel = fs::relative(entry.path(), root).generic_string();
    if (rel.compare(0, prefix.size(), prefix) == 0) {
      out.push_back(rel);
    }
  }
  return out;
}

void LocalStorage::Delete(const std::string &path) { fs::remove(FullPath(path)); }

}  // namespace dbplay
