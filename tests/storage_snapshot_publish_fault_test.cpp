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
    const std::string backup = "storage_snapshot_publish_fault_test.backup.json";
    std::error_code ec;
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);
    std::filesystem::remove(backup, ec);
    std::filesystem::remove(backup + ".tmp", ec);

    std::string previous_snapshot;
    {
        StorageEngine db(path);
        // Establish a known-good published snapshot first.
        for (int i = 0; i < 64; ++i)
            CHECK(db.write_data("key-" + std::to_string(i), "value-" + std::to_string(i)));

        // Accumulate another checkpoint interval while preserving that snapshot.
        for (int i = 64; i < 127; ++i)
            CHECK(db.write_data("key-" + std::to_string(i), "value-" + std::to_string(i)));

        previous_snapshot = read_file(path);
        CHECK(!previous_snapshot.empty());

        // Fail before atomic replacement. The write itself has already been
        // durably committed to the WAL and must remain recoverable.
        set_publish_failure(true);
        CHECK(db.write_data("key-127", "value-127"));
        set_publish_failure(false);

        CHECK(read_file(path) == previous_snapshot);
        CHECK(!std::filesystem::exists(path + ".tmp"));
        CHECK(db.read_data("key-127") == "value-127");
        CHECK(db.get_last_error().empty());
    }

    CHECK(std::filesystem::file_size(path + ".wal", ec) > 0);
    CHECK(!ec);

    {
        StorageEngine recovered(path);
        CHECK(recovered.get_last_error().empty());
        for (int i = 0; i < 128; ++i) {
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
        CHECK(reopened.read_data("key-127") == "value-127");
        CHECK(reopened.read_data("after-publication-failure") == "recovered");
        CHECK(reopened.check_integrity());

        // A failed backup publication must not leave a partial backup artifact.
        set_publish_failure(true);
        CHECK(!reopened.backup_database(backup));
        set_publish_failure(false);
        CHECK(!std::filesystem::exists(backup));
        CHECK(!std::filesystem::exists(backup + ".tmp"));
        CHECK(reopened.check_integrity());

        // A restore publication failure must not replace the current snapshot.
        // The failed handle is deliberately disabled until a clean reopen.
        CHECK(reopened.backup_database(backup));
        CHECK(reopened.write_data("key-0", "mutated-before-restore"));
        const std::string active_snapshot = read_file(path);
        CHECK(!active_snapshot.empty());
        set_publish_failure(true);
        CHECK(!reopened.restore_database(backup));
        set_publish_failure(false);
        CHECK(read_file(path) == active_snapshot);
        CHECK(!std::filesystem::exists(path + ".tmp"));
        CHECK(reopened.read_data("key-0").empty());
        CHECK(!reopened.get_last_error().empty());
    }

    {
        StorageEngine recovered_after_failed_restore(path);
        CHECK(recovered_after_failed_restore.get_last_error().empty());
        CHECK(recovered_after_failed_restore.read_data("key-0") == "mutated-before-restore");
        CHECK(recovered_after_failed_restore.read_data("key-127") == "value-127");
        CHECK(recovered_after_failed_restore.read_data("after-publication-failure") == "recovered");
        CHECK(recovered_after_failed_restore.check_integrity());

        // A subsequent clean restore must still work and publish the backup state.
        CHECK(recovered_after_failed_restore.restore_database(backup));
        CHECK(recovered_after_failed_restore.read_data("key-0") == "value-0");
        CHECK(recovered_after_failed_restore.read_data("key-127") == "value-127");
        CHECK(recovered_after_failed_restore.check_integrity());
    }

    {
        StorageEngine final_reopen(path);
        CHECK(final_reopen.get_last_error().empty());
        CHECK(final_reopen.read_data("key-0") == "value-0");
        CHECK(final_reopen.read_data("key-127") == "value-127");
        CHECK(final_reopen.check_integrity());
    }

    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"})
        std::filesystem::remove(path + suffix, ec);
    std::filesystem::remove(backup, ec);
    std::filesystem::remove(backup + ".tmp", ec);

    std::cout << "Snapshot publication fault recovery test passed\n";
    return 0;
}
