#include <string>

#include "Common/Slice.h"
#include "gtest/gtest.h"

namespace dbplay {

// A default-constructed Slice is empty and has zero size.
TEST(SliceTest, DefaultConstructor) {
  Slice s;
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(0U, s.size());
}

// Constructing from an explicit (data, size) keeps the given length even when
// the buffer contains embedded NUL bytes -- Slice is a byte view, not a C string.
TEST(SliceTest, ConstructFromDataAndSize) {
  const char buf[] = {'a', '\0', 'b'};
  Slice s(buf, sizeof(buf));
  EXPECT_FALSE(s.empty());
  EXPECT_EQ(3U, s.size());
  EXPECT_EQ('a', s[0]);
  EXPECT_EQ('\0', s[1]);
  EXPECT_EQ('b', s[2]);
}

// Constructing from std::string views the string's bytes and length.
TEST(SliceTest, ConstructFromString) {
  std::string str("hello");
  Slice s(str);
  EXPECT_EQ(str.size(), s.size());
  EXPECT_EQ(str, s.ToString());
}

// Constructing from a C string uses strlen for the size.
TEST(SliceTest, ConstructFromCString) {
  Slice s("hello");
  EXPECT_EQ(5U, s.size());
  EXPECT_EQ("hello", s.ToString());
}

// ToString round-trips the viewed bytes, embedded NULs included.
TEST(SliceTest, ToString) {
  const char buf[] = {'x', '\0', 'y'};
  Slice s(buf, sizeof(buf));
  EXPECT_EQ(std::string(buf, sizeof(buf)), s.ToString());
}

// compare returns <0, 0, >0 for less/equal/greater at the first differing byte.
TEST(SliceTest, CompareByContent) {
  EXPECT_LT(Slice("ABCD").compare(Slice("ABCE")), 0);
  EXPECT_GT(Slice("ABCE").compare(Slice("ABCD")), 0);
  EXPECT_EQ(0, Slice("ABCD").compare(Slice("ABCD")));
}

// When one slice is a prefix of the other, the shorter one is smaller.
TEST(SliceTest, ComparePrefixIsSmaller) {
  EXPECT_EQ(-1, Slice("ABCD").compare(Slice("ABCD   ")));
  EXPECT_EQ(1, Slice("ABCD   ").compare(Slice("ABCD")));
}

// An empty slice is smaller than any non-empty slice and equal to another empty.
TEST(SliceTest, CompareEmpty) {
  EXPECT_LT(Slice().compare(Slice("A")), 0);
  EXPECT_EQ(0, Slice().compare(Slice()));
}

// compare treats the data as raw bytes, so embedded NULs do not terminate it.
TEST(SliceTest, CompareRespectsEmbeddedNul) {
  const char a[] = {'a', '\0', 'a'};
  const char b[] = {'a', '\0', 'b'};
  EXPECT_LT(Slice(a, sizeof(a)).compare(Slice(b, sizeof(b))), 0);
}

// operator== / operator!= compare by length and bytes, not pointer identity.
TEST(SliceTest, Equality) {
  std::string lhs("data");
  std::string rhs("data");
  EXPECT_TRUE(Slice(lhs) == Slice(rhs));
  EXPECT_FALSE(Slice(lhs) != Slice(rhs));

  EXPECT_TRUE(Slice("data") != Slice("datum"));
  // Same prefix but different length must not be equal.
  EXPECT_TRUE(Slice("data") != Slice("data!"));
}

}  // namespace dbplay
