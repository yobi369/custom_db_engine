# CustomDB Product Roadmap

## Product direction

CustomDB is being developed as a reusable database service and engine that can eventually support multiple independent applications. The embedded C++17 engine remains the storage core; the optional HTTP service is a separate access layer. Applications must connect through documented APIs and credentials, not share internal database files.

The immediate deployment target is a single host with local persistent storage. Multi-host clustering, distributed consensus, and high-availability replication are not current capabilities or near-term claims.

## Current implementation

### Storage engine
- Persistent key-value storage in versioned JSON database files.
- WAL v2 JSON Patch records and legacy v1 record compatibility.
- Snapshot checkpoints, integrity checks, backup and restore.
- JSON document collections, equality filters, subset schema validation.
- Configurable lazy in-memory equality indexes.
- In-process transactions with commit/rollback.
- Exclusive OS file locking; concurrent independent database writers are not supported.
- No page-oriented storage engine, full JSON Schema, query planner, or persistent index structures.

### Local HTTP server
- Optional C++17 HTTP server target.
- Named database files managed under a configured data directory.
- Key-value and document endpoints, schema creation, index management, transactions, integrity checks, backups and restore.
- Request body limits, timeouts, database-name validation, managed file paths, loopback default.
- Global engine-operation mutex to respect current engine locking and transaction semantics.
- One server/bootstrap process per data directory.

### Identity and security
- One-time local administrator bootstrap with hidden password entry.
- PBKDF2-HMAC-SHA-256 password verifiers via OpenSSL 3.x.
- Bearer tokens stored as digests, with expiry and revocation.
- Administrator, read-write, and read-only roles with database-scoped grants.
- Central pre-routing authentication and authorization.
- Login throttling and password-change/session-revocation behavior.
- Bounded local JSONL security events for authentication and account-management actions.
- POSIX auth/audit file permissions; Windows ACLs remain an operator responsibility.

## Roadmap and acceptance gates

### 1. Stabilize current service foundation
- Keep the embedded library build independent of the optional server dependencies.
- Pass Linux, macOS and Windows Debug/Release CI.
- Exercise server routes, restart persistence, auth lifecycle, malformed inputs, lock contention and backup/restore.
- Fix platform-specific test failures before expanding features.
- Maintain accurate API documentation and known-limitations list.

**Exit gate:** all supported CI jobs pass on the same branch commit; integration tests pass on each supported OS.

### 2. Storage correctness and recovery
- Define and test WAL replay, checkpoint ordering and crash recovery.
- Add fault injection around writes, snapshots, backup, restore and atomic auth-store replacement. Automated tests now cover interrupted snapshot publication during both checkpoint writes and restore, failed checkpoint WAL cleanup, partial WAL append, abandoned temporary snapshots, and a separate-process abrupt exit across the 64-record checkpoint boundary. Real power-loss and hardware/filesystem failure simulation remain future work.
- Specify durability guarantees and filesystem assumptions.
- Add format migration/version compatibility tests and recovery tooling.
- Revisit storage format only with migration tests and compatibility policy.

**Exit gate:** repeatable crash/fault tests demonstrate documented recovery behavior without silent corruption.

### 3. Server and API completeness
- Add database lifecycle administration with explicit safeguards.
- Add document read/update/delete semantics and schema alteration only after atomicity and validation tests.
- Publish an API versioning and compatibility policy.
- Add generated OpenAPI documentation and request/response contract tests.
- Improve graceful shutdown and operational health/readiness signals.

**Exit gate:** complete API contract, route coverage, compatibility tests and safe lifecycle operations.

### 4. Security hardening
- Add structured audit coverage for security-sensitive operations and tests proving secrets never enter logs.
- Define audit retention, export and tamper-resistance options.
- Add rate limits and abuse controls beyond the current in-memory login throttle.
- Evaluate TLS termination and native TLS options; never expose cleartext credentials on untrusted networks.
- Review Windows ACL handling and secure file creation.
- Commission independent security review before handling sensitive production data.

**Exit gate:** documented threat model, tested controls, deployment hardening guide and independent review findings addressed.

### 5. Performance and concurrency
- Establish reproducible benchmarks for read/write throughput, latency, WAL growth and recovery time.
- Profile lock contention and storage hot paths.
- Design a storage concurrency model before enabling concurrent writers.
- Consider connection pooling, bounded request queues and per-database coordination only after engine semantics support them.
- Do not claim concurrent transaction isolation until implemented and tested.

**Exit gate:** measured performance baseline and a verified concurrency model with stress tests.

### 6. Client integration and packaging
- Provide a small documented HTTP client/SDK and examples.
- Add environment-based configuration and deployment examples for local Linux first.
- Provide backup/restore runbooks, monitoring hooks and upgrade instructions.
- Keep the first deployment self-hosted and low-cost.
- Integrate other applications only after authentication, API stability, backup, restore and recovery gates are met.

**Exit gate:** a sample application can create a database, authenticate, perform authorized CRUD, back up, restore and verify integrity using only documented interfaces.

## Non-goals until separately designed

- Distributed consensus or multi-region replication.
- Automatic high availability or failover.
- Multi-host shared filesystem access.
- Full PostgreSQL/MySQL wire-protocol compatibility.
- Full SQL or a relational query planner.
- Production-readiness claims based solely on CI success.

## Product rule

A feature is complete only when implementation, tests, documentation, operational behavior and known limitations agree. A passing build alone is not a production-readiness certification.
