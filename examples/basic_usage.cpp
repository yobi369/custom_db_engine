#include "storage_engine.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "example.db.json";
    StorageEngine database(path);

    if (!database.write_data("greeting", "Hello from CustomDBEngine")) {
        std::cerr << "Write failed: " << database.get_last_error() << '\n';
        return 1;
    }

    const std::string greeting = database.read_data("greeting");
    if (!database.get_last_error().empty()) {
        std::cerr << "Read failed: " << database.get_last_error() << '\n';
        return 1;
    }

    if (!database.check_integrity()) {
        std::cerr << "Integrity check failed: " << database.get_last_error() << '\n';
        return 1;
    }

    std::cout << greeting << '\n';
    return 0;
}
