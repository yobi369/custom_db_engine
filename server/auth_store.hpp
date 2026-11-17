#pragma once

#include "auth_crypto.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace customdb::security {

class AuthPolicyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DatabaseGrant {
    std::string database;
    std::string permission; // "read" or "write"
};

struct UserRecord {
    std::string id;
    std::string username;
    PasswordVerifier password;
    std::string role; // "admin", "read_write", or "read_only"
    bool enabled = true;
    std::vector<DatabaseGrant> grants;
};

struct IssuedToken {
    std::string id;
    std::string token; // returned once; never persisted
    std::int64_t expires_at = 0;
};

class AuthStore {
public:
    explicit AuthStore(std::filesystem::path file);

    // Creates a new empty store. Fails if a store already exists.
    void initialize();
    UserRecord bootstrap_admin(const std::string& username, const std::string& password);
    bool initialized() const;

    // Usernames are case-sensitive. The first user may be an administrator.
    UserRecord create_user(const std::string& username, const std::string& password,
                           const std::string& role,
                           const std::vector<DatabaseGrant>& grants = {});
    std::optional<UserRecord> find_user(const std::string& username) const;
    std::vector<UserRecord> list_users() const;
    bool set_user_grants(const std::string& username, const std::vector<DatabaseGrant>& grants);
    std::optional<UserRecord> authenticate_password(const std::string& username,
                                                      const std::string& password) const;
    bool set_user_enabled(const std::string& username, bool enabled);

    IssuedToken issue_token(const std::string& username,
                            std::chrono::seconds lifetime = std::chrono::hours(24));
    std::optional<UserRecord> authenticate_token(
        const std::string& token,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;
    bool revoke_token(const std::string& token_id);

private:
    void load_locked();
    void persist_locked() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    bool loaded_ = false;
    std::vector<UserRecord> users_;
    struct TokenRecord {
        std::string id;
        std::string user_id;
        std::string digest;
        std::int64_t created_at = 0;
        std::int64_t expires_at = 0;
        bool revoked = false;
    };
    std::vector<TokenRecord> tokens_;
};

} // namespace customdb::security
