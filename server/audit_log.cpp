#include "audit_log.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace customdb::security {
namespace {
using json = nlohmann::json;

void validate_field(const std::string& value, std::size_t maximum, const char* name) {
    if (value.size() > maximum)
        throw std::invalid_argument(std::string("audit field exceeds limit: ") + name);
}
} // namespace

AuditLog::AuditLog(std::filesystem::path file, std::size_t maximum_bytes)
    : file_(std::move(file)), maximum_bytes_(maximum_bytes) {
    if (file_.empty()) throw std::invalid_argument("audit log path must not be empty");
    if (maximum_bytes_ < 512 || maximum_bytes_ > 16 * 1024 * 1024)
        throw std::invalid_argument("audit log size limit must be between 512 bytes and 16 MiB");
}

void AuditLog::record(const std::string& event, const std::string& actor,
                      const std::string& target, const std::string& outcome,
                      const std::string& remote_address) {
    validate_field(event, 64, "event");
    validate_field(actor, 128, "actor");
    validate_field(target, 256, "target");
    validate_field(outcome, 64, "outcome");
    validate_field(remote_address, 128, "remote_address");
    if (event.empty() || outcome.empty())
        throw std::invalid_argument("audit event and outcome must not be empty");

    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const auto line = json{{"timestamp_ms", timestamp}, {"event", event}, {"actor", actor},
                           {"target", target}, {"outcome", outcome},
                           {"remote_address", remote_address}}.dump() + "\n";
    if (line.size() > maximum_bytes_)
        throw std::invalid_argument("audit event exceeds configured log size");

    std::lock_guard<std::mutex> lock(mutex_);
    const auto parent = file_.parent_path().empty() ? std::filesystem::path(".") : file_.parent_path();
    std::filesystem::create_directories(parent);
    std::error_code ec;
    if (std::filesystem::exists(file_, ec)) {
        if (ec) throw std::runtime_error("cannot inspect audit log");
        const auto size = std::filesystem::file_size(file_, ec);
        if (ec) throw std::runtime_error("cannot read audit log size");
        if (size + line.size() > maximum_bytes_) {
            auto previous = file_;
            previous += ".1";
            std::filesystem::remove(previous, ec);
            ec.clear();
            std::filesystem::rename(file_, previous, ec);
            if (ec) throw std::runtime_error("cannot rotate audit log");
        }
    }

#ifdef _WIN32
    {
        std::ofstream output(file_, std::ios::binary | std::ios::app);
        if (!output) throw std::runtime_error("cannot open audit log");
        output.write(line.data(), static_cast<std::streamsize>(line.size()));
        output.flush();
        if (!output) throw std::runtime_error("cannot append audit event");
    }
#else
    const int descriptor = ::open(file_.c_str(), O_WRONLY | O_CREAT | O_APPEND
#ifdef O_CLOEXEC
                                  | O_CLOEXEC
#endif
                                  , S_IRUSR | S_IWUSR);
    if (descriptor < 0) throw std::runtime_error("cannot open audit log");
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        ::close(descriptor);
        throw std::runtime_error("cannot restrict audit log permissions");
    }
    std::size_t written = 0;
    while (written < line.size()) {
        const auto count = ::write(descriptor, line.data() + written, line.size() - written);
        if (count <= 0) {
            ::close(descriptor);
            throw std::runtime_error("cannot append audit event");
        }
        written += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) throw std::runtime_error("cannot close audit log");
#endif
}

} // namespace customdb::security
