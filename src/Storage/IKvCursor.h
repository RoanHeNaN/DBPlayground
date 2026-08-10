//
// T0 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// IKvCursor is an ordered (by encoded key) forward scan over a storage engine's
// whole key space. It is the row/KV engine's scan primitive, consumed by the
// higher, schema-aware RowTableSource.
//
// Key()/Value() are valid only while Valid() is true; the returned Slices point
// at buffers owned by the cursor and remain valid until the next Next() call.
//

#ifndef DBPLAYGROUND_IKVCURSOR_H
#define DBPLAYGROUND_IKVCURSOR_H

#include "Common/Slice.h"

namespace dbplay {

class IKvCursor {
 public:
  virtual ~IKvCursor() = default;

  virtual bool Valid() const = 0;
  virtual void Next() = 0;
  virtual Slice Key() const = 0;    // encoded key bytes
  virtual Slice Value() const = 0;  // value bytes
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IKVCURSOR_H
