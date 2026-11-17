#include "storage_engine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
        return 1; \
    } \
} while (false)

namespace {
void set_publish_failure(bool enabled) {
#ifdef _WIN32
    _putenv_s("CUSTOMDB_TEST_FAIL_SNAPSHOT_PUBLISH", enabled ? "1" : "");
#else
    if (enabled) setenv("CUSTOMDB_TEST_FAIL_SNAPSHOT_PUBLISH", "1", 1);
    else unsetenv("CUSTOMDB_TEST_FAIL_SNAPSHOT_PUBLISH");
#endif
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
}

int main() {
    const std::string path = "storage_snapshot_publish_fault_test.json";
    std::error_code ec;
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    std::string previous_snapshot;
    {
        StorageEngine db(path);
        for (int i = 0; i < 63; ++i)
            CHECK(db.write_data("key-" + std::to_string(i), "value-" + std::to_string(i)));

        previous_snapshot = read_file(path);
        CHECK(!previous_snapshot.empty());

        // Fail before atomic replacement. The write itself has already been
        // durably committed to the WAL and must remain recoverable.
        set_publish_failure(true);
        CHECK(db.write_data("key-63", "value-63"));
        set_publish_failure(false);

        CHECK(read_file(path) == previous_snapshot);
        CHECK(!std::filesystem::exists(path + ".tmp"));
        CHECK(db.read_data("key-63") == "value-63");
        CHECK(db.get_last_error().empty());
    }

    CHECK(std::filesystem::file_size(path + ".wal", ec) > 0);
    CHECK(!ec);

    {
        StorageEngine recovered(path);
        CHECK(recovered.get_last_error().empty());
        for (int i = 0; i < 64; ++i) {
            CHECK(recovered.read_data("key-" + std::to_string(i)) ==
                  "value-" + std::to_string(i));
            CHECK(recovered.get_last_error().empty());
        }
        CHECK(recovered.check_integrity());
        CHECK(recovered.write_data("after-publication-failure", "recovered"));
    }

    {
        StorageEngine reopened(path);
        CHECK(reopened.get_last_error().empty());
        CHECK(reopened.read_data("key-0") == "value-0");
        CHECK(reopened.read_data("key-63") == "value-63");
        CHECK(reopened.read_data("after-publication-failure") == "recovered");
        CHECK(reopened.check_integrity());
    }

    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    std::cout << "Snapshot publication fault recovery test passed\n";
    return 0;
}
