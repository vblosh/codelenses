#include "codelenses/app/config.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "codelenses/app/version.h"

namespace codelenses {

namespace {

template <typename T>
bool parse_complete_integer(std::string_view str, T& out, T min_val, T max_val) {
    if (str.empty()) {
        return false;
    }
    const char* first = str.data();
    const char* last = first + str.size();
    T val = 0;
    auto [ptr, ec] = std::from_chars(first, last, val);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    if (val < min_val || val > max_val) {
        return false;
    }
    out = val;
    return true;
}

} // namespace

void to_json(nlohmann::json& j, const ServerConfig& config) {
    j = nlohmann::json{{"host", config.host},
                       {"port", config.port},
                       {"db_path", config.db_path},
                       {"log_level", config.log_level},
                       {"worker_threads", config.worker_threads},
                       {"enable_cors", config.enable_cors},
                       {"static_dir", config.static_dir}};
    if (config.initial_workspace.has_value()) {
        j["initial_workspace"] = *config.initial_workspace;
    } else {
        j["initial_workspace"] = nullptr;
    }
}

void from_json(const nlohmann::json& j, ServerConfig& config) {
    if (j.contains("host") && j["host"].is_string()) {
        config.host = j["host"].get<std::string>();
    }
    if (j.contains("port")) {
        if (!j["port"].is_number_integer() && !j["port"].is_number_unsigned()) {
            throw std::invalid_argument("Server port must be an integer");
        }
        int64_t raw_port = j["port"].get<int64_t>();
        if (raw_port <= 0 || raw_port > 65535) {
            throw std::out_of_range("Server port out of range (1-65535): " +
                                    std::to_string(raw_port));
        }
        config.port = static_cast<uint16_t>(raw_port);
    }
    if (j.contains("db_path") && j["db_path"].is_string()) {
        config.db_path = j["db_path"].get<std::string>();
    }
    if (j.contains("initial_workspace") && j["initial_workspace"].is_string()) {
        config.initial_workspace = j["initial_workspace"].get<std::string>();
    }
    if (j.contains("log_level") && j["log_level"].is_string()) {
        config.log_level = j["log_level"].get<std::string>();
    }
    if (j.contains("worker_threads")) {
        if (!j["worker_threads"].is_number_integer() && !j["worker_threads"].is_number_unsigned()) {
            throw std::invalid_argument("Worker threads must be an integer");
        }
        int64_t raw_threads = j["worker_threads"].get<int64_t>();
        if (raw_threads <= 0) {
            throw std::out_of_range("Worker threads must be at least 1: " +
                                    std::to_string(raw_threads));
        }
        config.worker_threads = static_cast<size_t>(raw_threads);
    }
    if (j.contains("enable_cors") && j["enable_cors"].is_boolean()) {
        config.enable_cors = j["enable_cors"].get<bool>();
    }
    if (j.contains("static_dir") && j["static_dir"].is_string()) {
        config.static_dir = j["static_dir"].get<std::string>();
    }
}

void to_json(nlohmann::json& j, const AppConfig& config) {
    j = nlohmann::json{{"server", config.server}};
}

void from_json(const nlohmann::json& j, AppConfig& config) {
    if (j.contains("server") && j["server"].is_object()) {
        config.server = j["server"].get<ServerConfig>();
    }
}

AppConfig AppConfig::default_config() {
    return AppConfig{};
}

AppConfig AppConfig::from_json_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Configuration file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open configuration file: " + path.string());
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("JSON parse error in config: ") + e.what());
    }

    try {
        return from_json(j);
    } catch (const std::exception& e) {
        throw std::runtime_error("Configuration error in " + path.string() + ": " + e.what());
    }
}

AppConfig AppConfig::from_json(const nlohmann::json& j) {
    AppConfig config = default_config();
    codelenses::from_json(j, config);
    return config;
}

nlohmann::json AppConfig::to_json() const {
    nlohmann::json j;
    codelenses::to_json(j, *this);
    return j;
}

std::optional<std::string> AppConfig::validate() const {
    if (server.host.empty()) {
        return "Server host cannot be empty";
    }
    if (server.port == 0) {
        return "Server port must be greater than 0";
    }
    if (server.db_path.empty()) {
        return "Database path cannot be empty";
    }
    if (server.worker_threads == 0) {
        return "Worker threads must be at least 1";
    }

    static const std::vector<std::string> valid_log_levels = {"trace", "debug", "info", "warn",
                                                              "error"};

    auto it = std::ranges::find(valid_log_levels, server.log_level);
    if (it == valid_log_levels.end()) {
        return "Invalid log level: '" + server.log_level +
               "'. Allowed: trace, debug, info, warn, error";
    }

    return std::nullopt;
}

void AppConfig::print_help(const char* program_name) {
    std::cout
        << "CodeLenses - Local-first Code Indexer and Browser (v" << kVersionString << ")\n\n"
        << "Usage: " << program_name << " [options]\n\n"
        << "Options:\n"
        << "  -h, --help                 Display this help message and exit\n"
        << "  -v, --version              Display version information and exit\n"
        << "  -c, --config <file>        Load configuration from specified JSON file\n"
        << "      --host <addr>          Bind host address (default: 127.0.0.1)\n"
        << "  -p, --port <port>          Bind port number (default: 8080)\n"
        << "  -d, --db <path>            Path to SQLite database (default: codelenses.db)\n"
        << "  -w, --workspace <path>     Initial workspace root directory to register\n"
        << "  -l, --log-level <level>    Log level [trace|debug|info|warn|error] (default: info)\n"
        << "  -t, --threads <num>        Worker thread pool size (default: 4)\n"
        << "      --static-dir <dir>     Path to frontend static assets directory\n"
        << "\n";
}

CliParseResult AppConfig::parse_cli(int argc, const char* const argv[]) {
    CliParseResult result;
    result.config = default_config();

    const char* program_name = (argc > 0 && argv[0] != nullptr) ? argv[0] : "codelenses";

    std::optional<std::filesystem::path> config_file_path;

    // First pass: locate --config or -c if any
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::ostringstream ss;
            ss << "CodeLenses - Local-first Code Indexer and Browser (v" << kVersionString
               << ")\n\n"
               << "Usage: " << program_name << " [options]\n\n"
               << "Options:\n"
               << "  -h, --help                 Display this help message and exit\n"
               << "  -v, --version              Display version information and exit\n"
               << "  -c, --config <file>        Load configuration from specified JSON file\n"
               << "      --host <addr>          Bind host address (default: 127.0.0.1)\n"
               << "  -p, --port <port>          Bind port number (default: 8080)\n"
               << "  -d, --db <path>            Path to SQLite database (default: codelenses.db)\n"
               << "  -w, --workspace <path>     Initial workspace root directory to register\n"
               << "  -l, --log-level <level>    Log level [trace|debug|info|warn|error] (default: "
                  "info)\n"
               << "  -t, --threads <num>        Worker thread pool size (default: 4)\n"
               << "      --static-dir <dir>     Path to frontend static assets directory\n";
            result.should_exit = true;
            result.exit_code = 0;
            result.message = ss.str();
            return result;
        }

        if (arg == "-v" || arg == "--version") {
            result.should_exit = true;
            result.exit_code = 0;
            result.message = std::string("codelenses version ") + std::string(kVersionString);
            return result;
        }

        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --config requires a file path argument";
                return result;
            }
            config_file_path = argv[++i];
        }
    }

    if (config_file_path.has_value()) {
        try {
            result.config = from_json_file(*config_file_path);
        } catch (const std::exception& e) {
            result.should_exit = true;
            result.exit_code = 1;
            result.message = "Failed to load config file: " + std::string(e.what());
            return result;
        }
    }

    // Second pass: apply CLI overrides
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-c" || arg == "--config") {
            ++i; // already handled
            continue;
        }

        if (arg == "--host") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --host requires an address argument";
                return result;
            }
            result.config.server.host = argv[++i];
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --port requires a port argument";
                return result;
            }
            int port = 0;
            if (!parse_complete_integer(std::string_view(argv[++i]), port, 1, 65535)) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: Invalid port number: " + std::string(argv[i]);
                return result;
            }
            result.config.server.port = static_cast<uint16_t>(port);
        } else if (arg == "-d" || arg == "--db") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --db requires a database path argument";
                return result;
            }
            result.config.server.db_path = argv[++i];
        } else if (arg == "-w" || arg == "--workspace") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --workspace requires a directory path argument";
                return result;
            }
            result.config.server.initial_workspace = argv[++i];
        } else if (arg == "-l" || arg == "--log-level") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --log-level requires a level argument";
                return result;
            }
            result.config.server.log_level = argv[++i];
        } else if (arg == "-t" || arg == "--threads") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --threads requires a thread count argument";
                return result;
            }
            size_t threads = 0;
            if (!parse_complete_integer(std::string_view(argv[++i]), threads,
                                        static_cast<size_t>(1),
                                        std::numeric_limits<size_t>::max())) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: Invalid thread count: " + std::string(argv[i]);
                return result;
            }
            result.config.server.worker_threads = threads;
        } else if (arg == "--static-dir") {
            if (i + 1 >= argc) {
                result.should_exit = true;
                result.exit_code = 1;
                result.message = "Error: --static-dir requires a path argument";
                return result;
            }
            result.config.server.static_dir = argv[++i];
        } else {
            result.should_exit = true;
            result.exit_code = 1;
            result.message =
                "Error: Unknown argument '" + std::string(arg) + "'. Use --help for usage.";
            return result;
        }
    }

    auto validation_error = result.config.validate();
    if (validation_error.has_value()) {
        result.should_exit = true;
        result.exit_code = 1;
        result.message = "Configuration error: " + *validation_error;
        return result;
    }

    return result;
}

} // namespace codelenses
