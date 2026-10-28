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

    const ReadResult greeting_result = database.read_data_result("greeting");
    if (!greeting_result.found) {
        std::cerr << "Read failed: " << greeting_result.error << '\n';
        return 1;
    }
    const std::string& greeting = greeting_result.value;

    if (!database.check_integrity()) {
        std::cerr << "Integrity check failed: " << database.get_last_error() << '\n';
        return 1;
    }

    std::cout << greeting << '\n';
    return 0;
}
