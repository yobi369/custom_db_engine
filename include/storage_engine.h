#ifndef STORAGE_ENGINE_H
#define STORAGE_ENGINE_H

#include <string>
#include <vector>
#include <map>
#include <memory>

class StorageEngine {
public:
    // Constructor/Destructor
    StorageEngine(const std::string& db_path);
    ~StorageEngine();

    // Database Operations
    bool create_database();
    bool delete_database();
    
    // Data Operations
    bool write_data(const std::string& key, const std::string& value);
    bool write_document(const std::string& collection, const std::string& json_data);
    std::string read_data(const std::string& key);
    std::vector<std::string> query_documents(const std::string& collection, const std::map<std::string, std::string>& filters);
    
    // Schema Management
    bool create_schema(const std::string& schema_definition);
    bool update_schema(const std::string& schema_definition);
    
    // Indexing
    bool create_index(const std::string& field_name);
    bool drop_index(const std::string& field_name);
    
    // Transactions
    bool begin_transaction();
    bool commit_transaction();
    bool rollback_transaction();
    
    // Error Handling
    std::string get_last_error() const;

private:
    std::string db_path;
    std::string last_error;
    
    // Pimpl pattern for implementation details
    class Impl;
    std::unique_ptr<Impl> pimpl;
};

#endif // STORAGE_ENGINE_H
