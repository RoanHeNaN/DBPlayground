//
// T2 of the columnar-readiness work (docs/design/ColumnarTableSource.md).
//
// Value is a single typed field value -- one cell of a row. Used to build a row
// for RowCodec::Encode and to hand back a decoded row from RowCodec::Decode.
// (A small tagged struct is enough for the playground; no std::variant.)
//

#ifndef DBPLAYGROUND_VALUE_H
#define DBPLAYGROUND_VALUE_H

#include <cstdint>
#include <string>
#include <utility>

#include "Common/Type.h"

namespace dbplay {

struct Value {
  Type type = Type::Invalid;
  int64_t int_val = 0;    // Int32 / Int64 / Bool
  double dbl_val = 0.0;   // Float / Double
  std::string bytes_val;  // String / Blob

  static Value Int32(int32_t v) { return {Type::Int32, v, 0.0, {}}; }
  static Value Int64(int64_t v) { return {Type::Int64, v, 0.0, {}}; }
  static Value Bool(bool v) { return {Type::Bool, v ? 1 : 0, 0.0, {}}; }
  static Value Float(float v) { return {Type::Float, 0, static_cast<double>(v), {}}; }
  static Value Double(double v) { return {Type::Double, 0, v, {}}; }
  static Value String(std::string v) { return {Type::String, 0, 0.0, std::move(v)}; }

  int32_t AsInt32() const { return static_cast<int32_t>(int_val); }
  int64_t AsInt64() const { return int_val; }
  bool AsBool() const { return int_val != 0; }
  float AsFloat() const { return static_cast<float>(dbl_val); }
  double AsDouble() const { return dbl_val; }
  const std::string &AsString() const { return bytes_val; }
};

}  // namespace dbplay

#endif  // DBPLAYGROUND_VALUE_H
