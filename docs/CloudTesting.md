# Testing the Cloud Path (存算分离)

How to build and test the cloud storage-compute-separation path, both the
in-memory tests (no dependencies) and the S3 integration tests against a local
MinIO. Design context: `docs/design/CloudImportQueryFramework.md`,
`CloudTableLayering.md`.

## 1. Build

```bash
# One-time configure (Ninja + Debug). Re-run after adding new source/test files.
cmake -S . -B build -G Ninja

# Build everything (library + all test executables).
ninja -C build

# Or build a single test executable:
ninja -C build CloudPathTest_exe
```

Requirements: CMake ≥ 3.20, Ninja, a C++17 compiler, plus **libcurl** and
**OpenSSL** (used by the S3 client). On macOS both are found automatically
(system libcurl, Homebrew `openssl@3`).

## 2. In-memory tests (no MinIO needed)

The cloud protocol is fully exercised on in-memory fakes (`MemStorage`,
`MemMetadataStore`) — these need nothing external and always run.

```bash
# Build + run a single suite (the friendly target builds then runs it):
ninja -C build CloudPathTest             # end-to-end import/query/compaction, in memory
ninja -C build TableMetadataStoreTest    # CURRENT CAS protocol, fencing, publish/rebase
ninja -C build WalFileFormatTest         # WAL codec round-trip + corruption
ninja -C build RowCodecTest              # row encode/decode + truncation guards

# Or run an already-built executable directly:
./build/test/CloudPathTest_exe --gtest_color=yes

# Build + run the ENTIRE suite (S3 tests below auto-skip without MinIO env):
ninja -C build test
```

## 3. S3 integration tests (against local MinIO)

Three suites talk to a real S3-compatible endpoint. They are **env-gated**: with
no `MINIO_ENDPOINT` they `GTEST_SKIP`, so they are safe in the default run.

| Suite | What it checks |
|---|---|
| `S3ClientSmokeTest` | SigV4 signing, conditional writes, range read, HEAD/DELETE/List |
| `S3AdaptersTest` | `S3FileStorage` (IStorage) + `S3MetadataStore` (IMetadataStore) contracts |
| `S3IntegrationTest` | Full CloudTable cycle: import → query → snapshot isolation → compaction → re-import, and second-writer fencing |

### 3.1 Start MinIO

Managed by the dotfiles helper (installs binaries on first `deploy-minio`):

```bash
export PATH="$HOME/code/dotfiles/bin:$PATH"
start-minio                       # API 127.0.0.1:19900, console :19901
# status-minio / stop-minio / restart-minio

# One-time: create the test bucket
~/tools/minio/mc mb -p local/dbplayground
```

Defaults (from `~/workspace/minio/minio.conf`): endpoint `http://127.0.0.1:19900`,
credentials `minioadmin` / `minioadmin`.

### 3.2 Run the S3 tests

Export the connection env, then run the executables:

```bash
export MINIO_ENDPOINT=http://127.0.0.1:19900
export MINIO_ACCESS_KEY=minioadmin
export MINIO_SECRET_KEY=minioadmin
export MINIO_BUCKET=dbplayground
# Optional: MINIO_REGION (default us-east-1)

ninja -C build S3ClientSmokeTest_exe S3AdaptersTest_exe S3IntegrationTest_exe

./build/test/S3ClientSmokeTest_exe  --gtest_color=yes
./build/test/S3AdaptersTest_exe     --gtest_color=yes
./build/test/S3IntegrationTest_exe  --gtest_color=yes
```

With the env exported, `ninja -C build test` also runs these live instead of
skipping them. Each `S3IntegrationTest` case writes under a unique object prefix
(`itest/run-<time>-<pid>-<n>/…`), so reruns never collide on the single-shot
`CURRENT`; objects are left behind for inspection (the `dbplayground` bucket is
disposable — `mc rm --recursive --force local/dbplayground/itest/` to clean up).

## 4. One-liner: everything, live

```bash
export PATH="$HOME/code/dotfiles/bin:$PATH"; start-minio
export MINIO_ENDPOINT=http://127.0.0.1:19900 MINIO_ACCESS_KEY=minioadmin \
       MINIO_SECRET_KEY=minioadmin MINIO_BUCKET=dbplayground
cmake -S . -B build -G Ninja && ninja -C build test
```

## 5. Troubleshooting

- **All S3 tests SKIPPED** — `MINIO_ENDPOINT` is not exported. Expected when
  MinIO is not running.
- **403 / SignatureDoesNotMatch** — wrong `MINIO_ACCESS_KEY`/`SECRET_KEY`, or a
  clock skew large enough to invalidate the SigV4 `x-amz-date`.
- **Connection refused** — MinIO not started, or on a different port; check
  `status-minio` and `~/workspace/minio/minio.conf`.
- **NoSuchBucket** — create it: `~/tools/minio/mc mb -p local/dbplayground`.
- **libc++ `<wchar.h>` build error** — the curl imported target leaked the macOS
  SDK include dir onto every source; the build links `${CURL_LIBRARIES}`
  directly to avoid this (see `src/CMakeLists.txt`).
