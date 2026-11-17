#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>

namespace customdb::security {

class AuditLog {
public:
    explicit AuditLog(std::filesystem::path file, std::size_t maximum_bytes = 1024 * 1024);

    // Writes a structured event. Callers must never pass passwords, tokens, or credential verifiers.
    void record(const std::string& event, const std::string& actor,
                const std::string& target, const std::string& outcome,
                const std::string& remote_address = {});

private:
    std::filesystem::path file_;
    std::size_t maximum_bytes_;
    std::mutex mutex_;
};

} // namespace customdb::security
