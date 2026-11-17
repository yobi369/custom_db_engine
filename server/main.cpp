#include "storage_engine.h"
#include "auth_store.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
class ServerInstanceLock {
public:
    explicit ServerInstanceLock(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("another CustomDB process is already using this data directory");
#else
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR
#ifdef O_CLOEXEC
                             | O_CLOEXEC
#endif
                             , S_IRUSR | S_IWUSR);
        if (descriptor_ < 0)
            throw std::runtime_error("cannot open CustomDB data-directory lock file");
        if (::fchmod(descriptor_, S_IRUSR | S_IWUSR) != 0 ||
            ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("another CustomDB process is already using this data directory");
        }
#endif
    }

    ~ServerInstanceLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }

    ServerInstanceLock(const ServerInstanceLock&) = delete;
    ServerInstanceLock& operator=(const ServerInstanceLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

std::mutex engine_mutex;
bool bearer_auth_active = false;
std::mutex login_attempts_mutex;
struct LoginAttemptWindow {
    std::chrono::steady_clock::time_point started;
    unsigned int failures = 0;
};
std::unordered_map<std::string, LoginAttemptWindow> login_attempts;
constexpr unsigned int kLoginFailureLimit = 5;
constexpr auto kLoginWindow = std::chrono::minutes(1);

bool login_is_limited(const std::string& remote_address) {
    std::lock_guard<std::mutex> lock(login_attempts_mutex);
    const auto now = std::chrono::steady_clock::now();
    auto it = login_attempts.find(remote_address);
    if (it == login_attempts.end()) return false;
    if (now - it->second.started >= kLoginWindow) {
        login_attempts.erase(it);
        return false;
    }
    return it->second.failures >= kLoginFailureLimit;
}

void record_login_failure(const std::string& remote_address) {
    std::lock_guard<std::mutex> lock(login_attempts_mutex);
    const auto now = std::chrono::steady_clock::now();
    auto it = login_attempts.find(remote_address);
    if (it == login_attempts.end()) {
        if (login_attempts.size() >= 4096) {
            auto oldest = login_attempts.begin();
            for (auto candidate = login_attempts.begin(); candidate != login_attempts.end(); ++candidate)
                if (candidate->second.started < oldest->second.started) oldest = candidate;
            login_attempts.erase(oldest);
        }
        it = login_attempts.emplace(remote_address, LoginAttemptWindow{now, 0}).first;
    } else if (now - it->second.started >= kLoginWindow) {
        it->second = LoginAttemptWindow{now, 0};
    }
    ++it->second.failures;
}

void clear_login_failures(const std::string& remote_address) {
    std::lock_guard<std::mutex> lock(login_attempts_mutex);
    login_attempts.erase(remote_address);
}
const std::regex database_name_pattern("^[a-zA-Z][a-zA-Z0-9_-]{0,62}$");

std::string env_or(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value && *value ? value : fallback;
}

bool valid_database_name(const std::string& name) {
    return std::regex_match(name, database_name_pattern);
}

void respond_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

bool authorized(const httplib::Request& request, const std::string& api_key) {
    if (bearer_auth_active && request.has_header("Authorization")) return true;
    if (api_key.empty()) return true;
    const auto supplied = request.get_header_value("X-CustomDB-Key");
    if (supplied.size() != api_key.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < api_key.size(); ++i)
        difference |= static_cast<unsigned char>(supplied[i] ^ api_key[i]);
    return difference == 0;
}

fs::path database_path(const fs::path& root, const std::string& name) {
    return root / (name + ".cdb.json");
}

std::string read_password_hidden(const std::string& prompt) {
#ifdef _WIN32
    if (!_isatty(_fileno(stdin))) throw std::runtime_error("administrator bootstrap requires an interactive terminal");
#else
    if (!::isatty(STDIN_FILENO)) throw std::runtime_error("administrator bootstrap requires an interactive terminal");
#endif
    std::cout << prompt << std::flush;
#ifdef _WIN32
    std::string password;
    for (;;) {
        const int ch = _getch();
        if (ch == '\r' || ch == '\n') break;
        if (ch == 3) throw std::runtime_error("password input cancelled");
        if (ch == 8) { if (!password.empty()) password.pop_back(); continue; }
        if (ch >= 32 && ch <= 126 && password.size() < 1024) password.push_back(static_cast<char>(ch));
    }
    std::cout << '\n';
    return password;
#else
    termios original{};
    if (tcgetattr(STDIN_FILENO, &original) != 0)
        throw std::runtime_error("cannot read terminal settings");
    termios hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0)
        throw std::runtime_error("cannot disable terminal echo");
    std::string password;
    const bool read_ok = static_cast<bool>(std::getline(std::cin, password));
    const int restore_result = tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    std::cout << '\n';
    if (restore_result != 0) throw std::runtime_error("cannot restore terminal settings");
    if (!read_ok) throw std::runtime_error("password input failed");
    if (password.size() > 1024) throw std::runtime_error("password exceeds 1024 bytes");
    return password;
#endif
}
}

int main(int argc, char* argv[]) {
    const auto host = env_or("CUSTOMDB_HOST", "127.0.0.1");
    const auto port_text = env_or("CUSTOMDB_PORT", "7680");
    const auto root = fs::absolute(env_or("CUSTOMDB_DATA_DIR", "./customdb-data")).lexically_normal();
    const bool bootstrap_mode = argc == 2 && std::string(argv[1]) == "--bootstrap-admin";
    if (argc != 1 && !bootstrap_mode) {
        std::cerr << "Usage: custom_db_server [--bootstrap-admin]\n";
        return 2;
    }
    try {
        fs::create_directories(root);
    } catch (const std::exception& error) {
        std::cerr << "Cannot create database directory: " << error.what() << '\n';
        return 2;
    }
    std::unique_ptr<ServerInstanceLock> instance_lock;
    try {
        instance_lock = std::make_unique<ServerInstanceLock>(root / ".customdb-server.lock");
    } catch (const std::exception& error) {
        std::cerr << "Cannot acquire data-directory lock: " << error.what() << '\n';
        return 2;
    }
    if (bootstrap_mode) {
        try {
            std::string username;
            std::cout << "Administrator username (3-64 characters): " << std::flush;
            if (!std::getline(std::cin, username)) throw std::runtime_error("username input failed");
            const auto password = read_password_hidden("Administrator password (minimum 12 characters): ");
            const auto confirmation = read_password_hidden("Confirm administrator password: ");
            if (password != confirmation) throw std::runtime_error("passwords do not match");
            customdb::security::AuthStore auth(root / "auth" / "auth_store.json");
            auth.bootstrap_admin(username, password);
            std::cout << "Administrator account created in " << (root / "auth" / "auth_store.json") << ".\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Administrator bootstrap failed: " << error.what() << '\n';
            return 2;
        }
    }
    const auto api_key = env_or("CUSTOMDB_API_KEY", "");
    const bool allow_non_loopback = env_or("CUSTOMDB_ALLOW_NON_LOOPBACK", "0") == "1";
    customdb::security::AuthStore auth(root / "auth" / "auth_store.json");
    const bool token_auth_enabled = auth.initialized();
    bearer_auth_active = token_auth_enabled;

    int port = 7680;
    try {
        std::size_t consumed = 0;
        port = std::stoi(port_text, &consumed);
        if (consumed != port_text.size() || port < 1 || port > 65535)
            throw std::invalid_argument("invalid port");
    } catch (...) {
        std::cerr << "CUSTOMDB_PORT must be an integer from 1 to 65535\n";
        return 2;
    }

    if (host != "127.0.0.1" && host != "localhost" && host != "::1" && !allow_non_loopback) {
        std::cerr << "Refusing non-loopback bind without CUSTOMDB_ALLOW_NON_LOOPBACK=1.\n";
        return 2;
    }

    httplib::Server server;
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(5, 0);

    // Once an identity store exists, every route except health and password login
    // requires a valid bearer token and an explicit database grant.
    server.set_pre_routing_handler([&auth, token_auth_enabled](const httplib::Request& request,
                                                               httplib::Response& response) {
        if (request.path == "/health" || request.path == "/v1/auth/login")
            return httplib::Server::HandlerResponse::Unhandled;
        if (!token_auth_enabled)
            return httplib::Server::HandlerResponse::Unhandled;

        const auto authorization = request.get_header_value("Authorization");
        if (authorization.size() <= 7 ||
            std::tolower(static_cast<unsigned char>(authorization[0])) != 'b' ||
            std::tolower(static_cast<unsigned char>(authorization[1])) != 'e' ||
            std::tolower(static_cast<unsigned char>(authorization[2])) != 'a' ||
            std::tolower(static_cast<unsigned char>(authorization[3])) != 'r' ||
            std::tolower(static_cast<unsigned char>(authorization[4])) != 'e' ||
            std::tolower(static_cast<unsigned char>(authorization[5])) != 'r' ||
            authorization[6] != ' ') {
            respond_json(response, 401, {{"error", "bearer token required"}});
            response.set_header("WWW-Authenticate", "Bearer");
            response.set_header("Connection", "close");
            return httplib::Server::HandlerResponse::Handled;
        }
        std::optional<customdb::security::UserRecord> principal;
        try { principal = auth.authenticate_token(authorization.substr(7)); }
        catch (const std::exception&) {
            respond_json(response, 503, {{"error", "authentication store unavailable"}});
            response.set_header("Connection", "close");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!principal) {
            respond_json(response, 401, {{"error", "invalid or expired bearer token"}});
            response.set_header("WWW-Authenticate", "Bearer error=\"invalid_token\"");
            response.set_header("Connection", "close");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (principal->role == "admin" || request.path == "/v1/auth/logout")
            return httplib::Server::HandlerResponse::Unhandled;

        if (request.path == "/v1/databases" || request.path == "/v1/auth/users" ||
            request.path.rfind("/v1/auth/tokens/", 0) == 0) {
            respond_json(response, 403, {{"error", "administrator permission required"}});
            response.set_header("Connection", "close");
            return httplib::Server::HandlerResponse::Handled;
        }

        static const std::regex restore_route("^/v1/databases/[A-Za-z][A-Za-z0-9_-]{0,62}/backups/[A-Za-z0-9_-]{1,64}/restore$");
        if (request.method == "POST" && std::regex_match(request.path, restore_route)) {
            respond_json(response, 403, {{"error", "administrator permission required for restore"}});
            response.set_header("Connection", "close");
            return httplib::Server::HandlerResponse::Handled;
        }

        static const std::regex database_route("^/v1/databases/([A-Za-z][A-Za-z0-9_-]{0,62})(?:/|$)");
        std::smatch match;
        if (std::regex_search(request.path, match, database_route)) {
            const auto database = match[1].str();
            const bool write_request = request.method != "GET" && request.method != "HEAD";
            if (write_request && principal->role == "read_only") {
                respond_json(response, 403, {{"error", "read-only account cannot modify data"}});
                response.set_header("Connection", "close");
                return httplib::Server::HandlerResponse::Handled;
            }
            const auto grant = std::find_if(principal->grants.begin(), principal->grants.end(),
                [&](const customdb::security::DatabaseGrant& item) { return item.database == database; });
            const bool permitted = grant != principal->grants.end() &&
                (!write_request || grant->permission == "write");
            if (!permitted) {
                respond_json(response, 403, {{"error", "database permission denied"}});
                response.set_header("Connection", "close");
                return httplib::Server::HandlerResponse::Handled;
            }
        } else if (request.path.rfind("/v1/", 0) == 0) {
            respond_json(response, 403, {{"error", "administrator permission required"}});
            response.set_header("Connection", "close");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Post("/v1/auth/login", [&auth, token_auth_enabled](const httplib::Request& request,
                                                               httplib::Response& response) {
        if (!token_auth_enabled)
            return respond_json(response, 503, {{"error", "administrator bootstrap is required before token login"}});
        const auto remote_address = request.remote_addr;
        if (login_is_limited(remote_address)) {
            response.set_header("Retry-After", "60");
            return respond_json(response, 429, {{"error", "too many failed login attempts; try again later"}});
        }
        const auto body = json::parse(request.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("username") ||
            !body["username"].is_string() || !body.contains("password") || !body["password"].is_string())
            return respond_json(response, 400, {{"error", "expected username and password strings"}});
        std::optional<customdb::security::UserRecord> user;
        try { user = auth.authenticate_password(body["username"].get<std::string>(),
                                                 body["password"].get<std::string>()); }
        catch (const std::exception&) { return respond_json(response, 503, {{"error", "authentication store unavailable"}}); }
        if (!user) {
            record_login_failure(remote_address);
            return respond_json(response, 401, {{"error", "invalid username or password"}});
        }
        clear_login_failures(remote_address);
        try {
            const auto issued = auth.issue_token(user->username, std::chrono::hours(24), &user->password);
            respond_json(response, 200, {{"token_type", "Bearer"}, {"access_token", issued.token}, {"token_id", issued.id},
                {"expires_at", issued.expires_at}, {"user", {{"username", user->username}, {"role", user->role}}}});
        } catch (const customdb::security::AuthPolicyError&) {
            respond_json(response, 401, {{"error", "credentials changed during login; retry"}});
        } catch (const std::exception& error) {
            respond_json(response, 500, {{"error", error.what()}});
        }
    });

    server.Post("/v1/auth/users", [&auth, token_auth_enabled](const httplib::Request& request,
                                                               httplib::Response& response) {
        if (!token_auth_enabled) return respond_json(response, 503, {{"error", "administrator bootstrap is required"}});
        const auto body = json::parse(request.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("username") ||
            !body["username"].is_string() || !body.contains("password") || !body["password"].is_string() ||
            !body.contains("role") || !body["role"].is_string())
            return respond_json(response, 400, {{"error", "expected username, password, and role strings"}});
        std::vector<customdb::security::DatabaseGrant> grants;
        if (body.contains("grants")) {
            if (!body["grants"].is_array() || body["grants"].size() > 1000)
                return respond_json(response, 400, {{"error", "grants must be an array of at most 1000 entries"}});
            for (const auto& item : body["grants"]) {
                if (!item.is_object() || !item.contains("database") || !item["database"].is_string() ||
                    !item.contains("permission") || !item["permission"].is_string())
                    return respond_json(response, 400, {{"error", "each grant requires database and permission strings"}});
                grants.push_back({item["database"].get<std::string>(), item["permission"].get<std::string>()});
            }
        }
        try {
            const auto user = auth.create_user(body["username"].get<std::string>(),
                body["password"].get<std::string>(), body["role"].get<std::string>(), grants);
            respond_json(response, 201, {{"username", user.username}, {"role", user.role},
                {"enabled", user.enabled}, {"grants", body.value("grants", json::array())}});
        } catch (const std::invalid_argument& error) {
            respond_json(response, 400, {{"error", error.what()}});
        } catch (const std::exception& error) {
            respond_json(response, 409, {{"error", error.what()}});
        }
    });

    server.Get("/v1/auth/users", [&auth, token_auth_enabled](const httplib::Request&, httplib::Response& response) {
        if (!token_auth_enabled) return respond_json(response, 503, {{"error", "authentication is not initialized"}});
        json users = json::array();
        try {
            for (const auto& user : auth.list_users()) {
                json grants = json::array();
                for (const auto& grant : user.grants)
                    grants.push_back({{"database", grant.database}, {"permission", grant.permission}});
                users.push_back({{"username", user.username}, {"role", user.role},
                                 {"enabled", user.enabled}, {"grants", grants}});
            }
            respond_json(response, 200, {{"users", users}});
        } catch (const std::exception&) {
            respond_json(response, 503, {{"error", "authentication store unavailable"}});
        }
    });

    server.Put(R"(/v1/auth/users/([A-Za-z0-9_.@-]{3,64})/grants)",
        [&auth, token_auth_enabled](const httplib::Request& request, httplib::Response& response) {
            if (!token_auth_enabled) return respond_json(response, 503, {{"error", "authentication is not initialized"}});
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("grants") ||
                !body["grants"].is_array() || body["grants"].size() > 1000)
                return respond_json(response, 400, {{"error", "expected grants array with at most 1000 entries"}});
            std::vector<customdb::security::DatabaseGrant> grants;
            for (const auto& item : body["grants"]) {
                if (!item.is_object() || !item.contains("database") || !item["database"].is_string() ||
                    !item.contains("permission") || !item["permission"].is_string())
                    return respond_json(response, 400, {{"error", "each grant requires database and permission strings"}});
                grants.push_back({item["database"].get<std::string>(), item["permission"].get<std::string>()});
            }
            try {
                if (!auth.set_user_grants(request.matches[1].str(), grants))
                    return respond_json(response, 404, {{"error", "user not found"}});
                respond_json(response, 200, {{"status", "grants_updated"}});
            } catch (const std::invalid_argument& error) {
                respond_json(response, 400, {{"error", error.what()}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Put(R"(/v1/auth/users/([A-Za-z0-9_.@-]{3,64})/enabled)",
        [&auth, token_auth_enabled](const httplib::Request& request, httplib::Response& response) {
            if (!token_auth_enabled) return respond_json(response, 503, {{"error", "authentication is not initialized"}});
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("enabled") ||
                !body["enabled"].is_boolean())
                return respond_json(response, 400, {{"error", "expected boolean field 'enabled'"}});
            try {
                if (!auth.set_user_enabled(request.matches[1].str(), body["enabled"].get<bool>()))
                    return respond_json(response, 404, {{"error", "user not found"}});
                respond_json(response, 200, {{"status", "updated"}});
            } catch (const customdb::security::AuthPolicyError& error) {
                respond_json(response, 409, {{"error", error.what()}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Put(R"(/v1/auth/users/([A-Za-z0-9_.@-]{3,64})/password)",
        [&auth, token_auth_enabled](const httplib::Request& request, httplib::Response& response) {
            if (!token_auth_enabled) return respond_json(response, 503, {{"error", "authentication is not initialized"}});
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("password") ||
                !body["password"].is_string())
                return respond_json(response, 400, {{"error", "expected string field 'password'"}});
            try {
                if (!auth.set_user_password(request.matches[1].str(), body["password"].get<std::string>()))
                    return respond_json(response, 404, {{"error", "user not found"}});
                respond_json(response, 200, {{"status", "password_updated"}, {"sessions_revoked", true}});
            } catch (const std::invalid_argument& error) {
                respond_json(response, 400, {{"error", error.what()}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Post("/v1/auth/logout", [&auth](const httplib::Request& request, httplib::Response& response) {
        const auto authorization = request.get_header_value("Authorization");
        if (authorization.size() <= 7)
            return respond_json(response, 401, {{"error", "session is no longer active"}});
        try {
            if (!auth.revoke_token_value(authorization.substr(7)))
                return respond_json(response, 401, {{"error", "session is no longer active"}});
            respond_json(response, 200, {{"status", "logged_out"}});
        } catch (const std::exception&) {
            respond_json(response, 503, {{"error", "authentication store unavailable"}});
        }
    });

    server.Post(R"(/v1/auth/tokens/([a-f0-9]{32})/revoke)",
        [&auth, token_auth_enabled](const httplib::Request& request, httplib::Response& response) {
            if (!token_auth_enabled) return respond_json(response, 503, {{"error", "authentication is not initialized"}});
            try {
                if (!auth.revoke_token(request.matches[1].str()))
                    return respond_json(response, 404, {{"error", "token not found"}});
                respond_json(response, 200, {{"status", "revoked"}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        respond_json(response, 200, {{"status", "ok"}, {"service", "customdb-server"}, {"api_version", 1}});
    });

    server.Get("/v1/databases", [root, api_key](const httplib::Request& request, httplib::Response& response) {
        if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
        json databases = json::array();
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                const auto filename = entry.path().filename().string();
                constexpr const char* suffix = ".cdb.json";
                const std::string suffix_string(suffix);
                if (filename.size() > suffix_string.size() &&
                    filename.compare(filename.size() - suffix_string.size(), suffix_string.size(), suffix) == 0)
                    databases.push_back(filename.substr(0, filename.size() - suffix_string.size()));
            }
        }
        respond_json(response, 200, {{"databases", databases}});
    });

    server.Post("/v1/databases", [root, api_key](const httplib::Request& request, httplib::Response& response) {
        if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
        json body = json::parse(request.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("name") || !body["name"].is_string())
            return respond_json(response, 400, {{"error", "expected JSON body with string field 'name'"}});
        const auto name = body["name"].get<std::string>();
        if (!valid_database_name(name))
            return respond_json(response, 400, {{"error", "invalid database name"}});
        const auto path = database_path(root, name);
        std::lock_guard<std::mutex> lock(engine_mutex);
        if (fs::exists(path)) return respond_json(response, 409, {{"error", "database already exists"}});
        try {
            StorageEngine engine(path.string());
            if (!engine.create_database())
                return respond_json(response, 500, {{"error", engine.get_last_error()}});
        } catch (const std::exception& error) {
            return respond_json(response, 500, {{"error", error.what()}});
        }
        respond_json(response, 201, {{"name", name}, {"status", "created"}});
    });

    server.Put(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/kv/([^/]+))",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto key = request.matches[2].str();
            json body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("value") || !body["value"].is_string())
                return respond_json(response, 400, {{"error", "expected JSON body with string field 'value'"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.write_data(key, body["value"].get<std::string>()))
                    return respond_json(response, 500, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"key", key}, {"status", "stored"}});
        });

    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/kv/([^/]+))",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto key = request.matches[2].str();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                const auto value = engine.read_data(key);
                const auto error = engine.get_last_error();
                if (!error.empty()) return respond_json(response, 404, {{"error", error}});
                respond_json(response, 200, {{"key", key}, {"value", value}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Delete(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/kv/([^/]+))",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto key = request.matches[2].str();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.delete_data(key)) return respond_json(response, 404, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"key", key}, {"status", "deleted"}});
        });


    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/collections/([a-zA-Z][a-zA-Z0-9_-]{0,62})/documents)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto collection = request.matches[2].str();
            const auto document = json::parse(request.body, nullptr, false);
            if (document.is_discarded() || !document.is_object())
                return respond_json(response, 400, {{"error", "request body must be a JSON object"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.write_document(collection, document.dump()))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 201, {{"collection", collection}, {"status", "inserted"}, {"document", document}});
        });

    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/collections/([a-zA-Z][a-zA-Z0-9_-]{0,62})/documents)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto collection = request.matches[2].str();
            std::map<std::string, std::string> filters;
            for (const auto& parameter : request.params) filters[parameter.first] = parameter.second;
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                const auto rows = engine.query_documents(collection, filters);
                const auto error = engine.get_last_error();
                if (!error.empty()) return respond_json(response, 400, {{"error", error}});
                json documents = json::array();
                for (const auto& row : rows) {
                    auto parsed = json::parse(row, nullptr, false);
                    if (!parsed.is_discarded()) documents.push_back(std::move(parsed));
                }
                respond_json(response, 200, {{"collection", collection}, {"count", documents.size()}, {"documents", documents}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Put(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/collections/([a-zA-Z][a-zA-Z0-9_-]{0,62})/documents)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto collection = request.matches[2].str();
            const auto document = json::parse(request.body, nullptr, false);
            if (document.is_discarded() || !document.is_object())
                return respond_json(response, 400, {{"error", "request body must be a JSON object"}});
            std::map<std::string, std::string> filters;
            for (const auto& parameter : request.params) filters[parameter.first] = parameter.second;
            if (filters.empty())
                return respond_json(response, 400, {{"error", "at least one equality query parameter is required"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.update_documents(collection, filters, document.dump()))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"collection", collection}, {"status", "updated"}});
        });

    server.Delete(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/collections/([a-zA-Z][a-zA-Z0-9_-]{0,62})/documents)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto collection = request.matches[2].str();
            std::map<std::string, std::string> filters;
            for (const auto& parameter : request.params) filters[parameter.first] = parameter.second;
            if (filters.empty())
                return respond_json(response, 400, {{"error", "at least one equality query parameter is required"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.delete_documents(collection, filters))
                    return respond_json(response, 404, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"collection", collection}, {"status", "deleted"}});
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/schemas)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto schema = json::parse(request.body, nullptr, false);
            if (schema.is_discarded() || !schema.is_object())
                return respond_json(response, 400, {{"error", "request body must be a schema JSON object"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.create_schema(schema.dump()))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 201, {{"status", "schema_created"}, {"schema", schema}});
        });

    server.Put(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/indexes)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("field") || !body["field"].is_string())
                return respond_json(response, 400, {{"error", "expected JSON body with string field 'field'"}});
            const auto field = body["field"].get<std::string>();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.create_index(field))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 201, {{"status", "index_created"}, {"field", field}});
        });

    server.Delete(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/indexes)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto field = request.get_param_value("field");
            if (field.empty()) return respond_json(response, 400, {{"error", "missing field query parameter"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.drop_index(field))
                    return respond_json(response, 404, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"status", "index_dropped"}, {"field", field}});
        });



    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/integrity)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.check_integrity())
                    return respond_json(response, 409, {{"status", "failed"}, {"error", engine.get_last_error()}});
                respond_json(response, 200, {{"status", "ok"}, {"database", name}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/backups)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto directory = root / "backups" / name;
            json backups = json::array();
            std::error_code ec;
            if (fs::exists(directory, ec)) {
                if (ec || !fs::is_directory(directory, ec))
                    return respond_json(response, 500, {{"error", "cannot inspect backup directory"}});
                for (const auto& entry : fs::directory_iterator(directory, ec)) {
                    if (ec) return respond_json(response, 500, {{"error", "cannot list backups"}});
                    if (entry.is_regular_file() && entry.path().extension() == ".backup") {
                        backups.push_back({{"name", entry.path().stem().string()},
                                           {"size_bytes", entry.file_size()}});
                    }
                }
            }
            respond_json(response, 200, {{"database", name}, {"backups", backups}});
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/backups)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("name") ||
                !body["name"].is_string())
                return respond_json(response, 400, {{"error", "expected JSON body with string field 'name'"}});
            const auto backup_name = body["name"].get<std::string>();
            static const std::regex backup_pattern("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,62}$");
            if (!std::regex_match(backup_name, backup_pattern))
                return respond_json(response, 400, {{"error", "invalid backup name"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto source = database_path(root, name);
            if (!fs::is_regular_file(source)) return respond_json(response, 404, {{"error", "database not found"}});
            const auto directory = root / "backups" / name;
            const auto target = directory / (backup_name + ".backup");
            try {
                fs::create_directories(directory);
                if (fs::exists(target)) return respond_json(response, 409, {{"error", "backup already exists"}});
                StorageEngine engine(source.string());
                if (!engine.backup_database(target.string()))
                    return respond_json(response, 500, {{"error", engine.get_last_error()}});
                respond_json(response, 201, {{"database", name}, {"backup", backup_name}, {"status", "created"}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/backups/([a-zA-Z0-9][a-zA-Z0-9_-]{0,62})/restore)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto backup_name = request.matches[2].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("confirm") ||
                !body["confirm"].is_boolean() || !body["confirm"].get<bool>())
                return respond_json(response, 400, {{"error", "restore requires JSON body {\"confirm\":true}"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto active = database_path(root, name);
            const auto backup = root / "backups" / name / (backup_name + ".backup");
            if (!fs::is_regular_file(active)) return respond_json(response, 404, {{"error", "database not found"}});
            if (!fs::is_regular_file(backup)) return respond_json(response, 404, {{"error", "backup not found"}});
            try {
                StorageEngine engine(active.string());
                if (!engine.restore_database(backup.string()))
                    return respond_json(response, 409, {{"error", engine.get_last_error()}});
                respond_json(response, 200, {{"database", name}, {"backup", backup_name}, {"status", "restored"}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/transactions)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("operations") ||
                !body["operations"].is_array() || body["operations"].empty() || body["operations"].size() > 100)
                return respond_json(response, 400, {{"error", "operations must be a non-empty array with at most 100 entries"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.begin_transaction())
                    return respond_json(response, 409, {{"error", engine.get_last_error()}});
                std::size_t completed = 0;
                for (const auto& operation : body["operations"]) {
                    if (!operation.is_object() || !operation.contains("type") || !operation["type"].is_string()) {
                        engine.rollback_transaction();
                        return respond_json(response, 400, {{"error", "each operation requires a string type"}, {"failed_operation", completed}});
                    }
                    const auto type = operation["type"].get<std::string>();
                    bool ok = false;
                    if (type == "put" && operation.contains("key") && operation["key"].is_string() &&
                        operation.contains("value") && operation["value"].is_string()) {
                        ok = engine.write_data(operation["key"].get<std::string>(), operation["value"].get<std::string>());
                    } else if (type == "delete" && operation.contains("key") && operation["key"].is_string()) {
                        ok = engine.delete_data(operation["key"].get<std::string>());
                    } else if (type == "insert_document" && operation.contains("collection") &&
                        operation["collection"].is_string() && operation.contains("document") && operation["document"].is_object()) {
                        ok = engine.write_document(operation["collection"].get<std::string>(), operation["document"].dump());
                    } else {
                        engine.rollback_transaction();
                        return respond_json(response, 400, {{"error", "unsupported or malformed operation"}, {"failed_operation", completed}});
                    }
                    if (!ok) {
                        const auto error = engine.get_last_error();
                        engine.rollback_transaction();
                        return respond_json(response, 400, {{"error", error}, {"failed_operation", completed}});
                    }
                    ++completed;
                }
                if (!engine.commit_transaction()) {
                    const auto error = engine.get_last_error();
                    engine.rollback_transaction();
                    return respond_json(response, 500, {{"error", error}});
                }
                respond_json(response, 200, {{"status", "committed"}, {"operations", completed}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    std::cout << "CustomDB server listening on " << host << ':' << port
              << " using data directory " << root << '\n';
    if (!api_key.empty()) std::cout << "API-key authentication is enabled.\n";
    if (!server.listen(host.c_str(), port)) {
        std::cerr << "Failed to start HTTP listener.\n";
        return 1;
    }
    return 0;
}
