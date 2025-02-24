#include "../include/storage_engine.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <algorithm>
#include <set>
#include <nlohmann/json.hpp>

// Pimpl implementation class
class StorageEngine::Impl {
public:
    Impl(const std::string& db_path) : db_path(db_path) {}
    std::string last_error;
    
    // Implementation of storage engine methods
    bool create_database_impl();
    bool delete_database_impl();
    bool write_data_impl(const std::string& key, const std::string& value);
    bool write_document_impl(const std::string& collection, const std::string& json_data);
    std::string read_data_impl(const std::string& key);
    std::vector<std::string> query_documents_impl(const std::string& collection, 
                                                const std::map<std::string, std::string>& filters);
    bool create_schema_impl(const std::string& schema_definition);
    bool update_schema_impl(const std::string& schema_definition);
    bool create_index_impl(const std::string& field_name);
    bool drop_index_impl(const std::string& field_name);
    bool begin_transaction_impl();
    bool commit_transaction_impl();
    bool rollback_transaction_impl();
    
private:
    std::string db_path;
    bool in_transaction = false;
    std::map<std::string, std::vector<std::string>> collections;
    std::map<std::string, std::string> schemas;
    std::vector<std::string> indexes;
    
    // Transaction state
    struct TransactionState {
        std::map<std::string, std::string> original_key_value_store;
        std::map<std::string, std::string> modified_key_value_store;
        std::map<std::string, std::vector<std::string>> document_store;
    };
    TransactionState* transaction_state = nullptr;
};

StorageEngine::StorageEngine(const std::string& db_path) 
    : pimpl(std::make_unique<Impl>(db_path)) {
    std::cout << "Initializing storage engine at: " << db_path << "\n";
}

StorageEngine::~StorageEngine() = default;

bool StorageEngine::create_database() {
    return pimpl->create_database_impl();
}

bool StorageEngine::Impl::create_database_impl() {
    std::ofstream db_file(db_path);
    if (!db_file) {
        last_error = "Failed to create database file";
        return false;
    }
    return true;
}

bool StorageEngine::write_data(const std::string& key, const std::string& value) {
    return pimpl->write_data_impl(key, value);
}

bool StorageEngine::Impl::write_data_impl(const std::string& key, const std::string& value) {
    if (!in_transaction) {
        // Only write to database when not in transaction
        std::ofstream db_file(db_path, std::ios::app);
        if (!db_file) {
            last_error = "Failed to open database file for writing";
            return false;
        }
        db_file << key << ":" << value << "\n";
        return true;
    }
    
    // During transaction, only update transaction state
    if (!transaction_state) {
        last_error = "Transaction state not initialized";
        return false;
    }
    transaction_state->modified_key_value_store[key] = value;
    return true;
}

std::string StorageEngine::read_data(const std::string& key) {
    return pimpl->read_data_impl(key);
}

std::string StorageEngine::Impl::read_data_impl(const std::string& key) {
    if (in_transaction && transaction_state) {
        // Check modified values first
        auto it = transaction_state->modified_key_value_store.find(key);
        if (it != transaction_state->modified_key_value_store.end()) {
            return it->second;
        }
        // Fall back to original values
        it = transaction_state->original_key_value_store.find(key);
        if (it != transaction_state->original_key_value_store.end()) {
            return it->second;
        }
    } else {
        std::ifstream db_file(db_path);
        std::string line;
        while (std::getline(db_file, line)) {
            size_t pos = line.find(':');
            if (pos != std::string::npos && line.substr(0, pos) == key) {
                return line.substr(pos + 1);
            }
        }
    }
    last_error = "Key not found: " + key;
    return "";
}

bool StorageEngine::delete_database() {
    return pimpl->delete_database_impl();
}

bool StorageEngine::Impl::delete_database_impl() {
    if (std::remove(db_path.c_str()) != 0) {
        last_error = "Failed to delete database file";
        return false;
    }
    return true;
}

bool StorageEngine::write_document(const std::string& collection, const std::string& json_data) {
    return pimpl->write_document_impl(collection, json_data);
}

bool StorageEngine::Impl::write_document_impl(const std::string& collection, const std::string& json_data) {
    if (in_transaction && transaction_state) {
        if (!transaction_state->document_store.count(collection)) {
            transaction_state->document_store[collection] = {};
        }
        transaction_state->document_store[collection].push_back(json_data);
    }
    collections[collection].push_back(json_data);
    return true;
}

std::vector<std::string> StorageEngine::query_documents(const std::string& collection, 
                                                        const std::map<std::string, std::string>& filters) {
    return pimpl->query_documents_impl(collection, filters);
}

std::vector<std::string> StorageEngine::Impl::query_documents_impl(const std::string& collection, 
                                                                  const std::map<std::string, std::string>& filters) {
    std::vector<std::string> results;
    try {
        for (const auto& doc : collections[collection]) {
            auto json = nlohmann::json::parse(doc);
            bool match = true;
            for (const auto& [key, value] : filters) {
                if (!json.contains(key) || json[key] != value) {
                    match = false;
                    break;
                }
            }
            if (match) {
                results.push_back(doc);
            }
        }
    } catch (const std::exception& e) {
        last_error = "Error parsing JSON: " + std::string(e.what());
    }
    return results;
}

bool StorageEngine::create_schema(const std::string& schema_definition) {
    return pimpl->create_schema_impl(schema_definition);
}

bool StorageEngine::Impl::create_schema_impl(const std::string& schema_definition) {
    schemas["default"] = schema_definition;
    return true;
}

bool StorageEngine::update_schema(const std::string& schema_definition) {
    return pimpl->update_schema_impl(schema_definition);
}

bool StorageEngine::Impl::update_schema_impl(const std::string& schema_definition) {
    schemas["default"] = schema_definition;
    return true;
}

bool StorageEngine::create_index(const std::string& field_name) {
    return pimpl->create_index_impl(field_name);
}

bool StorageEngine::Impl::create_index_impl(const std::string& field_name) {
    indexes.push_back(field_name);
    return true;
}

bool StorageEngine::drop_index(const std::string& field_name) {
    return pimpl->drop_index_impl(field_name);
}

bool StorageEngine::Impl::drop_index_impl(const std::string& field_name) {
    auto it = std::find(indexes.begin(), indexes.end(), field_name);
    if (it != indexes.end()) {
        indexes.erase(it);
        return true;
    }
    return false;
}

bool StorageEngine::begin_transaction() {
    return pimpl->begin_transaction_impl();
}

bool StorageEngine::Impl::begin_transaction_impl() {
    if (in_transaction) {
        last_error = "Transaction already in progress";
        return false;
    }
    in_transaction = true;
    transaction_state = new TransactionState();
    transaction_state->document_store = collections;
    
    // Read current database state into transaction state
    std::ifstream db_file(db_path);
    std::string line;
    while (std::getline(db_file, line)) {
        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            transaction_state->original_key_value_store[key] = value;
        }
    }
    transaction_state->modified_key_value_store = transaction_state->original_key_value_store;
    return true;
}

bool StorageEngine::commit_transaction() {
    return pimpl->commit_transaction_impl();
}

bool StorageEngine::Impl::commit_transaction_impl() {
    if (!in_transaction) {
        last_error = "No transaction to commit";
        return false;
    }
    in_transaction = false;
    delete transaction_state;
    transaction_state = nullptr;
    return true;
}

bool StorageEngine::rollback_transaction() {
    return pimpl->rollback_transaction_impl();
}

bool StorageEngine::Impl::rollback_transaction_impl() {
    if (!in_transaction) {
        last_error = "No transaction to rollback";
        return false;
    }
    
    if (!transaction_state) {
        last_error = "Transaction state not initialized";
        return false;
    }
    
    // Clear modified state before restoring original
    transaction_state->modified_key_value_store.clear();
    
    // Restore both collections and key-value store to pre-transaction state
    collections = transaction_state->document_store;
    transaction_state->modified_key_value_store = transaction_state->original_key_value_store;
    
    // Clear the database file
    std::ofstream db_file(db_path, std::ios::trunc);
    if (!db_file) {
        last_error = "Failed to rollback transaction";
        return false;
    }
    
    // Write all original key-value pairs
    for (const auto& [key, value] : transaction_state->original_key_value_store) {
        db_file << key << ":" << value << "\n";
    }
    
    // Clean up transaction state
    in_transaction = false;
    delete transaction_state;
    transaction_state = nullptr;
    
    return true;
}


std::string StorageEngine::get_last_error() const {
    return pimpl->last_error;
}
