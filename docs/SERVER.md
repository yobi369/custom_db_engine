# CustomDB Server (initial local service)

The machine-readable API contract is available in [OpenAPI 3.1 JSON](openapi.json). It is a route/schema reference; conditional authentication and current limitations are described below.

This is the first HTTP server layer around the existing C++ storage engine. It is intentionally a local, single-host service and does not change the embedded engine's on-disk format.

## Build

The server is an optional target. Enabling it requires OpenSSL 3.x (libcrypto) for authentication cryptographic primitives. CMake downloads cpp-httplib when the server option is enabled and the dependency is not otherwise provided by the build environment.

```bash
cmake -S . -B build -DCUSTOM_DB_ENGINE_BUILD_SERVER=ON
cmake --build build --parallel
```

## Run

Create the first administrator locally. This command prompts for a username and hidden password confirmation; it refuses to overwrite an existing auth store:

```bash
CUSTOMDB_DATA_DIR=./customdb-data ./build/custom_db_server --bootstrap-admin
```

Start the local service:

```bash
CUSTOMDB_HOST=127.0.0.1 CUSTOMDB_PORT=7680 CUSTOMDB_DATA_DIR=./customdb-data ./build/custom_db_server
```

Stop with Ctrl+C on a POSIX terminal (SIGINT); service managers may send SIGTERM. On Windows, Ctrl+C or Ctrl+Break in the owning console requests shutdown. The handler only records the signal; a monitor thread calls the HTTP server's stop operation outside the signal handler. The integration test verifies clean process exit followed by reopening the same data directory. Validate the actual service manager's stop behavior separately.

The service binds to loopback by default. If no auth store exists, the server retains its legacy mode: `CUSTOMDB_API_KEY` may protect routes with one shared secret, but a loopback deployment with no key is unauthenticated. Bootstrap the administrator before using the service with real data. Once `CUSTOMDB_DATA_DIR/auth/auth_store.json` exists, bearer-token authentication takes over for every route except `GET /health` and `POST /v1/auth/login`; legacy API-key headers do not replace bearer tokens. Non-loopback binding is refused unless the operator explicitly sets `CUSTOMDB_ALLOW_NON_LOOPBACK=1`. Use TLS termination in a trusted reverse proxy before exposing the service over an untrusted network.

## Configuration and request limits

| Environment variable | Default | Purpose |
|---|---|---|
| `CUSTOMDB_HOST` | `127.0.0.1` | Bind address. Non-loopback binding also requires explicit opt-in below. |
| `CUSTOMDB_PORT` | `7680` | TCP port, from 1 to 65535. |
| `CUSTOMDB_DATA_DIR` | `./customdb-data` | Root for database files, backups, auth metadata, audit log and the exclusive process lock. |
| `CUSTOMDB_API_KEY` | unset | Optional shared key for legacy mode only, when no identity store exists. |
| `CUSTOMDB_ALLOW_NON_LOOPBACK` | `0` | Must be set to `1` to bind beyond loopback. This is not a TLS switch. |

Requests are limited to 1 MiB and have five-second read and write timeouts. Login throttling is five failed attempts per remote address per minute. Bearer tokens currently expire after 24 hours; logout, password changes, password resets and administrator revocation invalidate sessions as documented below.

## Authentication API

- `GET /health` — public readiness alias; checks the configured data directory and filesystem capacity. Use `/health/live` for process liveness.
- `POST /v1/auth/login` — authenticate with JSON `{"username":"owner","password":"..."}`; returns a bearer access token, token ID, expiry timestamp, and non-secret user summary. Store the access token securely; it is returned only at issuance.
- `GET /v1/auth/users` — administrator-only list of accounts and grants; password verifiers and tokens are never returned.
- `POST /v1/auth/users` — administrator-only account creation. Body includes `username`, `password`, `role` (`admin`, `read_write`, or `read_only`), and optional `grants` array such as `[{"database":"example","permission":"write"}]`.
- `PUT /v1/auth/users/{username}/grants` — administrator-only replacement of that account's database grants.
- `PUT /v1/auth/users/{username}/enabled` — administrator-only account disable/re-enable using `{"enabled":false}`. Disabling a user revokes that user's existing tokens; the last enabled administrator cannot be disabled.
- `PUT /v1/auth/users/{username}/password` — administrator-only password reset using `{"password":"a new long password"}`. A password reset revokes all sessions for that account.
- `PUT /v1/auth/password` — authenticated users change their own password using `{"current_password":"...","new_password":"..."}`. All sessions for the account are revoked after a successful change.
- `POST /v1/auth/logout` — authenticated users revoke the bearer token used for the request.
- `POST /v1/auth/tokens/{token_id}/revoke` — administrator-only token revocation.

When identity mode is active, send `Authorization: Bearer <access_token>`. Administrators can manage all databases. Other users need explicit per-database grants. `read` permits GET/HEAD operations; `write` permits reads and mutations. The `read_only` role is never allowed to mutate data, even if a write grant is accidentally assigned. Database listing/creation and identity administration are administrator-only. Login attempts are throttled to five failed attempts per remote address per minute. Password reset revokes all sessions for the affected account, and users can revoke their own current session with logout. The last enabled administrator cannot be disabled.

## Database API

- `GET /health` — liveness response.
- `GET /v1/databases` — list databases.
- `POST /v1/databases` — create a database with JSON body `{"name":"example"}`.
- `PUT /v1/databases/{database}/kv/{key}` — store a string value with JSON body `{"value":"hello"}`.
- `GET /v1/databases/{database}/kv/{key}` — read a string value.
- `DELETE /v1/databases/{database}/kv/{key}` — delete a key.
- `POST /v1/databases/{database}/collections/{collection}/documents` — append a JSON object to a collection.
- `GET /v1/databases/{database}/collections/{collection}/documents` — query documents; URL query parameters are equality filters.
- `PUT /v1/databases/{database}/collections/{collection}/documents?field=value` — replace every matching document with the JSON object in the request body. At least one equality filter is required.
- `DELETE /v1/databases/{database}/collections/{collection}/documents?field=value` — delete every matching document. At least one equality filter is required; an empty filter cannot delete an entire collection.
- `POST /v1/databases/{database}/schemas` — create a collection schema using the engine's supported schema subset.
- `PUT /v1/databases/{database}/indexes` with JSON body `{"field":"email"}` — create an equality index.
- `DELETE /v1/databases/{database}/indexes?field=email` — drop an index.
- `POST /v1/databases/{database}/transactions` — execute an atomic batch of up to 100 operations. Supported operation types are `put`, `delete`, and `insert_document`.
- `GET /v1/databases/{database}/integrity` — verify the currently loaded state against the on-disk snapshot/WAL; returns `200` for a successful check.
- `GET /v1/databases/{database}/backups` — list managed backups for one database.
- `POST /v1/databases/{database}/backups` with JSON body `{"name":"before-upgrade"}` — create a new backup. Backup names are restricted and cannot contain paths; existing backup names are not overwritten.
- `POST /v1/databases/{database}/backups/{backup}/restore` with JSON body `{"confirm":true}` — restore a managed backup. This replaces the active database state and requires explicit confirmation.

Backups are stored beneath `CUSTOMDB_DATA_DIR/backups/{database}/`. The HTTP API does not accept filesystem paths for backup or restore operations.

Database names are restricted to letters, digits, underscore, and hyphen, and must begin with a letter. The server constructs database paths itself; clients cannot supply filesystem paths.

## Concurrency and scope

Only one CustomDB server or bootstrap process may hold a given data directory at a time; an OS-level lock prevents concurrent processes from racing over the auth store or database files. Every engine operation is also serialized by a server-wide mutex and opens one engine handle for the duration of the request. This is deliberate: the current engine's exclusive file lock and transaction state do not provide independent concurrent-client transactions. The initial server does not claim multi-writer throughput, transaction isolation between clients, TLS built into the process, distributed operation, or production readiness.

The current HTTP API supports key-value operations, document insertion, equality-filtered document queries and mutations, schema creation, index management, managed backups, integrity checks, and serialized atomic batches. The automated storage suite now injects failures around snapshot publication, checkpoint WAL cleanup, partial WAL append, abandoned temporary snapshots, backup publication, restore publication, and authentication-store replacement; it also verifies recovery after a separate-process abrupt exit across a WAL checkpoint. These tests do not simulate real power loss, faulty hardware, or every filesystem failure mode. Schema alteration, native TLS, multi-host coordination, load testing, and client SDKs remain future work. The storage test suite does include a separate-process abrupt-exit recovery scenario across a WAL checkpoint. Structured security events are written to `CUSTOMDB_DATA_DIR/audit/security.jsonl`; the log is size-bounded, rotates one prior segment, and uses POSIX mode `0600`. Audit writes are best-effort, not tamper-evident, and do not yet provide a remote collector or a query API. The multi-user authentication design is documented in [AUTHENTICATION_DESIGN.md](AUTHENTICATION_DESIGN.md), with implementation decisions in [AUTH_IMPLEMENTATION_DECISIONS.md](AUTH_IMPLEMENTATION_DECISIONS.md). Bearer login, user creation, account disable/re-enable, password reset, user logout, token revocation, database-scoped grants, and centralized route authorization are implemented on the development branch and covered by HTTP integration tests. This is not yet a production-ready or internet-facing authentication system; use loopback or a trusted TLS reverse proxy and apply operational controls. Keep backups of important data; the underlying engine itself documents its current durability and format limitations.
