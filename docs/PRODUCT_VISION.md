# CustomDB product vision

## Product direction

CustomDB is a lightweight, embeddable C++ data engine for applications that need local persistent key-value data and JSON documents without operating a separate database server.

The product promise is deliberately narrow: **simple integration, explicit storage behavior, portable builds, and inspectable failure modes**. Reliability comes before feature count.

## Target users

- C++ desktop and command-line application developers.
- Edge and local-first applications that own their data directory.
- Prototypes and internal tools that need persistence without a database service.
- Developers evaluating embedded storage trade-offs.

## Not the target (yet)

CustomDB is not currently positioned as a replacement for PostgreSQL, MySQL, SQLite, RocksDB, or a distributed database. It does not claim SQL compatibility, distributed replication, multi-writer concurrency, full JSON Schema, or enterprise availability.

## Current product shape

The repository currently provides:
- C++17 library and CMake package export.
- String key-value operations and JSON-object collections.
- A write-ahead log, versioned JSON snapshots, checkpointing, and integrity checks.
- Single-instance transactions and exclusive database-file locking.
- Flat subset schema validation and in-memory equality indexes.
- Backup/restore APIs, tests, CI, sanitizers, and a benchmark executable.

These are repository-documented capabilities, not a substitute for running the current build and CI before a release.

## Product principles

1. **Data integrity over speed claims.** No performance number without a reproducible workload and machine context.
2. **Fail closed.** Corrupt or unsupported persisted state must not silently become an empty database or be overwritten.
3. **Document guarantees precisely.** Distinguish process-crash recovery, OS flushes, filesystem behavior, and hardware power-loss guarantees.
4. **Small, composable API.** Keep the core embeddable and avoid requiring a hosted service.
5. **Portable by design.** Build and test on Linux, macOS, and Windows before promising support.
6. **No misleading feature labels.** An in-memory equality index is not a persistent index; a transaction is not a multi-client isolation guarantee.
7. **Safe migrations.** Every on-disk format change needs compatibility tests and a documented recovery path.
8. **Open development.** Keep decisions, known limitations, benchmark methodology, and release evidence in the repository.

## Product roadmap

### Phase 1 — Release foundation
- Keep CI green on supported operating systems and build configurations.
- Add installed-package consumer checks and verify install instructions.
- Expand malformed-input, WAL-tail, snapshot-publication, backup/restore, and reopen regression coverage.
- Define a versioned compatibility policy and release checklist.
- Publish benchmark methodology and baseline artifacts without unsupported capacity claims.

### Phase 2 — Operational confidence
- Add deterministic fault-injection hooks in test builds for write, flush, rename, and checkpoint failure paths.
- Add fuzz targets for snapshot and WAL parsing.
- Add repeatable long-running randomized CRUD/transaction/reopen tests.
- Document locking, filesystem assumptions, backup procedures, and recovery steps.
- Review path handling and permissions across supported platforms.

### Phase 3 — Product usability
- Introduce a typed result/error API so missing values are distinguishable from empty strings without relying on mutable last-error state.
- Add examples for key-value data, JSON documents, transactions, and backup/restore.
- Add a small compatibility-tested public API surface and semantic versioning.
- Add package-consumer examples for CMake and clear dependency handling.

### Phase 4 — Storage architecture decision
- Profile representative workloads before selecting a page-oriented storage manager or persistent index design.
- Specify atomicity, consistency, isolation, and durability guarantees before implementing broader concurrency.
- Treat any storage-format change as a migration project with old-format fixtures, rollback planning, and recovery tests.

## Release positioning

Until release gates are met, label releases as experimental/preview and advise users not to keep their only copy of critical data in CustomDB. A stable release requires evidence from CI, fault-injection tests, restore drills, and a reviewed guarantee document—not just a version number or polished website.
