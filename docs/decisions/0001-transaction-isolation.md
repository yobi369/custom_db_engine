# ADR 0001: Transaction isolation and engine concurrency

- Status: Accepted for the initial single-process engine contract
- Date: 2026-11-20
- Scope: Embedded C++ engine and callers of one StorageEngine instance

## Context

The engine protects individual public operations with a mutex and maintains one transaction working snapshot per instance. A mutex held only for each method call is insufficient: while one thread has an open transaction, another thread could otherwise read uncommitted values or accidentally mutate the same transaction snapshot.

The OS file lock separately excludes other database handles and processes. It does not provide in-process transaction isolation.

## Decision

A transaction owns the engine's recursive mutex from successful begin_transaction() until successful commit_transaction() or rollback_transaction(). Public operations from other threads wait until the transaction finishes. The transaction-owning thread can call normal engine methods because the mutex is recursive.

The engine remains single-transaction-per-instance and single-handle-per-database. This does not introduce multi-process concurrency, parallel transactions, MVCC, snapshot isolation, or a multi-writer server mode.

A failed commit leaves the transaction open and the handle locked to the transaction-owning thread so the caller can inspect the error and attempt rollback. If a persistence failure disables the handle, reopening remains the recovery path.

## Required tests

- A second thread cannot read a transaction's uncommitted value.
- After rollback, the waiting reader sees the pre-transaction committed value.
- A second thread cannot write into the active transaction's working snapshot.
- After rollback releases the transaction, the waiting writer executes as a normal independent write.
- Existing commit, rollback, crash-recovery, process-lock, and sanitizer tests remain green.

## Consequences

This provides serializable access at the engine-instance boundary by blocking other threads for the full transaction lifetime. Long-running transactions therefore block all other operations on that instance. Applications must keep transactions short and must not begin a transaction on one thread and expect another thread to commit or roll it back.

The server must not expose a transaction that spans unrelated HTTP worker threads unless a future transaction/session API explicitly binds ownership and lifecycle.

## Explicit limits

- One active transaction per StorageEngine instance.
- One active handle per database path across processes.
- No concurrent multi-process readers/writers.
- No automatic deadlock detection, transaction timeout, cancellation, or MVCC.
- WAL flush/checkpoint behavior and filesystem-specific durability limits remain as documented in FORMAT.md.
