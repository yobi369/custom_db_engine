#include "../include/storage_engine.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <map>
#include <vector>

class StorageEngine::Impl {
public:
    Impl(const std::string& db_path) : db_path(db_path), in_transaction(false) {}
    std::string last_error;
    
    bool begin_transaction_impl();
    bool write_data_impl(const std::string& key, const std::string& value);
    std::string read_data_impl(const std::string& key);
    bool commit_transaction_impl();
    bool rollback_transaction_impl();

private:
    std::string db_path;
    bool in_transaction;
    std::map<std::string, std::string> key_value_store;
    
    struct TransactionState {
        std::map<std::string, std::string> original_key_value_store;
        std::map<std::string, std::string> modified_key_value_store;
    };
    std::unique_ptr<TransactionState> transaction_state;
};

StorageEngine::StorageEngine(const std::string& db_path) 
    : pimpl(std::make_unique<Impl>(db_path)) {
    std::cout << "Initializing storage engine at: " << db_path << "\n";
}

StorageEngine::~StorageEngine() = default;

bool StorageEngine::begin_transaction() {
    return pimpl->begin_transaction_impl();
}

bool StorageEngine::Impl::begin_transaction_impl() {
    if (in_transaction) {
        last_error = "Transaction already in progress";
        return false;
    }
    in_transaction = true;
    transaction_state = std::make_unique<TransactionState>();
    transaction_state->original_key_value_store = key_value_store;
    transaction_state->modified_key_value_store = key_value_store;
    return true;
}

bool StorageEngine::write_data(const std::string& key, const std::string& value) {
    return pimpl->write_data_impl(key, value);
}

bool StorageEngine::Impl::write_data_impl(const std::string& key, const std::string& value) {
    if (!in_transaction) {
        key_value_store[key] = value;
        return true;
    }
    transaction_state->modified_key_value_store[key] = value;
    return true;
}

std::string StorageEngine::read_data(const std::string& key) {
    return pimpl->read_data_impl(key);
}

std::string StorageEngine::Impl::read_data_impl(const std::string& key) {
    if (in_transaction && transaction_state) {
        auto it = transaction_state->modified_key_value_store.find(key);
        if (it != transaction_state->modified_key_value_store.end()) {
            return it->second;
        }
    }
    auto it = key_value_store.find(key);
    if (it != key_value_store.end()) {
        return it->second;
    }
    last_error = "Key not found";
    return "";
}

bool StorageEngine::commit_transaction() {
    return pimpl->commit_transaction_impl();
}

bool StorageEngine::Impl::commit_transaction_impl() {
    if (!in_transaction) {
        last_error = "No transaction to commit";
        return false;
    }
    key_value_store = transaction_state->modified_key_value_store;
    in_transaction = false;
    transaction_state.reset();
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
    
    // Restore original state
    key_value_store = transaction_state->original_key_value_store;
    
    // Clean up transaction state
    in_transaction = false;
    transaction_state.reset();
    
    return true;
}

std::string StorageEngine::get_last_error() const {
    return pimpl->last_error;
}
