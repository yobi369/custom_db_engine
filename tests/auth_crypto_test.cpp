#include "../server/auth_crypto.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace customdb::security;

    const auto first = hash_password("correct horse battery staple");
    const auto second = hash_password("correct horse battery staple");
    assert(first.algorithm == "pbkdf2-hmac-sha256");
    assert(first.iterations == 600000);
    assert(first.salt_hex.size() == 32);
    assert(first.verifier_hex.size() == 64);
    assert(first.salt_hex != second.salt_hex);
    assert(verify_password("correct horse battery staple", first));
    assert(!verify_password("incorrect password", first));

    auto malformed = first;
    malformed.algorithm = "unknown";
    assert(!verify_password("correct horse battery staple", malformed));
    malformed = first;
    malformed.iterations = 1;
    assert(!verify_password("correct horse battery staple", malformed));
    malformed = first;
    malformed.salt_hex = "not-hex";
    assert(!verify_password("correct horse battery staple", malformed));

    bool rejected_empty_password = false;
    try { (void)hash_password(""); } catch (const std::invalid_argument&) {
        rejected_empty_password = true;
    }
    assert(rejected_empty_password);

    const auto token = generate_bearer_token();
    assert(token.size() == 64);
    const auto digest = digest_bearer_token(token);
    assert(digest.size() == 64);
    assert(verify_bearer_token(token, digest));
    assert(verify_bearer_token_digest(digest, digest));
    assert(!verify_bearer_token_digest(digest, digest_bearer_token("different token")));
    assert(!verify_bearer_token(token + "x", digest));
    assert(!verify_bearer_token("different-token", digest));
    assert(!verify_bearer_token(token, "invalid"));

    std::cout << "Authentication crypto tests passed\n";
    return 0;
}
