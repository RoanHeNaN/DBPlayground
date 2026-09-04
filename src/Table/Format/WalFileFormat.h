//
// Cloud WAL file format: a row-oriented append log, distinct from DBC1.
// Compaction later rewrites these files into NativeColumnarFileFormat.
//
// File layout:
//   [magic "DBW1"]
//   [codec_version: u32]
//   [column_count: u32][type: u8] * column_count
//   [entry]*
//     [row_count: u32]
//     [payload_size: u32]
//     [payload: (u32 row_len + RowCodec bytes)*]
//     [crc32: u32 of row_count||payload_size||payload]
//   [footer: total_row_count u64][entry_count u64]
//   [footer_size: u64]
//   [magic "DBW1"]
//

#ifndef DBPLAYGROUND_WALFILEFORMAT_H
#define DBPLAYGROUND_WALFILEFORMAT_H

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Storage/File/IStorage.h"
#include "Table/Format/IFileFormat.h"
#include "Table/Schema.h"

namespace dbplay {

class WalFileFormat : public IFileFormat {
 public:
  explicit WalFileFormat(Schema schema, size_t batch_rows = 1024)
      : schema_(std::move(schema)), batch_rows_(batch_rows) {}

  const Schema &schema() const override { return schema_; }

  std::unique_ptr<IFileReader> OpenReader(IStorage &store, const std::string &file,
                                          const std::vector<int> &projection) override;

  std::unique_ptr<IBatchCursor> Scan(IStorage &store, const std::vector<std::string> &files,
                                     const std::vector<int> &projection) override;

  std::unique_ptr<IChunkWriter> OpenWriter(IStorage &store, const std::string &path) override;

 private:
  Schema schema_;
  size_t batch_rows_;
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_WALFILEFORMAT_H
