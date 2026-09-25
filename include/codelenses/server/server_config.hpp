#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace codelenses::server {

struct WorkspacePolicy {
    bool allow_external_symlinks = false;
    size_t max_file_size_bytes = 32U * 1024U * 1024U;
    size_t default_page_size = 50;
    size_t max_page_size = 200;
    std::vector<std::string> default_ignores = {
        ".git",   "node_modules", "build",       "dist",   "target",
        "vendor", ".codelens",    ".codelenses", ".cache",
    };

    bool operator==(const WorkspacePolicy& other) const = default;
};

struct HttpServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string db_path = "codelenses.db";
    std::optional<std::string> initial_workspace = std::nullopt;
    std::string log_level = "info";
    size_t worker_threads = 4;
    bool enable_cors = true;
    std::string static_dir;
    WorkspacePolicy workspace_policy;

    bool operator==(const HttpServerConfig& other) const = default;
};

inline void to_json(nlohmann::json& j, const WorkspacePolicy& p) {
    j = nlohmann::json{
        {"allowExternalSymlinks", p.allow_external_symlinks},
        {"maxFileSizeBytes", p.max_file_size_bytes},
        {"defaultPageSize", p.default_page_size},
        {"maxPageSize", p.max_page_size},
        {"defaultIgnores", p.default_ignores},
    };
}

inline void from_json(const nlohmann::json& j, WorkspacePolicy& p) {
    if (j.contains("allowExternalSymlinks"))
        p.allow_external_symlinks = j["allowExternalSymlinks"].get<bool>();
    if (j.contains("maxFileSizeBytes"))
        p.max_file_size_bytes = j["maxFileSizeBytes"].get<size_t>();
    if (j.contains("defaultPageSize"))
        p.default_page_size = j["defaultPageSize"].get<size_t>();
    if (j.contains("maxPageSize"))
        p.max_page_size = j["maxPageSize"].get<size_t>();
    if (j.contains("defaultIgnores"))
        p.default_ignores = j["defaultIgnores"].get<std::vector<std::string>>();
}

inline void to_json(nlohmann::json& j, const HttpServerConfig& c) {
    j = nlohmann::json{
        {"host", c.host},
        {"port", c.port},
        {"dbPath", c.db_path},
        {"logLevel", c.log_level},
        {"workerThreads", c.worker_threads},
        {"enableCors", c.enable_cors},
        {"staticDir", c.static_dir},
        {"workspacePolicy", c.workspace_policy},
    };
    if (c.initial_workspace.has_value()) {
        j["initialWorkspace"] = *c.initial_workspace;
    } else {
        j["initialWorkspace"] = nullptr;
    }
}

inline void from_json(const nlohmann::json& j, HttpServerConfig& c) {
    if (j.contains("host"))
        c.host = j["host"].get<std::string>();
    if (j.contains("port"))
        c.port = j["port"].get<uint16_t>();
    if (j.contains("dbPath"))
        c.db_path = j["dbPath"].get<std::string>();
    if (j.contains("initialWorkspace") && !j["initialWorkspace"].is_null()) {
        c.initial_workspace = j["initialWorkspace"].get<std::string>();
    }
    if (j.contains("logLevel"))
        c.log_level = j["logLevel"].get<std::string>();
    if (j.contains("workerThreads"))
        c.worker_threads = j["workerThreads"].get<size_t>();
    if (j.contains("enableCors"))
        c.enable_cors = j["enableCors"].get<bool>();
    if (j.contains("staticDir"))
        c.static_dir = j["staticDir"].get<std::string>();
    if (j.contains("workspacePolicy"))
        c.workspace_policy = j["workspacePolicy"].get<WorkspacePolicy>();
}

} // namespace codelenses::server
