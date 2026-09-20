#include "Metadata/TableMetadataCodec.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dbplay {
namespace {

// Keep the existing bytes for on-disk compatibility; only the C++ name is
// being clarified.
constexpr std::array<char, 4> kCurrentTableStateMagic{'D', 'B', 'T', 'S'};
constexpr std::array<char, 4> kCommitRecordMagic{'D', 'B', 'C', 'R'};
constexpr std::array<char, 4> kCompactedDataManifestMagic{'D', 'B', 'B', 'M'};
constexpr uint32_t kCodecVersion = 1;

class Writer {
 public:
  void Bytes(const std::array<char, 4> &value) { bytes_.append(value.data(), value.size()); }

  void U32(uint32_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
      bytes_.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
  }

  void U64(uint64_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
      bytes_.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
  }

  void String(const std::string &value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::invalid_argument("TableMetadataCodec: string is too large");
    }
    U32(static_cast<uint32_t>(value.size()));
    bytes_.append(value);
  }

  void Strings(const std::vector<std::string> &values) {
    if (values.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::invalid_argument("TableMetadataCodec: vector is too large");
    }
    U32(static_cast<uint32_t>(values.size()));
    for (const auto &value : values) {
      String(value);
    }
  }

  std::string Finish() { return std::move(bytes_); }

 private:
  std::string bytes_;
};

class Reader {
 public:
  explicit Reader(const Slice &bytes) : bytes_(bytes) {}

  void ExpectMagic(const std::array<char, 4> &expected) {
    Require(expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      if (bytes_[offset_ + i] != expected[i]) {
        throw std::invalid_argument("TableMetadataCodec: invalid magic");
      }
    }
    offset_ += expected.size();
  }

  uint32_t U32() {
    Require(sizeof(uint32_t));
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
      value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes_[offset_ + i])) << (i * 8);
    }
    offset_ += sizeof(value);
    return value;
  }

  uint64_t U64() {
    Require(sizeof(uint64_t));
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
      value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes_[offset_ + i])) << (i * 8);
    }
    offset_ += sizeof(value);
    return value;
  }

  std::string String() {
    const uint32_t size = U32();
    Require(size);
    std::string value(bytes_.data() + offset_, size);
    offset_ += size;
    return value;
  }

  std::vector<std::string> Strings() {
    const uint32_t count = U32();
    // Every encoded string needs at least its four-byte length field. This
    // also rejects corrupt counts before attempting a huge allocation.
    if (count > Remaining() / sizeof(uint32_t)) {
      throw std::invalid_argument("TableMetadataCodec: invalid vector length");
    }
    std::vector<std::string> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      values.push_back(String());
    }
    return values;
  }

  void ExpectEnd() const {
    if (offset_ != bytes_.size()) {
      throw std::invalid_argument("TableMetadataCodec: trailing bytes");
    }
  }

 private:
  size_t Remaining() const { return bytes_.size() - offset_; }

  void Require(size_t size) const {
    if (size > Remaining()) {
      throw std::invalid_argument("TableMetadataCodec: truncated input");
    }
  }

  Slice bytes_;
  size_t offset_ = 0;
};

void ValidateCurrentTableState(const CurrentTableState &state) {
  if (state.format_version != CurrentTableState::kFormatVersion) {
    throw std::invalid_argument("TableMetadataCodec: unsupported CurrentTableState format version");
  }
  if (state.table_id.empty()) {
    throw std::invalid_argument("TableMetadataCodec: CurrentTableState table_id is empty");
  }
  if (state.compacted_cursor > state.committed_cursor) {
    throw std::invalid_argument("TableMetadataCodec: compacted_cursor exceeds committed_cursor");
  }
}

void ValidateCompactedDataManifest(const CompactedDataManifest &manifest) {
  if (manifest.format_version != CompactedDataManifest::kFormatVersion) {
    throw std::invalid_argument("TableMetadataCodec: unsupported CompactedDataManifest format version");
  }
  if (manifest.table_id.empty()) {
    throw std::invalid_argument("TableMetadataCodec: CompactedDataManifest table_id is empty");
  }
  if (manifest.compacted_cursor == 0 || manifest.compacted_data_files.empty()) {
    throw std::invalid_argument("TableMetadataCodec: CompactedDataManifest has no compacted data files");
  }
  for (const auto &compacted_data_file : manifest.compacted_data_files) {
    if (compacted_data_file.empty()) {
      throw std::invalid_argument("TableMetadataCodec: CompactedDataManifest has an empty compacted data file");
    }
  }
}

void ValidateCommitRecord(const CommitRecord &record) {
  if (record.format_version != CommitRecord::kFormatVersion) {
    throw std::invalid_argument("TableMetadataCodec: unsupported CommitRecord format version");
  }
  if (record.table_id.empty()) {
    throw std::invalid_argument("TableMetadataCodec: CommitRecord table_id is empty");
  }
  if (record.first_cursor == 0 || record.first_cursor > record.last_cursor) {
    throw std::invalid_argument("TableMetadataCodec: invalid CommitRecord cursor range");
  }
  if (record.wal_files.empty() || record.batch_ids.empty()) {
    throw std::invalid_argument("TableMetadataCodec: CommitRecord has no WAL files or batch ids");
  }
  for (const auto &wal_file : record.wal_files) {
    if (wal_file.empty()) {
      throw std::invalid_argument("TableMetadataCodec: CommitRecord has an empty WAL file");
    }
  }
  for (const auto &batch_id : record.batch_ids) {
    if (batch_id.empty()) {
      throw std::invalid_argument("TableMetadataCodec: CommitRecord has an empty batch id");
    }
  }
}

void WriteHeader(Writer *writer, const std::array<char, 4> &magic) {
  writer->Bytes(magic);
  writer->U32(kCodecVersion);
}

void ReadHeader(Reader *reader, const std::array<char, 4> &magic) {
  reader->ExpectMagic(magic);
  if (reader->U32() != kCodecVersion) {
    throw std::invalid_argument("TableMetadataCodec: unsupported codec version");
  }
}

}  // namespace

std::string TableMetadataCodec::EncodeCurrentTableState(const CurrentTableState &state) {
  ValidateCurrentTableState(state);
  Writer writer;
  WriteHeader(&writer, kCurrentTableStateMagic);
  writer.U32(state.format_version);
  writer.String(state.table_id);
  writer.U64(state.current_state_version);
  writer.U64(state.writer_epoch);
  writer.U64(state.committed_cursor);
  writer.U64(state.compacted_cursor);
  writer.String(state.compacted_data_manifest_key);
  writer.String(state.latest_commit_key);
  return writer.Finish();
}

CurrentTableState TableMetadataCodec::DecodeCurrentTableState(const Slice &bytes) {
  Reader reader(bytes);
  ReadHeader(&reader, kCurrentTableStateMagic);
  CurrentTableState state;
  state.format_version = reader.U32();
  state.table_id = reader.String();
  state.current_state_version = reader.U64();
  state.writer_epoch = reader.U64();
  state.committed_cursor = reader.U64();
  state.compacted_cursor = reader.U64();
  state.compacted_data_manifest_key = reader.String();
  state.latest_commit_key = reader.String();
  reader.ExpectEnd();
  ValidateCurrentTableState(state);
  return state;
}

std::string TableMetadataCodec::EncodeCommitRecord(const CommitRecord &record) {
  ValidateCommitRecord(record);
  Writer writer;
  WriteHeader(&writer, kCommitRecordMagic);
  writer.U32(record.format_version);
  writer.String(record.table_id);
  writer.U64(record.writer_epoch);
  writer.U64(record.first_cursor);
  writer.U64(record.last_cursor);
  writer.String(record.parent_commit_key);
  writer.Strings(record.wal_files);
  writer.Strings(record.batch_ids);
  return writer.Finish();
}

CommitRecord TableMetadataCodec::DecodeCommitRecord(const Slice &bytes) {
  Reader reader(bytes);
  ReadHeader(&reader, kCommitRecordMagic);
  CommitRecord record;
  record.format_version = reader.U32();
  record.table_id = reader.String();
  record.writer_epoch = reader.U64();
  record.first_cursor = reader.U64();
  record.last_cursor = reader.U64();
  record.parent_commit_key = reader.String();
  record.wal_files = reader.Strings();
  record.batch_ids = reader.Strings();
  reader.ExpectEnd();
  ValidateCommitRecord(record);
  return record;
}

std::string TableMetadataCodec::EncodeCompactedDataManifest(const CompactedDataManifest &manifest) {
  ValidateCompactedDataManifest(manifest);
  Writer writer;
  WriteHeader(&writer, kCompactedDataManifestMagic);
  writer.U32(manifest.format_version);
  writer.String(manifest.table_id);
  writer.U64(manifest.compacted_cursor);
  writer.Strings(manifest.compacted_data_files);
  return writer.Finish();
}

CompactedDataManifest TableMetadataCodec::DecodeCompactedDataManifest(const Slice &bytes) {
  Reader reader(bytes);
  ReadHeader(&reader, kCompactedDataManifestMagic);
  CompactedDataManifest manifest;
  manifest.format_version = reader.U32();
  manifest.table_id = reader.String();
  manifest.compacted_cursor = reader.U64();
  manifest.compacted_data_files = reader.Strings();
  reader.ExpectEnd();
  ValidateCompactedDataManifest(manifest);
  return manifest;
}

}  // namespace dbplay
