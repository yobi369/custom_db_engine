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
void set_checkpoint_failure(bool enabled) {
#ifdef _WIN32
    _putenv_s("CUSTOMDB_TEST_FAIL_CHECKPOINT_WAL_OPEN", enabled ? "1" : "");
#else
    if (enabled) setenv("CUSTOMDB_TEST_FAIL_CHECKPOINT_WAL_OPEN", "1", 1);
    else unsetenv("CUSTOMDB_TEST_FAIL_CHECKPOINT_WAL_OPEN");
#endif
}
}

int main() {
    const std::string path = "storage_checkpoint_fault_test.json";
    std::error_code ec;
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    {
        StorageEngine db(path);
        for (int i = 0; i < 63; ++i)
            CHECK(db.write_data("key-" + std::to_string(i), "value-" + std::to_string(i)));

        // The 64th commit must remain successful even if checkpoint WAL cleanup
        // fails after the new snapshot has already been published.
        set_checkpoint_failure(true);
        CHECK(db.write_data("key-63", "value-63"));
        set_checkpoint_failure(false);
        CHECK(db.get_last_error().empty());
        CHECK(db.read_data("key-63") == "value-63");
    }

    // The failure is injected after snapshot publication. The old WAL is expected
    // to remain, and reopening must select the newer snapshot without replay damage.
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
        CHECK(recovered.write_data("after-failed-checkpoint", "still-writable"));
    }

    {
        StorageEngine reopened(path);
        CHECK(reopened.get_last_error().empty());
        CHECK(reopened.read_data("key-0") == "value-0");
        CHECK(reopened.read_data("key-63") == "value-63");
        CHECK(reopened.read_data("after-failed-checkpoint") == "still-writable");
        CHECK(reopened.check_integrity());
    }

    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);

    std::cout << "Checkpoint WAL-open fault recovery test passed\n";
    return 0;
}
