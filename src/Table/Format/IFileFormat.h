//
// C3 of the composable storage model (docs/design/StorageAbstraction.md).
//
// IFileFormat is the file-layout seam: it carries the row-vs-columnar difference
// and owns decode-into-Chunks / encode-from-Chunks. It reads/writes bytes only
// through an IStorage (so the medium stays swappable) and, for columnar formats,
// resolves per-column codecs by id. The composed TableSource(IFileFormat,
// IStorage) exposes it to the query layer as a plain ITableSource.
//

#ifndef DBPLAYGROUND_IFILEFORMAT_H
#define DBPLAYGROUND_IFILEFORMAT_H

#include <memory>
#include <string>
#include <vector>

#include "Storage/File/IStorage.h"
#include "Table/ITableSource.h"

namespace dbplay {

// Writes batches (full-schema Chunks) into one file, sealing it on Close().
class IChunkWriter {
 public:
  virtual ~IChunkWriter() = default;

  // Append a batch. `chunk.columns` must cover the whole schema in schema order
  // (column_ids 0..n-1); it is one row group's worth of rows.
  virtual void Write(const Chunk &chunk) = 0;

  // Flush the footer and close the file. Idempotent.
  virtual void Close() = 0;
};

class IFileFormat {
 public:
  virtual ~IFileFormat() = default;

  virtual const Schema &schema() const = 0;

  // Scan `files` (each an independent unit -- a row group) reading only the
  // projected columns' bytes from `store`. `store` must outlive the cursor.
  virtual std::unique_ptr<IBatchCursor> Scan(IStorage &store, const std::vector<std::string> &files,
                                             const std::vector<int> &projection) = 0;

  // Open `path` in `store` for writing one file of this format.
  virtual std::unique_ptr<IChunkWriter> OpenWriter(IStorage &store, const std::string &path) = 0;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_IFILEFORMAT_H
