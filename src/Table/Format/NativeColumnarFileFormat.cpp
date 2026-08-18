//
// C3 of the composable storage model (docs/design/StorageAbstraction.md).
// R1 read path (docs/design/ColumnarReadPath.md): positional ReadRange + a
// batched Scan cursor built on it.
//

#include "Table/Format/NativeColumnarFileFormat.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "Storage/Encoding/CodecRegistry.h"

namespace dbplay {

namespace {

constexpr char kMagic[4] = {'D', 'B', 'C', '1'};
constexpr EncodingId kWriteEncoding = EncodingId::Plain;

// Per-column physical record in the footer.
struct ColumnMeta {
  uint8_t encoding;
  uint8_t compression;
  uint64_t offset;       // start of the stored page within the file
  uint64_t stored_size;  // compressed length
  uint64_t raw_size;     // encoded (pre-compression) length
  uint64_t value_count;
};

void PutU8(std::string *b, uint8_t v) { b->push_back(static_cast<char>(v)); }
void PutU64(std::string *b, uint64_t v) { b->append(reinterpret_cast<const char *>(&v), sizeof(v)); }

uint8_t GetU8(const char *&p) { return static_cast<uint8_t>(*p++); }
uint64_t GetU64(const char *&p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  p += sizeof(v);
  return v;
}

// Copy `count` values of `src` starting at `start` onto the end of `dst` (same
// Type). Used to buffer whole chunks (writer) and to slice row ranges (reader).
void AppendRange(Column *dst, const Column &src, size_t start, size_t count) {
  const size_t end = start + count;
  switch (dst->type()) {
    case Type::Int32:  for (size_t i = start; i < end; ++i) dst->Append<int32_t>(src.Get<int32_t>(i)); break;
    case Type::Int64:  for (size_t i = start; i < end; ++i) dst->Append<int64_t>(src.Get<int64_t>(i)); break;
    case Type::Float:  for (size_t i = start; i < end; ++i) dst->Append<float>(src.Get<float>(i)); break;
    case Type::Double: for (size_t i = start; i < end; ++i) dst->Append<double>(src.Get<double>(i)); break;
    case Type::Bool:   for (size_t i = start; i < end; ++i) dst->Append<bool>(src.Get<bool>(i)); break;
    case Type::String:
    case Type::Blob:   for (size_t i = start; i < end; ++i) dst->AppendBytes(src.GetBytes(i)); break;
    default:           throw std::invalid_argument("NativeColumnar: invalid column type");
  }
}

uint64_t ReadU64At(const IInputFile &in, uint64_t offset) {
  std::string buf;
  if (!in.ReadAt(offset, sizeof(uint64_t), &buf)) {
    throw std::runtime_error("NativeColumnar: read past end of file");
  }
  uint64_t v;
  std::memcpy(&v, buf.data(), sizeof(v));
  return v;
}

// ---- writer: buffers full columns, seals the file on Close() ----

class NativeColumnarWriter : public IChunkWriter {
 public:
  NativeColumnarWriter(std::unique_ptr<IOutputStream> out, const Schema &schema, CompressionId compression)
      : out_(std::move(out)), compression_(compression) {
    cols_.reserve(schema.size());
    for (const Field &f : schema) {
      cols_.emplace_back(f.type);
    }
  }
  ~NativeColumnarWriter() override { Close(); }

  void Write(const Chunk &chunk) override {
    if (chunk.columns.size() != cols_.size()) {
      throw std::invalid_argument("NativeColumnar: writer expects full-schema chunks");
    }
    for (size_t k = 0; k < cols_.size(); ++k) {
      AppendRange(&cols_[k], chunk.columns[k], 0, chunk.columns[k].size());
    }
  }

  void Close() override {
    if (closed_) {
      return;
    }
    closed_ = true;

    const CodecRegistry &reg = CodecRegistry::Instance();
    const ICodec &codec = reg.Get(kWriteEncoding);
    const ICompression &comp = reg.Get(compression_);

    std::string body(kMagic, sizeof(kMagic));  // pages accumulate after the magic
    std::vector<ColumnMeta> metas;
    metas.reserve(cols_.size());
    for (const Column &col : cols_) {
      std::string encoded;
      codec.Encode(col, &encoded);
      std::string stored;
      comp.Compress(Slice(encoded), &stored);
      metas.push_back({static_cast<uint8_t>(kWriteEncoding), static_cast<uint8_t>(compression_),
                       static_cast<uint64_t>(body.size()), stored.size(), encoded.size(), col.size()});
      body.append(stored);
    }

    const uint64_t row_count = cols_.empty() ? 0 : cols_[0].size();
    std::string footer;
    PutU64(&footer, row_count);
    PutU64(&footer, cols_.size());
    for (const ColumnMeta &m : metas) {
      PutU8(&footer, m.encoding);
      PutU8(&footer, m.compression);
      PutU64(&footer, m.offset);
      PutU64(&footer, m.stored_size);
      PutU64(&footer, m.raw_size);
      PutU64(&footer, m.value_count);
    }

    body.append(footer);
    PutU64(&body, footer.size());
    body.append(kMagic, sizeof(kMagic));

    out_->Append(Slice(body));
    out_->Close();
  }

 private:
  std::unique_ptr<IOutputStream> out_;
  CompressionId compression_;
  std::vector<Column> cols_;
  bool closed_ = false;
};

// ---- Scan cursor: stream one file at a time in batch_rows-sized windows ----

class NativeColumnarCursor : public IBatchCursor {
 public:
  NativeColumnarCursor(IStorage *store, Schema schema, std::vector<std::string> files, std::vector<int> projection,
                       size_t batch_rows)
      : store_(store),
        schema_(std::move(schema)),
        files_(std::move(files)),
        projection_(std::move(projection)),
        batch_rows_(batch_rows == 0 ? 1 : batch_rows) {}

  bool Next(Chunk *out) override {
    while (true) {
      if (reader_ == nullptr) {
        if (next_file_ >= files_.size()) {
          return false;
        }
        auto in = store_->OpenInput(files_[next_file_++]);
        if (in == nullptr) {
          continue;  // missing file
        }
        reader_ = std::make_unique<NativeColumnarReader>(*in, schema_, projection_);
        pos_ = 0;  // the reader decoded everything up front, so `in` can drop here
      }
      if (pos_ >= reader_->row_count()) {
        reader_.reset();
        continue;  // exhausted this file (also skips empty files)
      }
      const uint64_t n = std::min<uint64_t>(batch_rows_, reader_->row_count() - pos_);
      reader_->ReadRange(pos_, n, out);
      pos_ += n;
      return true;
    }
  }

 private:
  IStorage *store_;
  Schema schema_;
  std::vector<std::string> files_;
  std::vector<int> projection_;
  size_t batch_rows_;
  std::unique_ptr<NativeColumnarReader> reader_;
  uint64_t pos_ = 0;
  size_t next_file_ = 0;
};

}  // namespace

// ---- NativeColumnarReader: footer parse + decode projected columns up front --

NativeColumnarReader::NativeColumnarReader(const IInputFile &in, const Schema &schema, std::vector<int> projection)
    : projection_(std::move(projection)) {
  const uint64_t size = in.Size();
  const uint64_t trailer = sizeof(kMagic) + sizeof(uint64_t);  // magic + footer_size
  if (size < sizeof(kMagic) + trailer) {
    throw std::runtime_error("NativeColumnar: file too small / not a native file");
  }

  std::string tail_magic;
  if (!in.ReadAt(size - sizeof(kMagic), sizeof(kMagic), &tail_magic) ||
      std::memcmp(tail_magic.data(), kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("NativeColumnar: bad trailing magic");
  }

  const uint64_t footer_size = ReadU64At(in, size - trailer);
  const uint64_t footer_off = size - trailer - footer_size;
  std::string footer;
  if (!in.ReadAt(footer_off, footer_size, &footer)) {
    throw std::runtime_error("NativeColumnar: truncated footer");
  }

  const char *p = footer.data();
  row_count_ = GetU64(p);
  const uint64_t col_count = GetU64(p);
  std::vector<ColumnMeta> metas(col_count);
  for (uint64_t i = 0; i < col_count; ++i) {
    metas[i].encoding = GetU8(p);
    metas[i].compression = GetU8(p);
    metas[i].offset = GetU64(p);
    metas[i].stored_size = GetU64(p);
    metas[i].raw_size = GetU64(p);
    metas[i].value_count = GetU64(p);
  }

  const CodecRegistry &reg = CodecRegistry::Instance();
  decoded_.reserve(projection_.size());
  for (int field : projection_) {
    if (field < 0 || static_cast<uint64_t>(field) >= col_count) {
      throw std::out_of_range("NativeColumnar: projection index out of range");
    }
    const ColumnMeta &m = metas[field];
    std::string stored;
    if (!in.ReadAt(m.offset, m.stored_size, &stored)) {
      throw std::runtime_error("NativeColumnar: truncated column page");
    }
    std::string encoded;
    reg.Get(static_cast<CompressionId>(m.compression)).Decompress(Slice(stored), m.raw_size, &encoded);

    Column col(schema[field].type);
    reg.Get(static_cast<EncodingId>(m.encoding)).Decode(Slice(encoded), m.value_count, &col);
    decoded_.push_back(std::move(col));
  }
}

bool NativeColumnarReader::ReadRange(uint64_t first_row, uint64_t num_rows, Chunk *out) const {
  if (first_row >= row_count_) {
    return false;
  }
  const uint64_t n = std::min<uint64_t>(num_rows, row_count_ - first_row);
  if (n == 0) {
    return false;
  }

  Chunk chunk;
  chunk.column_ids = projection_;
  chunk.row_count = n;
  for (const Column &src : decoded_) {
    Column col(src.type());
    AppendRange(&col, src, static_cast<size_t>(first_row), static_cast<size_t>(n));
    chunk.columns.push_back(std::move(col));
  }
  *out = std::move(chunk);
  return true;
}

std::unique_ptr<IBatchCursor> NativeColumnarFileFormat::Scan(IStorage &store, const std::vector<std::string> &files,
                                                             const std::vector<int> &projection) {
  return std::make_unique<NativeColumnarCursor>(&store, schema_, files, projection, batch_rows_);
}

std::unique_ptr<IChunkWriter> NativeColumnarFileFormat::OpenWriter(IStorage &store, const std::string &path) {
  return std::make_unique<NativeColumnarWriter>(store.OpenOutput(path), schema_, write_compression_);
}

}  // namespace dbplay
