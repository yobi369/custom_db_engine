# Database file format and guarantees

## Format version 1

The database snapshot is a UTF-8 JSON object with these top-level members:

- `format_version`: integer, currently `1`.
- `generation`: monotonically increasing state generation.
- `kv`: object mapping keys to string values.
- `documents`: object mapping collection names to arrays of JSON objects.
- `schemas`: object mapping collection names to schema definitions. The supported subset validates required fields, flat property types, and `additionalProperties: false`.
- `indexes`: array of configured index-field names. Runtime equality indexes are built lazily in memory and are not serialized.

The implementation rejects unknown format versions and malformed snapshots rather than silently treating them as an empty database.

## Write-ahead log

Mutations append a complete serialized state to `<database-path>.wal`. Each newline-terminated record contains a generation number, an FNV-1a 64-bit checksum, and the JSON payload. Recovery replays valid records newer than the snapshot generation and ignores an incomplete trailing record. A malformed complete record or checksum mismatch causes the engine to report a load error and refuse to overwrite the existing files.

A snapshot checkpoint is written every 64 WAL records. If a process stops before checkpointing, the WAL is the recovery source. After the snapshot is published, the WAL is truncated. The generation field prevents old records left by an interrupted truncation from overwriting a newer snapshot.

## Transaction behavior

A transaction modifies an in-memory working snapshot. Commit appends the resulting state to the WAL; rollback discards it. This is a single-instance transaction model, not a multi-process or multi-client isolation guarantee.

## Explicit non-guarantees

WAL records and snapshot files are flushed using `fsync` on POSIX or `_commit` on Windows. POSIX snapshot publication also syncs the containing directory; Windows snapshot replacement uses `MOVEFILE_WRITE_THROUGH`. Filesystem, storage hardware, and platform-specific metadata behavior still affect guarantees, so the project does not claim unconditional power-loss durability on every supported setup. The FNV checksum detects accidental record damage; it is not cryptographic authentication.

There is no cross-process lock, multi-instance coordination, page-oriented storage, persistent index structure, cost-aware query planner, nested/full JSON Schema enforcement, or migration tooling. Do not use this prototype as the sole copy of important data. The whole state is written into each WAL record, so log size and write cost grow with database size between checkpoints.

Backup files are standalone versioned snapshots and intentionally do not copy the live WAL. Restore validates the backup, writes it as a new generation, and leaves older WAL records harmless because recovery replays only generations newer than the snapshot. A restore should still be followed by a successful integrity check.

Future format changes must include a version bump and migration tests. Never assume arbitrary manual edits to database files are safe.
