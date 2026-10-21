#include <storage_engine.h>
int main() {
    StorageEngine db("package-consumer.db.json");
    return db.write_data("installed-package", "works") ? 0 : 1;
}
