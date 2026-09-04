#include "Table/Format/WalFileFormat.h"

#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "Table/Chunk.h"
#include "Table/Column.h"
#include "Table/RowCodec.h"
#include "Table/Value.h"

namespace dbplay {
namespace {

constexpr char kMagic[4] = {'D', 'B', 'W', '1'};
constexpr uint32_t kCodecVersion = 1;

void PutU8(std::string *b, uint8_t v) { b->push_back(static_cast<char>(v)); }

void PutU32(std::string *b, uint32_t v) {
  for (size_t i = 0; i < sizeof(v); ++i) {
    b->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

void PutU64(std::string *b, uint64_t v) {
  for (size_t i = 0; i < sizeof(v); ++i) {
    b->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

uint8_t GetU8(const char *&p, const char *end) {
  if (p >= end) {
    throw std::runtime_error("WalFileFormat: truncated u8");
  }
  return static_cast<uint8_t>(*p++);
}

uint32_t GetU32(const char *&p, const char *end) {
  if (static_cast<size_t>(end - p) < sizeof(uint32_t)) {
    throw std::runtime_error("WalFileFormat: truncated u32");
  }
  uint32_t v = 0;
  for (size_t i = 0; i < sizeof(v); ++i) {
    v |= static_cast<uint32_t>(static_cast<unsigned char>(*p++)) << (i * 8);
  }
  return v;
}

uint64_t GetU64(const char *&p, const char *end) {
  if (static_cast<size_t>(end - p) < sizeof(uint64_t)) {
    throw std::runtime_error("WalFileFormat: truncated u64");
  }
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof(v); ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(*p++)) << (i * 8);
  }
  return v;
}

uint64_t ReadU64At(const IInputFile &in, uint64_t offset) {
  std::string buf;
  if (!in.ReadAt(offset, sizeof(uint64_t), &buf)) {
    throw std::runtime_error("WalFileFormat: read past end of file");
  }
  const char *p = buf.data();
  return GetU64(p, buf.data() + buf.size());
}

uint32_t Crc32(const void *data, size_t size) {
  return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef *>(data), static_cast<uInt>(size)));
}

Value ReadCell(const Column &col, size_t row) {
  switch (col.type()) {
    case Type::Int32:
      return Value::Int32(col.Get<int32_t>(row));
    case Type::Int64:
      return Value::Int64(col.Get<int64_t>(row));
    case Type::Float:
      return Value::Float(col.Get<float>(row));
    case Type::Double:
      return Value::Double(col.Get<double>(row));
    case Type::Bool:
      return Value::Bool(col.Get<bool>(row));
    case Type::String:
    case Type::Blob:
      return Value::String(col.GetBytes(row).ToString());
    default:
      throw std::invalid_argument("WalFileFormat: invalid column type");
  }
}

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
      throw std::invalid_argument("WalFileFormat: invalid column type");
  }
}

std::string EncodeHeader(const Schema &schema) {
  std::string header(kMagic, sizeof(kMagic));
  PutU32(&header, kCodecVersion);
  if (schema.size() > UINT32_MAX) {
    throw std::invalid_argument("WalFileFormat: schema is too large");
  }
  PutU32(&header, static_cast<uint32_t>(schema.size()));
  for (const Field &field : schema) {
    if (field.type == Type::Invalid) {
      throw std::invalid_argument("WalFileFormat: invalid field type");
    }
    PutU8(&header, static_cast<uint8_t>(field.type));
  }
  return header;
}

void ValidateHeader(const std::string &header, const Schema &schema) {
  const char *p = header.data();
  const char *end = header.data() + header.size();
  if (header.size() < sizeof(kMagic) + sizeof(uint32_t) * 2 ||
      std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("WalFileFormat: bad header magic");
  }
  p += sizeof(kMagic);
  if (GetU32(p, end) != kCodecVersion) {
    throw std::runtime_error("WalFileFormat: unsupported codec version");
  }
  const uint32_t column_count = GetU32(p, end);
  if (column_count != schema.size()) {
    throw std::runtime_error("WalFileFormat: schema column count mismatch");
  }
  for (uint32_t i = 0; i < column_count; ++i) {
    if (static_cast<Type>(GetU8(p, end)) != schema[i].type) {
      throw std::runtime_error("WalFileFormat: schema type mismatch");
    }
  }
  if (p != end) {
    throw std::runtime_error("WalFileFormat: trailing header bytes");
  }
}

size_t HeaderSize(const Schema &schema) { return sizeof(kMagic) + sizeof(uint32_t) * 2 + schema.size(); }

void DecodeRows(const Slice &payload, uint32_t row_count, const RowCodec &codec, std::vector<Column> *cols) {
  const char *p = payload.data();
  const char *end = payload.data() + payload.size();
  const std::vector<int> projection = [&] {
    std::vector<int> ids(codec.schema().size());
    for (size_t i = 0; i < ids.size(); ++i) {
      ids[i] = static_cast<int>(i);
    }
    return ids;
  }();
  for (uint32_t i = 0; i < row_count; ++i) {
    const uint32_t row_len = GetU32(p, end);
    if (static_cast<size_t>(end - p) < row_len) {
      throw std::runtime_error("WalFileFormat: truncated row");
    }
    codec.DecodeInto(Slice(p, row_len), projection, cols);
    p += row_len;
  }
  if (p != end) {
    throw std::runtime_error("WalFileFormat: trailing payload bytes");
  }
}

class WalFileReader : public IFileReader {
 public:
  WalFileReader(std::unique_ptr<IInputFile> in, Schema schema, std::vector<int> projection, uint64_t row_count,
                uint64_t body_end)
      : in_(std::move(in)),
        schema_(std::move(schema)),
        codec_(schema_),
        projection_(std::move(projection)),
        row_count_(row_count),
        body_end_(body_end) {}

  uint64_t row_count() const override { return row_count_; }

  bool ReadRange(uint64_t first_row, uint64_t num_rows, Chunk *out) override {
    EnsureDecoded();
    if (first_row >= row_count_ || num_rows == 0) {
      return false;
    }
    const uint64_t count = std::min(num_rows, row_count_ - first_row);
    out->column_ids = projection_;
    out->columns.clear();
    out->columns.reserve(projection_.size());
    for (size_t i = 0; i < projection_.size(); ++i) {
      out->columns.emplace_back(schema_[projection_[i]].type);
      AppendRange(&out->columns.back(), columns_[i], static_cast<size_t>(first_row), static_cast<size_t>(count));
    }
    out->row_count = static_cast<size_t>(count);
    return true;
  }

 private:
  void EnsureDecoded() {
    if (!columns_.empty() || row_count_ == 0) {
      if (columns_.empty()) {
        for (int field : projection_) {
          columns_.emplace_back(schema_[field].type);
        }
      }
      return;
    }

    const uint64_t header_size = HeaderSize(schema_);
    std::string body;
    if (body_end_ < header_size || !in_->ReadAt(header_size, body_end_ - header_size, &body)) {
      throw std::runtime_error("WalFileFormat: truncated entries");
    }

    for (int field : projection_) {
      if (field < 0 || static_cast<size_t>(field) >= schema_.size()) {
        throw std::out_of_range("WalFileFormat: projection index out of range");
      }
      columns_.emplace_back(schema_[field].type);
    }

    const char *p = body.data();
    const char *end = body.data() + body.size();
    uint64_t decoded_rows = 0;
    while (p < end) {
      const char *entry_start = p;
      const uint32_t entry_rows = GetU32(p, end);
      const uint32_t payload_size = GetU32(p, end);
      if (static_cast<size_t>(end - p) < payload_size + sizeof(uint32_t)) {
        throw std::runtime_error("WalFileFormat: truncated entry");
      }
      const Slice payload(p, payload_size);
      p += payload_size;
      const uint32_t crc = GetU32(p, end);
      const uint32_t actual =
          Crc32(entry_start,
                static_cast<size_t>(reinterpret_cast<const char *>(payload.data()) - entry_start) + payload_size);
      if (crc != actual) {
        throw std::runtime_error("WalFileFormat: checksum mismatch");
      }

      std::vector<Column> full;
      full.reserve(schema_.size());
      for (const Field &field : schema_) {
        full.emplace_back(field.type);
      }
      DecodeRows(payload, entry_rows, codec_, &full);
      for (size_t i = 0; i < projection_.size(); ++i) {
        AppendRange(&columns_[i], full[projection_[i]], 0, full[projection_[i]].size());
      }
      decoded_rows += entry_rows;
    }
    if (decoded_rows != row_count_) {
      throw std::runtime_error("WalFileFormat: footer row count does not match entries");
    }
  }

  std::unique_ptr<IInputFile> in_;
  Schema schema_;
  RowCodec codec_;
  std::vector<int> projection_;
  uint64_t row_count_;
  uint64_t body_end_;
  std::vector<Column> columns_;
};

class WalFileWriter : public IChunkWriter {
 public:
  WalFileWriter(std::unique_ptr<IOutputStream> out, Schema schema)
      : out_(std::move(out)), schema_(std::move(schema)), codec_(schema_) {
    if (out_ == nullptr) {
      throw std::runtime_error("WalFileFormat: output stream is null");
    }
    out_->Append(Slice(EncodeHeader(schema_)));
  }

  void Write(const Chunk &chunk) override {
    if (closed_) {
      throw std::runtime_error("WalFileFormat: write after close");
    }
    if (chunk.row_count == 0 || chunk.columns.size() != schema_.size() || chunk.column_ids.size() != schema_.size()) {
      throw std::invalid_argument("WalFileFormat: chunk must cover the full schema");
    }
    for (size_t i = 0; i < schema_.size(); ++i) {
      if (chunk.column_ids[i] != static_cast<int>(i) || chunk.columns[i].type() != schema_[i].type ||
          chunk.columns[i].size() != chunk.row_count) {
        throw std::invalid_argument("WalFileFormat: chunk must cover the full schema");
      }
    }
    if (chunk.row_count > UINT32_MAX) {
      throw std::invalid_argument("WalFileFormat: chunk is too large");
    }

    std::string payload;
    for (size_t row = 0; row < chunk.row_count; ++row) {
      std::vector<Value> values;
      values.reserve(schema_.size());
      for (const Column &col : chunk.columns) {
        values.push_back(ReadCell(col, row));
      }
      const std::string encoded = codec_.Encode(values);
      if (encoded.size() > UINT32_MAX) {
        throw std::invalid_argument("WalFileFormat: encoded row is too large");
      }
      PutU32(&payload, static_cast<uint32_t>(encoded.size()));
      payload.append(encoded);
    }

    std::string entry;
    PutU32(&entry, static_cast<uint32_t>(chunk.row_count));
    PutU32(&entry, static_cast<uint32_t>(payload.size()));
    entry.append(payload);
    PutU32(&entry, Crc32(entry.data(), entry.size()));
    out_->Append(Slice(entry));
    total_rows_ += chunk.row_count;
    ++entry_count_;
  }

  void Close() override {
    if (closed_) {
      return;
    }
    std::string footer;
    PutU64(&footer, total_rows_);
    PutU64(&footer, entry_count_);
    out_->Append(Slice(footer));
    std::string trailer;
    PutU64(&trailer, footer.size());
    trailer.append(kMagic, sizeof(kMagic));
    out_->Append(Slice(trailer));
    out_->Close();
    closed_ = true;
  }

 private:
  std::unique_ptr<IOutputStream> out_;
  Schema schema_;
  RowCodec codec_;
  uint64_t total_rows_ = 0;
  uint64_t entry_count_ = 0;
  bool closed_ = false;
};

class WalScanCursor : public IBatchCursor {
 public:
  WalScanCursor(IFileFormat *fmt, IStorage *store, std::vector<std::string> files, std::vector<int> projection,
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
        reader_ = fmt_->OpenReader(*store_, files_[next_file_++], projection_);
        if (reader_ == nullptr) {
          throw std::runtime_error("WalFileFormat: WAL file is missing");
        }
        pos_ = 0;
      }
      if (pos_ >= reader_->row_count()) {
        reader_.reset();
        continue;
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

std::unique_ptr<IFileReader> WalFileFormat::OpenReader(IStorage &store, const std::string &file,
                                                       const std::vector<int> &projection) {
  auto in = store.OpenInput(file);
  if (in == nullptr) {
    return nullptr;
  }

  const uint64_t size = in->Size();
  const uint64_t trailer = sizeof(kMagic) + sizeof(uint64_t);
  const uint64_t header_size = HeaderSize(schema_);
  if (size < header_size + trailer + sizeof(uint64_t) * 2) {
    throw std::runtime_error("WalFileFormat: file too small");
  }

  std::string header;
  if (!in->ReadAt(0, header_size, &header)) {
    throw std::runtime_error("WalFileFormat: truncated header");
  }
  ValidateHeader(header, schema_);

  std::string tail_magic;
  if (!in->ReadAt(size - sizeof(kMagic), sizeof(kMagic), &tail_magic) ||
      std::memcmp(tail_magic.data(), kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("WalFileFormat: bad trailing magic");
  }
  const uint64_t footer_size = ReadU64At(*in, size - trailer);
  if (footer_size != sizeof(uint64_t) * 2) {
    throw std::runtime_error("WalFileFormat: invalid footer size");
  }
  std::string footer;
  if (!in->ReadAt(size - trailer - footer_size, footer_size, &footer)) {
    throw std::runtime_error("WalFileFormat: truncated footer");
  }
  const char *p = footer.data();
  const char *end = footer.data() + footer.size();
  const uint64_t row_count = GetU64(p, end);
  GetU64(p, end);  // entry_count, validated against decoded entries
  if (p != end) {
    throw std::runtime_error("WalFileFormat: trailing footer bytes");
  }

  return std::make_unique<WalFileReader>(std::move(in), schema_, projection, row_count, size - trailer - footer_size);
}

std::unique_ptr<IBatchCursor> WalFileFormat::Scan(IStorage &store, const std::vector<std::string> &files,
                                                  const std::vector<int> &projection) {
  return std::make_unique<WalScanCursor>(this, &store, files, projection, batch_rows_);
}

std::unique_ptr<IChunkWriter> WalFileFormat::OpenWriter(IStorage &store, const std::string &path) {
  return std::make_unique<WalFileWriter>(store.OpenOutput(path), schema_);
}

}  // namespace dbplay
