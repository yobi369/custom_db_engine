#include "storage_engine.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>

#define REQUIRE(condition) do { if (!(condition)) { std::cerr << "Property test failed at line " << __LINE__ << ": " #condition << '\n'; return false; } } while (false)

namespace {
bool run_phase(const std::string& path, std::map<std::string, std::string>& model,
               std::mt19937& rng, int operations) {
    StorageEngine db(path);
    REQUIRE(db.get_last_error().empty());
    std::uniform_int_distribution<int> key_dist(0, 49);
    std::uniform_int_distribution<int> op_dist(0, 2);
    std::uniform_int_distribution<int> value_dist(0, 1000000);
    for (int i = 0; i < operations; ++i) {
        const std::string key = "key-" + std::to_string(key_dist(rng));
        const int op = op_dist(rng);
        if (op == 0) {
            const std::string value = "value-" + std::to_string(value_dist(rng));
            REQUIRE(db.write_data(key, value));
            model[key] = value;
        } else if (op == 1) {
            const bool expected = model.erase(key) != 0;
            REQUIRE(db.delete_data(key) == expected);
        } else {
            const auto expected = model.find(key);
            const std::string value = db.read_data(key);
            if (expected == model.end()) {
                REQUIRE(value.empty());
                REQUIRE(db.get_last_error().find("Key not found") != std::string::npos);
            } else {
                REQUIRE(value == expected->second);
                REQUIRE(db.get_last_error().empty());
            }
        }
    }
    for (const auto& [key, expected] : model) {
        REQUIRE(db.read_data(key) == expected);
        REQUIRE(db.get_last_error().empty());
    }
    REQUIRE(db.check_integrity());
    return true;
}
}

int main() {
    const std::string path = "storage_engine_property_test.json";
    std::error_code ec;
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"}) std::filesystem::remove(path + suffix, ec);
    std::map<std::string, std::string> model;
    std::mt19937 rng(0xCDB2026U);
    for (int phase = 0; phase < 8; ++phase) {
        if (!run_phase(path, model, rng, 50)) return 1;
    }
    {
        StorageEngine reopened(path);
        if (!reopened.get_last_error().empty()) {
            std::cerr << reopened.get_last_error() << '\n';
            return 1;
        }
        for (const auto& [key, expected] : model) {
            if (reopened.read_data(key) != expected) {
                std::cerr << "Persistence mismatch for " << key << '\n';
                return 1;
            }
        }
        if (!reopened.check_integrity()) {
            std::cerr << reopened.get_last_error() << '\n';
            return 1;
        }
    }
    for (const auto& suffix : {"", ".wal", ".tmp", ".lock"}) std::filesystem::remove(path + suffix, ec);
    std::cout << "Deterministic randomized storage properties passed\n";
    return 0;
}
