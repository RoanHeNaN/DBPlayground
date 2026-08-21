#ifndef DBPLAYGROUND_IWRITERROUTING_H
#define DBPLAYGROUND_IWRITERROUTING_H

#include <memory>
#include <optional>
#include <string>

#include "Cloud/CloudTypes.h"

namespace dbplay {

class IWriterRouter {
 public:
  virtual ~IWriterRouter() = default;
  virtual std::optional<WriterTarget> Route(const std::string &table_id) const = 0;
};

// RPC/local dispatch seam. The receiving writer node is responsible for
// serializing requests through its per-table coordinator before invoking a
// CloudTableWriter.
class IWriterTransport {
 public:
  virtual ~IWriterTransport() = default;
  virtual CloudImportResult Import(const WriterTarget &target, const std::string &table_id,
                                   const CloudImportBatch &batch) = 0;
};

class ITableWriteCoordinator {
 public:
  virtual ~ITableWriteCoordinator() = default;
  virtual CloudImportResult Import(const CloudImportBatch &batch) = 0;
};

class IWriteCoordinatorProvider {
 public:
  virtual ~IWriteCoordinatorProvider() = default;
  virtual std::shared_ptr<ITableWriteCoordinator> Get(const std::string &table_id) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IWRITERROUTING_H
