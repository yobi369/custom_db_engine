#ifndef STORAGE_ENGINE_H
#define STORAGE_ENGINE_H
#include <map>
#include <memory>
#include <string>
#include <vector>
struct ReadResult {
    bool found = false;
    std::string value;
    std::string error;
};

class StorageEngine {
public:
    explicit StorageEngine(const std::string& db_path);
    ~StorageEngine();
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;
    StorageEngine(StorageEngine&&) = delete;
    StorageEngine& operator=(StorageEngine&&) = delete;
    bool create_database();
    bool delete_database();
    bool backup_database(const std::string& backup_path);
    bool restore_database(const std::string& backup_path);
    bool check_integrity();
    bool write_data(const std::string& key, const std::string& value);
    std::string read_data(const std::string& key);
    // Unambiguous read API: distinguishes a missing key from a stored empty string.
    ReadResult read_data_result(const std::string& key);
    bool delete_data(const std::string& key);
    bool write_document(const std::string& collection, const std::string& json_data);
    std::vector<std::string> query_documents(const std::string& collection, const std::map<std::string, std::string>& filters);
    bool create_schema(const std::string& schema_definition);
    bool update_schema(const std::string& schema_definition);
    bool create_index(const std::string& field_name);
    bool drop_index(const std::string& field_name);
    bool begin_transaction();
    bool commit_transaction();
    bool rollback_transaction();
    std::string get_last_error() const;
private:
    class Impl;
    std::unique_ptr<Impl> pimpl;
};
#endif
