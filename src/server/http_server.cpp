#include "codelenses/server/http_server.hpp"

#include <charconv>
#include <iostream>

#include "codelenses/app/version.hpp"
#include "codelenses/server/error.hpp"

namespace codelenses::server {

namespace {

int64_t parse_id(std::string_view str, std::string_view name = "id") {
    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val);
    if (ec != std::errc{} || ptr != str.data() + str.size() || val <= 0) {
        throw ApiError::bad_request("invalid_id",
                                    std::string(name) + " must be a positive integer");
    }
    return val;
}

std::optional<int64_t> get_int_query_param(const httplib::Request& req, const std::string& name) {
    if (req.has_param(name)) {
        std::string val_str = req.get_param_value(name);
        int64_t val = 0;
        auto [ptr, ec] = std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
        if (ec == std::errc{} && ptr == val_str.data() + val_str.size()) {
            return val;
        }
    }
    return std::nullopt;
}

std::optional<std::string> get_str_query_param(const httplib::Request& req,
                                               const std::string& name) {
    if (req.has_param(name)) {
        return req.get_param_value(name);
    }
    return std::nullopt;
}

template <typename Fn>
void handle_json(const httplib::Request& req, httplib::Response& res, Fn&& fn) {
    std::string req_id = req.has_header("X-Request-ID") ? req.get_header_value("X-Request-ID")
                                                        : generate_request_id();
    res.set_header("X-Request-ID", req_id);

    try {
        nlohmann::json body = fn(req, req_id);
        res.set_content(body.dump(), "application/json");
        if (res.status <= 0) {
            res.status = 200;
        }
    } catch (const ApiError& err) {
        res.status = err.status_code;
        res.set_content(make_error_envelope(err, req_id).dump(), "application/json");
    } catch (const nlohmann::json::exception& jerr) {
        ApiError err = ApiError::bad_request("invalid_json", jerr.what());
        res.status = err.status_code;
        res.set_content(make_error_envelope(err, req_id).dump(), "application/json");
    } catch (const std::exception& ex) {
        ApiError err = ApiError::internal_error(ex.what());
        res.status = err.status_code;
        res.set_content(make_error_envelope(err, req_id).dump(), "application/json");
    }
}

} // namespace

HttpServer::HttpServer(ApiService& service, HttpServerConfig config)
    : service_(service), config_(std::move(config)) {
    setup_cors();
    register_routes();
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::setup_cors() {
    svr_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (config_.enable_cors && req.method == "OPTIONS") {
            res.status = 204;
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, Authorization, X-Request-ID");
            res.set_header("Access-Control-Max-Age", "86400");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr_.set_post_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (config_.enable_cors) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, Authorization, X-Request-ID");
            res.set_header("Access-Control-Max-Age", "86400");
        }
        if (!res.has_header("X-Request-ID")) {
            std::string req_id = req.has_header("X-Request-ID")
                                     ? req.get_header_value("X-Request-ID")
                                     : generate_request_id();
            res.set_header("X-Request-ID", req_id);
        }
    });

    svr_.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (res.body.empty() || res.get_header_value("Content-Type") != "application/json") {
            std::string req_id = req.has_header("X-Request-ID")
                                     ? req.get_header_value("X-Request-ID")
                                     : generate_request_id();
            int st = res.status > 0 ? res.status : 404;
            ApiError err(st, st == 404 ? "not_found" : "http_error",
                         "HTTP request failed with status " + std::to_string(st));
            res.set_content(make_error_envelope(err, req_id).dump(), "application/json");
        }
    });
}

void HttpServer::register_routes() {
    // Static assets
    if (!config_.static_dir.empty()) {
        svr_.set_mount_point("/", config_.static_dir);
    }

    // Health and version
    svr_.Get("/api/v1/health", [](const httplib::Request& req, httplib::Response& res) {
        handle_json(req, res, [](const httplib::Request&, const std::string&) -> nlohmann::json {
            return nlohmann::json{
                {"status", "ok"},
                {"version", std::string(kVersionString)},
            };
        });
    });

    svr_.Get("/api/v1/version", [](const httplib::Request& req, httplib::Response& res) {
        handle_json(req, res, [](const httplib::Request&, const std::string&) -> nlohmann::json {
            return nlohmann::json{
                {"name", "codelenses"},   {"version", std::string(kVersionString)},
                {"major", kVersionMajor}, {"minor", kVersionMinor},
                {"patch", kVersionPatch},
            };
        });
    });

    // ==========================================
    // Workspace CRUD (F-04)
    // ==========================================
    svr_.Post("/api/v1/workspaces", [this](const httplib::Request& req, httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            if (r.body.empty()) {
                throw ApiError::bad_request("empty_body", "Request body cannot be empty");
            }
            auto j = nlohmann::json::parse(r.body);
            CreateWorkspaceRequest create_req = j.get<CreateWorkspaceRequest>();
            auto dto = service_.create_workspace(create_req);
            res.status = 201;
            return dto;
        });
    });

    svr_.Get("/api/v1/workspaces", [this](const httplib::Request& req, httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request&, const std::string&) -> nlohmann::json {
            auto workspaces = service_.list_workspaces();
            return nlohmann::json{
                {"workspaces", workspaces},
                {"total", workspaces.size()},
            };
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+))", [this](const httplib::Request& req,
                                                   httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "workspaceId");
            return service_.get_workspace(id);
        });
    });

    svr_.Patch(R"(/api/v1/workspaces/(\d+))", [this](const httplib::Request& req,
                                                     httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "workspaceId");
            if (r.body.empty()) {
                throw ApiError::bad_request("empty_body", "Request body cannot be empty");
            }
            auto j = nlohmann::json::parse(r.body);
            UpdateWorkspaceRequest update_req = j.get<UpdateWorkspaceRequest>();
            return service_.update_workspace(id, update_req);
        });
    });

    svr_.Delete(R"(/api/v1/workspaces/(\d+))", [this](const httplib::Request& req,
                                                      httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "workspaceId");
            service_.delete_workspace(id);
            return nlohmann::json{
                {"status", "deleted"},
                {"id", id},
            };
        });
    });

    // ==========================================
    // Indexing, Status & Jobs (F-05)
    // ==========================================
    svr_.Post(R"(/api/v1/workspaces/(\d+)/index)", [this](const httplib::Request& req,
                                                          httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "workspaceId");
            IndexJobRequest index_req;
            if (!r.body.empty()) {
                auto j = nlohmann::json::parse(r.body);
                index_req = j.get<IndexJobRequest>();
            }
            auto dto = service_.trigger_indexing(id, index_req);
            res.status = 202;
            return dto;
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/status)", [this](const httplib::Request& req,
                                                          httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "workspaceId");
            return service_.get_workspace_status(id);
        });
    });

    svr_.Get(R"(/api/v1/jobs/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "jobId");
            return service_.get_job(id);
        });
    });

    svr_.Post(R"(/api/v1/jobs/(\d+)/cancel)", [this](const httplib::Request& req,
                                                     httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "jobId");
            return service_.cancel_job(id);
        });
    });

    // ==========================================
    // Tree, File Metadata & Range Content (F-06, F-10)
    // ==========================================
    svr_.Get(R"(/api/v1/workspaces/(\d+)/tree)", [this](const httplib::Request& req,
                                                        httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t id = parse_id(r.matches[1].str(), "workspaceId");
            std::string path = get_str_query_param(r, "path").value_or("");
            return service_.get_tree(id, path);
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/files/(\d+))", [this](const httplib::Request& req,
                                                               httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            int64_t file_id = parse_id(r.matches[2].str(), "fileId");
            return service_.get_file(ws_id, file_id);
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/files/(\d+)/content)", [this](const httplib::Request& req,
                                                                       httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            int64_t file_id = parse_id(r.matches[2].str(), "fileId");
            auto sl = get_int_query_param(r, "startLine");
            auto el = get_int_query_param(r, "endLine");
            auto sb = get_int_query_param(r, "startByte");
            auto eb = get_int_query_param(r, "endByte");
            return service_.get_file_content(ws_id, file_id, sl, el, sb, eb);
        });
    });

    // ==========================================
    // Highlights, Symbols & Outline (F-07)
    // ==========================================
    svr_.Get(R"(/api/v1/workspaces/(\d+)/files/(\d+)/highlights)",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_json(req, res,
                             [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
                                 int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
                                 int64_t file_id = parse_id(r.matches[2].str(), "fileId");
                                 return service_.get_file_highlights(ws_id, file_id);
                             });
             });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/files/(\d+)/symbols)", [this](const httplib::Request& req,
                                                                       httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            int64_t file_id = parse_id(r.matches[2].str(), "fileId");
            auto syms = service_.get_file_symbols(ws_id, file_id);
            return nlohmann::json{
                {"fileId", file_id},
                {"symbols", syms},
                {"total", syms.size()},
            };
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/files/(\d+)/outline)", [this](const httplib::Request& req,
                                                                       httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            int64_t file_id = parse_id(r.matches[2].str(), "fileId");
            return service_.get_file_outline(ws_id, file_id);
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/files/(\d+)/occurrences)",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_json(req, res,
                             [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
                                 int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
                                 int64_t file_id = parse_id(r.matches[2].str(), "fileId");
                                 auto kind = get_str_query_param(r, "kind");
                                 auto sb = get_int_query_param(r, "startByte");
                                 auto eb = get_int_query_param(r, "endByte");
                                 auto occs =
                                     service_.get_file_occurrences(ws_id, file_id, kind, sb, eb);
                                 return nlohmann::json{
                                     {"fileId", file_id},
                                     {"occurrences", occs},
                                     {"total", occs.size()},
                                 };
                             });
             });

    // ==========================================
    // Workspace Symbols & Detail (F-07, F-08)
    // ==========================================
    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols)", [this](const httplib::Request& req,
                                                           httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            auto q = get_str_query_param(r, "query");
            if (!q.has_value()) {
                q = get_str_query_param(r, "q");
            }
            auto kind = get_str_query_param(r, "kind");
            auto lang = get_str_query_param(r, "language");
            auto file_id = get_int_query_param(r, "fileId");
            int64_t limit = get_int_query_param(r, "limit").value_or(50);
            int64_t offset = get_int_query_param(r, "offset").value_or(0);
            return service_.list_symbols(ws_id, q, kind, lang, file_id, limit, offset);
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols/(\d+))", [this](const httplib::Request& req,
                                                                 httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            int64_t sym_id = parse_id(r.matches[2].str(), "symbolId");
            return service_.get_symbol_detail(ws_id, sym_id);
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols/(\d+)/references)",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_json(req, res,
                             [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
                                 int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
                                 int64_t sym_id = parse_id(r.matches[2].str(), "symbolId");
                                 int64_t limit = get_int_query_param(r, "limit").value_or(50);
                                 int64_t offset = get_int_query_param(r, "offset").value_or(0);
                                 return service_.get_symbol_references(ws_id, sym_id, limit,
                                                                       offset);
                             });
             });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols/(\d+)/definitions)",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_json(req, res,
                             [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
                                 int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
                                 int64_t sym_id = parse_id(r.matches[2].str(), "symbolId");
                                 auto defs = service_.get_symbol_definitions(ws_id, sym_id);
                                 return nlohmann::json{
                                     {"symbolId", sym_id},
                                     {"definitions", defs},
                                     {"total", defs.size()},
                                 };
                             });
             });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols/(\d+)/callers)",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_json(req, res,
                             [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
                                 int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
                                 int64_t sym_id = parse_id(r.matches[2].str(), "symbolId");
                                 auto callers = service_.get_symbol_callers(ws_id, sym_id);
                                 return nlohmann::json{
                                     {"symbolId", sym_id},
                                     {"callers", callers},
                                     {"total", callers.size()},
                                 };
                             });
             });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols/(\d+)/callees)",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_json(req, res,
                             [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
                                 int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
                                 int64_t sym_id = parse_id(r.matches[2].str(), "symbolId");
                                 auto callees = service_.get_symbol_callees(ws_id, sym_id);
                                 return nlohmann::json{
                                     {"symbolId", sym_id},
                                     {"callees", callees},
                                     {"total", callees.size()},
                                 };
                             });
             });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/symbols/(\d+)/graph)", [this](const httplib::Request& req,
                                                                       httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            int64_t sym_id = parse_id(r.matches[2].str(), "symbolId");
            int depth = static_cast<int>(get_int_query_param(r, "depth").value_or(1));
            size_t max_nodes = static_cast<size_t>(get_int_query_param(r, "maxNodes").value_or(50));
            size_t max_edges =
                static_cast<size_t>(get_int_query_param(r, "maxEdges").value_or(100));
            return service_.get_symbol_graph(ws_id, sym_id, depth, max_nodes, max_edges);
        });
    });

    // ==========================================
    // Search (F-09)
    // ==========================================
    svr_.Get(R"(/api/v1/workspaces/(\d+)/search)", [this](const httplib::Request& req,
                                                          httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            auto q = get_str_query_param(r, "query");
            if (!q.has_value() || q->empty()) {
                q = get_str_query_param(r, "q");
            }
            if (!q.has_value() || q->empty()) {
                throw ApiError::bad_request("missing_query",
                                            "Query parameter 'query' or 'q' is required");
            }
            int64_t limit = get_int_query_param(r, "limit").value_or(50);
            int64_t offset = get_int_query_param(r, "offset").value_or(0);
            return service_.search_source(ws_id, *q, limit, offset);
        });
    });

    svr_.Get(R"(/api/v1/workspaces/(\d+)/search/symbols)", [this](const httplib::Request& req,
                                                                  httplib::Response& res) {
        handle_json(req, res, [&](const httplib::Request& r, const std::string&) -> nlohmann::json {
            int64_t ws_id = parse_id(r.matches[1].str(), "workspaceId");
            auto q = get_str_query_param(r, "query");
            if (!q.has_value() || q->empty()) {
                q = get_str_query_param(r, "q");
            }
            if (!q.has_value() || q->empty()) {
                throw ApiError::bad_request("missing_query",
                                            "Query parameter 'query' or 'q' is required");
            }
            int64_t limit = get_int_query_param(r, "limit").value_or(50);
            int64_t offset = get_int_query_param(r, "offset").value_or(0);
            return service_.search_symbols(ws_id, *q, limit, offset);
        });
    });
}

bool HttpServer::start() {
    if (is_running_.load()) {
        return true;
    }

    if (config_.port == 0) {
        int bound = svr_.bind_to_any_port(config_.host);
        if (bound <= 0) {
            return false;
        }
        bound_port_ = static_cast<uint16_t>(bound);
    } else {
        if (!svr_.bind_to_port(config_.host, config_.port)) {
            return false;
        }
        bound_port_ = config_.port;
    }

    std::atomic<bool> listen_failed{false};
    server_thread_ = std::thread([this, &listen_failed]() {
        if (!svr_.listen_after_bind()) {
            listen_failed.store(true);
        }
        is_running_.store(false);
    });

    svr_.wait_until_ready();
    if (!svr_.is_running() || listen_failed.load()) {
        svr_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        return false;
    }

    is_running_.store(true);
    return true;
}

void HttpServer::stop() {
    if (stop_requested_.exchange(true)) {
        return;
    }

    service_.shutdown();
    svr_.stop();
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    is_running_.store(false);
}

void HttpServer::wait() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

} // namespace codelenses::server
