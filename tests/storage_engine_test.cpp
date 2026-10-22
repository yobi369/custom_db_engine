#include "storage_engine.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <iomanip>
#include <sstream>


namespace {
std::string test_checksum(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char value : bytes) { hash ^= value; hash *= 1099511628211ULL; }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
std::string test_wal_record(unsigned long long generation) {
    const std::string payload = "{\"format_version\":1,\"generation\":" +
        std::to_string(generation) +
        ",\"kv\":{},\"documents\":{},\"schemas\":{},\"indexes\":[]}";
    return std::to_string(generation) + " " + test_checksum(payload) + " " + payload + "\n";
}
}

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; return 1; } } while (false)
int main() {
    const std::string path = "storage_engine_test_db.json";
    const std::string backup = "storage_engine_test_backup.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    std::filesystem::remove(path + ".wal", ec);
    {
        StorageEngine db(path);
        {
            StorageEngine competing(path);
            CHECK(competing.get_last_error().find("already open") != std::string::npos);
            CHECK(!competing.create_database());
            CHECK(!competing.delete_database());
            CHECK(!competing.write_data("must_not_write", "x"));
        }
        CHECK(db.write_data("empty", ""));
        CHECK(db.read_data("empty").empty());
        CHECK(db.get_last_error().empty());
        CHECK(db.write_data("alpha", "one"));
        CHECK(db.write_data("alpha", "two"));
        CHECK(db.read_data("alpha") == "two");
        CHECK(db.begin_transaction());
        CHECK(!db.begin_transaction());
        CHECK(db.write_data("alpha", "transactional"));
        CHECK(db.read_data("alpha") == "transactional");
        CHECK(db.rollback_transaction());
        CHECK(db.read_data("alpha") == "two");
        CHECK(db.begin_transaction());
        CHECK(db.write_data("beta", "durable"));
        CHECK(db.commit_transaction());
        CHECK(db.read_data("beta") == "durable");
        CHECK(db.write_document("users", R"({"name":"Alice","age":30})"));
        CHECK(db.write_document("users", R"({"name":"Bob","age":25})"));
        const auto results = db.query_documents("users", {{"name", "Alice"}});
        CHECK(results.size() == 1);
        CHECK(results.front() == R"({"age":30,"name":"Alice"})");
        CHECK(!db.write_document("users", "not json"));
        CHECK(db.create_schema(R"({"collection":"users","type":"object"})"));
        CHECK(!db.create_schema(R"({"collection":"users","type":"object"})"));
        CHECK(db.update_schema(R"({"collection":"users","type":"object","version":2})"));
        CHECK(db.create_schema(R"({"collection":"validated","type":"object","required":["name"],"properties":{"name":{"type":"string"},"age":{"type":"integer"}},"additionalProperties":false})"));
        CHECK(!db.write_document("validated", R"({"age":5})"));
        CHECK(!db.write_document("validated", R"({"name":12,"age":5})"));
        CHECK(!db.write_document("validated", R"({"name":"Ada","extra":true})"));
        CHECK(db.write_document("validated", R"({"name":"Ada","age":5})"));
        CHECK(db.create_index("name"));
        CHECK(db.query_documents("users", {{"name", "Bob"}}).size() == 1);
        CHECK(db.write_document("users", R"({"name":"Carol","age":22})"));
        CHECK(db.query_documents("users", {{"name", "Carol"}}).size() == 1);
        CHECK(!db.create_index("name"));
        CHECK(db.drop_index("name"));
        CHECK(db.delete_data("beta"));
        CHECK(!db.delete_data("missing"));
        CHECK(db.check_integrity());
        CHECK(db.backup_database(backup));
        // A backup must never replace the lock pathname held by the active handle.
        CHECK(!db.backup_database(path + ".lock"));
        {
            StorageEngine still_locked(path);
            CHECK(still_locked.get_last_error().find("already open") != std::string::npos);
        }
        CHECK(db.write_data("alpha", "changed-after-backup"));
        CHECK(db.restore_database(backup));
        CHECK(db.read_data("alpha") == "two");
        CHECK(db.check_integrity());
    }
    { std::ofstream tail(path + ".wal", std::ios::binary | std::ios::app); tail << "incomplete-record"; }
    {
        StorageEngine reopened(path);
        CHECK(reopened.read_data("alpha") == "two");
        CHECK(reopened.read_data("beta").empty());
        CHECK(reopened.get_last_error().find("Key not found") != std::string::npos);
        CHECK(reopened.query_documents("users", {{"name", "Bob"}}).size() == 1);
        CHECK(reopened.write_data("after_torn_tail", "recovery-truncated-the-tail"));
    }
    {
        StorageEngine db(path);
        CHECK(db.begin_transaction());
        CHECK(db.write_data("never_committed", "x"));
        CHECK(db.rollback_transaction());
    }
    {
        StorageEngine reopened(path);
        CHECK(reopened.read_data("never_committed").empty());
    }
    {
        StorageEngine db(path);
        for (int i = 0; i < 70; ++i) {
            CHECK(db.write_data("bulk" + std::to_string(i), "value" + std::to_string(i)));
        }
    }
    {
        StorageEngine reopened(path);
        CHECK(reopened.read_data("bulk69") == "value69");
        CHECK(reopened.read_data("after_torn_tail") == "recovery-truncated-the-tail");
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    std::filesystem::remove(path + ".wal", ec);
    std::filesystem::remove(path + ".lock", ec);
    std::filesystem::remove(backup, ec);
    std::filesystem::remove(backup + ".tmp", ec);
    const std::string corrupt_path = "storage_engine_corrupt_test.json";
    std::filesystem::remove(corrupt_path, ec);
    std::filesystem::remove(corrupt_path + ".wal", ec);
    std::filesystem::remove(corrupt_path + ".lock", ec);
    {
        StorageEngine db(corrupt_path);
        CHECK(db.write_data("safe", "preserve"));
    }
    { std::ofstream corrupt(corrupt_path + ".wal", std::ios::binary | std::ios::app); corrupt << "not-a-valid-record\n"; }
    {
        StorageEngine damaged(corrupt_path);
        CHECK(!damaged.get_last_error().empty());
        CHECK(!damaged.write_data("unsafe", "must-not-overwrite"));
        CHECK(!damaged.create_database());
        CHECK(!damaged.delete_database());
    }
    std::filesystem::remove(corrupt_path, ec);
    std::filesystem::remove(corrupt_path + ".wal", ec);
    std::filesystem::remove(corrupt_path + ".tmp", ec);
    std::filesystem::remove(corrupt_path + ".lock", ec);

    const std::string malformed_snapshot = "storage_engine_malformed_snapshot.json";
    { std::ofstream snapshot(malformed_snapshot, std::ios::binary); snapshot << R"({"format_version":1,"generation":0,"kv":{"unsafe":42},"documents":{},"schemas":{},"indexes":[]})"; }
    {
        StorageEngine damaged(malformed_snapshot);
        CHECK(!damaged.get_last_error().empty());
        CHECK(!damaged.write_data("unsafe", "must-not-overwrite"));
        CHECK(!damaged.create_database());
        CHECK(!damaged.delete_database());
    }
    std::filesystem::remove(malformed_snapshot, ec);
    std::filesystem::remove(malformed_snapshot + ".tmp", ec);
    std::filesystem::remove(malformed_snapshot + ".wal", ec);
    std::filesystem::remove(malformed_snapshot + ".lock", ec);

    // Complete, checksummed WAL records must still have strictly increasing generations.
    // A valid checksum alone must not let an older state follow a newer state.
    const std::string unordered_wal_path = "storage_engine_unordered_wal.json";
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"}) std::filesystem::remove(unordered_wal_path + suffix, ec);
    {
        std::ofstream wal(unordered_wal_path + ".wal", std::ios::binary);
        wal << test_wal_record(2) << test_wal_record(1);
    }
    {
        StorageEngine damaged(unordered_wal_path);
        CHECK(damaged.get_last_error().find("strictly increasing") != std::string::npos);
        CHECK(!damaged.write_data("must_not_write", "x"));
    }
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"}) std::filesystem::remove(unordered_wal_path + suffix, ec);


    const std::string invalid_name_snapshot = "storage_engine_invalid_name_snapshot.json";
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"}) std::filesystem::remove(invalid_name_snapshot + suffix, ec);
    {
        std::ofstream snapshot(invalid_name_snapshot, std::ios::binary);
        snapshot << R"({"format_version":1,"generation":0,"kv":{"":"invalid-key"},"documents":{},"schemas":{},"indexes":[]})";
    }
    {
        StorageEngine damaged(invalid_name_snapshot);
        CHECK(!damaged.get_last_error().empty());
        CHECK(!damaged.write_data("must_not_write", "x"));
    }
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"}) std::filesystem::remove(invalid_name_snapshot + suffix, ec);

    std::cout << "All storage engine tests passed\n";
    return 0;
}
