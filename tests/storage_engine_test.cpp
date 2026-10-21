#include "storage_engine.h"
#include <filesystem>
#include <fstream>
#include <iostream>

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
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    std::filesystem::remove(path + ".wal", ec);
    std::filesystem::remove(backup, ec);
    std::filesystem::remove(backup + ".tmp", ec);
    std::cout << "All storage engine tests passed\n";
    return 0;
}
