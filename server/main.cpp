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

    std::cout << "CustomDB server listening on " << host << ':' << port
              << " using data directory " << root << '\n';
    if (!api_key.empty()) std::cout << "API-key authentication is enabled.\n";
    if (!server.listen(host.c_str(), port)) {
        std::cerr << "Failed to start HTTP listener.\n";
        return 1;
    }
    return 0;
}
