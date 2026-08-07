
#pragma once

#include <string>

namespace dbplay {

// Reference: https://stackoverflow.com/a/32821650/9057530
template <typename... Args>
std::string StrFormat(const char *format, Args... args) {
  int length = std::snprintf(nullptr, 0, format, args...);

  char *buf = new char[length + 1];
  std::snprintf(buf, length + 1, format, args...);

  std::string str(buf);
  delete[] buf;
  return str;
}
}  // namespace dbplay
