#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "codelenses/app/config.hpp"
#include "codelenses/app/version.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <tree_sitter/api.h>

namespace {
std::atomic<bool> g_stop_requested{false};

void handle_signal(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_stop_requested.store(true);
    }
}
} // namespace

int main(int argc, char* argv[]) {
    auto parse_result = codelenses::AppConfig::parse_cli(argc, argv);
    if (parse_result.should_exit) {
        if (!parse_result.message.empty()) {
            if (parse_result.exit_code == 0) {
                std::cout << parse_result.message << "\n";
            } else {
                std::cerr << parse_result.message << "\n";
            }
        }
        return parse_result.exit_code;
    }

    const auto& config = parse_result.config;

    std::cout << "========================================\n"
              << " CodeLenses Server v" << codelenses::kVersionString << "\n"
              << "========================================\n"
              << "Host:           " << config.server.host << "\n"
              << "Port:           " << config.server.port << "\n"
              << "Database:       " << config.server.db_path << "\n"
              << "Log Level:      " << config.server.log_level << "\n"
              << "Worker Threads: " << config.server.worker_threads << "\n";
    if (config.server.initial_workspace.has_value()) {
        std::cout << "Workspace:      " << *config.server.initial_workspace << "\n";
    }
    std::cout << "========================================\n";

    // Verify SQLite availability
    sqlite3* db = nullptr;
    int rc = sqlite3_open(config.server.db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open database at " << config.server.db_path << ": "
                  << sqlite3_errmsg(db) << "\n";
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return 1;
    }
    sqlite3_close(db);

    // Setup signal handlers
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Initialize HTTP server
    httplib::Server svr;

    svr.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json body = {{"status", "ok"},
                               {"version", std::string(codelenses::kVersionString)}};
        res.set_content(body.dump(), "application/json");
    });

    svr.Get("/api/v1/version", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json body = {{"name", "codelenses"},
                               {"version", std::string(codelenses::kVersionString)},
                               {"major", codelenses::kVersionMajor},
                               {"minor", codelenses::kVersionMinor},
                               {"patch", codelenses::kVersionPatch}};
        res.set_content(body.dump(), "application/json");
    });

    std::string host = config.server.host;
    uint16_t port = config.server.port;

    // Bind to the port synchronously to catch address/port errors immediately
    if (!svr.bind_to_port(host, port)) {
        std::cerr << "Failed to bind HTTP server to " << host << ":" << port << "\n";
        return 1;
    }

    std::atomic<bool> server_failed{false};

    // Listen on the bound socket in a background thread
    std::thread server_thread([&svr, &server_failed]() {
        if (!svr.listen_after_bind()) {
            server_failed.store(true);
        }
    });

    // Wait until listener is ready or startup failure detected
    svr.wait_until_ready();
    if (!svr.is_running() || server_failed.load()) {
        std::cerr << "Failed to start HTTP server on " << host << ":" << port << "\n";
        svr.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        return 1;
    }

    std::cout << "Server starting on http://" << host << ":" << port << " ...\n";

    while (!g_stop_requested.load() && !server_failed.load() && svr.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (server_failed.load() || (!g_stop_requested.load() && !svr.is_running())) {
        std::cerr << "Server stopped listening unexpectedly.\n";
        if (server_thread.joinable()) {
            server_thread.join();
        }
        return 1;
    }

    std::cout << "\nShutdown signal received. Stopping server...\n";
    svr.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    std::cout << "Server stopped cleanly.\n";

    return 0;
}
