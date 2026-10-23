# Database file format and guarantees

## Format version 1

The database snapshot is a UTF-8 JSON object with these top-level members:

- `format_version`: integer, currently `1`.
- `generation`: monotonically increasing state generation.
- `kv`: object mapping keys to string values.
- `documents`: object mapping collection names to arrays of JSON objects.
- `schemas`: object mapping collection names to schema definitions. The supported subset validates required fields, flat property types, and `additionalProperties: false`.
- `indexes`: array of configured index-field names. Runtime equality indexes are built lazily in memory and are not serialized.

The implementation rejects unknown format versions and malformed snapshots rather than silently treating them as an empty database. Persisted key names, collection names, schema names, and index names are validated when loading. Each open handle canonicalizes the database path and takes a non-blocking exclusive OS lock on `<canonical-database-path>.lock`; symlink aliases to the same existing database therefore resolve to the same lock path. If another handle owns the lock, open operations fail closed. The lock file is persistent and must not be manually removed while the database may be in use. Hard-link aliases are not a supported way to share a database.

## Write-ahead log

Mutations append a newline-terminated WAL record containing a generation number, an FNV-1a 64-bit checksum, and a JSON payload. Legacy version 1 records contain a complete serialized state. New version 2 records contain a JSON Patch, the base generation, and the target generation; recovery validates the base generation before applying the patch. This reduces repeated serialization and log write amplification for small mutations, although large document edits can still produce large patches. Recovery reads records one at a time with an 8 MiB maximum encoded record size. The reader enforces this limit while reading rather than after allocating a whole line, so a corrupt or hostile oversized line cannot force unbounded record-string allocation. Applications must keep each serialized WAL mutation below this limit; larger records fail closed. Recovery replays valid records newer than the snapshot generation. If the final record is incomplete (no terminating newline), recovery truncates the WAL back to the last complete record and flushes the truncation before permitting future appends. This prevents a subsequent valid record from being concatenated to torn bytes. An oversized record, malformed complete record, zero or non-monotonic generation, generation/payload mismatch, invalid state, or checksum mismatch causes the engine to report a load error and refuse to overwrite the existing files. If an append fails after it may have partially written, the current handle is disabled; reopen the database so recovery can determine the authoritative state before attempting more writes.

A snapshot checkpoint is written every 64 WAL records. If a process stops before checkpointing, the WAL is the recovery source. After the snapshot is published, the WAL is truncated. The generation field prevents old records left by an interrupted truncation from overwriting a newer snapshot.

## Transaction behavior

A transaction modifies an in-memory working snapshot. Commit appends the resulting state to the WAL; rollback discards it. This is a single-instance transaction model, not a multi-process or multi-client isolation guarantee.

## Explicit non-guarantees

WAL records and snapshot files are flushed using `fsync` on POSIX or `_commit` on Windows. POSIX snapshot publication also syncs the containing directory; Windows snapshot replacement uses `MOVEFILE_WRITE_THROUGH`. Filesystem, storage hardware, and platform-specific metadata behavior still affect guarantees, so the project does not claim unconditional power-loss durability on every supported setup. The FNV checksum detects accidental record damage; it is not cryptographic authentication.

The exclusive lock prevents simultaneous writers/open handles; it is not a multi-reader concurrency model. There is no page-oriented storage, persistent index structure, cost-aware query planner, nested/full JSON Schema enforcement, or migration tooling. Do not use this prototype as the sole copy of important data. Legacy version 1 WAL records contain full states; version 2 records use JSON Patch deltas. This reduces write amplification but is not a substitute for page-oriented storage or persistent indexes.

Backup files are standalone versioned snapshots and intentionally do not copy the live WAL. Restore validates the backup, writes it as a new generation, and leaves older WAL records harmless because recovery replays only generations newer than the snapshot. If snapshot publication reports a failure after the rename may have occurred, the current handle is disabled until reopening re-establishes the on-disk state. A restore should still be followed by a successful integrity check.

Future format changes must include a version bump and migration tests. Never assume arbitrary manual edits to database files are safe.

## Performance and scalability notes

New writes use version 2 JSON Patch WAL records while existing version 1 full-state records remain readable. Benchmark on the target machine to quantify the improvement; patch size and application cost still depend on the mutation and current state shape. Run `custom_db_engine_benchmark` on the target machine to capture a baseline before comparing future storage-format changes. Do not infer critical-workload capacity from a small synthetic run.
