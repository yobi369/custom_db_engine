#include "storage_engine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

int main(int argc, char** argv) {
    std::size_t record_count = 250;
    try {
        if (argc > 1) {
            const auto parsed = std::stoull(argv[1]);
            if (parsed == 0 || parsed > 100000) {
                std::cerr << "Record count must be between 1 and 100000.\n";
                return 2;
            }
            record_count = static_cast<std::size_t>(parsed);
        }
        if (argc > 2) {
            std::cerr << "Usage: custom_db_engine_benchmark [record-count]\n";
            return 2;
        }
    } catch (...) {
        std::cerr << "Usage: custom_db_engine_benchmark [record-count]\n";
        return 2;
    }

    const auto unique = std::to_string(Clock::now().time_since_epoch().count());
    const fs::path work_dir = fs::current_path() / ("custom_db_engine_benchmark_" + unique);
    std::error_code ec;
    if (!fs::create_directory(work_dir, ec) || ec) {
        std::cerr << "Cannot create isolated benchmark directory: " << ec.message() << '\n';
        return 1;
    }

    const fs::path database_path = work_dir / "benchmark.json";
    std::vector<double> write_latencies;
    write_latencies.reserve(record_count);
    double read_ms = 0.0;
    double reopen_ms = 0.0;
    std::uintmax_t snapshot_bytes = 0;
    std::uintmax_t wal_bytes = 0;
    bool success = false;
    std::string failure;

    // Defer cleanup until all StorageEngine handles have released their locks.
    {
        StorageEngine database(database_path.string());
        if (!database.create_database()) {
            failure = "Database creation failed: " + database.get_last_error();
        } else {
            const std::string value(128, 'x');
            for (std::size_t i = 0; i < record_count; ++i) {
                const auto start = Clock::now();
                if (!database.write_data("key-" + std::to_string(i), value)) {
                    failure = "Write failed at record " + std::to_string(i) + ": " + database.get_last_error();
                    break;
                }
                write_latencies.push_back(milliseconds(Clock::now() - start));
            }

            if (failure.empty()) {
                const auto read_start = Clock::now();
                for (std::size_t i = 0; i < record_count; ++i) {
                    if (database.read_data("key-" + std::to_string(i)) != value) {
                        failure = "Read verification failed at record " + std::to_string(i) + ": " + database.get_last_error();
                        break;
                    }
                }
                read_ms = milliseconds(Clock::now() - read_start);
                if (failure.empty() && !database.check_integrity())
                    failure = "Integrity check failed: " + database.get_last_error();
            }
        }
    }

    if (failure.empty()) {
        const auto start = Clock::now();
        {
            StorageEngine reopened(database_path.string());
            reopen_ms = milliseconds(Clock::now() - start);
            if (!reopened.get_last_error().empty() || !reopened.check_integrity()) {
                failure = "Reopen verification failed: " + reopened.get_last_error();
            } else {
                const std::string expected(128, 'x');
                for (std::size_t i = 0; i < record_count; ++i) {
                    if (reopened.read_data("key-" + std::to_string(i)) != expected) {
                        failure = "Persistence verification failed at record " + std::to_string(i);
                        break;
                    }
                }
            }
        }
        if (failure.empty()) {
            snapshot_bytes = fs::file_size(database_path, ec);
            if (ec) {
                failure = "Cannot measure snapshot size: " + ec.message();
                ec.clear();
            }
            if (failure.empty()) {
                wal_bytes = fs::file_size(database_path.string() + ".wal", ec);
                if (ec) {
                    failure = "Cannot measure WAL size: " + ec.message();
                    ec.clear();
                }
            }
        }
    }

    if (failure.empty()) {
        std::sort(write_latencies.begin(), write_latencies.end());
        const double total_write_ms = std::accumulate(write_latencies.begin(), write_latencies.end(), 0.0);
        const auto percentile = [&write_latencies](double p) {
            const auto index = static_cast<std::size_t>(p * static_cast<double>(write_latencies.size() - 1));
            return write_latencies[index];
        };
        std::cout << "CustomDBEngine reproducible baseline\n"
                  << "records: " << record_count << "\n"
                  << "payload_bytes_per_record: 128\n"
                  << "write_total_ms: " << total_write_ms << "\n"
                  << "write_average_ms: " << total_write_ms / static_cast<double>(record_count) << "\n"
                  << "write_p50_ms: " << percentile(0.50) << "\n"
                  << "write_p95_ms: " << percentile(0.95) << "\n"
                  << "read_all_ms: " << read_ms << "\n"
                  << "reopen_ms: " << reopen_ms << "\n"
                  << "snapshot_bytes: " << snapshot_bytes << "\n"
                  << "wal_bytes: " << wal_bytes << "\n"
                  << "integrity_and_persistence: passed\n";
        success = true;
    } else {
        std::cerr << failure << '\n';
    }

    fs::remove_all(work_dir, ec);
    if (ec) {
        std::cerr << "Warning: benchmark temp directory cleanup failed: " << ec.message() << '\n';
        if (success) return 1;
    }
    return success ? 0 : 1;
}