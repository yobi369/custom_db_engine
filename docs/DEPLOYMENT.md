# CustomDB Deployment Guide (single host)

CustomDB's HTTP service is an early-stage, single-host database service. Use it only for development, evaluation, or non-critical workloads until independent security review, recovery testing, and operational controls are complete. Do not expose the HTTP server directly to the public internet.

## 1. Build

Install a C++17 toolchain, CMake, nlohmann/json, and OpenSSL 3.x development files. The optional HTTP server also fetches cpp-httplib during CMake configuration.

```bash
cmake -S . -B build -DCUSTOM_DB_ENGINE_BUILD_SERVER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure
```

## 2. Create a dedicated data directory and bootstrap the administrator

Use a directory owned by the service account and not served by the web server. Keep database files, backups, auth metadata, audit log, and the process lock inside this directory.

```bash
mkdir -p ./customdb-data
chmod 700 ./customdb-data
CUSTOMDB_DATA_DIR=./customdb-data ./build/custom_db_server --bootstrap-admin
```

Bootstrap prompts for the administrator username and password twice. The auth store is created only once; bootstrap refuses to overwrite it. Back up the auth store securely as part of your host-level recovery plan. Passwords should be at least 12 bytes and no more than 1024 bytes.

## 3. Start locally

```bash
CUSTOMDB_HOST=127.0.0.1 \
CUSTOMDB_PORT=7680 \
CUSTOMDB_DATA_DIR=./customdb-data \
./build/custom_db_server
```

In identity mode, login and `GET /health` are the only public routes. Use a bearer token in the `Authorization: Bearer <token>` header. Administrators can create databases and manage users; non-admin accounts require database grants. The default token lifetime is 24 hours.

## 4. Put TLS in front before remote access

Keep CustomDB bound to loopback and configure a trusted reverse proxy with HTTPS certificates. Example Nginx location block (insert into a TLS-enabled server block):

```nginx
location / {
    proxy_pass http://127.0.0.1:7680;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
}
```

This snippet is only a proxy example, not a complete TLS configuration. Configure certificates, renewal, firewalling, and proxy limits on the host. Do not enable non-loopback binding as a substitute for TLS. If you must bind non-loopback, the server requires `CUSTOMDB_ALLOW_NON_LOOPBACK=1`, but transport remains unencrypted.

## 5. Basic API workflow

After login, set the token in a shell variable:

```bash
TOKEN='paste-token-from-login-response'
```

Create a database (administrator):

```bash
curl -sS -X POST http://127.0.0.1:7680/v1/databases \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"name":"example"}'
```

Write and read a key:

```bash
curl -sS -X PUT http://127.0.0.1:7680/v1/databases/example/kv/greeting \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"value":"hello"}'

curl -sS http://127.0.0.1:7680/v1/databases/example/kv/greeting \
  -H "Authorization: Bearer $TOKEN"
```

Prefer a dedicated non-admin user with a minimum database grant for applications. Do not embed administrator credentials or bearer tokens in source code, public repositories, URLs, logs, or frontend bundles.

## 6. Backups and restore

Create a managed backup (administrator):

```bash
curl -sS -X POST http://127.0.0.1:7680/v1/databases/example/backups \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"name":"before-change"}'
```

List backups:

```bash
curl -sS http://127.0.0.1:7680/v1/databases/example/backups \
  -H "Authorization: Bearer $TOKEN"
```

Restore replaces the active database state and requires explicit confirmation. Test restores in a non-production environment first. Managed backups cover database state only; plan separate secure backups for the auth store and host configuration. Keep backup copies off-host and protect them with appropriate access controls.

## 7. Operational checks and limitations

- Check liveness: `GET /health`; it is not a deep dependency or database readiness check.
- Check database integrity: `GET /v1/databases/{database}/integrity` using an authorized token.
- Monitor free disk space and protect the data directory from untrusted access.
- Stop the server before host-level file copies or restores of the data directory.
- A single server process per data directory is enforced; storage operations are serialized.
- In-process transactions are not multi-client isolated transactions; concurrent independent writers and distributed coordination are unsupported.
- Audit logging is bounded, local, best-effort, and not tamper-evident or remotely delivered.
- Windows file ACLs are the operator's responsibility.
- No native TLS, replication, high availability, distributed consensus, or SQL-wire compatibility is provided.

CI passing is a quality gate, not production-readiness certification. Complete threat-model review, independent security review, recovery drills, monitoring, and operational acceptance before using critical or sensitive data.
