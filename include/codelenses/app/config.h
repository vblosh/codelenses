#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace codelenses {

struct ServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string db_path = "codelenses.db";
    std::optional<std::string> initial_workspace = std::nullopt;
    std::string log_level = "info";
    size_t worker_threads = 4;
    bool enable_cors = true;
    std::string static_dir;

    bool operator==(const ServerConfig& other) const = default;
};

struct CliParseResult;

struct AppConfig {
    ServerConfig server;

    static AppConfig default_config();
    static AppConfig from_json_file(const std::filesystem::path& path);
    static AppConfig from_json(const nlohmann::json& j);
    [[nodiscard]] nlohmann::json to_json() const;

    [[nodiscard]] std::optional<std::string> validate() const;

    static CliParseResult parse_cli(int argc, const char* const argv[]);
    static void print_help(const char* program_name);
};

struct CliParseResult {
    bool should_exit = false;
    int exit_code = 0;
    std::string message;
    AppConfig config;
};

void to_json(nlohmann::json& j, const ServerConfig& config);
void from_json(const nlohmann::json& j, ServerConfig& config);

void to_json(nlohmann::json& j, const AppConfig& config);
void from_json(const nlohmann::json& j, AppConfig& config);

} // namespace codelenses
