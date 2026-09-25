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

    std::vector<std::string> allowed_roots;
    std::vector<std::string> forbidden_roots = {"/",     "/etc",     "/proc", "/sys",    "/dev",
                                                "/boot", "/root",    "/bin",  "/sbin",   "/usr/bin",
                                                "/lib",  "/usr/lib", "/run",  "/var/run"};

    [[nodiscard]] bool is_allowed_root(const std::filesystem::path& canonical_path,
                                       std::string* error_reason = nullptr) const {
        if (canonical_path.empty()) {
            if (error_reason) {
                *error_reason = "Workspace root path is empty";
            }
            return false;
        }

        auto norm_path = canonical_path.lexically_normal();
        if (norm_path == "/" || norm_path == norm_path.root_path()) {
            if (error_reason) {
                *error_reason = "Root filesystem directory '/' cannot be used as a workspace";
            }
            return false;
        }

        for (const auto& forbidden : forbidden_roots) {
            std::error_code ec;
            std::filesystem::path fb_path(forbidden);
            auto fb_canon = std::filesystem::exists(fb_path, ec)
                                ? std::filesystem::canonical(fb_path, ec)
                                : fb_path.lexically_normal();
            if (fb_canon.empty()) {
                fb_canon = fb_path.lexically_normal();
            }

            if (norm_path == fb_canon) {
                if (error_reason) {
                    *error_reason =
                        "Workspace root cannot be forbidden system directory: " + forbidden;
                }
                return false;
            }

            if (fb_canon == "/" || fb_canon == fb_canon.root_path()) {
                continue;
            }

            auto rel = norm_path.lexically_relative(fb_canon);
            if (!rel.empty() && rel.native() != "." && rel.native() != ".." &&
                !rel.native().starts_with("..")) {
                if (error_reason) {
                    *error_reason =
                        "Workspace root is inside forbidden system directory: " + forbidden;
                }
                return false;
            }
        }

        if (!allowed_roots.empty()) {
            bool matches_allowed = false;
            for (const auto& allowed : allowed_roots) {
                std::error_code ec;
                std::filesystem::path al_path(allowed);
                auto al_canon = std::filesystem::exists(al_path, ec)
                                    ? std::filesystem::canonical(al_path, ec)
                                    : al_path.lexically_normal();
                if (al_canon.empty()) {
                    al_canon = al_path.lexically_normal();
                }

                if (norm_path == al_canon) {
                    matches_allowed = true;
                    break;
                }
                auto rel = norm_path.lexically_relative(al_canon);
                if (!rel.empty() && rel.native() != "." && rel.native() != ".." &&
                    !rel.native().starts_with("..")) {
                    matches_allowed = true;
                    break;
                }
            }
            if (!matches_allowed) {
                if (error_reason) {
                    *error_reason = "Workspace root is not within any allowed roots";
                }
                return false;
            }
        }

        return true;
    }

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
        {"allowedRoots", p.allowed_roots},
        {"forbiddenRoots", p.forbidden_roots},
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
    if (j.contains("allowedRoots"))
        p.allowed_roots = j["allowedRoots"].get<std::vector<std::string>>();
    if (j.contains("forbiddenRoots"))
        p.forbidden_roots = j["forbiddenRoots"].get<std::vector<std::string>>();
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
