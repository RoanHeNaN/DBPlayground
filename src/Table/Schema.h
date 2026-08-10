//
// T1 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// A table's schema: an ordered list of Fields (name + type). This is the
// "column description" (what a column is called and its type), as opposed to
// Column, which holds a column's actual batch of data.
//

#ifndef DBPLAYGROUND_SCHEMA_H
#define DBPLAYGROUND_SCHEMA_H

#include <string>
#include <vector>

#include "Common/Type.h"

namespace dbplay {

struct Field {
  std::string name;
  Type type;
};

using Schema = std::vector<Field>;

}  // namespace dbplay

#endif  // DBPLAYGROUND_SCHEMA_H
