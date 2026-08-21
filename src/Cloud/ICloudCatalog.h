#ifndef DBPLAYGROUND_ICLOUDCATALOG_H
#define DBPLAYGROUND_ICLOUDCATALOG_H

#include <optional>

#include "Cloud/CloudTypes.h"

namespace dbplay {

class ICloudCatalog {
 public:
  virtual ~ICloudCatalog() = default;
  virtual std::optional<TableDescriptor> Resolve(const TableName &name) const = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_ICLOUDCATALOG_H
