//
// Contract tests for S3FileStorage (IStorage) and S3MetadataStore
// (IMetadataStore) against a live MinIO. Env-gated via MINIO_ENDPOINT.
//

#include <cstdlib>
#include <memory>
#include <string>

#include "Cloud/S3/S3Client.h"
#include "Cloud/S3/S3FileStorage.h"
#include "Cloud/S3/S3MetadataStore.h"
#include "Common/Slice.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

const char *EnvOr(const char *name, const char *fallback) {
  const char *v = std::getenv(name);
  return (v != nullptr && *v != 0) ? v : fallback;
}

class S3AdaptersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *endpoint = std::getenv("MINIO_ENDPOINT");
    if (endpoint == nullptr || *endpoint == 0) {
      GTEST_SKIP() << "MINIO_ENDPOINT not set; skipping live S3 adapter test";
    }
    s3::S3Config cfg;
    cfg.endpoint = endpoint;
    cfg.region = EnvOr("MINIO_REGION", "us-east-1");
    cfg.access_key = EnvOr("MINIO_ACCESS_KEY", "minioadmin");
    cfg.secret_key = EnvOr("MINIO_SECRET_KEY", "minioadmin");
    bucket_ = EnvOr("MINIO_BUCKET", "dbplayground");
    client_ = std::make_shared<s3::S3Client>(cfg);
  }

  std::string bucket_;
  std::shared_ptr<s3::S3Client> client_;
};

TEST_F(S3AdaptersTest, FileStorageWriteReadListDelete) {
  S3FileStorage storage(client_, bucket_);
  const std::string path = "adapters/file/blob.bin";
  storage.Delete(path);

  // Write via OpenOutput + Close, then read back a byte range.
  const std::string payload = "the-quick-brown-fox-jumps";
  {
    auto out = storage.OpenOutput(path);
    out->Append(Slice(payload.substr(0, 10)));
    out->Append(Slice(payload.substr(10)));
    out->Close();
  }
  EXPECT_TRUE(storage.Exists(path));

  auto in = storage.OpenInput(path);
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(in->Size(), payload.size());
  std::string chunk;
  ASSERT_TRUE(in->ReadAt(4, 5, &chunk));  // "quick"
  EXPECT_EQ(chunk, "quick");
  EXPECT_FALSE(in->ReadAt(payload.size(), 1, &chunk));  // out of range

  // List sees it; missing input is nullptr; delete removes it.
  const auto keys = storage.List("adapters/file/");
  EXPECT_NE(std::find(keys.begin(), keys.end(), path), keys.end());
  EXPECT_EQ(storage.OpenInput("adapters/file/nope.bin"), nullptr);
  storage.Delete(path);
  EXPECT_FALSE(storage.Exists(path));
}

TEST_F(S3AdaptersTest, MetadataStoreCasProtocol) {
  S3MetadataStore meta(client_, bucket_);
  const std::string key = "adapters/meta/CURRENT";
  // Ensure a clean slot (delete underlying object directly).
  client_->Delete(bucket_ + "/" + key);

  // Missing key -> nullopt.
  EXPECT_FALSE(meta.Get(key).has_value());

  // PutIfAbsent succeeds once and yields a version.
  MetadataVersion v1;
  EXPECT_EQ(meta.PutIfAbsent(key, Slice("state-1"), &v1), ConditionalWriteResult::Applied);
  EXPECT_FALSE(v1.opaque().empty());

  // Second PutIfAbsent fails the precondition.
  EXPECT_EQ(meta.PutIfAbsent(key, Slice("state-x"), nullptr), ConditionalWriteResult::PreconditionFailed);

  // Get returns the value and the current version.
  auto got = meta.Get(key);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->value, "state-1");
  EXPECT_EQ(got->version, v1);

  // CAS with the right version applies and advances the version.
  MetadataVersion v2;
  EXPECT_EQ(meta.CompareExchange(key, v1, Slice("state-2"), &v2), ConditionalWriteResult::Applied);
  EXPECT_NE(v2, v1);

  // CAS with the now-stale version fails.
  EXPECT_EQ(meta.CompareExchange(key, v1, Slice("state-3"), nullptr), ConditionalWriteResult::PreconditionFailed);

  // The winning value is visible.
  got = meta.Get(key);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->value, "state-2");

  client_->Delete(bucket_ + "/" + key);
}

}  // namespace
}  // namespace dbplay
