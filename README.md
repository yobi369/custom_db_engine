# Custom Database Engine

A C++17 embedded database prototype with persistent key-value storage, write-ahead logging (WAL), snapshot checkpoints, transactions, JSON documents, simple equality queries, subset JSON schema validation, and index metadata.

> **Status:** early-stage software. This release is suitable for development and experimentation, not critical production data. It stores complete state snapshots in a write-ahead log and periodically checkpoints to JSON. It does not claim unconditional power-loss durability across all filesystems and hardware, and it does not provide multi-process coordination or full JSON Schema validation.

## Features

- Persistent key-value records stored in a versioned JSON database file.
- Write-ahead log records with generation numbers and FNV-1a checksums; periodic snapshot checkpoints.
- In-process transactions with commit and rollback.
- JSON object document collections and simple equality filters.
- A documented subset of JSON schema validation (required fields, basic property types, and additional-property rejection).
- Lazy in-memory equality indexes for configured fields; index caches are rebuilt after collection writes and process restarts.
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

The `custom_db_engine_example` target demonstrates basic storage and integrity checking:

```bash
./build/custom_db_engine_example example.db.json
```

On multi-configuration generators such as Visual Studio, the executable is typically under `build/Debug` or `build/Release`.


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
- `backup_database(path)` writes a consistent committed-state snapshot to a separate file.
- `restore_database(path)` restores a validated snapshot and advances its generation so older WAL records cannot override it.
- `check_integrity()` reloads the on-disk snapshot and WAL and compares them with the loaded state.
- `begin_transaction()`, `commit_transaction()`, and `rollback_transaction()` operate on one transaction per engine instance.
- `write_document(collection, json)` accepts JSON objects.
- `query_documents(collection, filters)` applies exact string comparisons for string fields and JSON-text comparisons for non-string fields.
- `create_schema` and `update_schema` store JSON metadata keyed by the `collection` property (or `default`). Document writes enforce a documented subset: required fields, top-level object schemas, basic property types, and optional rejection of unknown properties. Nested schema validation and the full JSON Schema standard are not implemented.
- `create_index` and `drop_index` configure field indexes. Matching equality queries can use a lazily built in-memory index; the index cache is not persisted and is rebuilt after restart or collection changes.

Check every boolean return value and inspect `get_last_error()` after a failure. A database file should be used by only one process at a time.

## Install and consume as a CMake package

Install the library and headers:

```bash
cmake --install build --prefix install
```

A consuming project can then use:

```cmake
find_package(CustomDBEngine 0.2 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE CustomDBEngine::custom_db_engine)
```

The consuming environment must also provide the `nlohmann_json` CMake package (version 3.10 or newer).


## Current limitations and roadmap

1. **Storage durability:** validate durability behavior under filesystem-specific fault injection and document the exact guarantees of each supported platform.
2. **Concurrency:** add explicit cross-process locking and define transaction isolation.
3. **Query engine:** implement typed predicates, projections, ordering, limits, and persistent index structures and a cost-aware query planner.
4. **Schema enforcement:** expand beyond the currently supported flat required/property-type subset and add nested-schema validation.
5. **Storage scaling:** replace whole-file snapshots with a page-oriented storage manager and B+ tree after profiling.
6. **Operations:** add automated backup scheduling, format migrations, fuzzing, broader fault injection, sanitizers, and reproducible benchmarks.

See [docs/FORMAT.md](docs/FORMAT.md) for the on-disk format and guarantees. See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidance.

## License

MIT. See [LICENSE](LICENSE).
