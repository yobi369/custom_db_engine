# Database file format and guarantees

## Format version 1

The database is a UTF-8 JSON object with these top-level members:

- `format_version`: integer, currently `1`.
- `kv`: object mapping keys to string values.
- `documents`: object mapping collection names to arrays of JSON objects.
- `schemas`: object mapping collection names to stored schema metadata.
- `indexes`: array of index-field names (metadata only in this release).

The implementation rejects unknown format versions and malformed or incomplete snapshots rather than silently treating them as an empty database.

## Write protocol

Each standalone mutation serializes the complete state to `<database-path>.tmp`, flushes the C++ stream, closes it, and renames the temporary file over the target. Filesystem rename behavior varies by platform; the fallback replacement path is not crash-atomic on every platform. Stream flush is not equivalent to an OS-level fsync or directory sync.

A transaction modifies an in-memory working snapshot. Commit publishes that snapshot; rollback discards it. If the process exits before commit, uncommitted changes are not intentionally persisted.

## Explicit non-guarantees

This format is currently a prototype. It has no write-ahead log, checksums, torn-write recovery, fsync-based durability contract, cross-process lock, multi-instance coordination, or migration tooling. Do not use it as the sole copy of important data. The whole database is rewritten for each committed mutation, so write cost grows with database size.

Future format changes must include a version bump and migration tests. Never assume arbitrary manual edits to the database file are safe.
