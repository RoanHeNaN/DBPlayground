#include "Cloud/SequentialProtocol.h"

#include <openssl/sha.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "Cloud/CloudValidation.h"
#include "Table/RowCodec.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

namespace dbplay {
namespace {

class Writer {
 public:
  void Raw(const char *p, size_t n) { bytes_.append(p, n); }
  void U8(uint8_t n) { bytes_.push_back(static_cast<char>(n)); }
  void U32(uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) U8(static_cast<uint8_t>(n >> (8 * i)));
  }
  void U64(uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) U8(static_cast<uint8_t>(n >> (8 * i)));
  }
  void String(const std::string &s) {
    if (s.size() > UINT32_MAX) throw std::invalid_argument("sequential: string too large");
    U32(static_cast<uint32_t>(s.size()));
    Raw(s.data(), s.size());
  }
  void Strings(const std::vector<std::string> &xs) {
    if (xs.size() > UINT32_MAX) throw std::invalid_argument("sequential: list too large");
    U32(static_cast<uint32_t>(xs.size()));
    for (const auto &x : xs) String(x);
  }
  const std::string &bytes() const { return bytes_; }
  std::string Take() { return std::move(bytes_); }

 private:
  std::string bytes_;
};

class Reader {
 public:
  explicit Reader(const Slice &s) : p_(s.data()), end_(s.data() + s.size()) {}
  std::string Raw(size_t n) {
    Need(n);
    std::string s(p_, n);
    p_ += n;
    return s;
  }
  void Magic(const char *s) {
    if (Raw(4) != std::string(s, 4)) throw std::invalid_argument("sequential: bad magic");
  }
  uint8_t U8() {
    Need(1);
    return static_cast<uint8_t>(*p_++);
  }
  uint32_t U32() {
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= uint32_t(U8()) << (8 * i);
    return n;
  }
  uint64_t U64() {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n |= uint64_t(U8()) << (8 * i);
    return n;
  }
  std::string String() { return Raw(U32()); }
  std::vector<std::string> Strings() {
    const uint32_t count = U32();
    if (count > Remaining() / 4) throw std::invalid_argument("sequential: bad list count");
    std::vector<std::string> xs;
    xs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) xs.push_back(String());
    return xs;
  }
  size_t Remaining() const { return static_cast<size_t>(end_ - p_); }
  void End() const {
    if (p_ != end_) throw std::invalid_argument("sequential: trailing bytes");
  }

 private:
  void Need(size_t n) const {
    if (n > Remaining()) throw std::invalid_argument("sequential: truncated object");
  }
  const char *p_;
  const char *end_;
};

void Header(Writer *w, const char *magic) {
  w->Raw(magic, 4);
  w->U32(2);
}
void Header(Reader *r, const char *magic) {
  r->Magic(magic);
  if (r->U32() != 2) throw std::invalid_argument("sequential: bad version");
}
uint32_t Crc(const char *p, size_t n) {
  return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef *>(p), static_cast<uInt>(n)));
}
std::string Digest(const std::string &s) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> out{};
  SHA256(reinterpret_cast<const unsigned char *>(s.data()), s.size(), out.data());
  return std::string(reinterpret_cast<const char *>(out.data()), out.size());
}
Value Cell(const Column &col, size_t row) {
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
      throw std::invalid_argument("sequential: invalid cell type");
  }
}

std::string ReadAll(IInputFile &in) {
  if (in.Size() > 256ULL * 1024 * 1024) throw std::runtime_error("sequential: WAL too large");
  std::string bytes;
  if (!in.ReadAt(0, static_cast<size_t>(in.Size()), &bytes)) throw std::runtime_error("sequential: short WAL read");
  return bytes;
}

}  // namespace

std::string SequentialState::toString() const {
  return fmt::format(
      "SequentialState{{table_id={}, state_version={}, schema_version={}, writer_epoch={}, indexed_seq={}, "
      "published_seq={}, manifest_key={}}}",
      table_id, state_version, schema_version, writer_epoch, indexed_seq, published_seq, manifest_key);
}

std::string SequentialManifest::DataFile::toString() const {
  return fmt::format("DataFile{{path={}, first_seq={}, last_seq={}, row_count={}}}", path, first_seq, last_seq,
                     row_count);
}

std::string SequentialManifest::toString() const {
  std::vector<std::string> files;
  files.reserve(data_files.size());
  for (const auto &file : data_files) files.push_back(file.toString());
  return fmt::format(
      "SequentialManifest{{table_id={}, covered_through_seq={}, schema_version={}, schema_fields={}, "
      "data_files=[{}], batch_index_files=[{}]}}",
      table_id, covered_through_seq, schema_version, schema.size(), fmt::join(files, ", "),
      fmt::join(batch_index_files, ", "));
}

std::string SequentialWal::toString() const {
  uint64_t row_count = 0;
  for (const auto &chunk : chunks) row_count += chunk.row_count;
  return fmt::format(
      "SequentialWal{{kind={}, table_id={}, seq={}, writer_epoch={}, schema_version={}, operation_id={}, "
      "batch_ids=[{}], chunk_count={}, row_count={}}}",
      kind == Kind::Append ? "Append" : "Fence", table_id, seq, writer_epoch, schema_version, operation_id,
      fmt::join(batch_ids, ", "), chunks.size(), row_count);
}

std::string VersionedSequentialState::toString() const {
  return fmt::format("VersionedSequentialState{{state={}, version={}}}", state.toString(), version.toString());
}

std::string SequentialWalKey(const TableDescriptor &table, uint64_t seq) {
  if (seq == 0) throw std::invalid_argument("sequential: WAL seq is zero");
  char name[64];
  std::snprintf(name, sizeof(name), "/wal/%020llu.wal", static_cast<unsigned long long>(seq));
  std::string prefix = table.file_prefix;
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  if (prefix.empty()) throw std::invalid_argument("sequential: empty file prefix");
  return prefix + name;
}

std::string EncodeSequentialState(const SequentialState &s) {
  if (s.table_id.empty() || s.manifest_key.compare(0, 9, "manifest/") != 0 || s.indexed_seq > s.published_seq ||
      s.schema_version != 1)
    throw std::invalid_argument("sequential: invalid CURRENT");
  Writer w;
  Header(&w, "DBS2");
  w.String(s.table_id);
  w.U64(s.state_version);
  w.U64(s.schema_version);
  w.U64(s.writer_epoch);
  w.U64(s.indexed_seq);
  w.U64(s.published_seq);
  w.String(s.manifest_key);
  return w.Take();
}

SequentialState DecodeSequentialState(const Slice &bytes) {
  Reader r(bytes);
  Header(&r, "DBS2");
  SequentialState s;
  s.table_id = r.String();
  s.state_version = r.U64();
  s.schema_version = r.U64();
  s.writer_epoch = r.U64();
  s.indexed_seq = r.U64();
  s.published_seq = r.U64();
  s.manifest_key = r.String();
  r.End();
  EncodeSequentialState(s);
  return s;
}

std::string EncodeSequentialManifest(const SequentialManifest &m) {
  if (m.table_id.empty() || m.schema.empty()) throw std::invalid_argument("sequential: invalid manifest");
  Writer w;
  Header(&w, "DBM2");
  w.String(m.table_id);
  w.U64(m.covered_through_seq);
  w.U64(m.schema_version);
  w.U32(static_cast<uint32_t>(m.schema.size()));
  for (const auto &field : m.schema) {
    if (field.name.empty() || field.type == Type::Invalid || field.type > Type::Blob)
      throw std::invalid_argument("sequential: invalid schema field");
    w.String(field.name);
    w.U8(static_cast<uint8_t>(field.type));
  }
  if (m.data_files.size() > UINT32_MAX) throw std::invalid_argument("sequential: too many data files");
  w.U32(static_cast<uint32_t>(m.data_files.size()));
  for (const auto &file : m.data_files) {
    if (file.path.empty() || file.first_seq == 0 || file.first_seq > file.last_seq ||
        file.last_seq > m.covered_through_seq || file.row_count == 0)
      throw std::invalid_argument("sequential: invalid data file");
    w.String(file.path);
    w.U64(file.first_seq);
    w.U64(file.last_seq);
    w.U64(file.row_count);
  }
  for (const auto &key : m.batch_index_files)
    if (key.compare(0, 6, "dedup/") != 0) throw std::invalid_argument("sequential: invalid batch index key");
  w.Strings(m.batch_index_files);
  return w.Take();
}

SequentialManifest DecodeSequentialManifest(const Slice &bytes) {
  Reader r(bytes);
  Header(&r, "DBM2");
  SequentialManifest m;
  m.table_id = r.String();
  m.covered_through_seq = r.U64();
  m.schema_version = r.U64();
  const uint32_t n = r.U32();
  if (n > 1024) throw std::invalid_argument("sequential: too many columns");
  for (uint32_t i = 0; i < n; ++i) m.schema.push_back({r.String(), static_cast<Type>(r.U8())});
  const uint32_t file_count = r.U32();
  if (file_count > r.Remaining() / (4 + 8 + 8 + 8)) throw std::invalid_argument("sequential: bad data file count");
  for (uint32_t i = 0; i < file_count; ++i) {
    SequentialManifest::DataFile file;
    file.path = r.String();
    file.first_seq = r.U64();
    file.last_seq = r.U64();
    file.row_count = r.U64();
    m.data_files.push_back(std::move(file));
  }
  m.batch_index_files = r.Strings();
  r.End();
  EncodeSequentialManifest(m);
  return m;
}

std::string EncodeSequentialBatchIndex(const std::vector<SequentialBatchEntry> &entries) {
  Writer w;
  Header(&w, "DBI2");
  if (entries.size() > UINT32_MAX) throw std::invalid_argument("sequential: batch index too large");
  w.U32(static_cast<uint32_t>(entries.size()));
  for (const auto &entry : entries) {
    if (entry.batch_id.empty() || entry.seq == 0 || entry.operation_id.empty() ||
        entry.payload_digest.size() != SHA256_DIGEST_LENGTH)
      throw std::invalid_argument("sequential: invalid batch index entry");
    w.String(entry.batch_id);
    w.U64(entry.seq);
    w.String(entry.operation_id);
    w.Raw(entry.payload_digest.data(), entry.payload_digest.size());
  }
  return w.Take();
}
std::vector<SequentialBatchEntry> DecodeSequentialBatchIndex(const Slice &bytes) {
  Reader r(bytes);
  Header(&r, "DBI2");
  const uint32_t count = r.U32();
  if (count > r.Remaining() / (4 + 8 + 4 + SHA256_DIGEST_LENGTH))
    throw std::invalid_argument("sequential: invalid batch index count");
  std::vector<SequentialBatchEntry> entries;
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    SequentialBatchEntry entry;
    entry.batch_id = r.String();
    entry.seq = r.U64();
    entry.operation_id = r.String();
    entry.payload_digest = r.Raw(SHA256_DIGEST_LENGTH);
    entries.push_back(std::move(entry));
  }
  r.End();
  EncodeSequentialBatchIndex(entries);
  return entries;
}

std::string EncodeSequentialWal(const SequentialWal &wal, const Schema &schema) {
  if (wal.table_id.empty() || wal.seq == 0 || wal.writer_epoch == 0 || wal.operation_id.empty())
    throw std::invalid_argument("sequential: invalid WAL header");
  if (wal.kind == SequentialWal::Kind::Append && (wal.batch_ids.empty() || wal.chunks.empty()))
    throw std::invalid_argument("sequential: empty Append WAL");
  if (wal.kind == SequentialWal::Kind::Fence && (!wal.batch_ids.empty() || !wal.chunks.empty()))
    throw std::invalid_argument("sequential: nonempty Fence WAL");
  if (wal.kind != SequentialWal::Kind::Append && wal.kind != SequentialWal::Kind::Fence)
    throw std::invalid_argument("sequential: invalid WAL kind");
  if (wal.schema_version != 1 || wal.chunks.size() > UINT32_MAX)
    throw std::invalid_argument("sequential: unsupported WAL schema or chunk count");
  Writer w;
  Header(&w, "DBW2");
  w.U8(static_cast<uint8_t>(wal.kind));
  w.String(wal.table_id);
  w.U64(wal.seq);
  w.U64(wal.writer_epoch);
  w.U64(wal.schema_version);
  w.String(wal.operation_id);
  w.Strings(wal.batch_ids);
  w.U32(static_cast<uint32_t>(wal.chunks.size()));
  Writer logical;
  logical.U64(wal.schema_version);
  logical.Strings(wal.batch_ids);
  RowCodec codec(schema);
  uint64_t total_rows = 0;
  for (const auto &chunk : wal.chunks) {
    if (!IsFullSchemaChunk(chunk, schema) || chunk.row_count > UINT32_MAX)
      throw std::invalid_argument("sequential: invalid WAL chunk");
    Writer payload;
    for (size_t row = 0; row < chunk.row_count; ++row) {
      std::vector<Value> values;
      for (const auto &col : chunk.columns) values.push_back(Cell(col, row));
      payload.String(codec.Encode(values));
    }
    Writer entry;
    entry.U32(static_cast<uint32_t>(chunk.row_count));
    entry.U32(static_cast<uint32_t>(payload.bytes().size()));
    entry.Raw(payload.bytes().data(), payload.bytes().size());
    w.Raw(entry.bytes().data(), entry.bytes().size());
    w.U32(Crc(entry.bytes().data(), entry.bytes().size()));
    logical.Raw(entry.bytes().data(), entry.bytes().size());
    total_rows += chunk.row_count;
  }
  w.U64(total_rows);
  const std::string digest = Digest(logical.bytes());
  w.Raw(digest.data(), digest.size());
  w.U32(Crc(w.bytes().data(), w.bytes().size()));
  w.Raw("DBW2", 4);
  return w.Take();
}

SequentialWal DecodeSequentialWal(const Slice &bytes, const Schema &schema) {
  if (bytes.size() < 8) throw std::invalid_argument("sequential: short WAL");
  Reader r(bytes);
  Header(&r, "DBW2");
  SequentialWal wal;
  wal.kind = static_cast<SequentialWal::Kind>(r.U8());
  if (wal.kind != SequentialWal::Kind::Append && wal.kind != SequentialWal::Kind::Fence)
    throw std::invalid_argument("sequential: bad WAL kind");
  wal.table_id = r.String();
  wal.seq = r.U64();
  wal.writer_epoch = r.U64();
  wal.schema_version = r.U64();
  wal.operation_id = r.String();
  wal.batch_ids = r.Strings();
  const uint32_t count = r.U32();
  if (count > 1000000) throw std::invalid_argument("sequential: too many WAL chunks");
  Writer logical;
  logical.U64(wal.schema_version);
  logical.Strings(wal.batch_ids);
  RowCodec codec(schema);
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t rows = r.U32();
    if (rows == 0) throw std::invalid_argument("sequential: empty WAL chunk");
    const uint32_t size = r.U32();
    const std::string payload = r.Raw(size);
    Writer entry;
    entry.U32(rows);
    entry.U32(size);
    entry.Raw(payload.data(), payload.size());
    if (r.U32() != Crc(entry.bytes().data(), entry.bytes().size()))
      throw std::invalid_argument("sequential: bad chunk CRC");
    logical.Raw(entry.bytes().data(), entry.bytes().size());
    Reader row_reader{Slice(payload)};
    Chunk chunk;
    chunk.row_count = rows;
    for (size_t col = 0; col < schema.size(); ++col) {
      chunk.column_ids.push_back(static_cast<int>(col));
      chunk.columns.emplace_back(schema[col].type);
    }
    std::vector<int> projection = chunk.column_ids;
    for (uint32_t row = 0; row < rows; ++row) {
      const std::string encoded = row_reader.String();
      codec.DecodeInto(Slice(encoded), projection, &chunk.columns);
    }
    row_reader.End();
    wal.chunks.push_back(std::move(chunk));
    total += rows;
  }
  if (r.U64() != total) throw std::invalid_argument("sequential: row count mismatch");
  wal.payload_digest = r.Raw(SHA256_DIGEST_LENGTH);
  if (wal.payload_digest != Digest(logical.bytes())) throw std::invalid_argument("sequential: digest mismatch");
  if (r.Remaining() != 8) throw std::invalid_argument("sequential: bad WAL footer");
  const uint32_t actual_crc = Crc(bytes.data(), bytes.size() - 8);
  if (r.U32() != actual_crc) throw std::invalid_argument("sequential: bad WAL CRC");
  r.Magic("DBW2");
  r.End();
  if (wal.table_id.empty() || wal.seq == 0 || wal.writer_epoch == 0 || wal.operation_id.empty() ||
      (wal.kind == SequentialWal::Kind::Append && (wal.batch_ids.empty() || wal.chunks.empty())) ||
      (wal.kind == SequentialWal::Kind::Fence && (!wal.batch_ids.empty() || !wal.chunks.empty())))
    throw std::invalid_argument("sequential: invalid WAL contents");
  return wal;
}

std::optional<SequentialWal> LoadSequentialWal(IStorage &files, const TableDescriptor &table, uint64_t seq) {
  auto in = files.OpenInput(SequentialWalKey(table, seq));
  if (!in) return std::nullopt;
  const std::string bytes = ReadAll(*in);
  auto wal = DecodeSequentialWal(Slice(bytes), table.schema);
  if (wal.seq != seq || wal.table_id != table.table_id || wal.schema_version != 1)
    throw std::runtime_error("sequential: WAL identity mismatch");
  return wal;
}

SequentialStateStore::SequentialStateStore(const TableDescriptor &table, std::shared_ptr<IMetadataStore> metadata)
    : table_(table), metadata_(std::move(metadata)), current_key_(table.metadata_prefix + "/CURRENT") {
  if (!metadata_) throw std::invalid_argument("sequential: null metadata store");
  while (!table_.metadata_prefix.empty() && table_.metadata_prefix.back() == '/') table_.metadata_prefix.pop_back();
  if (table_.metadata_prefix.empty()) throw std::invalid_argument("sequential: empty metadata prefix");
  current_key_ = table_.metadata_prefix + "/CURRENT";
}

std::optional<VersionedSequentialState> SequentialStateStore::Load() const {
  auto v = metadata_->Get(current_key_);
  if (!v) {
    return std::nullopt;
  }
  auto state = DecodeSequentialState(Slice(v->value));
  if (state.table_id != table_.table_id) {
    throw std::runtime_error("sequential: CURRENT table mismatch");
  }
  return VersionedSequentialState{std::move(state), std::move(v->version)};
}

SequentialManifest SequentialStateStore::LoadManifest(const SequentialState &state) const {
  auto v = metadata_->Get(table_.metadata_prefix + "/" + state.manifest_key);
  if (!v) throw std::runtime_error("sequential: manifest missing");
  auto m = DecodeSequentialManifest(Slice(v->value));
  if (m.table_id != table_.table_id || m.covered_through_seq != state.indexed_seq ||
      m.schema_version != state.schema_version || !SchemasEqual(m.schema, table_.schema))
    throw std::runtime_error("sequential: manifest mismatch");
  return m;
}

std::vector<SequentialBatchEntry> SequentialStateStore::LoadBatchIndex(const SequentialManifest &manifest) const {
  std::vector<SequentialBatchEntry> entries;
  for (const auto &key : manifest.batch_index_files) {
    auto v = metadata_->Get(table_.metadata_prefix + "/" + key);
    if (!v) throw std::runtime_error("sequential: batch index missing");
    auto part = DecodeSequentialBatchIndex(Slice(v->value));
    entries.insert(entries.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
  }
  return entries;
}

ConditionalWriteResult SequentialStateStore::Initialize() {
  SequentialManifest manifest;
  manifest.table_id = table_.table_id;
  manifest.schema = table_.schema;
  const std::string manifest_key = "manifest/initial.meta";
  const auto result = WriteManifest(manifest_key, manifest);
  if (result == ConditionalWriteResult::RetryableConflict) return result;
  if (result == ConditionalWriteResult::PreconditionFailed) {
    auto existing = metadata_->Get(table_.metadata_prefix + "/" + manifest_key);
    if (!existing || existing->value != EncodeSequentialManifest(manifest))
      throw std::runtime_error("sequential: initial manifest collision");
  }
  SequentialState state;
  state.table_id = table_.table_id;
  state.manifest_key = manifest_key;
  return metadata_->PutIfAbsent(current_key_, Slice(EncodeSequentialState(state)), nullptr);
}

ConditionalWriteResult SequentialStateStore::CompareExchange(const VersionedSequentialState &expected,
                                                             const SequentialState &next,
                                                             VersionedSequentialState *written) {
  if (next.table_id != table_.table_id || next.state_version != expected.state.state_version + 1 ||
      next.writer_epoch < expected.state.writer_epoch || next.indexed_seq < expected.state.indexed_seq ||
      next.published_seq < expected.state.published_seq || next.indexed_seq > next.published_seq)
    throw std::invalid_argument("sequential: invalid CURRENT transition");
  const auto bytes = EncodeSequentialState(next);
  MetadataVersion version;
  const auto result = metadata_->CompareExchange(current_key_, expected.version, Slice(bytes), &version);
  if (result == ConditionalWriteResult::Applied && written) *written = {next, version};
  return result;
}

ConditionalWriteResult SequentialStateStore::Publish(const VersionedSequentialState &expected,
                                                     const SequentialPublishUpdate &update,
                                                     VersionedSequentialState *written) {
  if (update.through_seq == 0 || (!update.indexed_manifest_key && update.through_seq <= expected.state.published_seq) ||
      (update.indexed_manifest_key && (update.through_seq <= expected.state.indexed_seq ||
                                       update.indexed_manifest_key->compare(0, 9, "manifest/") != 0)))
    throw std::invalid_argument("sequential: invalid publish update");

  SequentialState next = expected.state;
  ++next.state_version;
  next.published_seq = std::max(next.published_seq, update.through_seq);
  if (update.indexed_manifest_key) {
    next.indexed_seq = update.through_seq;
    next.manifest_key = *update.indexed_manifest_key;
  }
  return CompareExchange(expected, next, written);
}

ConditionalWriteResult SequentialStateStore::WriteManifest(const std::string &key, const SequentialManifest &manifest) {
  const auto bytes = EncodeSequentialManifest(manifest);
  return metadata_->PutIfAbsent(table_.metadata_prefix + "/" + key, Slice(bytes), nullptr);
}
ConditionalWriteResult SequentialStateStore::WriteBatchIndex(const std::string &key,
                                                             const std::vector<SequentialBatchEntry> &entries) {
  const auto bytes = EncodeSequentialBatchIndex(entries);
  return metadata_->PutIfAbsent(table_.metadata_prefix + "/" + key, Slice(bytes), nullptr);
}

}  // namespace dbplay
