#ifndef DBPLAYGROUND_CLOUDVALIDATION_H
#define DBPLAYGROUND_CLOUDVALIDATION_H

#include <cstddef>

#include "Table/Chunk.h"
#include "Table/Schema.h"

namespace dbplay {

inline bool SchemasEqual(const Schema &left, const Schema &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    if (left[i].name != right[i].name || left[i].type != right[i].type) {
      return false;
    }
  }
  return true;
}

inline bool IsFullSchemaChunk(const Chunk &chunk, const Schema &schema) {
  if (chunk.row_count == 0 || chunk.columns.size() != schema.size() || chunk.column_ids.size() != schema.size()) {
    return false;
  }
  for (size_t i = 0; i < schema.size(); ++i) {
    if (chunk.column_ids[i] != static_cast<int>(i) || chunk.columns[i].type() != schema[i].type ||
        chunk.columns[i].size() != chunk.row_count) {
      return false;
    }
  }
  return true;
}

}  // namespace dbplay

#endif  // DBPLAYGROUND_CLOUDVALIDATION_H
