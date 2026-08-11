//
// C3 of the composable storage model (docs/design/StorageAbstraction.md).
//
// TableSource is the composition: an ITableSource built from an IFileFormat (the
// layout) + an IStorage (the medium) + the set of files. It contains no row/col
// logic itself -- it just delegates Scan to the format, handing it the store.
// Swap the medium -> swap the IStorage; swap row<->columnar -> swap the format;
// the query layer, which sees only ITableSource, is untouched either way.
//

#ifndef DBPLAYGROUND_TABLESOURCE_H
#define DBPLAYGROUND_TABLESOURCE_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"
#include "Table/ITableSource.h"

namespace dbplay {

class TableSource : public ITableSource {
 public:
  TableSource(std::shared_ptr<IFileFormat> format, std::shared_ptr<IStorage> store, std::vector<std::string> files)
      : format_(std::move(format)), store_(std::move(store)), files_(std::move(files)) {}

  const Schema &schema() const override { return format_->schema(); }

  std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &projection) override {
    return format_->Scan(*store_, files_, projection);
  }

 private:
  std::shared_ptr<IFileFormat> format_;
  std::shared_ptr<IStorage> store_;
  std::vector<std::string> files_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_TABLESOURCE_H
