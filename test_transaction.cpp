#include "include/storage_engine.h"
#include <iostream>

int main() {
    StorageEngine db("test_db.txt");
    
    // Test transaction rollback
    db.begin_transaction();
    db.write_data("key1", "value1");
    db.write_data("key2", "value2");
    db.rollback_transaction();
    
    std::string value = db.read_data("key1");
    if (value.empty()) {
        std::cout << "Rollback test passed: key1 not found\n";
    } else {
        std::cout << "Rollback test failed: key1 found with value " << value << "\n";
    }

    // Test transaction commit
    db.begin_transaction();
    db.write_data("key3", "value3");
    db.write_data("key4", "value4");
    db.commit_transaction();
    
    value = db.read_data("key3");
    if (!value.empty()) {
        std::cout << "Commit test passed: key3 found with value " << value << "\n";
    } else {
        std::cout << "Commit test failed: key3 not found\n";
    }

    return 0;
}
