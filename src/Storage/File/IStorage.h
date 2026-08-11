//
// C1 of the composable storage model (docs/design/StorageAbstraction.md).
//
// IStorage is the byte-range Object Store seam: the medium a file-based table
// format reads/writes bytes through (memory / disk / S3). It is deliberately
// the ONLY addressing model here -- offset-addressed, not key-addressed -- so
// any IFileFormat composes with any IStorage (real orthogonality). A B+Tree is
// an access method, not an IStorage (see the design doc).
//
// Error convention (matches the repo): bool + out-pointer for expected sad
// paths, a null unique_ptr for "not found", throw std::runtime_error for hard
// IO errors. Owning output goes through std::string* since Slice is a view.
//

#ifndef DBPLAYGROUND_ISTORAGE_H
#define DBPLAYGROUND_ISTORAGE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Common/Slice.h"

namespace dbplay {

// A single file opened for random-access reads.
class IInputFile {
 public:
  virtual ~IInputFile() = default;

  virtual uint64_t Size() const = 0;

  // Read [offset, offset+len) into *out (out owns the bytes). Returns false if
  // the range lies outside the file; throws std::runtime_error on an IO error.
  virtual bool ReadAt(uint64_t offset, size_t len, std::string *out) const = 0;
};

// A single file opened for sequential writing (truncating what was there).
class IOutputStream {
 public:
  virtual ~IOutputStream() = default;

  // Append `data` to the file. Throws std::runtime_error on an IO error.
  virtual void Append(const Slice &data) = 0;

  // Commit the written bytes so a subsequent OpenInput sees them. Idempotent.
  virtual void Close() = 0;
};

// The medium. `path` is an opaque name within this store's namespace.
class IStorage {
 public:
  virtual ~IStorage() = default;

  // Open `path` for random reads; nullptr if it does not exist.
  virtual std::unique_ptr<IInputFile> OpenInput(const std::string &path) = 0;

  // Open `path` for sequential writing, truncating any existing content.
  virtual std::unique_ptr<IOutputStream> OpenOutput(const std::string &path) = 0;

  virtual bool Exists(const std::string &path) const = 0;

  // Every stored path that begins with `prefix` (unordered).
  virtual std::vector<std::string> List(const std::string &prefix) const = 0;

  // Remove `path` if present (no error if absent).
  virtual void Delete(const std::string &path) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ISTORAGE_H
