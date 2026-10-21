# Custom Database Engine

A C++17 embedded database prototype with persistent key-value storage, write-ahead logging (WAL), snapshot checkpoints, transactions, JSON documents, simple equality queries, schema metadata, and index metadata.

> **Status:** early-stage software. This release is suitable for development and experimentation, not critical production data. It stores complete state snapshots in a write-ahead log and periodically checkpoints to JSON. It does not yet provide fsync-backed durability guarantees, multi-process coordination, indexed query execution, or full JSON Schema validation.

## Features

- Persistent key-value records stored in a versioned JSON database file.
- Write-ahead log records with generation numbers and FNV-1a checksums; periodic snapshot checkpoints.
- In-process transactions with commit and rollback.
- JSON object document collections and simple equality filters.
- Schema metadata storage and index metadata operations.
- Per-instance mutex protection for calls on a single StorageEngine object.
- CMake build, CTest test suite, and GitHub Actions CI.

## Requirements

- C++17 compiler
- CMake 3.16+
- nlohmann/json 3.10+ (found locally or fetched by CMake)

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Quick start

```cpp
#include "storage_engine.h"
#include <iostream>

int main() {
    StorageEngine db("example.db.json");
    if (!db.write_data("greeting", "hello")) {
        std::cerr << db.get_last_error() << '\n';
        return 1;
    }
    const std::string value = db.read_data("greeting");
    if (!db.get_last_error().empty()) {
        std::cerr << db.get_last_error() << '\n';
        return 1;
    }
    std::cout << value << '\n';
    if (!db.begin_transaction() || !db.write_data("temporary", "value") || !db.rollback_transaction()) {
        std::cerr << db.get_last_error() << '\n';
        return 1;
    }
}
```

## API notes

- `write_data(key, value)` inserts or replaces a string value.
- `read_data(key)` returns the value; an empty result is ambiguous, so check `get_last_error()` to distinguish a missing key from a stored empty string.
- `delete_data(key)` removes a key and reports failure if it does not exist.
- `begin_transaction()`, `commit_transaction()`, and `rollback_transaction()` operate on one transaction per engine instance.
- `write_document(collection, json)` accepts JSON objects.
- `query_documents(collection, filters)` applies exact string comparisons for string fields and JSON-text comparisons for non-string fields.
- `create_schema` and `update_schema` store JSON metadata keyed by the `collection` property (or `default`). They do not enforce a full JSON Schema specification.
- `create_index` and `drop_index` manage index metadata only; they do not yet accelerate queries.

Check every boolean return value and inspect `get_last_error()` after a failure. A database file should be used by only one process at a time.

## Current limitations and roadmap

1. **Storage durability:** add platform-specific durable flushes and directory synchronization before claiming power-loss durability.
2. **Concurrency:** add explicit cross-process locking and define transaction isolation.
3. **Query engine:** implement typed predicates, projections, ordering, limits, and real index-backed execution.
4. **Schema enforcement:** implement a documented subset of JSON Schema and validate documents on write.
5. **Storage scaling:** replace whole-file snapshots with a page-oriented storage manager and B+ tree after profiling.
6. **Operations:** add backup/restore, integrity checking, format migrations, fuzzing, fault injection, sanitizers, and reproducible benchmarks.

See [docs/FORMAT.md](docs/FORMAT.md) for the on-disk format and guarantees. See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidance.

## License

MIT. See [LICENSE](LICENSE).
