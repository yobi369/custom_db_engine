# Custom Database Engine

A high-performance, lightweight database engine implemented in C++.

## Features
- Transaction support with rollback capability
- Persistent storage using flat files
- Simple key-value data model
- Thread-safe operations

## Installation

### Prerequisites
- CMake (version 3.28 or higher)
- C++ compiler with C++17 support

### Build Instructions
```bash
mkdir build && cd build
cmake ..
make
```

## Usage

### Basic Operations
```cpp
#include "storage_engine.h"

int main() {
    StorageEngine db("my_database.txt");
    
    // Write data
    db.write_data("key1", "value1");
    
    // Read data
    std::string value = db.read_data("key1");
    
    // Delete data
    db.delete_data("key1");
    
    return 0;
}
```

### Transactions
```cpp
db.begin_transaction();
try {
    db.write_data("key2", "value2");
    db.commit_transaction();
} catch (...) {
    db.rollback_transaction();
}
```

## Testing

Run the test suite:
```bash
./build/storage_engine_test
```

## Contributing

Contributions are welcome! Please follow these guidelines:
1. Fork the repository
2. Create a feature branch
3. Submit a pull request

## License

MIT License
