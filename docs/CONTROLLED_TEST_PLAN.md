# Controlled Local Test Plan

This plan is for local development and non-critical evaluation of the CustomDB server. Use a disposable data directory and test data only. It is not a production deployment or independent security certification.

## 1. Prerequisites

- C++17 compiler and standard library
- CMake 3.16 or newer
- OpenSSL 3.x development package
- Python 3 (used by the integration tests)
- Git and network access for CMake to fetch cpp-httplib if it is not already available

On Ubuntu/Zorin OS, install the toolchain with:

```bash
sudo apt update
sudo apt install -y build-essential cmake git libssl-dev python3
```

## 2. Configure, build, and run the automated suite

From the repository root:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCUSTOM_DB_ENGINE_BUILD_SERVER=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Expected result: all CTest cases pass. The suite includes storage recovery/fault-injection tests, authentication and authorization integration, HTTP integration, graceful shutdown/restart, OpenAPI contract validation, and benchmark smoke checks.

For a Release build, use a separate directory:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCUSTOM_DB_ENGINE_BUILD_SERVER=ON
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
```

## 3. Create an isolated test data directory

On Linux/macOS:

```bash
mkdir -p ./customdb-test-data
chmod 700 ./customdb-test-data
CUSTOMDB_DATA_DIR=./customdb-test-data ./build/custom_db_server --bootstrap-admin
```

Choose a test administrator username and password when prompted. Use a unique test password; do not reuse a personal or production credential.

The bootstrap command is one-time for a data directory. Do not point this test at an existing data directory containing information you need.

## 4. Start the local server

In a second terminal:

```bash
CUSTOMDB_HOST=127.0.0.1 \
CUSTOMDB_PORT=7680 \
CUSTOMDB_DATA_DIR=./customdb-test-data \
./build/custom_db_server
```

Keep the service bound to loopback. Do not enable non-loopback binding for this test.

Check liveness and readiness:

```bash
curl -i http://127.0.0.1:7680/health/live
curl -i http://127.0.0.1:7680/health/ready
```

Expected results: HTTP 200 and JSON status `alive` / `ready`.

## 5. Exercise authentication and core database operations

Login with the administrator account created during bootstrap. Replace the username and password values:

```bash
curl -sS -X POST http://127.0.0.1:7680/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"testadmin","password":"REPLACE_WITH_TEST_PASSWORD"}'
```

Copy the returned `access_token` into a shell variable:

```bash
TOKEN='paste-the-test-access-token-here'
```

Verify that an unauthenticated database request is rejected:

```bash
curl -i http://127.0.0.1:7680/v1/databases
```

Expected result: HTTP 401.

Create a disposable database:

```bash
curl -i -X POST http://127.0.0.1:7680/v1/databases \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"name":"smoke"}'
```

Write and read a key:

```bash
curl -i -X PUT http://127.0.0.1:7680/v1/databases/smoke/kv/greeting \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"value":"hello-customdb"}'

curl -i http://127.0.0.1:7680/v1/databases/smoke/kv/greeting \
  -H "Authorization: Bearer $TOKEN"
```

Expected result: the read returns `hello-customdb`.

Check database integrity:

```bash
curl -i http://127.0.0.1:7680/v1/databases/smoke/integrity \
  -H "Authorization: Bearer $TOKEN"
```

Expected result: HTTP 200 and status `ok`.

## 6. Exercise backup and restore

Create a managed backup:

```bash
curl -i -X POST http://127.0.0.1:7680/v1/databases/smoke/backups \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"name":"before-test-restore"}'
```

Change the test value:

```bash
curl -i -X PUT http://127.0.0.1:7680/v1/databases/smoke/kv/greeting \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"value":"changed"}'
```

Restore the backup (this replaces the active state of the disposable test database):

```bash
curl -i -X POST http://127.0.0.1:7680/v1/databases/smoke/backups/before-test-restore/restore \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"confirm":true}'
```

Read the key again. It should return `hello-customdb`. Run the integrity endpoint again and confirm status `ok`.

## 7. Verify graceful shutdown and persistence

- On Linux/macOS, press Ctrl+C in the server terminal or send SIGTERM to the server process.
- On Windows, use Ctrl+C or Ctrl+Break in the owning console.
- Wait for the process to exit before copying, moving, or restoring data files.
- Start the server again with the same test data directory.
- Log in again and read the key; it should still return `hello-customdb`.
- Run the integrity endpoint again.

The automated integration test exercises signal-driven shutdown and reopening on the supported CI platforms. Still verify the stop behavior of any service manager or wrapper you intend to use.

## 8. Cleanup

Stop the server, then remove only the disposable test directory when you no longer need its contents:

```bash
rm -rf ./customdb-test-data
```

## 9. Test boundary

A successful local run establishes a useful development/evaluation baseline only. Before controlled external testing, complete the remaining items in [SECURITY_ACCEPTANCE.md](SECURITY_ACCEPTANCE.md), including the staging recovery drill, deployed API security review, target-host permissions, reverse-proxy TLS/firewall review, operational shutdown verification, and repeatable workload benchmarks.

Do not use this early-stage single-host service as the sole store for critical or sensitive production data.
