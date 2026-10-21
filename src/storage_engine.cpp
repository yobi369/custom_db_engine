#include "storage_engine.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
json empty_state() {
    return json{{"format_version", 1}, {"generation", 0}, {"kv", json::object()},
                {"documents", json::object()}, {"schemas", json::object()}, {"indexes", json::array()}};
}
bool valid_name(const std::string& value) {
    return !value.empty() && value.find('\0') == std::string::npos;
}
bool valid_state(const json& value) {
    return value.is_object() &&
        value.value("format_version", 0) == 1 &&
        value.contains("generation") &&
        value["generation"].is_number_integer() &&
        value["generation"].get<long long>() >= 0 &&
        value.contains("kv") && value["kv"].is_object() &&
        value.contains("documents") && value["documents"].is_object() &&
        value.contains("schemas") && value["schemas"].is_object() &&
        value.contains("indexes") && value["indexes"].is_array();
}
std::uint64_t checksum(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ULL; }
    return hash;
}
std::string checksum_text(const std::string& bytes) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << checksum(bytes);
    return out.str();
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
        if (fs::exists(path_, ec) || fs::exists(wal_path(), ec)) return fail("Database already exists");
        if (ec) return fail("Cannot inspect database path: " + ec.message());
        state_ = empty_state();
        if (!checkpoint_locked(state_)) return false;
        last_error_.clear();
        return true;
    }
    bool delete_database() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_transaction_) return fail("Cannot delete database during a transaction");
        std::error_code ec1, ec2;
        const bool snapshot_removed = fs::remove(path_, ec1);
        const bool wal_removed = fs::remove(wal_path(), ec2);
        if (ec1 || ec2) return fail("Cannot delete database: " + (ec1 ? ec1.message() : ec2.message()));
        if (!snapshot_removed && !wal_removed) return fail("Database file does not exist");
        state_ = empty_state();
        wal_records_ = 0;
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
        if (load_failed_) return {};
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
        if (load_failed_) return result;
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
        if (load_failed_) return fail("Database failed to load; refusing transaction");
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
    std::string wal_path() const { return path_ + ".wal"; }
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
        if (load_failed_) return fail("Database failed to load; refusing to overwrite the existing file");
        if (in_transaction_) { transaction_state_ = std::move(next); last_error_.clear(); return true; }
        if (!persist_locked(next)) return false;
        state_ = std::move(next);
        last_error_.clear();
        return true;
    }
    bool write_snapshot_locked(const json& value) {
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
            if (!out) { out.close(); fs::remove(temporary, ec); return fail("Failed while writing database snapshot"); }
        }
        fs::rename(temporary, target, ec);
        if (ec) {
            std::error_code remove_ec;
            fs::remove(target, remove_ec);
            ec.clear();
            fs::rename(temporary, target, ec);
        }
        if (ec) { std::error_code cleanup_ec; fs::remove(temporary, cleanup_ec); return fail("Cannot publish database snapshot: " + ec.message()); }
        return true;
    }
    bool checkpoint_locked(const json& value) {
        if (!write_snapshot_locked(value)) return false;
        std::ofstream truncate(wal_path(), std::ios::binary | std::ios::trunc);
        if (!truncate) return fail("Snapshot saved, but could not truncate WAL");
        truncate.flush();
        if (!truncate) return fail("Snapshot saved, but failed while truncating WAL");
        wal_records_ = 0;
        return true;
    }
    bool persist_locked(json& value) {
        if (load_failed_) return fail("Database failed to load; refusing to overwrite the existing file");
        if (path_.empty()) return fail("Database path must not be empty");
        const auto current_generation = state_.value("generation", std::uint64_t{0});
        value["generation"] = current_generation + 1;
        const std::string payload = value.dump();
        const std::string record = std::to_string(value["generation"].get<std::uint64_t>()) + " " + checksum_text(payload) + " " + payload + "\n";
        {
            std::error_code ec;
            const fs::path target(path_);
            if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
            if (ec) return fail("Cannot create database directory: " + ec.message());
            std::ofstream wal(wal_path(), std::ios::binary | std::ios::app);
            if (!wal) return fail("Cannot open write-ahead log for appending");
            wal.write(record.data(), static_cast<std::streamsize>(record.size()));
            wal.flush();
            if (!wal) return fail("Failed while appending write-ahead log record");
        }
        ++wal_records_;
        // Checkpoint periodically. A valid WAL record remains the recovery source if checkpointing fails.
        if (wal_records_ >= 64) {
            const std::string previous_error = last_error_;
            if (!checkpoint_locked(value)) last_error_ = previous_error;
        }
        return true;
    }
    bool load_wal_locked() {
        std::ifstream in(wal_path(), std::ios::binary);
        if (!in) return true;
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t start = 0;
        while (true) {
            const std::size_t end = bytes.find('\n', start);
            if (end == std::string::npos) break; // Ignore an incomplete trailing record.
            const std::string line = bytes.substr(start, end - start);
            start = end + 1;
            if (line.empty()) continue;
            const auto first = line.find(' ');
            const auto second = first == std::string::npos ? first : line.find(' ', first + 1);
            if (first == std::string::npos || second == std::string::npos)
                return fail("Malformed write-ahead log record");
            const std::string generation_text = line.substr(0, first);
            const std::string expected_checksum = line.substr(first + 1, second - first - 1);
            const std::string payload = line.substr(second + 1);
            if (checksum_text(payload) != expected_checksum) return fail("Write-ahead log checksum mismatch");
            json recovered;
            try { recovered = json::parse(payload); }
            catch (const std::exception& e) { return fail(std::string("Invalid write-ahead log payload: ") + e.what()); }
            if (!valid_state(recovered)) return fail("Unsupported or malformed state in write-ahead log");
            std::uint64_t record_generation = 0;
            try { record_generation = static_cast<std::uint64_t>(std::stoull(generation_text)); }
            catch (...) { return fail("Invalid write-ahead log generation"); }
            if (record_generation != recovered["generation"].get<std::uint64_t>())
                return fail("Write-ahead log generation mismatch");
            ++wal_records_;
            if (record_generation > state_.value("generation", std::uint64_t{0})) state_ = std::move(recovered);
        }
        return true;
    }
    void load_locked() {
        if (path_.empty()) return;
        std::error_code ec;
        const bool exists = fs::exists(path_, ec);
        if (ec) { fail("Cannot inspect database path: " + ec.message()); return; }
        if (exists) {
            std::ifstream in(path_, std::ios::binary);
            if (!in) { fail("Cannot open database file"); return; }
            try {
                json loaded;
                in >> loaded;
                if (!valid_state(loaded)) { fail("Invalid or unsupported database file format"); return; }
                state_ = std::move(loaded);
            } catch (const std::exception& e) { fail(std::string("Cannot load database file: ") + e.what()); return; }
        }
        if (!load_wal_locked()) return;
        last_error_.clear();
    }
    std::string path_;
    mutable std::mutex mutex_;
    json state_;
    json transaction_state_;
    bool in_transaction_ = false;
    bool load_failed_ = false;
    std::size_t wal_records_ = 0;
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
