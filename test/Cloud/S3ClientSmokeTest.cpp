//
// Smoke test for the S3 client + SigV4 signer against a live S3-compatible
// endpoint (MinIO locally). Env-gated: skips unless MINIO_ENDPOINT is set.
//
//   MINIO_ENDPOINT=http://127.0.0.1:19900 MINIO_ACCESS_KEY=minioadmin \
//   MINIO_SECRET_KEY=minioadmin MINIO_BUCKET=dbplayground ./S3ClientSmokeTest_exe
//
// If signing were wrong every request would come back 403, so a green run is
// an authoritative check of the SigV4 implementation.
//

#include <cstdlib>
#include <string>

#include "Cloud/S3/S3Client.h"
#include "Common/Slice.h"
#include "gtest/gtest.h"

namespace dbplay {
namespace {

const char *EnvOr(const char *name, const char *fallback) {
  const char *v = std::getenv(name);
  return (v != nullptr && *v != 0) ? v : fallback;
}

class S3ClientSmokeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *endpoint = std::getenv("MINIO_ENDPOINT");
    if (endpoint == nullptr || *endpoint == 0) {
      GTEST_SKIP() << "MINIO_ENDPOINT not set; skipping live S3 smoke test";
    }
    s3::S3Config cfg;
    cfg.endpoint = endpoint;
    cfg.region = EnvOr("MINIO_REGION", "us-east-1");
    cfg.access_key = EnvOr("MINIO_ACCESS_KEY", "minioadmin");
    cfg.secret_key = EnvOr("MINIO_SECRET_KEY", "minioadmin");
    bucket_ = EnvOr("MINIO_BUCKET", "dbplayground");
    client_ = std::make_unique<s3::S3Client>(cfg);
  }

  std::string Key(const std::string &name) const { return bucket_ + "/smoke/" + name; }

  std::string bucket_;
  std::unique_ptr<s3::S3Client> client_;
};

TEST_F(S3ClientSmokeTest, ConditionalWriteAndReadRoundTrip) {
  const std::string key = Key("cas-object.bin");
  client_->Delete(key);  // start clean

  // PutIfAbsent on an absent key -> success.
  const std::string v1 = "hello-cloud-v1";
  auto r = client_->Put(key, Slice(v1), s3::PutCondition::IfNoneMatchStar);
  ASSERT_FALSE(r.transport_error) << r.body;
  EXPECT_EQ(r.status, 200) << r.body;

  // PutIfAbsent on the now-present key -> 412 Precondition Failed.
  r = client_->Put(key, Slice("v2"), s3::PutCondition::IfNoneMatchStar);
  EXPECT_EQ(r.status, 412);

  // GET returns the first value and an ETag.
  r = client_->Get(key);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, v1);
  ASSERT_FALSE(r.etag.empty());
  const std::string etag = r.etag;

  // CAS with the correct ETag -> success.
  const std::string v3 = "hello-cloud-v3-longer";
  r = client_->Put(key, Slice(v3), s3::PutCondition::IfMatch, etag);
  EXPECT_EQ(r.status, 200) << r.body;

  // CAS with the now-stale ETag -> 412.
  r = client_->Put(key, Slice("v4"), s3::PutCondition::IfMatch, etag);
  EXPECT_EQ(r.status, 412);

  // HEAD reports the object exists.
  r = client_->Head(key);
  EXPECT_EQ(r.status, 200);

  // Range read of the current value.
  r = client_->GetRange(key, 6, 5);  // "cloud" within "hello-cloud-v3-longer"
  EXPECT_EQ(r.status, 206);
  EXPECT_EQ(r.body, "cloud");

  // Delete, then GET -> 404.
  r = client_->Delete(key);
  EXPECT_TRUE(r.status == 204 || r.status == 200) << r.status;
  r = client_->Get(key);
  EXPECT_EQ(r.status, 404);
}

TEST_F(S3ClientSmokeTest, GetMissingKeyIs404) {
  auto r = client_->Get(Key("definitely-absent-object"));
  EXPECT_EQ(r.status, 404);
}

TEST_F(S3ClientSmokeTest, ListV2ReturnsWrittenKeys) {
  const std::string a = Key("list/a.txt");
  const std::string b = Key("list/b.txt");
  client_->Put(a, Slice("a"));
  client_->Put(b, Slice("b"));
  auto r = client_->ListV2(bucket_, "smoke/list/");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("smoke/list/a.txt"), std::string::npos) << r.body;
  EXPECT_NE(r.body.find("smoke/list/b.txt"), std::string::npos);
  client_->Delete(a);
  client_->Delete(b);
}

}  // namespace
}  // namespace dbplay
