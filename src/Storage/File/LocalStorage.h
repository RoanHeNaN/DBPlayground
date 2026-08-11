//
// C1 of the composable storage model (docs/design/StorageAbstraction.md).
//
// LocalStorage is the on-disk IStorage: each `path` is a file under a root
// directory, read by byte range (seek + read) and written sequentially. Swap
// MemStorage -> LocalStorage and a file format is now persistent, with no change
// to the format or the query layer.
//

#ifndef DBPLAYGROUND_LOCALSTORAGE_H
#define DBPLAYGROUND_LOCALSTORAGE_H

#include <memory>
#include <string>
#include <vector>

#include "Storage/File/IStorage.h"

namespace dbplay {

class LocalStorage : public IStorage {
 public:
  // Files live under `root` (created if absent). `path`s are relative to it.
  explicit LocalStorage(std::string root);

  std::unique_ptr<IInputFile> OpenInput(const std::string &path) override;
  std::unique_ptr<IOutputStream> OpenOutput(const std::string &path) override;
  bool Exists(const std::string &path) const override;
  std::vector<std::string> List(const std::string &prefix) const override;
  void Delete(const std::string &path) override;

 private:
  std::string FullPath(const std::string &path) const;

  std::string root_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_LOCALSTORAGE_H
