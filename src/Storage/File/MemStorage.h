//
// C1 of the composable storage model (docs/design/StorageAbstraction.md).
//
// MemStorage keeps every file in memory (path -> bytes). It is the trivial
// IStorage: the swappable medium for tests and small tables. Reads copy out the
// bytes (an input file owns its snapshot), so they stay valid even if the file
// is later rewritten or deleted. A MemStorage must outlive the streams it hands
// out (the borrow convention used across the storage layer).
//

#ifndef DBPLAYGROUND_MEMSTORAGE_H
#define DBPLAYGROUND_MEMSTORAGE_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Storage/File/IStorage.h"

namespace dbplay {

class MemStorage : public IStorage {
 public:
  std::unique_ptr<IInputFile> OpenInput(const std::string &path) override;
  std::unique_ptr<IOutputStream> OpenOutput(const std::string &path) override;
  bool Exists(const std::string &path) const override;
  std::vector<std::string> List(const std::string &prefix) const override;
  void Delete(const std::string &path) override;

  // Called by this store's IOutputStream on Close() to publish written bytes.
  // Not part of IStorage; an implementation detail of MemStorage's streams.
  void Put(const std::string &path, std::string bytes);

 private:
  std::unordered_map<std::string, std::string> files_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_MEMSTORAGE_H
