//
// C3 of the composable storage model (docs/design/StorageAbstraction.md).
// R1/R2 read path (docs/design/ColumnarReadPath.md): metadata (footer) is read
// in OpenReader (driven by Scan, per file); the reader's ctor does no page IO.
// The one encoding-dependent step -- "read rows [a,b) of a page" -- is a
// PageAccessor: a universal whole-page-decode default, plus a direct-offset fast
// path for fixed-width + uncompressed columns. Everything else is shared.
//

#include "Table/Format/NativeColumnarFileFormat.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "Storage/Encoding/CodecRegistry.h"
#include "Table/Chunk.h"
#include "Table/Column.h"

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

// Copy `count` values of `src` starting at `start` onto the end of `dst`.
void AppendRange(Column *dst, const Column &src, size_t start, size_t count) {
  const size_t end = start + count;
  switch (dst->type()) {
    case Type::Int32:
      for (size_t i = start; i < end; ++i) dst->Append<int32_t>(src.Get<int32_t>(i));
      break;
    case Type::Int64:
      for (size_t i = start; i < end; ++i) dst->Append<int64_t>(src.Get<int64_t>(i));
      break;
    case Type::Float:
      for (size_t i = start; i < end; ++i) dst->Append<float>(src.Get<float>(i));
      break;
    case Type::Double:
      for (size_t i = start; i < end; ++i) dst->Append<double>(src.Get<double>(i));
      break;
    case Type::Bool:
      for (size_t i = start; i < end; ++i) dst->Append<bool>(src.Get<bool>(i));
      break;
    case Type::String:
    case Type::Blob:
      for (size_t i = start; i < end; ++i) dst->AppendBytes(src.GetBytes(i));
      break;
    default:
      throw std::invalid_argument("NativeColumnar: invalid column type");
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

// Read + validate the footer: row_count and every column's ColumnMeta.
void ParseFooter(const IInputFile &in, uint64_t *row_count, std::vector<ColumnMeta> *metas) {
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
  std::string footer;
  if (!in.ReadAt(size - trailer - footer_size, footer_size, &footer)) {
    throw std::runtime_error("NativeColumnar: truncated footer");
  }

  const char *p = footer.data();
  *row_count = GetU64(p);
  const uint64_t col_count = GetU64(p);
  metas->resize(col_count);
  for (uint64_t i = 0; i < col_count; ++i) {
    (*metas)[i].encoding = GetU8(p);
    (*metas)[i].compression = GetU8(p);
    (*metas)[i].offset = GetU64(p);
    (*metas)[i].stored_size = GetU64(p);
    (*metas)[i].raw_size = GetU64(p);
    (*metas)[i].value_count = GetU64(p);
  }
}

// ---- PageAccessor: the one encoding-dependent step (read rows [a,b) of a page) ----

class PageAccessor {
 public:
  virtual ~PageAccessor() = default;
  // Append rows [first, first+count) of this page (page-local indices) to *out.
  virtual void ReadRows(uint64_t first, uint64_t count, Column *out) = 0;
};

// Universal path: fetch + decode the whole page once (memoized), then slice.
// Correct for every encoding/compression.
class WholePageAccessor : public PageAccessor {
 public:
  WholePageAccessor(const IInputFile &in, ColumnMeta meta, Type type) : in_(in), meta_(meta), type_(type) {}

  void ReadRows(uint64_t first, uint64_t count, Column *out) override {
    if (decoded_ == nullptr) {
      std::string stored;
      if (!in_.ReadAt(meta_.offset, meta_.stored_size, &stored)) {
        throw std::runtime_error("NativeColumnar: truncated column page");
      }
      std::string encoded;
      const CodecRegistry &reg = CodecRegistry::Instance();
      reg.Get(static_cast<CompressionId>(meta_.compression)).Decompress(Slice(stored), meta_.raw_size, &encoded);
      decoded_ = std::make_unique<Column>(type_);
      reg.Get(static_cast<EncodingId>(meta_.encoding)).Decode(Slice(encoded), meta_.value_count, decoded_.get());
    }
    AppendRange(out, *decoded_, static_cast<size_t>(first), static_cast<size_t>(count));
  }

 private:
  const IInputFile &in_;
  ColumnMeta meta_;
  Type type_;
  std::unique_ptr<Column> decoded_;  // whole page, decoded on first use
};

// Fast path: fixed-width + uncompressed -> read only the requested values'
// bytes (value i at offset + i*width) and decode just those.
class FixedWidthDirectAccessor : public PageAccessor {
 public:
  FixedWidthDirectAccessor(const IInputFile &in, ColumnMeta meta, size_t width) : in_(in), meta_(meta), width_(width) {}

  void ReadRows(uint64_t first, uint64_t count, Column *out) override {
    std::string bytes;
    if (!in_.ReadAt(meta_.offset + first * width_, count * width_, &bytes)) {
      throw std::runtime_error("NativeColumnar: truncated column page (direct)");
    }
    CodecRegistry::Instance().Get(static_cast<EncodingId>(meta_.encoding)).Decode(Slice(bytes), count, out);
  }

 private:
  const IInputFile &in_;
  ColumnMeta meta_;
  size_t width_;
};

// Pick the fast path only when the encoding is fixed-width AND the page is
// uncompressed; otherwise the universal whole-page path. Chosen once per page.
std::unique_ptr<PageAccessor> MakeAccessor(const IInputFile &in, const ColumnMeta &m, Type type) {
  const size_t width = CodecRegistry::Instance().Get(static_cast<EncodingId>(m.encoding)).FixedWidth(type);
  if (width > 0 && static_cast<CompressionId>(m.compression) == CompressionId::None) {
    return std::make_unique<FixedWidthDirectAccessor>(in, m, width);
  }
  return std::make_unique<WholePageAccessor>(in, m, type);
}

// ---- per-file reader: no page IO in the ctor; pages read via PageAccessors ----

class NativeColumnarFileReader : public IFileReader {
 public:
  NativeColumnarFileReader(std::unique_ptr<IInputFile> in, std::vector<int> projection, std::vector<Type> types,
                           const std::vector<ColumnMeta> &metas, uint64_t row_count)
      : in_(std::move(in)), projection_(std::move(projection)), types_(std::move(types)), row_count_(row_count) {
    accessors_.reserve(projection_.size());
    for (size_t k = 0; k < projection_.size(); ++k) {
      accessors_.push_back(MakeAccessor(*in_, metas[k], types_[k]));  // metas parallel to projection_
    }
  }

  uint64_t row_count() const override { return row_count_; }

  bool ReadRange(uint64_t first_row, uint64_t num_rows, Chunk *out) override {
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
    for (size_t k = 0; k < projection_.size(); ++k) {
      Column col(types_[k]);
      // Single row group today: page-local index == file row number.
      accessors_[k]->ReadRows(first_row, n, &col);
      chunk.columns.push_back(std::move(col));
    }
    *out = std::move(chunk);
    return true;
  }

 private:
  std::unique_ptr<IInputFile> in_;  // retained; accessors reference it
  std::vector<int> projection_;     // schema field ids, output order
  std::vector<Type> types_;         // parallel to projection_
  uint64_t row_count_;
  std::vector<std::unique_ptr<PageAccessor>> accessors_;  // parallel to projection_
};

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

// ---- Scan cursor: drives OpenReader per file, streams batch_rows windows ----

class NativeColumnarCursor : public IBatchCursor {
 public:
  NativeColumnarCursor(IFileFormat *fmt, IStorage *store, std::vector<std::string> files, std::vector<int> projection,
                       size_t batch_rows)
      : fmt_(fmt),
        store_(store),
        files_(std::move(files)),
        projection_(std::move(projection)),
        batch_rows_(batch_rows == 0 ? 1 : batch_rows) {}

  bool Next(Chunk *out) override {
    while (true) {
      if (reader_ == nullptr) {
        if (next_file_ >= files_.size()) {
          return false;
        }
        reader_ = fmt_->OpenReader(*store_, files_[next_file_++], projection_);  // metadata read here
        if (reader_ == nullptr) {
          continue;  // missing file
        }
        pos_ = 0;
      }
      if (pos_ >= reader_->row_count()) {
        reader_.reset();
        continue;  // exhausted / empty file
      }
      const uint64_t n = std::min<uint64_t>(batch_rows_, reader_->row_count() - pos_);
      reader_->ReadRange(pos_, n, out);
      pos_ += n;
      return true;
    }
  }

 private:
  IFileFormat *fmt_;
  IStorage *store_;
  std::vector<std::string> files_;
  std::vector<int> projection_;
  size_t batch_rows_;
  std::unique_ptr<IFileReader> reader_;
  uint64_t pos_ = 0;
  size_t next_file_ = 0;
};

}  // namespace

std::unique_ptr<IFileReader> NativeColumnarFileFormat::OpenReader(IStorage &store, const std::string &file,
                                                                  const std::vector<int> &projection) {
  auto in = store.OpenInput(file);
  if (in == nullptr) {
    return nullptr;
  }

  uint64_t row_count = 0;
  std::vector<ColumnMeta> metas;
  ParseFooter(*in, &row_count, &metas);  // the metadata / "init" step

  std::vector<Type> types;
  std::vector<ColumnMeta> proj_metas;
  types.reserve(projection.size());
  proj_metas.reserve(projection.size());
  for (int field : projection) {
    if (field < 0 || static_cast<size_t>(field) >= metas.size()) {
      throw std::out_of_range("NativeColumnar: projection index out of range");
    }
    types.push_back(schema_[field].type);
    proj_metas.push_back(metas[field]);
  }

  return std::make_unique<NativeColumnarFileReader>(std::move(in), projection, std::move(types), proj_metas, row_count);
}

std::unique_ptr<IBatchCursor> NativeColumnarFileFormat::Scan(IStorage &store, const std::vector<std::string> &files,
                                                             const std::vector<int> &projection) {
  return std::make_unique<NativeColumnarCursor>(this, &store, files, projection, batch_rows_);
}

std::unique_ptr<IChunkWriter> NativeColumnarFileFormat::OpenWriter(IStorage &store, const std::string &path) {
  return std::make_unique<NativeColumnarWriter>(store.OpenOutput(path), schema_, write_compression_);
}

}  // namespace dbplay
