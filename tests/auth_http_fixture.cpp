#include "../server/auth_store.hpp"
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    customdb::security::AuthStore store(root / "auth" / "auth_store.json");
    store.bootstrap_admin("rootadmin", "administrator password 123");
    store.create_user("writer01", "writer password 123", "read_write", {{"alpha", "write"}});
    store.create_user("reader01", "reader password 123", "read_only", {{"alpha", "read"}});
    store.create_user("limited01", "limited password 123", "read_write", {{"alpha", "read"}});
    return 0;
}
