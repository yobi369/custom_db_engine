# Database file format and guarantees

## Format version 1

The database snapshot is a UTF-8 JSON object with these top-level members:

- `format_version`: integer, currently `1`.
- `generation`: monotonically increasing state generation.
- `kv`: object mapping keys to string values.
- `documents`: object mapping collection names to arrays of JSON objects.
- `schemas`: object mapping collection names to stored schema metadata.
- `indexes`: array of index-field names (metadata only in this release).

The implementation rejects unknown format versions and malformed snapshots rather than silently treating them as an empty database.

## Write-ahead log

Mutations append a complete serialized state to `<database-path>.wal`. Each newline-terminated record contains a generation number, an FNV-1a 64-bit checksum, and the JSON payload. Recovery replays valid records newer than the snapshot generation and ignores an incomplete trailing record. A malformed complete record or checksum mismatch causes the engine to report a load error and refuse to overwrite the existing files.

A snapshot checkpoint is written every 64 WAL records. If a process stops before checkpointing, the WAL is the recovery source. After the snapshot is published, the WAL is truncated. The generation field prevents old records left by an interrupted truncation from overwriting a newer snapshot.

## Transaction behavior

A transaction modifies an in-memory working snapshot. Commit appends the resulting state to the WAL; rollback discards it. This is a single-instance transaction model, not a multi-process or multi-client isolation guarantee.

## Explicit non-guarantees

The C++ stream is flushed, but the implementation does not call platform-specific fsync/FlushFileBuffers or sync the containing directory. Consequently, it does not promise power-loss durability. Snapshot replacement fallback behavior varies by platform. The FNV checksum detects accidental record damage; it is not cryptographic authentication.

There is no cross-process lock, multi-instance coordination, page-oriented storage, index-backed query execution, full JSON Schema enforcement, or migration tooling. Do not use this prototype as the sole copy of important data. The whole state is written into each WAL record, so log size and write cost grow with database size between checkpoints.

Future format changes must include a version bump and migration tests. Never assume arbitrary manual edits to database files are safe.
