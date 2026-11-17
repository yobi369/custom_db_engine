#include "storage_engine.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\\n'; \
        return 1; \
    } \
} while (false)

int main() {
    const std::string path = "storage_temp_recovery_test.json";
    std::error_code ec;
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    {
        StorageEngine db(path);
        CHECK(db.create_database());
        for (int i = 0; i < 63; ++i) {
            CHECK(db.write_data("key-" + std::to_string(i),
                                "value-" + std::to_string(i)));
        }
    }

    // Model an abandoned temporary snapshot left by a process that stopped
    // before the atomic rename. The published snapshot and WAL remain authoritative.
    {
        std::ofstream abandoned(path + ".tmp", std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(abandoned));
        abandoned << "{ partially written snapshot";
        CHECK(static_cast<bool>(abandoned));
    }

    {
        StorageEngine recovered(path);
        CHECK(recovered.get_last_error().empty());
        for (int i = 0; i < 63; ++i) {
            CHECK(recovered.read_data("key-" + std::to_string(i)) ==
                  "value-" + std::to_string(i));
            CHECK(recovered.get_last_error().empty());
        }

        // The next commit reaches the checkpoint boundary. It must replace the
        // abandoned temp file, publish the valid snapshot, and clear the WAL.
        CHECK(recovered.write_data("key-63", "value-63"));
        CHECK(recovered.check_integrity());
    }

    CHECK(std::filesystem::file_size(path + ".wal", ec) == 0);
    CHECK(!ec);

    {
        StorageEngine reopened(path);
        CHECK(reopened.get_last_error().empty());
        CHECK(reopened.read_data("key-0") == "value-0");
        CHECK(reopened.read_data("key-63") == "value-63");
        CHECK(reopened.check_integrity());
    }

    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    std::cout << "Temporary snapshot recovery test passed\\n";
    return 0;
}
