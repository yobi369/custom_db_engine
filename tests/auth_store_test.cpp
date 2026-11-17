#include "../server/auth_store.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace customdb::security;
    const auto root = std::filesystem::temp_directory_path() / ("customdb-auth-test-" + generate_bearer_token());
    const auto file = root / "auth.json";
    AuthStore store(file);
    assert(!store.initialized());
    store.initialize();
    assert(store.initialized());

    bool refused_non_admin = false;
    try { (void)store.create_user("firstuser", "long enough password", "read_only"); }
    catch (const std::runtime_error&) { refused_non_admin = true; }
    assert(refused_non_admin);

    bool refused_overwrite = false;
    try { store.initialize(); } catch (const std::runtime_error&) { refused_overwrite = true; }
    assert(refused_overwrite);

    const auto admin = store.create_user("admin@example.test", "correct horse battery staple", "admin");
    assert(admin.role == "admin");
    assert(store.authenticate_password("admin@example.test", "correct horse battery staple"));
    bool last_admin_protected = false;
    try { (void)store.set_user_enabled("admin@example.test", false); }
    catch (const std::runtime_error&) { last_admin_protected = true; }
    assert(last_admin_protected);
    assert(!store.authenticate_password("admin@example.test", "wrong password"));

    bool duplicate_rejected = false;
    try { (void)store.create_user("admin@example.test", "another password", "read_only"); }
    catch (const std::runtime_error&) { duplicate_rejected = true; }
    assert(duplicate_rejected);

    const auto user = store.create_user("reader01", "reader password", "read_only", {{"sales", "read"}});
    assert(user.grants.size() == 1);
    const auto issued = store.issue_token("reader01");
    assert(issued.token.size() == 64);
    assert(store.authenticate_token(issued.token));
    assert(store.revoke_token(issued.id));
    assert(!store.authenticate_token(issued.token));

    const auto second = store.issue_token("reader01", std::chrono::seconds(5));
    assert(store.authenticate_token(second.token));
    assert(!store.authenticate_token(second.token, std::chrono::system_clock::now() + std::chrono::seconds(10)));
    const auto third = store.issue_token("reader01");
    assert(store.authenticate_token(third.token));
    const auto verified_before_reset = store.authenticate_password("reader01", "reader password");
    assert(verified_before_reset);
    assert(store.set_user_password("reader01", "replacement reader password"));
    bool stale_login_rejected = false;
    try { (void)store.issue_token("reader01", std::chrono::hours(24), &verified_before_reset->password); }
    catch (const AuthPolicyError&) { stale_login_rejected = true; }
    assert(stale_login_rejected);
    assert(!store.authenticate_token(third.token));
    assert(!store.authenticate_password("reader01", "reader password"));
    assert(store.authenticate_password("reader01", "replacement reader password"));
    const auto fourth = store.issue_token("reader01");
    assert(store.authenticate_token(fourth.token));
    assert(!store.change_user_password("reader01", "incorrect current password", "new reader password"));
    assert(store.change_user_password("reader01", "replacement reader password", "new reader password"));
    assert(!store.authenticate_token(fourth.token));
    assert(!store.authenticate_password("reader01", "replacement reader password"));
    assert(store.authenticate_password("reader01", "new reader password"));
    const auto fifth = store.issue_token("reader01");
    assert(store.authenticate_token(fifth.token));
    assert(store.revoke_token_value(fifth.token));
    assert(!store.authenticate_token(fifth.token));
    assert(store.set_user_enabled("reader01", false));
    assert(!store.authenticate_password("reader01", "new reader password"));
    assert(!store.authenticate_token(fourth.token));

    AuthStore reopened(file);
    assert(reopened.find_user("admin@example.test"));
    assert(reopened.find_user("reader01")->role == "read_only");

    AuthStore bootstrapped(root / "bootstrap" / "auth.json");
    const auto first_admin = bootstrapped.bootstrap_admin("owner01", "strong bootstrap password");
    assert(first_admin.role == "admin");
    assert(bootstrapped.authenticate_password("owner01", "strong bootstrap password"));
    bool bootstrap_refused = false;
    try { (void)bootstrapped.bootstrap_admin("owner02", "another strong password"); }
    catch (const std::runtime_error&) { bootstrap_refused = true; }
    assert(bootstrap_refused);

    std::filesystem::remove_all(root);
    std::cout << "Authentication store tests passed\n";
    return 0;
}
