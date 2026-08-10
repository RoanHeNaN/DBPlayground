#include <string>
#include "Common/Slice.h"
#include "gtest/gtest.h"

namespace dbplay {

TEST(SliceTest, Constructor) {

}

TEST(SliceTest, compare) {
    std::string str1("ABCD");
    std::string str2("ABCE");
    Slice slice1(str1);
    Slice slice2(str2);

    ASSERT_TRUE(slice1.compare(slice2) < 0);
}

TEST(SliceTest, compare2) {
    std::string str1("ABCD");
    std::string str2("ABCD   ");
    Slice slice1(str1);
    Slice slice2(str2);

    ASSERT_TRUE(slice1.compare(slice2) == -1);
}
}