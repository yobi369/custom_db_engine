#include "storage_engine.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::mutex engine_mutex;
const std::regex database_name_pattern("^[a-zA-Z][a-zA-Z0-9_-]{0,62}$");

std::string env_or(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value && *value ? value : fallback;
}

bool valid_database_name(const std::string& name) {
    return std::regex_match(name, database_name_pattern);
}

void respond_json(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

bool authorized(const httplib::Request& request, const std::string& api_key) {
    if (api_key.empty()) return true;
    const auto supplied = request.get_header_value("X-CustomDB-Key");
    if (supplied.size() != api_key.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < api_key.size(); ++i)
        difference |= static_cast<unsigned char>(supplied[i] ^ api_key[i]);
    return difference == 0;
}

fs::path database_path(const fs::path& root, const std::string& name) {
    return root / (name + ".cdb.json");
}
}

int main() {
    const auto host = env_or("CUSTOMDB_HOST", "127.0.0.1");
    const auto port_text = env_or("CUSTOMDB_PORT", "7680");
    const auto root = fs::absolute(env_or("CUSTOMDB_DATA_DIR", "./customdb-data")).lexically_normal();
    const auto api_key = env_or("CUSTOMDB_API_KEY", "");

    int port = 7680;
    try {
        std::size_t consumed = 0;
        port = std::stoi(port_text, &consumed);
        if (consumed != port_text.size() || port < 1 || port > 65535)
            throw std::invalid_argument("invalid port");
    } catch (...) {
        std::cerr << "CUSTOMDB_PORT must be an integer from 1 to 65535\n";
        return 2;
    }

    if (host != "127.0.0.1" && host != "localhost" && host != "::1" && api_key.empty()) {
        std::cerr << "Refusing non-loopback bind without CUSTOMDB_API_KEY.\n";
        return 2;
    }

    try {
        fs::create_directories(root);
    } catch (const std::exception& error) {
        std::cerr << "Cannot create database directory: " << error.what() << '\n';
        return 2;
    }

    httplib::Server server;
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(5, 0);

    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        respond_json(response, 200, {{"status", "ok"}, {"service", "customdb-server"}, {"api_version", 1}});
    });

    server.Get("/v1/databases", [root, api_key](const httplib::Request& request, httplib::Response& response) {
        if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
        json databases = json::array();
        for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                const auto filename = entry.path().filename().string();
                constexpr const char* suffix = ".cdb.json";
                const std::string suffix_string(suffix);
                if (filename.size() > suffix_string.size() &&
                    filename.compare(filename.size() - suffix_string.size(), suffix_string.size(), suffix) == 0)
                    databases.push_back(filename.substr(0, filename.size() - suffix_string.size()));
            }
        }
        respond_json(response, 200, {{"databases", databases}});
    });

    server.Post("/v1/databases", [root, api_key](const httplib::Request& request, httplib::Response& response) {
        if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
        json body = json::parse(request.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("name") || !body["name"].is_string())
            return respond_json(response, 400, {{"error", "expected JSON body with string field 'name'"}});
        const auto name = body["name"].get<std::string>();
        if (!valid_database_name(name))
            return respond_json(response, 400, {{"error", "invalid database name"}});
        const auto path = database_path(root, name);
        std::lock_guard<std::mutex> lock(engine_mutex);
        if (fs::exists(path)) return respond_json(response, 409, {{"error", "database already exists"}});
        try {
            StorageEngine engine(path.string());
            if (!engine.create_database())
                return respond_json(response, 500, {{"error", engine.get_last_error()}});
        } catch (const std::exception& error) {
            return respond_json(response, 500, {{"error", error.what()}});
        }
        respond_json(response, 201, {{"name", name}, {"status", "created"}});
    });

    server.Put(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/kv/([^/]+))",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto key = request.matches[2].str();
            json body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("value") || !body["value"].is_string())
                return respond_json(response, 400, {{"error", "expected JSON body with string field 'value'"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.write_data(key, body["value"].get<std::string>()))
                    return respond_json(response, 500, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"key", key}, {"status", "stored"}});
        });

    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/kv/([^/]+))",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto key = request.matches[2].str();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                const auto value = engine.read_data(key);
                const auto error = engine.get_last_error();
                if (!error.empty()) return respond_json(response, 404, {{"error", error}});
                respond_json(response, 200, {{"key", key}, {"value", value}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Delete(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/kv/([^/]+))",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto key = request.matches[2].str();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.delete_data(key)) return respond_json(response, 404, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"key", key}, {"status", "deleted"}});
        });


    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/collections/([a-zA-Z][a-zA-Z0-9_-]{0,62})/documents)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto collection = request.matches[2].str();
            const auto document = json::parse(request.body, nullptr, false);
            if (document.is_discarded() || !document.is_object())
                return respond_json(response, 400, {{"error", "request body must be a JSON object"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.write_document(collection, document.dump()))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 201, {{"collection", collection}, {"status", "inserted"}, {"document", document}});
        });

    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/collections/([a-zA-Z][a-zA-Z0-9_-]{0,62})/documents)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto collection = request.matches[2].str();
            std::map<std::string, std::string> filters;
            for (const auto& parameter : request.params) filters[parameter.first] = parameter.second;
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                const auto rows = engine.query_documents(collection, filters);
                const auto error = engine.get_last_error();
                if (!error.empty()) return respond_json(response, 400, {{"error", error}});
                json documents = json::array();
                for (const auto& row : rows) {
                    auto parsed = json::parse(row, nullptr, false);
                    if (!parsed.is_discarded()) documents.push_back(std::move(parsed));
                }
                respond_json(response, 200, {{"collection", collection}, {"count", documents.size()}, {"documents", documents}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/schemas)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto schema = json::parse(request.body, nullptr, false);
            if (schema.is_discarded() || !schema.is_object())
                return respond_json(response, 400, {{"error", "request body must be a schema JSON object"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.create_schema(schema.dump()))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 201, {{"status", "schema_created"}, {"schema", schema}});
        });

    server.Put(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/indexes)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("field") || !body["field"].is_string())
                return respond_json(response, 400, {{"error", "expected JSON body with string field 'field'"}});
            const auto field = body["field"].get<std::string>();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.create_index(field))
                    return respond_json(response, 400, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 201, {{"status", "index_created"}, {"field", field}});
        });

    server.Delete(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/indexes)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto field = request.get_param_value("field");
            if (field.empty()) return respond_json(response, 400, {{"error", "missing field query parameter"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.drop_index(field))
                    return respond_json(response, 404, {{"error", engine.get_last_error()}});
            } catch (const std::exception& error) {
                return respond_json(response, 500, {{"error", error.what()}});
            }
            respond_json(response, 200, {{"status", "index_dropped"}, {"field", field}});
        });



    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/integrity)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.check_integrity())
                    return respond_json(response, 409, {{"status", "failed"}, {"error", engine.get_last_error()}});
                respond_json(response, 200, {{"status", "ok"}, {"database", name}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Get(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/backups)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto directory = root / "backups" / name;
            json backups = json::array();
            std::error_code ec;
            if (fs::exists(directory, ec)) {
                if (ec || !fs::is_directory(directory, ec))
                    return respond_json(response, 500, {{"error", "cannot inspect backup directory"}});
                for (const auto& entry : fs::directory_iterator(directory, ec)) {
                    if (ec) return respond_json(response, 500, {{"error", "cannot list backups"}});
                    if (entry.is_regular_file() && entry.path().extension() == ".backup") {
                        backups.push_back({{"name", entry.path().stem().string()},
                                           {"size_bytes", entry.file_size()}});
                    }
                }
            }
            respond_json(response, 200, {{"database", name}, {"backups", backups}});
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/backups)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("name") ||
                !body["name"].is_string())
                return respond_json(response, 400, {{"error", "expected JSON body with string field 'name'"}});
            const auto backup_name = body["name"].get<std::string>();
            static const std::regex backup_pattern("^[a-zA-Z0-9][a-zA-Z0-9_-]{0,62}$");
            if (!std::regex_match(backup_name, backup_pattern))
                return respond_json(response, 400, {{"error", "invalid backup name"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto source = database_path(root, name);
            if (!fs::is_regular_file(source)) return respond_json(response, 404, {{"error", "database not found"}});
            const auto directory = root / "backups" / name;
            const auto target = directory / (backup_name + ".backup");
            try {
                fs::create_directories(directory);
                if (fs::exists(target)) return respond_json(response, 409, {{"error", "backup already exists"}});
                StorageEngine engine(source.string());
                if (!engine.backup_database(target.string()))
                    return respond_json(response, 500, {{"error", engine.get_last_error()}});
                respond_json(response, 201, {{"database", name}, {"backup", backup_name}, {"status", "created"}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/backups/([a-zA-Z0-9][a-zA-Z0-9_-]{0,62})/restore)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto backup_name = request.matches[2].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || body.value("confirm", false) != true)
                return respond_json(response, 400, {{"error", "restore requires JSON body {\"confirm\":true}"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto active = database_path(root, name);
            const auto backup = root / "backups" / name / (backup_name + ".backup");
            if (!fs::is_regular_file(active)) return respond_json(response, 404, {{"error", "database not found"}});
            if (!fs::is_regular_file(backup)) return respond_json(response, 404, {{"error", "backup not found"}});
            try {
                StorageEngine engine(active.string());
                if (!engine.restore_database(backup.string()))
                    return respond_json(response, 409, {{"error", engine.get_last_error()}});
                respond_json(response, 200, {{"database", name}, {"backup", backup_name}, {"status", "restored"}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    server.Post(R"(/v1/databases/([a-zA-Z][a-zA-Z0-9_-]{0,62})/transactions)",
        [root, api_key](const httplib::Request& request, httplib::Response& response) {
            if (!authorized(request, api_key)) return respond_json(response, 401, {{"error", "unauthorized"}});
            const auto name = request.matches[1].str();
            const auto body = json::parse(request.body, nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("operations") ||
                !body["operations"].is_array() || body["operations"].empty() || body["operations"].size() > 100)
                return respond_json(response, 400, {{"error", "operations must be a non-empty array with at most 100 entries"}});
            std::lock_guard<std::mutex> lock(engine_mutex);
            const auto path = database_path(root, name);
            if (!fs::is_regular_file(path)) return respond_json(response, 404, {{"error", "database not found"}});
            try {
                StorageEngine engine(path.string());
                if (!engine.begin_transaction())
                    return respond_json(response, 409, {{"error", engine.get_last_error()}});
                std::size_t completed = 0;
                for (const auto& operation : body["operations"]) {
                    if (!operation.is_object() || !operation.contains("type") || !operation["type"].is_string()) {
                        engine.rollback_transaction();
                        return respond_json(response, 400, {{"error", "each operation requires a string type"}, {"failed_operation", completed}});
                    }
                    const auto type = operation["type"].get<std::string>();
                    bool ok = false;
                    if (type == "put" && operation.contains("key") && operation["key"].is_string() &&
                        operation.contains("value") && operation["value"].is_string()) {
                        ok = engine.write_data(operation["key"].get<std::string>(), operation["value"].get<std::string>());
                    } else if (type == "delete" && operation.contains("key") && operation["key"].is_string()) {
                        ok = engine.delete_data(operation["key"].get<std::string>());
                    } else if (type == "insert_document" && operation.contains("collection") &&
                        operation["collection"].is_string() && operation.contains("document") && operation["document"].is_object()) {
                        ok = engine.write_document(operation["collection"].get<std::string>(), operation["document"].dump());
                    } else {
                        engine.rollback_transaction();
                        return respond_json(response, 400, {{"error", "unsupported or malformed operation"}, {"failed_operation", completed}});
                    }
                    if (!ok) {
                        const auto error = engine.get_last_error();
                        engine.rollback_transaction();
                        return respond_json(response, 400, {{"error", error}, {"failed_operation", completed}});
                    }
                    ++completed;
                }
                if (!engine.commit_transaction()) {
                    const auto error = engine.get_last_error();
                    engine.rollback_transaction();
                    return respond_json(response, 500, {{"error", error}});
                }
                respond_json(response, 200, {{"status", "committed"}, {"operations", completed}});
            } catch (const std::exception& error) {
                respond_json(response, 500, {{"error", error.what()}});
            }
        });

    std::cout << "CustomDB server listening on " << host << ':' << port
              << " using data directory " << root << '\n';
    if (!api_key.empty()) std::cout << "API-key authentication is enabled.\n";
    if (!server.listen(host.c_str(), port)) {
        std::cerr << "Failed to start HTTP listener.\n";
        return 1;
    }
    return 0;
}
