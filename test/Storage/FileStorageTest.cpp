//
// C1 acceptance (docs/design/StorageAbstraction.md): the byte-range IStorage
// seam. One contract runs against both MemStorage and LocalStorage, proving the
// medium is swappable with no change to the caller.
//

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Storage/File/LocalStorage.h"
#include "Storage/File/MemStorage.h"
#include "gtest/gtest.h"

namespace fs = std::filesystem;

namespace dbplay {

namespace {

// Write `bytes` to `path` in one Append + Close.
void WriteFile(IStorage &s, const std::string &path, const std::string &bytes) {
  auto out = s.OpenOutput(path);
  out->Append(Slice(bytes));
  out->Close();
}

// The behavior every IStorage must satisfy, independent of medium.
void RunStorageContract(IStorage &s) {
  const std::string body = "0123456789ABCDEF";  // 16 bytes

  // Missing file: OpenInput -> nullptr, Exists -> false.
  EXPECT_FALSE(s.Exists("missing"));
  EXPECT_EQ(s.OpenInput("missing"), nullptr);

  // Write then read back the whole file; Size() is exact.
  WriteFile(s, "a.dat", body);
  EXPECT_TRUE(s.Exists("a.dat"));
  auto in = s.OpenInput("a.dat");
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(in->Size(), body.size());

  std::string got;
  ASSERT_TRUE(in->ReadAt(0, body.size(), &got));
  EXPECT_EQ(got, body);

  // Byte-range reads at assorted offsets/lengths.
  ASSERT_TRUE(in->ReadAt(4, 6, &got));
  EXPECT_EQ(got, "456789");
  ASSERT_TRUE(in->ReadAt(10, 6, &got));
  EXPECT_EQ(got, "ABCDEF");
  ASSERT_TRUE(in->ReadAt(16, 0, &got));  // empty read at EOF is valid
  EXPECT_EQ(got, "");

  // Out-of-range reads return false (no throw).
  EXPECT_FALSE(in->ReadAt(10, 7, &got));   // runs past EOF
  EXPECT_FALSE(in->ReadAt(17, 1, &got));   // offset past EOF

  // An input file is a snapshot: rewriting the path doesn't disturb it.
  WriteFile(s, "a.dat", "XY");
  ASSERT_TRUE(in->ReadAt(0, body.size(), &got));
  EXPECT_EQ(got, body);
  // ...but a fresh open sees the overwrite (truncation, not append).
  auto in2 = s.OpenInput("a.dat");
  ASSERT_NE(in2, nullptr);
  EXPECT_EQ(in2->Size(), 2u);
  ASSERT_TRUE(in2->ReadAt(0, 2, &got));
  EXPECT_EQ(got, "XY");

  // Multiple appends concatenate.
  {
    auto out = s.OpenOutput("b.dat");
    out->Append(Slice("foo"));
    out->Append(Slice("bar"));
    out->Close();
  }
  auto inb = s.OpenInput("b.dat");
  ASSERT_NE(inb, nullptr);
  ASSERT_TRUE(inb->ReadAt(0, 6, &got));
  EXPECT_EQ(got, "foobar");

  // List by prefix.
  WriteFile(s, "dir/c.dat", "z");
  std::vector<std::string> listed = s.List("dir/");
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed[0], "dir/c.dat");
  EXPECT_GE(s.List("").size(), 3u);  // a.dat, b.dat, dir/c.dat

  // Delete.
  s.Delete("b.dat");
  EXPECT_FALSE(s.Exists("b.dat"));
  EXPECT_EQ(s.OpenInput("b.dat"), nullptr);
  s.Delete("b.dat");  // deleting an absent path is a no-op
}

}  // namespace

TEST(FileStorageTest, MemStorageContract) {
  MemStorage s;
  RunStorageContract(s);
}

TEST(FileStorageTest, LocalStorageContract) {
  const std::string root = "filestorage_test_dir";
  fs::remove_all(root);
  {
    LocalStorage s(root);
    RunStorageContract(s);
  }
  fs::remove_all(root);
}

}  // namespace dbplay
