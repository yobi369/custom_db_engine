# Custom Database Engine

A lightweight, custom database engine implementation in C++ with transaction support.

## Features

- Key-value storage
- Transaction support with commit/rollback
- Simple document storage
- Basic query capabilities
- Schema management
- Indexing support

## Getting Started

### Prerequisites
- C++17 compiler
- CMake (optional)

### Building
```bash
mkdir build
cd build
cmake ..
make
```

### Running Tests
```bash
./test_transaction
```

### Usage Example
```cpp
#include "include/storage_engine.h"

int main() {
    StorageEngine db("test_db.txt");
    
    // Basic operations
    db.write_data("key1", "value1");
    std::string value = db.read_data("key1");
    
    // Transaction example
    db.begin_transaction();
    db.write_data("temp_key", "temp_value");
    db.rollback_transaction(); // Undo changes
    
    db.begin_transaction();
    db.write_data("key2", "value2");
    db.commit_transaction(); // Save changes
    
    return 0;
}
```

## Documentation

### StorageEngine Class
- `write_data(key, value)`: Store key-value pair
- `read_data(key)`: Retrieve value by key
- `begin_transaction()`: Start a new transaction
- `commit_transaction()`: Commit current transaction
- `rollback_transaction()`: Rollback current transaction

## License
MIT License - See LICENSE file for details

## Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.

## Acknowledgements
- JSON parsing using nlohmann/json
