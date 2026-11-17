#include "../server/audit_log.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#endif

int main() {
    using customdb::security::AuditLog;
    const auto root = std::filesystem::temp_directory_path() / "customdb-audit-log-test";
    std::filesystem::remove_all(root);
    const auto file = root / "audit" / "security.jsonl";
    AuditLog log(file, 1024);
    log.record("login_succeeded", "owner01", "owner01", "success", "127.0.0.1");
    log.record("user_password_changed", "owner01", "reader01", "success", "127.0.0.1");

    std::ifstream input(file, std::ios::binary);
    std::string first, second;
    assert(std::getline(input, first));
    assert(std::getline(input, second));
    assert(first.find("\"login_succeeded\"") != std::string::npos);
    assert(second.find("\"user_password_changed\"") != std::string::npos);
    assert(first.find("password") == std::string::npos);
    assert(first.find("token") == std::string::npos);
#ifndef _WIN32
    struct stat info {};
    assert(::stat(file.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0600);
#endif

    // Rotation retains the previous segment and keeps the active file bounded.
    for (int i = 0; i < 30; ++i)
        log.record("authorization_denied", "reader01", "database-alpha", "denied", "127.0.0.1");
    assert(std::filesystem::exists(file.string() + ".1"));
    assert(std::filesystem::file_size(file) <= 1024);

    bool oversized_field_rejected = false;
    try { log.record("event", std::string(129, 'a'), "target", "success"); }
    catch (const std::invalid_argument&) { oversized_field_rejected = true; }
    assert(oversized_field_rejected);

    std::filesystem::remove_all(root);
    std::cout << "Audit log tests passed\n";
    return 0;
}
