#include "../include/storage_engine.h"
#include <iostream>
#include <cassert>
#include <map>

int main() {
    StorageEngine engine("test_db.txt");
    
    // Test database operations
    assert(engine.create_database());
    
    // Test key-value operations
    assert(engine.write_data("key1", "value1"));
    assert(engine.write_data("key2", "value2"));
    assert(engine.read_data("key1") == "value1");
    assert(engine.read_data("key2") == "value2");
    assert(engine.read_data("key3").empty());
    
    // Test document model
    assert(engine.write_document("users", R"({"name": "Alice", "age": 30})"));
    assert(engine.write_document("users", R"({"name": "Bob", "age": 25})"));
    
    std::map<std::string, std::string> filters = {{"name", "Alice"}};
    auto results = engine.query_documents("users", filters);
    assert(results.size() == 1);
    assert(results[0] == R"({"name": "Alice", "age": 30})");
    
    // Test schema management
    std::string schema = R"({"type": "object", "properties": {"name": {"type": "string"}}})";
    assert(engine.create_schema(schema));
    assert(engine.update_schema(schema));
    
    // Test indexing
    assert(engine.create_index("name"));
    assert(engine.drop_index("name"));
    
    // Test transactions
    assert(engine.begin_transaction());
    assert(engine.write_data("tx_key", "tx_value"));
    assert(engine.commit_transaction());
    
    assert(engine.begin_transaction());
    assert(engine.write_data("rollback_key", "rollback_value"));
    assert(engine.rollback_transaction());
    assert(engine.read_data("rollback_key").empty());
    
    // Test database deletion
    assert(engine.delete_database());
    
    std::cout << "All storage engine tests passed!\n";
    return 0;
}
