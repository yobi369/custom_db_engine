#include "storage_engine.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <limits>
#include <set>
#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/file.h>
#endif
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
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
bool validate_schema_definition(const json& schema, std::string& error);
bool valid_state(const json& value) {
    try {
        if (!value.is_object() || !value.contains("format_version") ||
            !value["format_version"].is_number_integer() ||
            value["format_version"].get<int>() != 1 ||
            !value.contains("generation") ||
            !value["generation"].is_number_integer() ||
            !value.contains("kv") || !value["kv"].is_object() ||
            !value.contains("documents") || !value["documents"].is_object() ||
            !value.contains("schemas") || !value["schemas"].is_object() ||
            !value.contains("indexes") || !value["indexes"].is_array()) return false;

        std::uint64_t generation = 0;
        if (value["generation"].is_number_unsigned()) {
            generation = value["generation"].get<std::uint64_t>();
        } else {
            const auto signed_generation = value["generation"].get<std::int64_t>();
            if (signed_generation < 0) return false;
            generation = static_cast<std::uint64_t>(signed_generation);
        }
        if (generation == std::numeric_limits<std::uint64_t>::max()) return false;

        for (auto it = value["kv"].begin(); it != value["kv"].end(); ++it)
            if (!it.value().is_string()) return false;

        for (auto it = value["documents"].begin(); it != value["documents"].end(); ++it) {
            if (!it.value().is_array()) return false;
            for (const auto& document : it.value())
                if (!document.is_object()) return false;
        }

        for (auto it = value["schemas"].begin(); it != value["schemas"].end(); ++it) {
            if (!it.value().is_object()) return false;
            std::string schema_error;
            if (!validate_schema_definition(it.value(), schema_error)) return false;
            if (it.value().contains("collection") &&
                (!it.value()["collection"].is_string() ||
                 it.value()["collection"].get<std::string>() != it.key())) return false;
        }

        std::set<std::string> index_names;
        for (const auto& index : value["indexes"]) {
            if (!index.is_string()) return false;
            const auto name = index.get<std::string>();
            if (!valid_name(name) || !index_names.insert(name).second) return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
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
bool durable_flush(std::FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}
bool sync_parent_directory(const fs::path& target) {
#ifdef _WIN32
    (void)target;
    return true; // Snapshot rename uses MOVEFILE_WRITE_THROUGH on Windows.
#else
    const fs::path directory = target.has_parent_path() ? target.parent_path() : fs::path(".");
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(directory.c_str(), flags);
    if (fd < 0) return false;
    const bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
#endif
}
bool supported_json_type(const json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return false;
}
bool validate_schema_definition(const json& schema, std::string& error) {
    if (schema.contains("type") && (!schema["type"].is_string() || schema["type"] != "object")) {
        error = "top-level schema type must be object";
        return false;
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) { error = "required must be an array"; return false; }
        for (const auto& field : schema["required"]) {
            if (!field.is_string()) { error = "required entries must be strings"; return false; }
        }
    }
    if (schema.contains("properties")) {
        if (!schema["properties"].is_object()) { error = "properties must be an object"; return false; }
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            if (!it.value().is_object()) { error = "each property schema must be an object"; return false; }
            if (it.value().contains("type")) {
                if (!it.value()["type"].is_string()) { error = "property type must be a string"; return false; }
                const auto type = it.value()["type"].get<std::string>();
                if (type != "object" && type != "array" && type != "string" && type != "number" &&
                    type != "integer" && type != "boolean" && type != "null") {
                    error = "unsupported property type: " + type;
                    return false;
                }
            }
        }
    }
    if (schema.contains("additionalProperties") && !schema["additionalProperties"].is_boolean()) {
        error = "additionalProperties must be a boolean";
        return false;
    }
    return true;
}
bool validate_document_against_schema(const json& document, const json& schema, std::string& error) {
    if (schema.contains("required")) {
        for (const auto& required : schema["required"]) {
            const auto name = required.get<std::string>();
            if (!document.contains(name)) { error = "missing required field: " + name; return false; }
        }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        const auto& properties = schema["properties"];
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (!document.contains(it.key()) || !it.value().contains("type")) continue;
            const auto type = it.value()["type"].get<std::string>();
            if (!supported_json_type(document[it.key()], type)) {
                error = "field '" + it.key() + "' must have type " + type;
                return false;
            }
        }
        if (schema.value("additionalProperties", true) == false) {
            for (auto it = document.begin(); it != document.end(); ++it) {
                if (!properties.contains(it.key())) { error = "unexpected field: " + it.key(); return false; }
            }
        }
    }
    return true;
}
}
class StorageEngine::Impl {
public:
    explicit Impl(std::string path, bool acquire_process_lock = true) : path_(std::move(path)), state_(empty_state()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (path_.empty()) {
            fail("Database path must not be empty");
            load_failed_ = true;
            return;
        }
        if (acquire_process_lock && !acquire_lock_locked()) {
            load_failed_ = true;
            return;
        }
        load_locked();
        load_failed_ = !last_error_.empty();
    }
    ~Impl() {
#ifdef _WIN32
        if (lock_handle_ != INVALID_HANDLE_VALUE) {
            if (lock_held_) UnlockFileEx(lock_handle_, 0, MAXDWORD, MAXDWORD, &lock_overlapped_);
            CloseHandle(lock_handle_);
        }
#else
        if (lock_fd_ >= 0) {
            if (lock_held_) (void)::flock(lock_fd_, LOCK_UN);
            (void)::close(lock_fd_);
        }
#endif
    }
    bool create_database() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (load_failed_) return fail("Database failed to load or acquire its exclusive lock; refusing to create or overwrite files");
        if (path_.empty()) return fail("Database path must not be empty");
        std::error_code ec;
        if (fs::exists(path_, ec) || fs::exists(wal_path(), ec)) return fail("Database already exists");
        if (ec) return fail("Cannot inspect database path: " + ec.message());
        state_ = empty_state();
        index_cache_.clear();
        load_failed_ = false;
        if (!checkpoint_locked(state_)) return false;
        last_error_.clear();
        return true;
    }
    bool delete_database() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (load_failed_) return fail("Database failed to load or acquire its exclusive lock; refusing to delete files");
        if (in_transaction_) return fail("Cannot delete database during a transaction");
        std::error_code ec1, ec2;
        const bool snapshot_removed = fs::remove(path_, ec1);
        const bool wal_removed = fs::remove(wal_path(), ec2);
        if (ec1 || ec2) return fail("Cannot delete database: " + (ec1 ? ec1.message() : ec2.message()));
        if (!snapshot_removed && !wal_removed) return fail("Database file does not exist");
        state_ = empty_state();
        wal_records_ = 0;
        index_cache_.clear();
        load_failed_ = false;
        last_error_.clear();
        return true;
    }
    bool backup_database(const std::string& backup_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (load_failed_) return fail("Database failed to load or acquire its exclusive lock; refusing to create a backup");
        if (in_transaction_) return fail("Cannot back up an uncommitted transaction");
        if (backup_path.empty()) return fail("Backup path must not be empty");
        std::error_code ec;
        const auto target = fs::absolute(backup_path, ec).lexically_normal();
        if (ec) return fail("Cannot resolve backup path: " + ec.message());
        const auto active = fs::absolute(path_, ec).lexically_normal();
        if (ec) return fail("Cannot resolve database path: " + ec.message());
        const auto active_wal = fs::absolute(wal_path(), ec).lexically_normal();
        if (ec) return fail("Cannot resolve WAL path: " + ec.message());
        const auto active_snapshot_tmp = fs::absolute(path_ + ".tmp", ec).lexically_normal();
        if (ec) return fail("Cannot resolve temporary database path: " + ec.message());
        const auto active_lock = fs::absolute(path_ + ".lock", ec).lexically_normal();
        if (ec) return fail("Cannot resolve database lock path: " + ec.message());
        // Replacing the lock pathname would detach the lock held by this process:
        // another process could then lock a new inode and write concurrently.
        if (target == active || target == active_wal || target == active_snapshot_tmp || target == active_lock)
            return fail("Backup path must not overlap the active database, WAL, snapshot temp, or lock file");
        if (!write_snapshot_to_path_locked(target, state_)) return false;
        last_error_.clear();
        return true;
    }
    bool restore_database(const std::string& backup_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_transaction_) return fail("Cannot restore during a transaction");
        if (load_failed_) return fail("Cannot restore over an unreadable database; repair or remove the active files first");
        if (backup_path.empty()) return fail("Backup path must not be empty");
        std::error_code ec;
        const auto source = fs::absolute(backup_path, ec).lexically_normal();
        if (ec) return fail("Cannot resolve backup path: " + ec.message());
        const auto active = fs::absolute(path_, ec).lexically_normal();
        if (ec) return fail("Cannot resolve database path: " + ec.message());
        if (source == active || source == fs::absolute(wal_path(), ec).lexically_normal())
            return fail("Backup path must be separate from the active database");
        std::ifstream in(source, std::ios::binary);
        if (!in) return fail("Cannot open backup file");
        json restored;
        try { in >> restored; }
        catch (const std::exception& e) { return fail(std::string("Cannot parse backup file: ") + e.what()); }
        if (!valid_state(restored)) return fail("Backup has an invalid or unsupported database format");
        const auto current_generation = state_.value("generation", std::uint64_t{0});
        const auto backup_generation = restored.value("generation", std::uint64_t{0});
        const auto base_generation = std::max(current_generation, backup_generation);
        if (base_generation >= std::numeric_limits<std::uint64_t>::max() - 1)
            return fail("Database generation counter is exhausted; restore cannot safely advance it");
        restored["generation"] = base_generation + 1;
        if (!write_snapshot_locked(restored)) return false;
        state_ = std::move(restored);
        transaction_state_ = json();
        in_transaction_ = false;
        // Older WAL records are ignored because the restored snapshot has a newer generation.
        last_error_.clear();
        return true;
    }
    bool check_integrity() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_transaction_) return fail("Cannot check on-disk integrity during a transaction");
        if (load_failed_) return fail("Database load failed: " + last_error_);
        Impl verifier(path_, false);
        if (verifier.load_failed_) return fail("On-disk integrity check failed: " + verifier.last_error_);
        if (verifier.state_ != state_) return fail("On-disk database state differs from the loaded state");
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
        auto schema_it = next["schemas"].find(collection);
        if (schema_it != next["schemas"].end()) {
            std::string validation_error;
            if (!validate_document_against_schema(document, *schema_it, validation_error))
                return fail("Schema validation failed: " + validation_error);
        }
        if (!next["documents"].contains(collection)) next["documents"][collection] = json::array();
        next["documents"][collection].push_back(document);
        index_cache_.erase(collection);
        return finish_write_locked(std::move(next));
    }
    std::vector<std::string> query_documents(const std::string& collection, const std::map<std::string, std::string>& filters) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        if (load_failed_) return result;
        const json& view = readable_state_locked();
        auto coll = view["documents"].find(collection);
        if (coll == view["documents"].end()) { last_error_.clear(); return result; }
        const json& documents = *coll;
        std::vector<std::size_t> candidates;
        bool indexed_candidates = false;
        for (const auto& filter : filters) {
            bool index_enabled = false;
            for (const auto& field_name : view["indexes"]) {
                if (field_name == filter.first) { index_enabled = true; break; }
            }
            if (!index_enabled) continue;
            auto& collection_indexes = index_cache_[collection];
            auto cached = collection_indexes.find(filter.first);
            if (cached == collection_indexes.end()) {
                CachedIndex built;
                for (std::size_t i = 0; i < documents.size(); ++i) {
                    auto field = documents[i].find(filter.first);
                    if (field == documents[i].end()) continue;
                    const std::string value = field->is_string() ? field->get<std::string>() : field->dump();
                    built[value].push_back(i);
                }
                cached = collection_indexes.emplace(filter.first, std::move(built)).first;
            }
            auto matching = cached->second.find(filter.second);
            if (matching == cached->second.end()) { last_error_.clear(); return result; }
            candidates = matching->second;
            indexed_candidates = true;
            break;
        }
        if (!indexed_candidates) {
            candidates.reserve(documents.size());
            for (std::size_t i = 0; i < documents.size(); ++i) candidates.push_back(i);
        }
        for (const auto i : candidates) {
            if (i >= documents.size()) continue;
            const auto& document = documents[i];
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
        index_cache_.clear();
        last_error_.clear();
        return true;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }
private:
    std::string wal_path() const { return path_ + ".wal"; }
    bool acquire_lock_locked() {
        const fs::path lock_path(path_ + ".lock");
        std::error_code ec;
        if (lock_path.has_parent_path()) fs::create_directories(lock_path.parent_path(), ec);
        if (ec) return fail("Cannot create database directory for lock: " + ec.message());
#ifdef _WIN32
        lock_handle_ = CreateFileA(lock_path.string().c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock_handle_ == INVALID_HANDLE_VALUE)
            return fail("Cannot open database lock file (Windows error " + std::to_string(GetLastError()) + ")");
        if (!LockFileEx(lock_handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0, MAXDWORD, MAXDWORD, &lock_overlapped_))
            return fail("Database is already open by another StorageEngine instance (Windows error " + std::to_string(GetLastError()) + ")");
#else
        lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
        if (lock_fd_ < 0) return fail("Cannot open database lock file: " + std::string(std::strerror(errno)));
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
            return fail("Database is already open by another StorageEngine instance: " + std::string(std::strerror(errno)));
#endif
        lock_held_ = true;
        return true;
    }
    bool truncate_wal_locked(std::size_t length) {
        std::FILE* wal = std::fopen(wal_path().c_str(), "r+b");
        if (!wal) return fail("Cannot open write-ahead log to remove incomplete trailing record");
#ifdef _WIN32
        const bool resized = _chsize_s(_fileno(wal), static_cast<__int64>(length)) == 0;
#else
        const bool resized = ::ftruncate(::fileno(wal), static_cast<off_t>(length)) == 0;
#endif
        const bool flushed = resized && durable_flush(wal);
        const bool closed = std::fclose(wal) == 0;
        if (!resized || !flushed || !closed)
            return fail("Cannot durably remove incomplete trailing WAL record");
        return true;
    }
    bool fail(std::string message) { last_error_ = std::move(message); return false; }
    bool set_schema(const std::string& definition, bool create) {
        std::lock_guard<std::mutex> lock(mutex_);
        json schema;
        try { schema = json::parse(definition); }
        catch (const std::exception& e) { return fail(std::string("Invalid schema JSON: ") + e.what()); }
        if (!schema.is_object()) return fail("Schema definition must be a JSON object");
        std::string schema_error;
        if (!validate_schema_definition(schema, schema_error)) return fail("Invalid schema: " + schema_error);
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
        return write_snapshot_to_path_locked(fs::path(path_), value);
    }
    bool write_snapshot_to_path_locked(const fs::path& target, const json& value) {
        std::error_code ec;
        if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
        if (ec) return fail("Cannot create database directory: " + ec.message());
        const fs::path temporary = target.string() + ".tmp";
        const std::string bytes = value.dump(2) + "\n";
        std::FILE* out = std::fopen(temporary.string().c_str(), "wb");
        if (!out) return fail("Cannot open temporary database file for writing");
        const bool written = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
        const bool flushed = written && durable_flush(out);
        const bool closed = std::fclose(out) == 0;
        if (!written || !flushed || !closed) {
            std::error_code cleanup_ec;
            fs::remove(temporary, cleanup_ec);
            return fail("Failed to write and durably flush database snapshot");
        }
#ifdef _WIN32
        if (!MoveFileExA(temporary.string().c_str(), target.string().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code cleanup_ec;
            fs::remove(temporary, cleanup_ec);
            return fail("Cannot publish database snapshot (Windows error " + std::to_string(GetLastError()) + ")");
        }
#else
        fs::rename(temporary, target, ec);
        if (ec) {
            std::error_code cleanup_ec;
            fs::remove(temporary, cleanup_ec);
            return fail("Cannot publish database snapshot: " + ec.message());
        }
        if (!sync_parent_directory(target)) return fail("Snapshot renamed but parent directory sync failed");
#endif
        return true;
    }
    bool checkpoint_locked(const json& value) {
        if (!write_snapshot_locked(value)) return false;
        std::FILE* wal = std::fopen(wal_path().c_str(), "wb");
        if (!wal) return fail("Snapshot saved, but could not truncate WAL");
        const bool flushed = durable_flush(wal);
        const bool closed = std::fclose(wal) == 0;
        if (!flushed || !closed) return fail("Snapshot saved, but failed to durably truncate WAL");
        if (!sync_parent_directory(fs::path(wal_path()))) return fail("WAL truncated but parent directory sync failed");
        wal_records_ = 0;
        return true;
    }
    bool persist_locked(json& value) {
        if (load_failed_) return fail("Database failed to load; refusing to overwrite the existing file");
        if (path_.empty()) return fail("Database path must not be empty");
        const auto current_generation = state_.value("generation", std::uint64_t{0});
        if (current_generation >= std::numeric_limits<std::uint64_t>::max() - 1)
            return fail("Database generation counter is exhausted; refusing to wrap the generation");
        value["generation"] = current_generation + 1;
        const std::string payload = value.dump();
        const std::string record = std::to_string(value["generation"].get<std::uint64_t>()) + " " + checksum_text(payload) + " " + payload + "\n";
        {
            std::error_code ec;
            const fs::path target(path_);
            if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
            if (ec) return fail("Cannot create database directory: " + ec.message());
            const bool wal_existed = fs::exists(wal_path(), ec);
            if (ec) return fail("Cannot inspect write-ahead log: " + ec.message());
            std::FILE* wal = std::fopen(wal_path().c_str(), "ab");
            if (!wal) return fail("Cannot open write-ahead log for appending");
            const bool written = std::fwrite(record.data(), 1, record.size(), wal) == record.size();
            const bool flushed = written && durable_flush(wal);
            const bool closed = std::fclose(wal) == 0;
            if (!written || !flushed || !closed) {
                // A failed append may have left a partial record at EOF. Continuing to
                // append in this handle could turn a recoverable torn tail into a
                // malformed complete record. Poison the handle and require reopen/recovery.
                load_failed_ = true;
                return fail("Failed to append and durably flush write-ahead log record; handle disabled until reopen");
            }
            if (!wal_existed && !sync_parent_directory(fs::path(wal_path()))) {
                // The record may be present but its directory entry may not be durable.
                // Do not acknowledge later writes from a handle with uncertain durability.
                load_failed_ = true;
                return fail("WAL record written but parent directory sync failed; handle disabled until reopen");
            }
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
        if (!in) {
            std::error_code ec;
            if (!fs::exists(wal_path(), ec) && !ec) return true;
            return fail("Cannot open write-ahead log for recovery");
        }
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::size_t start = 0;
        std::uint64_t previous_record_generation = 0;
        bool saw_record = false;
        while (true) {
            const std::size_t end = bytes.find('\n', start);
            if (end == std::string::npos) {
                // Remove the torn tail before accepting future appends; otherwise the next
                // valid record would be concatenated to the damaged bytes.
                if (start < bytes.size() && !truncate_wal_locked(start)) return false;
                break;
            }
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
            try {
                std::size_t parsed = 0;
                record_generation = static_cast<std::uint64_t>(std::stoull(generation_text, &parsed));
                if (parsed != generation_text.size() || generation_text.empty() ||
                    generation_text.front() == '-' || generation_text.front() == '+')
                    return fail("Invalid write-ahead log generation");
            } catch (...) { return fail("Invalid write-ahead log generation"); }
            if (record_generation != recovered["generation"].get<std::uint64_t>())
                return fail("Write-ahead log generation mismatch");
            if (saw_record && record_generation <= previous_record_generation)
                return fail("Write-ahead log generations are not strictly increasing");
            previous_record_generation = record_generation;
            saw_record = true;
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
    using CachedIndex = std::map<std::string, std::vector<std::size_t>>;
    std::string path_;
#ifdef _WIN32
    HANDLE lock_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED lock_overlapped_{};
#else
    int lock_fd_ = -1;
#endif
    bool lock_held_ = false;
    mutable std::mutex mutex_;
    std::map<std::string, std::map<std::string, CachedIndex>> index_cache_;
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
bool StorageEngine::backup_database(const std::string& p) { return pimpl->backup_database(p); }
bool StorageEngine::restore_database(const std::string& p) { return pimpl->restore_database(p); }
bool StorageEngine::check_integrity() { return pimpl->check_integrity(); }
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
