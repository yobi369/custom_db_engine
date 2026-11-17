# CustomDB Server (initial local service)

This is the first HTTP server layer around the existing C++ storage engine. It is intentionally a local, single-host service and does not change the embedded engine's on-disk format.

## Build

The server is an optional target. Enabling it requires OpenSSL 3.x (libcrypto) for authentication cryptographic primitives. CMake downloads cpp-httplib when the server option is enabled and the dependency is not otherwise provided by the build environment.

```bash
cmake -S . -B build -DCUSTOM_DB_ENGINE_BUILD_SERVER=ON
cmake --build build --parallel
```

## Run

```bash
CUSTOMDB_HOST=127.0.0.1 CUSTOMDB_PORT=7680 CUSTOMDB_DATA_DIR=./customdb-data ./build/custom_db_server
```

The service binds to loopback by default. Binding to a non-loopback interface is refused unless `CUSTOMDB_API_KEY` is set. When configured, send the key in the `X-CustomDB-Key` header. This initial API-key mechanism is a single shared secret, not a user/role system. Use TLS termination in a trusted reverse proxy before exposing the service over an untrusted network.

## Initial API

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

Every engine operation is serialized by a server-wide mutex and opens one engine handle for the duration of the request. This is deliberate: the current engine's exclusive file lock and transaction state do not provide independent concurrent-client transactions. The initial server does not claim multi-writer throughput, transaction isolation between clients, TLS built into the process, distributed operation, or production readiness.

The current HTTP API supports key-value operations, document insertion, equality-filtered document queries and mutations, schema creation, index management, managed backups, integrity checks, and serialized atomic batches. Schema updates, per-client identities and permissions, robust audit logging, native TLS, load/fault testing, and SDKs remain planned work. The multi-user authentication design is documented in [AUTHENTICATION_DESIGN.md](AUTHENTICATION_DESIGN.md), with implementation decisions in [AUTH_IMPLEMENTATION_DECISIONS.md](AUTH_IMPLEMENTATION_DECISIONS.md). OpenSSL-backed password hashing and bearer-token primitives are now implemented and unit-tested, but are not yet wired into HTTP routes or identity storage. The shared API-key behavior remains active; do not treat these primitives as a complete authentication system. Keep backups of important data; the underlying engine itself documents its current durability and format limitations.
