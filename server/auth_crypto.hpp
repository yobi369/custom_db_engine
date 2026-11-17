#pragma once

#include <string>

namespace customdb::security {

struct PasswordVerifier {
    std::string algorithm;
    unsigned int iterations;
    std::string salt_hex;
    std::string verifier_hex;
};

// Uses PBKDF2-HMAC-SHA-256 with a fresh 128-bit random salt.
PasswordVerifier hash_password(const std::string& password);
bool verify_password(const std::string& password, const PasswordVerifier& stored);

// Returns 32 cryptographically random bytes encoded as 64 lowercase hex characters.
std::string generate_bearer_token();

// Returns the SHA-256 hex digest of a bearer token. Persist this digest, not the token.
std::string digest_bearer_token(const std::string& token);
bool verify_bearer_token(const std::string& token, const std::string& stored_digest_hex);

} // namespace customdb::security
