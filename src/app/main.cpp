#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "codelenses/app/config.hpp"
#include "codelenses/app/version.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/http_server.hpp"
#include "codelenses/server/server_config.hpp"
#include "codelenses/server/service.hpp"

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

    // Initialize SQLite Database and apply migrations
    auto db = codelenses::Database::open(config.server.db_path, true);
    if (!db) {
        std::cerr << "Failed to open or migrate database at " << config.server.db_path << "\n";
        return 1;
    }

    // Initialize Indexing Pipeline
    codelenses::index::IndexerOptions indexer_opts;
    indexer_opts.worker_threads = config.server.worker_threads;
    codelenses::index::IndexingPipeline pipeline(*db, indexer_opts);

    // Initialize API Service
    codelenses::server::WorkspacePolicy policy;
    codelenses::server::ApiService api_service(*db, pipeline, policy);

    // Register initial workspace if provided
    if (config.server.initial_workspace.has_value()) {
        try {
            codelenses::server::CreateWorkspaceRequest req{
                .root_path = *config.server.initial_workspace,
                .name = "",
                .include_patterns = {},
                .exclude_patterns = {},
                .default_ignores = {},
                .compile_commands_path = std::nullopt,
            };
            auto ws = api_service.create_workspace(req);
            std::cout << "Initialized workspace '" << ws.name << "' (ID: " << ws.id << ")\n";
        } catch (const std::exception& ex) {
            std::cerr << "Warning: Could not initialize workspace: " << ex.what() << "\n";
        }
    }

    // Setup signal handlers
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Configure and start HTTP server
    codelenses::server::HttpServerConfig server_config{
        .host = config.server.host,
        .port = config.server.port,
        .db_path = config.server.db_path,
        .initial_workspace = config.server.initial_workspace,
        .log_level = config.server.log_level,
        .worker_threads = config.server.worker_threads,
        .enable_cors = config.server.enable_cors,
        .static_dir = config.server.static_dir,
        .workspace_policy = policy,
    };

    codelenses::server::HttpServer server(api_service, server_config);

    if (!server.start()) {
        std::cerr << "Failed to start HTTP server on " << config.server.host << ":"
                  << config.server.port << "\n";
        return 1;
    }

    std::cout << "Server listening on http://" << config.server.host << ":" << server.port()
              << " ...\n";

    while (!g_stop_requested.load() && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutdown signal received. Stopping server...\n";
    server.stop();
    std::cout << "Server stopped cleanly.\n";

    return 0;
}
