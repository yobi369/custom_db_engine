# CustomDB Product Specification

**Document status:** Engineering baseline  
**Applies to:** `engineering/product-readiness` and subsequent product branches  
**Current product maturity:** Development/evaluation; not production-certified

## 1. Product promise

CustomDB is a self-hostable, developer-oriented database engine and service with a small operational footprint, a documented API, predictable data handling, and recovery behavior that is tested rather than assumed.

The product must be useful to application developers without requiring them to understand internal storage files or embed C++ code. The embedded C++ library remains a supported integration surface, while the optional server provides a network API for applications.

CustomDB will compete on approachability, portability, transparent guarantees, easy local development, and dependable single-host operation before attempting distributed scale.

## 2. Target users and first supported workloads

### Primary users
- Backend and full-stack developers building small-to-medium applications.
- Teams that want a self-hosted database for development, internal tools, and controlled single-host deployments.
- C++ developers embedding the storage library in a local application.

### Initial workload profile
- Application metadata and configuration.
- Key-value state and JSON document collections.
- Internal tools, prototypes, and non-critical services.
- Local development, test, and staging environments.

### Explicit exclusions until separately validated
- Sole storage for financial, medical, identity, safety-critical, or otherwise sensitive production records.
- Multi-region or multi-host workloads.
- Workloads requiring PostgreSQL/MySQL wire compatibility or full SQL semantics.
- High availability, automatic failover, or distributed transactions.

## 3. Product surfaces

1. **Embedded library:** stable C++ API and documented on-disk compatibility policy.
2. **Database server:** authenticated HTTP API, managed databases, bounded requests, and operational endpoints.
3. **Client SDKs:** typed clients with consistent authentication, timeouts, errors, and retry rules.
4. **Command-line tools:** local server lifecycle, health checks, backup/restore workflows, and diagnostics.
5. **CustomDB Studio:** optional management UI built only on the public API; it must not access database files directly.
6. **Documentation:** install, quick start, API reference, SDK examples, operations, security, migration, and troubleshooting.

## 4. Core product requirements

### Data correctness
- Acknowledged writes must have a precisely documented durability boundary.
- Transaction atomicity and isolation guarantees must be explicit and tested.
- Recovery must never silently accept known-corrupt state.
- Backup and restore must be verifiable and documented with an operator-run recovery drill.
- On-disk formats must be versioned; migrations must be tested against retained fixtures.
- Data deletion, database deletion, and restore operations must have explicit safeguards.

### Query and data model
- Preserve the current key-value and JSON document use cases.
- Introduce typed query predicates, projection, ordering, pagination, and limits through a versioned query contract.
- Support nested JSON paths only with defined escaping, type comparison, and missing/null semantics.
- Query planning and persistent indexes must be driven by representative benchmarks.
- Do not advertise full JSON Schema or SQL compatibility unless the complete relevant standards are implemented and tested.

### Concurrency
- Define the concurrency model before exposing concurrent writes.
- Required design decisions: read/write isolation, transaction boundaries, lock ordering, cancellation, crash behavior, and fairness.
- Stress tests must include concurrent readers/writers, transaction conflicts, process restart, and bounded resource use.
- If the first release remains single-writer, document this plainly and ensure the server rejects unsupported modes safely.

### Security and operations
- Authentication and authorization must be tested for every route and HTTP method.
- Secrets must not appear in logs, errors, examples, or telemetry.
- Rate limits and resource limits must be bounded and documented.
- Remote deployment requires verified TLS, firewalling, least-privilege filesystem permissions, and secure secret delivery.
- Backups must include an explicit strategy for auth metadata and configuration as well as database files.
- Audit retention, disk exhaustion, shutdown, upgrades, and incident response need operator procedures.

### Developer experience
- A clean-machine quick start must work from documented commands.
- Every SDK must include a runnable example and integration tests against a real local server.
- Errors must be structured, stable, and actionable.
- API changes must follow an explicit versioning and deprecation policy.
- Configuration must be discoverable and validate invalid values at startup.
- Installation and upgrade paths must not require editing internal source files.

## 5. Architecture guardrails

- Keep the embedded engine build independent of optional server dependencies.
- Keep the server as an access layer; do not bypass the storage API from SDKs or Studio.
- Do not change the on-disk format without versioning, migration fixtures, rollback strategy, and recovery tests.
- Do not add a query language before defining its grammar, type semantics, parameter binding, limits, and compatibility policy.
- Do not enable multi-writer concurrency while the engine's exclusive file lock and transaction implementation cannot provide the required isolation.
- Avoid introducing a page manager or B+ tree solely for appearance; first capture profiles and baseline measurements.
- Preserve cross-platform CI on Linux, macOS, and Windows for supported configurations.
- Keep `main` unchanged until acceptance gates and review are complete.

## 6. Delivery sequence and exit criteria

### Gate A — establish a trustworthy baseline
- Confirm the current PR foundation and its exact CI head.
- Run a clean-host build and controlled test plan.
- Complete a staging backup/restore drill and retain evidence.
- Review route authorization, filesystem boundaries, shutdown, and deployment assumptions.
- Publish an accurate limitations and support matrix.

**Exit:** repeatable evidence exists for build, tests, recovery, and operator workflow. CI success alone is insufficient.

### Gate B — define and verify the engine contract
- Specify durability, transaction atomicity/isolation, error behavior, and file-format compatibility.
- Add migration fixtures and property/fault tests for every guarantee.
- Decide the supported concurrency model using an architecture decision record.

**Exit:** guarantees are testable and the implementation matches the written contract.

### Gate C — developer-ready query/API surface
- Design versioned query request/response schemas.
- Add filters, projections, sort, pagination, and explicit result limits.
- Add query contract tests and OpenAPI examples.
- Publish compatibility and deprecation policy.

**Exit:** a sample application can use documented API operations without depending on implementation details.

### Gate D — SDK and CLI
- Build TypeScript and Python clients first, then C++ client ergonomics.
- Standardize auth, timeouts, retries, pagination, and typed errors.
- Provide CLI health, database, backup, restore, and diagnostic commands.

**Exit:** SDK integration tests pass against the server and a clean-machine tutorial is reproducible.

### Gate E — measured performance and concurrency
- Establish reproducible workloads and baseline results.
- Profile storage, serialization, indexes, and lock contention.
- Implement only optimizations justified by profiles.
- Add stress, soak, and resource-bound tests.

**Exit:** published measurements include hardware, workload, build mode, dataset size, and methodology; no unsupported performance claims.

### Gate F — Studio and release engineering
- Build Studio against the public API with least-privilege credentials.
- Add signed/checksummed release artifacts where feasible, packaging, upgrade/migration guidance, and release notes.
- Complete security review, recovery drill, operational acceptance, and known-issues publication.

**Exit:** an external developer can install, integrate, operate, back up, restore, upgrade, and troubleshoot the product using public documentation.

## 7. Initial SDK and query-language decisions

- **SDK order:** TypeScript first for common web/backend integration; Python second for automation and data workflows; C++ API ergonomics maintained alongside the core.
- **Query contract:** start with a structured JSON query API rather than a custom textual language. This avoids committing prematurely to a parser and grammar while type, null, path, and index semantics are still being defined.
- **SQL compatibility:** not a goal for the initial product. Do not imply that a JSON query API is SQL.
- **Studio:** consume the same documented HTTP API as external clients; no privileged direct file access.

These are product-direction decisions, not claims that the capabilities already exist.

## 8. Release maturity labels

| Label | Meaning |
|---|---|
| Experimental | API and behavior may change; disposable data only. |
| Developer preview | Documented install and API; controlled non-critical use; known gaps published. |
| Release candidate | Compatibility, security, recovery, packaging, and operational gates have evidence and are under review. |
| Stable | Supported guarantees are tested, release artifacts and upgrade policy exist, and independent security/operational review findings are addressed. |

A green CI run does not promote a release maturity label by itself.

## 9. Definition of done

A product change is done only when all applicable items are complete:
- Implementation and code review.
- Unit, integration, regression, and relevant fault/stress tests.
- Cross-platform CI where applicable.
- API/schema and compatibility updates.
- User and operator documentation.
- Security and resource-boundary review.
- Evidence for any new performance, reliability, or compatibility claim.
- Known limitations updated honestly.

## 10. Current baseline

At the time this specification was written, the server foundation is on the `server-foundation` branch in draft PR #21. It provides an optional HTTP server, named databases, key-value/document endpoints, authentication, database-scoped grants, backups, restore, integrity checks, and serialized engine access. It does **not** yet provide independent multi-client transaction isolation, persistent index structures, a query planner, SDKs, Studio, native TLS, replication, or high availability. The documented controlled-test and security-acceptance gates remain authoritative until evidence is recorded.
