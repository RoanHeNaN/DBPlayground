//
// T2 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//

#include "Table/RowCodec.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace dbplay {

namespace {

template <typename T>
void AppendPod(std::string *out, T v) {
  out->append(reinterpret_cast<const char *>(&v), sizeof(T));
}

template <typename T>
T ReadPod(const char *p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

// Bytes a fixed-width field occupies in the blob; 0 for variable-length types.
size_t FixedWidth(Type t) {
  switch (t) {
    case Type::Int32:
      return 4;
    case Type::Int64:
      return 8;
    case Type::Float:
      return 4;
    case Type::Double:
      return 8;
    case Type::Bool:
      return 1;
    default:
      return 0;  // String / Blob are variable-length
  }
}

// Guard a read of `need` bytes starting at `off` against a blob of `size`
// bytes. WAL/row blobs come from object storage, so a truncated or crafted
// length prefix must fault here rather than read out of bounds.
void RequireBytes(size_t off, size_t need, size_t size, const char *where) {
  if (off + need < off || off + need > size) {
    throw std::runtime_error(std::string("RowCodec: truncated row blob in ") + where);
  }
}

}  // namespace

std::string RowCodec::Encode(const std::vector<Value> &row) const {
  if (row.size() != schema_.size()) {
    throw std::invalid_argument("RowCodec::Encode: row arity does not match schema");
  }
  std::string out;
  for (size_t i = 0; i < schema_.size(); ++i) {
    const Value &v = row[i];
    switch (schema_[i].type) {
      case Type::Int32:
        AppendPod<int32_t>(&out, v.AsInt32());
        break;
      case Type::Int64:
        AppendPod<int64_t>(&out, v.AsInt64());
        break;
      case Type::Float:
        AppendPod<float>(&out, v.AsFloat());
        break;
      case Type::Double:
        AppendPod<double>(&out, v.AsDouble());
        break;
      case Type::Bool:
        AppendPod<uint8_t>(&out, v.AsBool() ? 1 : 0);
        break;
      case Type::String:
      case Type::Blob: {
        const std::string &b = v.AsString();
        AppendPod<uint32_t>(&out, static_cast<uint32_t>(b.size()));
        out.append(b);
        break;
      }
      default:
        throw std::invalid_argument("RowCodec::Encode: invalid field type");
    }
  }
  return out;
}

std::vector<Value> RowCodec::Decode(const Slice &blob) const {
  const char *p = blob.data();
  const size_t size = blob.size();
  size_t off = 0;
  std::vector<Value> row;
  row.reserve(schema_.size());
  for (const Field &f : schema_) {
    if (const size_t fw = FixedWidth(f.type); fw != 0) RequireBytes(off, fw, size, "Decode");
    switch (f.type) {
      case Type::Int32:
        row.push_back(Value::Int32(ReadPod<int32_t>(p + off)));
        off += 4;
        break;
      case Type::Int64:
        row.push_back(Value::Int64(ReadPod<int64_t>(p + off)));
        off += 8;
        break;
      case Type::Float:
        row.push_back(Value::Float(ReadPod<float>(p + off)));
        off += 4;
        break;
      case Type::Double:
        row.push_back(Value::Double(ReadPod<double>(p + off)));
        off += 8;
        break;
      case Type::Bool:
        row.push_back(Value::Bool(ReadPod<uint8_t>(p + off) != 0));
        off += 1;
        break;
      case Type::String:
      case Type::Blob: {
        RequireBytes(off, 4, size, "Decode");
        uint32_t len = ReadPod<uint32_t>(p + off);
        off += 4;
        RequireBytes(off, len, size, "Decode");
        row.push_back(Value::String(std::string(p + off, len)));
        off += len;
        break;
      }
      default:
        throw std::invalid_argument("RowCodec::Decode: invalid field type");
    }
  }
  return row;
}

void RowCodec::DecodeInto(const Slice &blob, const std::vector<int> &projection, std::vector<Column> *cols) const {
  // schema field index -> output column position (-1 if not projected).
  std::vector<int> out_pos(schema_.size(), -1);
  for (size_t k = 0; k < projection.size(); ++k) {
    out_pos[projection[k]] = static_cast<int>(k);
  }

  const char *p = blob.data();
  const size_t size = blob.size();
  size_t off = 0;
  for (size_t i = 0; i < schema_.size(); ++i) {
    const int k = out_pos[i];
    if (const size_t fw = FixedWidth(schema_[i].type); fw != 0) RequireBytes(off, fw, size, "DecodeInto");
    switch (schema_[i].type) {
      case Type::Int32: {
        int32_t v = ReadPod<int32_t>(p + off);
        off += 4;
        if (k >= 0) (*cols)[k].Append<int32_t>(v);
        break;
      }
      case Type::Int64: {
        int64_t v = ReadPod<int64_t>(p + off);
        off += 8;
        if (k >= 0) (*cols)[k].Append<int64_t>(v);
        break;
      }
      case Type::Float: {
        float v = ReadPod<float>(p + off);
        off += 4;
        if (k >= 0) (*cols)[k].Append<float>(v);
        break;
      }
      case Type::Double: {
        double v = ReadPod<double>(p + off);
        off += 8;
        if (k >= 0) (*cols)[k].Append<double>(v);
        break;
      }
      case Type::Bool: {
        bool v = ReadPod<uint8_t>(p + off) != 0;
        off += 1;
        if (k >= 0) (*cols)[k].Append<bool>(v);
        break;
      }
      case Type::String:
      case Type::Blob: {
        RequireBytes(off, 4, size, "DecodeInto");
        uint32_t len = ReadPod<uint32_t>(p + off);
        off += 4;
        RequireBytes(off, len, size, "DecodeInto");
        if (k >= 0) (*cols)[k].AppendBytes(Slice(p + off, len));
        off += len;
        break;
      }
      default:
        throw std::invalid_argument("RowCodec::DecodeInto: invalid field type");
    }
  }
}

}  // namespace dbplay
