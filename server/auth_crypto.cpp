#include "auth_crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace customdb::security {
namespace {
constexpr unsigned int kPasswordIterations = 600000;
constexpr int kSaltBytes = 16;
constexpr int kDerivedBytes = 32;
constexpr int kTokenBytes = 32;
constexpr std::size_t kMaximumPasswordBytes = 1024;

std::string to_hex(const unsigned char* bytes, std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (std::size_t i = 0; i < length; ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

bool from_hex(const std::string& value, unsigned char* output, std::size_t output_size) {
    if (value.size() != output_size * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < output_size; ++i) {
        const int high = nibble(value[i * 2]);
        const int low = nibble(value[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

void require_crypto_success(int result, const char* operation) {
    if (result != 1) throw std::runtime_error(std::string("OpenSSL operation failed: ") + operation);
}
} // namespace

PasswordVerifier hash_password(const std::string& password) {
    if (password.empty()) throw std::invalid_argument("password must not be empty");
    if (password.size() > kMaximumPasswordBytes)
        throw std::invalid_argument("password exceeds the 1024-byte limit");
    std::array<unsigned char, kSaltBytes> salt{};
    std::array<unsigned char, kDerivedBytes> derived{};
    require_crypto_success(RAND_bytes(salt.data(), static_cast<int>(salt.size())), "RAND_bytes");
    require_crypto_success(PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()),
        static_cast<int>(kPasswordIterations), EVP_sha256(), static_cast<int>(derived.size()), derived.data()),
        "PKCS5_PBKDF2_HMAC");
    const auto verifier = to_hex(derived.data(), derived.size());
    OPENSSL_cleanse(derived.data(), derived.size());
    return {"pbkdf2-hmac-sha256", kPasswordIterations,
            to_hex(salt.data(), salt.size()), verifier};
}

bool verify_password(const std::string& password, const PasswordVerifier& stored) {
    if (password.empty() || password.size() > kMaximumPasswordBytes || stored.algorithm != "pbkdf2-hmac-sha256" ||
        stored.iterations < 100000 || stored.iterations > 2000000) return false;
    std::array<unsigned char, kSaltBytes> salt{};
    std::array<unsigned char, kDerivedBytes> expected{};
    std::array<unsigned char, kDerivedBytes> actual{};
    if (!from_hex(stored.salt_hex, salt.data(), salt.size()) ||
        !from_hex(stored.verifier_hex, expected.data(), expected.size())) return false;
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
            salt.data(), static_cast<int>(salt.size()), static_cast<int>(stored.iterations),
            EVP_sha256(), static_cast<int>(actual.size()), actual.data()) != 1) return false;
    const bool matches = CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
    OPENSSL_cleanse(actual.data(), actual.size());
    OPENSSL_cleanse(expected.data(), expected.size());
    return matches;
}

std::string generate_bearer_token() {
    std::array<unsigned char, kTokenBytes> token{};
    require_crypto_success(RAND_bytes(token.data(), static_cast<int>(token.size())), "RAND_bytes");
    return to_hex(token.data(), token.size());
}

std::string digest_bearer_token(const std::string& token) {
    if (token.empty()) throw std::invalid_argument("token must not be empty");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    require_crypto_success(EVP_Digest(token.data(), token.size(), digest.data(), &length, EVP_sha256(), nullptr),
                           "EVP_Digest");
    if (length != 32) throw std::runtime_error("unexpected SHA-256 digest length");
    return to_hex(digest.data(), length);
}

bool verify_bearer_token(const std::string& token, const std::string& stored_digest_hex) {
    if (token.empty()) return false;
    std::array<unsigned char, 32> expected{};
    if (!from_hex(stored_digest_hex, expected.data(), expected.size())) return false;
    const auto actual_hex = digest_bearer_token(token);
    std::array<unsigned char, 32> actual{};
    if (!from_hex(actual_hex, actual.data(), actual.size())) return false;
    return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

} // namespace customdb::security
