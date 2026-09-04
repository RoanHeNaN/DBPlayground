//
// IStorage backed by an S3-compatible object store (MinIO locally). A `path`
// is an object key within `bucket`; the byte-range read/write contract of
// IStorage maps to GET Range / PUT / HEAD / ListObjectsV2 / DELETE via
// S3Client. See docs/design/StorageAbstraction.md (C1) and CloudTableLayering.
//

#ifndef DBPLAYGROUND_S3_S3FILESTORAGE_H
#define DBPLAYGROUND_S3_S3FILESTORAGE_H

#include <memory>
#include <string>
#include <vector>

#include "Cloud/S3/S3Client.h"
#include "Storage/File/IStorage.h"

namespace dbplay {

class S3FileStorage : public IStorage {
 public:
  S3FileStorage(std::shared_ptr<s3::S3Client> client, std::string bucket);

  std::unique_ptr<IInputFile> OpenInput(const std::string &path) override;
  std::unique_ptr<IOutputStream> OpenOutput(const std::string &path) override;
  bool Exists(const std::string &path) const override;
  std::vector<std::string> List(const std::string &prefix) const override;
  void Delete(const std::string &path) override;

 private:
  std::string ObjectKey(const std::string &path) const { return bucket_ + "/" + path; }

  std::shared_ptr<s3::S3Client> client_;
  std::string bucket_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_S3_S3FILESTORAGE_H
