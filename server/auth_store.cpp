#include "auth_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace customdb::security {
namespace {
using json = nlohmann::json;
constexpr std::uintmax_t kMaximumStoreBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumUsers = 10000;
constexpr std::size_t kMaximumTokens = 50000;
constexpr std::size_t kMaximumGrants = 1000;

std::int64_t unix_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

bool valid_username(const std::string& value) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.@-]{2,63}$");
    return std::regex_match(value, pattern);
}

bool valid_role(const std::string& role) {
    return role == "admin" || role == "read_write" || role == "read_only";
}

bool valid_database_name(const std::string& value) {
    static const std::regex pattern("^[A-Za-z][A-Za-z0-9_-]{0,62}$");
    return std::regex_match(value, pattern);
}

json user_to_json(const UserRecord& user) {
    json grants = json::array();
    for (const auto& grant : user.grants)
        grants.push_back({{"database", grant.database}, {"permission", grant.permission}});
    return {{"id", user.id}, {"username", user.username},
            {"password", {{"algorithm", user.password.algorithm},
                          {"iterations", user.password.iterations},
                          {"salt", user.password.salt_hex},
                          {"verifier", user.password.verifier_hex}}},
            {"role", user.role}, {"enabled", user.enabled}, {"grants", grants}};
}

UserRecord user_from_json(const json& value) {
    if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
        !value.contains("username") || !value["username"].is_string() ||
        !value.contains("password") || !value["password"].is_object() ||
        !value.contains("role") || !value["role"].is_string() ||
        !value.contains("enabled") || !value["enabled"].is_boolean() ||
        !value.contains("grants") || !value["grants"].is_array())
        throw std::runtime_error("invalid user record in auth store");
    const auto& p = value["password"];
    if (!p.contains("algorithm") || !p["algorithm"].is_string() ||
        !p.contains("iterations") || !p["iterations"].is_number_unsigned() ||
        !p.contains("salt") || !p["salt"].is_string() ||
        !p.contains("verifier") || !p["verifier"].is_string())
        throw std::runtime_error("invalid password verifier in auth store");
    const auto iterations = p["iterations"].get<std::uint64_t>();
    if (iterations < 100000 || iterations > 2000000)
        throw std::runtime_error("password verifier work factor is out of bounds");
    static const std::regex hex32("^[a-f0-9]{32}$");
    static const std::regex hex64("^[a-f0-9]{64}$");
    UserRecord user;
    user.id = value["id"].get<std::string>();
    user.username = value["username"].get<std::string>();
    user.password = {p["algorithm"].get<std::string>(), static_cast<unsigned int>(iterations),
                     p["salt"].get<std::string>(), p["verifier"].get<std::string>()};
    user.role = value["role"].get<std::string>();
    user.enabled = value["enabled"].get<bool>();
    if (!std::regex_match(user.id, hex32) || !valid_username(user.username) || !valid_role(user.role) ||
        value["grants"].size() > kMaximumGrants)
        throw std::runtime_error("invalid user fields in auth store");
    for (const auto& item : value["grants"]) {
        if (!item.is_object() || !item.contains("database") || !item["database"].is_string() ||
            !item.contains("permission") || !item["permission"].is_string())
            throw std::runtime_error("invalid database grant in auth store");
        DatabaseGrant grant{item["database"].get<std::string>(), item["permission"].get<std::string>()};
        if (!valid_database_name(grant.database) ||
            (grant.permission != "read" && grant.permission != "write"))
            throw std::runtime_error("invalid database grant values in auth store");
        user.grants.push_back(std::move(grant));
    }
    if (user.password.algorithm != "pbkdf2-hmac-sha256" ||
        !std::regex_match(user.password.salt_hex, hex32) ||
        !std::regex_match(user.password.verifier_hex, hex64))
        throw std::runtime_error("unsupported password verifier in auth store");
    return user;
}
} // namespace

AuthStore::AuthStore(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) throw std::invalid_argument("auth store path must not be empty");
}

void AuthStore::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::filesystem::exists(file_))
        throw std::runtime_error("auth store already exists; refusing to overwrite");
    users_.clear();
    tokens_.clear();
    if (!file_.parent_path().empty()) std::filesystem::create_directories(file_.parent_path());
    persist_locked();
    loaded_ = true;
}

UserRecord AuthStore::bootstrap_admin(const std::string& username, const std::string& password) {
    if (!valid_username(username)) throw std::invalid_argument("username must be 3-64 safe characters");
    auto verifier = hash_password(password);
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::filesystem::exists(file_))
        throw std::runtime_error("auth store already exists; refusing to replace administrator");
    users_.clear();
    tokens_.clear();
    UserRecord admin{generate_bearer_token().substr(0, 32), username, verifier, "admin", true, {}};
    users_.push_back(admin);
    try {
        persist_locked();
        loaded_ = true;
    } catch (...) {
        users_.clear();
        throw;
    }
    return admin;
}

bool AuthStore::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::filesystem::is_regular_file(file_);
}

void AuthStore::load_locked() {
    if (loaded_) return;
    if (!std::filesystem::is_regular_file(file_))
        throw std::runtime_error("auth store is not initialized");
    const auto size = std::filesystem::file_size(file_);
    if (size == 0 || size > kMaximumStoreBytes)
        throw std::runtime_error("auth store size is invalid");
    std::ifstream input(file_, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read auth store");
    json root;
    try { input >> root; } catch (...) { throw std::runtime_error("auth store JSON is malformed"); }
    if (!root.is_object() || root.value("version", 0) != 1 ||
        !root.contains("users") || !root["users"].is_array() ||
        !root.contains("tokens") || !root["tokens"].is_array() ||
        root["users"].size() > kMaximumUsers || root["tokens"].size() > kMaximumTokens)
        throw std::runtime_error("auth store structure or version is invalid");
    std::vector<UserRecord> users;
    std::vector<TokenRecord> tokens;
    for (const auto& item : root["users"]) users.push_back(user_from_json(item));
    for (const auto& item : root["tokens"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("user_id") || !item["user_id"].is_string() ||
            !item.contains("digest") || !item["digest"].is_string() ||
            !item.contains("created_at") || !item["created_at"].is_number_integer() ||
            !item.contains("expires_at") || !item["expires_at"].is_number_integer() ||
            !item.contains("revoked") || !item["revoked"].is_boolean())
            throw std::runtime_error("invalid token record in auth store");
        TokenRecord token{item["id"].get<std::string>(), item["user_id"].get<std::string>(),
                          item["digest"].get<std::string>(), item["created_at"].get<std::int64_t>(),
                          item["expires_at"].get<std::int64_t>(), item["revoked"].get<bool>()};
        if (!std::regex_match(token.id, std::regex("^[a-f0-9]{32}$")) ||
            !std::regex_match(token.digest, std::regex("^[a-f0-9]{64}$")) ||
            token.user_id.empty() || token.created_at <= 0 || token.expires_at <= token.created_at)
            throw std::runtime_error("invalid token values in auth store");
        tokens.push_back(std::move(token));
    }
    for (std::size_t i = 0; i < users.size(); ++i)
        for (std::size_t j = i + 1; j < users.size(); ++j)
            if (users[i].id == users[j].id || users[i].username == users[j].username)
                throw std::runtime_error("duplicate user identity in auth store");
    for (const auto& token : tokens)
        if (std::none_of(users.begin(), users.end(), [&](const UserRecord& user) { return user.id == token.user_id; }))
            throw std::runtime_error("token references an unknown user");
    users_ = std::move(users);
    tokens_ = std::move(tokens);
    loaded_ = true;
}

void AuthStore::persist_locked() const {
    json root = {{"version", 1}, {"users", json::array()}, {"tokens", json::array()}};
    for (const auto& user : users_) root["users"].push_back(user_to_json(user));
    for (const auto& token : tokens_)
        root["tokens"].push_back({{"id", token.id}, {"user_id", token.user_id},
            {"digest", token.digest}, {"created_at", token.created_at},
            {"expires_at", token.expires_at}, {"revoked", token.revoked}});
    const auto parent = file_.parent_path().empty() ? std::filesystem::path(".") : file_.parent_path();
    std::filesystem::create_directories(parent);
    const auto temporary = file_.string() + ".tmp-" + generate_bearer_token();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create temporary auth store");
        const auto serialized = root.dump(2);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) throw std::runtime_error("cannot write temporary auth store");
    }
#ifndef _WIN32
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot restrict auth store file permissions");
    }
    if (::rename(temporary.c_str(), file_.c_str()) != 0) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot atomically replace auth store");
    }
#else
    if (!MoveFileExW(std::filesystem::path(temporary).c_str(), file_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot atomically replace auth store");
    }
#endif
}

UserRecord AuthStore::create_user(const std::string& username, const std::string& password,
                                  const std::string& role, const std::vector<DatabaseGrant>& grants) {
    if (!valid_username(username)) throw std::invalid_argument("username must be 3-64 safe characters");
    if (!valid_role(role)) throw std::invalid_argument("unsupported user role");
    if (grants.size() > kMaximumGrants) throw std::invalid_argument("too many database grants");
    for (const auto& grant : grants)
        if (!valid_database_name(grant.database) ||
            (grant.permission != "read" && grant.permission != "write"))
            throw std::invalid_argument("invalid database grant");
    auto verifier = hash_password(password);
    std::lock_guard<std::mutex> lock(mutex_);
    load_locked();
    if (users_.size() >= kMaximumUsers) throw std::runtime_error("user limit reached");
    if (users_.empty() && role != "admin")
        throw std::runtime_error("the first account must have the administrator role");
    if (std::any_of(users_.begin(), users_.end(), [&](const UserRecord& u) { return u.username == username; }))
        throw std::runtime_error("username already exists");
    UserRecord user{generate_bearer_token().substr(0, 32), username, verifier, role, true, grants};
    users_.push_back(user);
    try { persist_locked(); } catch (...) { users_.pop_back(); throw; }
    return user;
}

std::optional<UserRecord> AuthStore::find_user(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<AuthStore*>(this)->load_locked();
    const auto it = std::find_if(users_.begin(), users_.end(), [&](const UserRecord& u) { return u.username == username; });
    if (it == users_.end()) return std::nullopt;
    return *it;
}

std::optional<UserRecord> AuthStore::authenticate_password(const std::string& username,
                                                            const std::string& password) const {
    const auto user = find_user(username);
    if (!user || !user->enabled || !verify_password(password, user->password)) return std::nullopt;
    return user;
}

bool AuthStore::set_user_enabled(const std::string& username, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_locked();
    const auto it = std::find_if(users_.begin(), users_.end(), [&](const UserRecord& u) { return u.username == username; });
    if (it == users_.end()) return false;
    const auto old = it->enabled;
    const auto old_tokens = tokens_;
    it->enabled = enabled;
    if (!enabled)
        for (auto& token : tokens_) if (token.user_id == it->id) token.revoked = true;
    try { persist_locked(); } catch (...) { it->enabled = old; tokens_ = old_tokens; throw; }
    return true;
}

IssuedToken AuthStore::issue_token(const std::string& username, std::chrono::seconds lifetime) {
    if (lifetime <= std::chrono::seconds::zero() || lifetime > std::chrono::hours(24 * 30))
        throw std::invalid_argument("token lifetime must be between 1 second and 30 days");
    const auto raw = generate_bearer_token();
    std::lock_guard<std::mutex> lock(mutex_);
    load_locked();
    const auto user = std::find_if(users_.begin(), users_.end(), [&](const UserRecord& u) { return u.username == username; });
    if (user == users_.end() || !user->enabled) throw std::runtime_error("user is unavailable");
    if (tokens_.size() >= kMaximumTokens) throw std::runtime_error("token limit reached");
    const auto now = unix_seconds(std::chrono::system_clock::now());
    TokenRecord record{generate_bearer_token().substr(0, 32), user->id, digest_bearer_token(raw),
                       now, now + lifetime.count(), false};
    tokens_.push_back(record);
    try { persist_locked(); } catch (...) { tokens_.pop_back(); throw; }
    return {record.id, raw, record.expires_at};
}

std::optional<UserRecord> AuthStore::authenticate_token(const std::string& token,
    std::chrono::system_clock::time_point now) const {
    if (token.size() != 64) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<AuthStore*>(this)->load_locked();
    const auto timestamp = unix_seconds(now);
    const auto record = std::find_if(tokens_.begin(), tokens_.end(), [&](const TokenRecord& t) {
        return !t.revoked && t.expires_at > timestamp && verify_bearer_token(token, t.digest);
    });
    if (record == tokens_.end()) return std::nullopt;
    const auto user = std::find_if(users_.begin(), users_.end(), [&](const UserRecord& u) {
        return u.id == record->user_id && u.enabled;
    });
    if (user == users_.end()) return std::nullopt;
    return *user;
}

bool AuthStore::revoke_token(const std::string& token_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_locked();
    const auto it = std::find_if(tokens_.begin(), tokens_.end(), [&](const TokenRecord& t) { return t.id == token_id; });
    if (it == tokens_.end()) return false;
    if (it->revoked) return true;
    it->revoked = true;
    try { persist_locked(); } catch (...) { it->revoked = false; throw; }
    return true;
}

} // namespace customdb::security
