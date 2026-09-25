#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "codelenses/server/server_config.hpp"
#include "codelenses/server/service.hpp"
#include <httplib.h>

namespace codelenses::server {

class HttpServer {
public:
    HttpServer(ApiService& service, HttpServerConfig config);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Starts HTTP server in background thread. Binds host and port.
    bool start();

    // Requests server shutdown, cancels active jobs, and joins server thread.
    void stop();

    // Blocks caller until server stops.
    void wait();

    [[nodiscard]] uint16_t port() const noexcept { return bound_port_; }
    [[nodiscard]] bool is_running() const noexcept { return is_running_.load(); }
    [[nodiscard]] const HttpServerConfig& config() const noexcept { return config_; }
    [[nodiscard]] httplib::Server& raw_server() noexcept { return svr_; }

private:
    void register_routes();
    void setup_cors();

    ApiService& service_;
    HttpServerConfig config_;
    httplib::Server svr_;

    std::thread server_thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<bool> stop_requested_{false};
    uint16_t bound_port_{0};
};

} // namespace codelenses::server
