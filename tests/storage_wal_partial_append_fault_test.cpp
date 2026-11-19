#include "storage_engine.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
        return 1; \
    } \
} while (false)

namespace {
void set_partial_append_failure(bool enabled) {
#ifdef _WIN32
    _putenv_s("CUSTOMDB_TEST_FAIL_WAL_APPEND_PARTIAL", enabled ? "1" : "");
#else
    if (enabled) setenv("CUSTOMDB_TEST_FAIL_WAL_APPEND_PARTIAL", "1", 1);
    else unsetenv("CUSTOMDB_TEST_FAIL_WAL_APPEND_PARTIAL");
#endif
}
}

int main() {
    const std::string path = "storage_wal_partial_append_fault_test.json";
    std::error_code ec;
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    {
        StorageEngine db(path);
        for (int i = 0; i < 64; ++i)
            CHECK(db.write_data("stable-" + std::to_string(i), "value-" + std::to_string(i)));

        CHECK(std::filesystem::exists(path));

        // Keep valid, committed WAL records after the snapshot. Recovery must
        // replay these records before discarding the incomplete final record.
        for (int i = 0; i < 5; ++i)
            CHECK(db.write_data("wal-" + std::to_string(i), "committed-" + std::to_string(i)));

        set_partial_append_failure(true);
        CHECK(!db.write_data("must-not-commit", "interrupted"));
        set_partial_append_failure(false);
        CHECK(!db.get_last_error().empty());

        // The failed handle must refuse further writes until a clean reopen.
        CHECK(!db.write_data("another-write", "blocked"));
    }

    CHECK(std::filesystem::file_size(path + ".wal", ec) > 0);
    CHECK(!ec);

    {
        StorageEngine recovered(path);
        CHECK(recovered.get_last_error().empty());
        for (int i = 0; i < 64; ++i) {
            CHECK(recovered.read_data("stable-" + std::to_string(i)) ==
                  "value-" + std::to_string(i));
            CHECK(recovered.get_last_error().empty());
        }
        for (int i = 0; i < 5; ++i) {
            CHECK(recovered.read_data("wal-" + std::to_string(i)) ==
                  "committed-" + std::to_string(i));
            CHECK(recovered.get_last_error().empty());
        }
        CHECK(recovered.read_data("must-not-commit").empty());
        CHECK(!recovered.get_last_error().empty());
        CHECK(recovered.check_integrity());
        CHECK(recovered.write_data("after-torn-tail", "recovered"));
    }

    {
        StorageEngine reopened(path);
        CHECK(reopened.get_last_error().empty());
        CHECK(reopened.read_data("stable-0") == "value-0");
        CHECK(reopened.read_data("wal-4") == "committed-4");
        CHECK(reopened.read_data("after-torn-tail") == "recovered");
        CHECK(reopened.check_integrity());
    }

    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    std::cout << "Partial WAL append fault recovery test passed\n";
    return 0;
}
