#ifndef DBPLAYGROUND_IOBJECTKEYGENERATOR_H
#define DBPLAYGROUND_IOBJECTKEYGENERATOR_H

#include <string>

namespace dbplay {

// Generates unique table-relative immutable keys. Implementations may use
// UUIDs, ULIDs, or another collision-resistant scheme.
class IObjectKeyGenerator {
 public:
  virtual ~IObjectKeyGenerator() = default;
  virtual std::string NewWalKey(const std::string &table_id) = 0;
  virtual std::string NewCommitKey(const std::string &table_id) = 0;
  virtual std::string NewCompactedDataFileKey(const std::string &table_id) = 0;
  virtual std::string NewCompactedDataManifestKey(const std::string &table_id) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IOBJECTKEYGENERATOR_H
