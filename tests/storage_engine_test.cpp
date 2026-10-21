#include "storage_engine.h"
#include <cassert>
#include <filesystem>
#include <iostream>
int main() {
    const std::string path = "storage_engine_test_db.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    {
        StorageEngine db(path);
        assert(db.write_data("empty", ""));
        assert(db.read_data("empty").empty());
        assert(db.get_last_error().empty());
        assert(db.write_data("alpha", "one"));
        assert(db.write_data("alpha", "two"));
        assert(db.read_data("alpha") == "two");
        assert(db.begin_transaction());
        assert(!db.begin_transaction());
        assert(db.write_data("alpha", "transactional"));
        assert(db.read_data("alpha") == "transactional");
        assert(db.rollback_transaction());
        assert(db.read_data("alpha") == "two");
        assert(db.begin_transaction());
        assert(db.write_data("beta", "durable"));
        assert(db.commit_transaction());
        assert(db.read_data("beta") == "durable");
        assert(db.write_document("users", R"({"name":"Alice","age":30})"));
        assert(db.write_document("users", R"({"name":"Bob","age":25})"));
        const auto results = db.query_documents("users", {{"name", "Alice"}});
        assert(results.size() == 1);
        assert(results.front() == R"({"age":30,"name":"Alice"})");
        assert(!db.write_document("users", "not json"));
        assert(db.create_schema(R"({"collection":"users","type":"object"})"));
        assert(!db.create_schema(R"({"collection":"users","type":"object"})"));
        assert(db.update_schema(R"({"collection":"users","type":"object","version":2})"));
        assert(db.create_index("name"));
        assert(!db.create_index("name"));
        assert(db.drop_index("name"));
        assert(db.delete_data("beta"));
        assert(!db.delete_data("missing"));
    }
    {
        StorageEngine reopened(path);
        assert(reopened.read_data("alpha") == "two");
        assert(reopened.read_data("beta").empty());
        assert(reopened.get_last_error().find("Key not found") != std::string::npos);
        assert(reopened.query_documents("users", {{"name", "Bob"}}).size() == 1);
    }
    {
        StorageEngine db(path);
        assert(db.begin_transaction());
        assert(db.write_data("never_committed", "x"));
        assert(db.rollback_transaction());
    }
    {
        StorageEngine reopened(path);
        assert(reopened.read_data("never_committed").empty());
    }
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    std::cout << "All storage engine tests passed\n";
    return 0;
}
