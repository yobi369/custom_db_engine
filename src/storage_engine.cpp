#include "storage_engine.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
json empty_state() {
    return json{{"format_version", 1}, {"kv", json::object()}, {"documents", json::object()}, {"schemas", json::object()}, {"indexes", json::array()}};
}
bool valid_name(const std::string& value) {
    return !value.empty() && value.find('\0') == std::string::npos;
}
}
class StorageEngine::Impl {
public:
    explicit Impl(std::string path) : path_(std::move(path)), state_(empty_state()) {
        std::lock_guard<std::mutex> lock(mutex_);
        load_locked();
        load_failed_ = !last_error_.empty();
    }
    bool create_database() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty()) return fail("Database path must not be empty");
        std::error_code ec;
        if (fs::exists(path_, ec)) return fail("Database already exists");
        if (ec) return fail("Cannot inspect database path: " + ec.message());
        state_ = empty_state();
        return persist_locked(state_);
    }
    bool delete_database() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_transaction_) return fail("Cannot delete database during a transaction");
        std::error_code ec;
        const bool removed = fs::remove(path_, ec);
        if (ec) return fail("Cannot delete database: " + ec.message());
        if (!removed) return fail("Database file does not exist");
        state_ = empty_state();
        last_error_.clear();
        return true;
    }
    bool write_data(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_name(key)) return fail("Key must not be empty or contain NUL");
        json next = writable_state_locked();
        next["kv"][key] = value;
        return finish_write_locked(std::move(next));
    }
    std::string read_data(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const json& view = readable_state_locked();
        auto it = view["kv"].find(key);
        if (it == view["kv"].end()) { fail("Key not found: " + key); return {}; }
        last_error_.clear();
        return it->get<std::string>();
    }
    bool delete_data(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        json next = writable_state_locked();
        if (!next["kv"].contains(key)) return fail("Key not found: " + key);
        next["kv"].erase(key);
        return finish_write_locked(std::move(next));
    }
    bool write_document(const std::string& collection, const std::string& json_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_name(collection)) return fail("Collection name must not be empty");
        json document;
        try { document = json::parse(json_data); }
        catch (const std::exception& e) { return fail(std::string("Invalid JSON document: ") + e.what()); }
        if (!document.is_object()) return fail("Documents must be JSON objects");
        json next = writable_state_locked();
        if (!next["documents"].contains(collection)) next["documents"][collection] = json::array();
        next["documents"][collection].push_back(document);
        return finish_write_locked(std::move(next));
    }
    std::vector<std::string> query_documents(const std::string& collection, const std::map<std::string, std::string>& filters) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        const json& view = readable_state_locked();
        auto coll = view["documents"].find(collection);
        if (coll == view["documents"].end()) { last_error_.clear(); return result; }
        for (const auto& document : *coll) {
            bool matches = true;
            for (const auto& filter : filters) {
                auto field = document.find(filter.first);
                if (field == document.end()) { matches = false; break; }
                const std::string actual = field->is_string() ? field->get<std::string>() : field->dump();
                if (actual != filter.second) { matches = false; break; }
            }
            if (matches) result.push_back(document.dump());
        }
        last_error_.clear();
        return result;
    }
    bool create_schema(const std::string& definition) { return set_schema(definition, true); }
    bool update_schema(const std::string& definition) { return set_schema(definition, false); }
    bool create_index(const std::string& field_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_name(field_name)) return fail("Index field name must not be empty");
        json next = writable_state_locked();
        for (const auto& index : next["indexes"])
            if (index == field_name) return fail("Index already exists: " + field_name);
        next["indexes"].push_back(field_name);
        return finish_write_locked(std::move(next));
    }
    bool drop_index(const std::string& field_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        json next = writable_state_locked();
        auto& indexes = next["indexes"];
        for (auto it = indexes.begin(); it != indexes.end(); ++it) {
            if (*it == field_name) { indexes.erase(it); return finish_write_locked(std::move(next)); }
        }
        return fail("Index does not exist: " + field_name);
    }
    bool begin_transaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_transaction_) return fail("Transaction already in progress");
        transaction_state_ = state_;
        in_transaction_ = true;
        last_error_.clear();
        return true;
    }
    bool commit_transaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_transaction_) return fail("No transaction to commit");
        if (!persist_locked(transaction_state_)) return false;
        state_ = std::move(transaction_state_);
        transaction_state_ = json();
        in_transaction_ = false;
        last_error_.clear();
        return true;
    }
    bool rollback_transaction() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_transaction_) return fail("No transaction to roll back");
        transaction_state_ = json();
        in_transaction_ = false;
        last_error_.clear();
        return true;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }
private:
    bool fail(std::string message) { last_error_ = std::move(message); return false; }
    bool set_schema(const std::string& definition, bool create) {
        std::lock_guard<std::mutex> lock(mutex_);
        json schema;
        try { schema = json::parse(definition); }
        catch (const std::exception& e) { return fail(std::string("Invalid schema JSON: ") + e.what()); }
        if (!schema.is_object()) return fail("Schema definition must be a JSON object");
        const std::string name = schema.value("collection", std::string("default"));
        if (!valid_name(name)) return fail("Schema collection name must not be empty");
        json next = writable_state_locked();
        if (create && next["schemas"].contains(name)) return fail("Schema already exists: " + name);
        if (!create && !next["schemas"].contains(name)) return fail("Schema does not exist: " + name);
        next["schemas"][name] = schema;
        return finish_write_locked(std::move(next));
    }
    json writable_state_locked() const { return in_transaction_ ? transaction_state_ : state_; }
    const json& readable_state_locked() const { return in_transaction_ ? transaction_state_ : state_; }
    bool finish_write_locked(json next) {
        if (in_transaction_) { transaction_state_ = std::move(next); last_error_.clear(); return true; }
        if (!persist_locked(next)) return false;
        state_ = std::move(next);
        last_error_.clear();
        return true;
    }
    bool persist_locked(const json& value) {
        if (load_failed_) return fail("Database failed to load; refusing to overwrite the existing file");
        if (path_.empty()) return fail("Database path must not be empty");
        const fs::path target(path_);
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        if (ec) return fail("Cannot create database directory: " + ec.message());
        const fs::path temporary = target.string() + ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) return fail("Cannot open temporary database file for writing");
            const std::string bytes = value.dump(2) + "\n";
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out) {
                out.close();
                fs::remove(temporary, ec);
                return fail("Failed while writing database snapshot");
            }
        }
        fs::rename(temporary, target, ec);
        if (ec) {
            // Replacement rename differs across platforms. This fallback is not crash-atomic.
            std::error_code remove_ec;
            fs::remove(target, remove_ec);
            ec.clear();
            fs::rename(temporary, target, ec);
        }
        if (ec) {
            std::error_code cleanup_ec;
            fs::remove(temporary, cleanup_ec);
            return fail("Cannot publish database snapshot: " + ec.message());
        }
        return true;
    }
    void load_locked() {
        if (path_.empty()) return;
        std::error_code ec;
        const bool exists = fs::exists(path_, ec);
        if (ec) { fail("Cannot inspect database path: " + ec.message()); return; }
        if (!exists) return;
        std::ifstream in(path_, std::ios::binary);
        if (!in) { fail("Cannot open database file"); return; }
        try {
            json loaded;
            in >> loaded;
            if (!loaded.is_object() || loaded.value("format_version", 0) != 1 ||
                !loaded.contains("kv") || !loaded["kv"].is_object() ||
                !loaded.contains("documents") || !loaded["documents"].is_object() ||
                !loaded.contains("schemas") || !loaded["schemas"].is_object() ||
                !loaded.contains("indexes") || !loaded["indexes"].is_array()) {
                fail("Invalid or unsupported database file format"); return;
            }
            state_ = std::move(loaded);
            last_error_.clear();
        } catch (const std::exception& e) { fail(std::string("Cannot load database file: ") + e.what()); }
    }
    std::string path_;
    mutable std::mutex mutex_;
    json state_;
    json transaction_state_;
    bool in_transaction_ = false;
    bool load_failed_ = false;
    std::string last_error_;
};
StorageEngine::StorageEngine(const std::string& path) : pimpl(std::make_unique<Impl>(path)) {}
StorageEngine::~StorageEngine() = default;
bool StorageEngine::create_database() { return pimpl->create_database(); }
bool StorageEngine::delete_database() { return pimpl->delete_database(); }
bool StorageEngine::write_data(const std::string& k, const std::string& v) { return pimpl->write_data(k, v); }
std::string StorageEngine::read_data(const std::string& k) { return pimpl->read_data(k); }
bool StorageEngine::delete_data(const std::string& k) { return pimpl->delete_data(k); }
bool StorageEngine::write_document(const std::string& c, const std::string& d) { return pimpl->write_document(c, d); }
std::vector<std::string> StorageEngine::query_documents(const std::string& c, const std::map<std::string, std::string>& f) { return pimpl->query_documents(c, f); }
bool StorageEngine::create_schema(const std::string& s) { return pimpl->create_schema(s); }
bool StorageEngine::update_schema(const std::string& s) { return pimpl->update_schema(s); }
bool StorageEngine::create_index(const std::string& f) { return pimpl->create_index(f); }
bool StorageEngine::drop_index(const std::string& f) { return pimpl->drop_index(f); }
bool StorageEngine::begin_transaction() { return pimpl->begin_transaction(); }
bool StorageEngine::commit_transaction() { return pimpl->commit_transaction(); }
bool StorageEngine::rollback_transaction() { return pimpl->rollback_transaction(); }
std::string StorageEngine::get_last_error() const { return pimpl->error(); }
