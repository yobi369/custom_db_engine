# Architecture decisions

This file records the principles that must guide major storage changes. Add dated decision records here when a choice changes compatibility or public guarantees.

## ADR-001 — Keep the first product embedded and single-writer

**Status:** Accepted for the current product phase.

**Context:** The engine currently uses versioned JSON snapshots, a WAL, and an exclusive OS lock. It is designed to be linked into a C++ application, not run as a database server.

**Decision:** Keep the initial product focused on embedded, single-active-handle workloads. Do not add a network server, replication, or multi-writer mode as a shortcut to appearing enterprise-ready.

**Consequences:** The scope remains understandable and testable. Applications requiring concurrent independent writers or distributed availability should use a database designed for those needs until CustomDB has a separately specified and verified concurrency architecture.

## ADR-002 — Preserve fail-closed recovery

**Status:** Accepted.

**Context:** Silently treating damaged persistence files as an empty database can destroy recoverable data.

**Decision:** Unsupported versions, malformed complete WAL records, checksum errors, generation mismatches, and invalid persisted states must surface an error and must not be overwritten automatically. Recovery behavior must be covered by tests using real persisted fixtures.

**Consequences:** Opening a damaged database may require explicit operator intervention. This is preferable to silently discarding data. Recovery tools should preserve the original files and explain next steps.

## ADR-003 — Treat format changes as migrations

**Status:** Accepted.

**Context:** Snapshots and WAL records are persistent user data and can outlive the binary that wrote them.

**Decision:** Any incompatible format change requires a version bump, old-format fixtures, migration tests, interruption/failure tests, backup guidance, and a documented rollback boundary.

**Consequences:** Storage optimization may take longer, but users receive a defined compatibility path. Never ship a format change based only on passing tests against newly created databases.

## ADR-004 — Profile before selecting a new storage layout

**Status:** Proposed.

**Context:** Whole-file JSON snapshots and document patches have workload-dependent scaling costs. Replacing them with pages, a B+ tree, or another layout is a substantial compatibility and correctness project.

**Decision:** First publish reproducible benchmark baselines for representative small, medium, and large workloads. Then compare candidate layouts and document trade-offs before implementation.

**Acceptance criteria:** A design proposal must include crash consistency, page allocation, free-space reuse, index consistency, recovery time, migration strategy, and measurable target workloads.
