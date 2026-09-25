#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "codelenses/app/version.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/dto.hpp"
#include "codelenses/server/error.hpp"
#include "codelenses/server/http_server.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::server;

namespace {

struct TestHttpServerEnv {
    fs::path root;
    fs::path db_file;
    std::unique_ptr<Database> db;
    std::unique_ptr<index::IndexingPipeline> pipeline;
    std::unique_ptr<ApiService> service;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
    uint16_t port{0};

    TestHttpServerEnv() {
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::temp_directory_path() / ("codelenses_test_http_" + now);
        db_file = fs::temp_directory_path() / ("codelenses_test_http_" + now + ".db");
        fs::remove_all(root);
        fs::remove(db_file);
        fs::create_directories(root / "src");

        // Write a test source file
        std::ofstream f1(root / "src" / "main.c");
        f1 << "int add(int a, int b) {\n    return a + b;\n}\n\nint main() {\n    return add(1, "
              "2);\n}\n";
        f1.close();

        // Write another file
        std::ofstream f2(root / "README.md");
        f2 << "# Test Project\nWelcome to codelenses.\n";
        f2.close();

        db = Database::open(db_file.string(), true);
        pipeline = std::make_unique<index::IndexingPipeline>(*db);
        service = std::make_unique<ApiService>(*db, *pipeline);

        HttpServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0; // Ephemeral port
        config.enable_cors = true;

        server = std::make_unique<HttpServer>(*service, config);
        REQUIRE(server->start());
        port = server->port();
        REQUIRE(port > 0);

        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_connection_timeout(5, 0);
        client->set_read_timeout(5, 0);
    }

    ~TestHttpServerEnv() {
        if (server) {
            server->stop();
        }
        service.reset();
        pipeline.reset();
        db.reset();

        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove(db_file, ec);
        fs::remove(db_file.string() + "-wal", ec);
        fs::remove(db_file.string() + "-shm", ec);
    }
};

} // namespace

TEST_CASE("HTTP Server starts and handles health and version (F-01, F-03)", "[server][api]") {
    TestHttpServerEnv env;

    SECTION("Health endpoint returns 200 OK and version") {
        auto res = env.client->Get("/api/v1/health");
        REQUIRE(res != nullptr);
        CHECK(res->status == 200);
        CHECK(res->has_header("X-Request-ID"));
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["status"] == "ok");
        CHECK(j["version"] == std::string(kVersionString));
    }

    SECTION("Version endpoint returns detailed version information") {
        auto res = env.client->Get("/api/v1/version");
        REQUIRE(res != nullptr);
        CHECK(res->status == 200);
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["name"] == "codelenses");
        CHECK(j["major"] == kVersionMajor);
    }
}

TEST_CASE("HTTP Server CORS policy and OPTIONS preflight (F-11)", "[server][cors]") {
    TestHttpServerEnv env;

    SECTION("OPTIONS preflight returns 204 with CORS headers") {
        httplib::Headers headers = {
            {"Origin", "http://localhost:3000"},
            {"Access-Control-Request-Method", "POST"},
            {"Access-Control-Request-Headers", "Content-Type, X-Request-ID"},
        };
        auto res = env.client->Options("/api/v1/workspaces", headers);
        REQUIRE(res != nullptr);
        CHECK(res->status == 204);
        CHECK(res->get_header_value("Access-Control-Allow-Origin") == "*");
        CHECK(res->has_header("Access-Control-Allow-Methods"));
        CHECK(res->has_header("Access-Control-Allow-Headers"));
    }

    SECTION("Normal GET response includes CORS headers") {
        auto res = env.client->Get("/api/v1/health");
        REQUIRE(res != nullptr);
        CHECK(res->get_header_value("Access-Control-Allow-Origin") == "*");
    }
}

TEST_CASE("HTTP Server Error envelope with request IDs (F-02, F-12)", "[server][error]") {
    TestHttpServerEnv env;

    SECTION("404 Not Found returns JSON error envelope with requestId") {
        auto res = env.client->Get("/api/v1/workspaces/999999");
        REQUIRE(res != nullptr);
        CHECK(res->status == 404);
        CHECK(res->get_header_value("Content-Type") == "application/json");
        CHECK(res->has_header("X-Request-ID"));

        auto j = nlohmann::json::parse(res->body);
        CHECK(j["code"] == "workspace_not_found");
        CHECK(j.contains("message"));
        CHECK(j["requestId"] == res->get_header_value("X-Request-ID"));
        CHECK(j.contains("error"));
        CHECK(j["error"]["code"] == "workspace_not_found");
    }

    SECTION("Custom X-Request-ID header is echoed in error response") {
        httplib::Headers headers = {{"X-Request-ID", "custom_test_req_42"}};
        auto res = env.client->Get("/api/v1/workspaces/999999", headers);
        REQUIRE(res != nullptr);
        CHECK(res->get_header_value("X-Request-ID") == "custom_test_req_42");
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["requestId"] == "custom_test_req_42");
    }
}

TEST_CASE("Workspace CRUD and path validation (F-04, F-10)", "[server][workspace]") {
    TestHttpServerEnv env;

    int64_t created_id = 0;

    SECTION("Create workspace with valid path returns 201 Created") {
        nlohmann::json body = {
            {"rootPath", env.root.string()},
            {"name", "My Test Project"},
        };
        auto res = env.client->Post("/api/v1/workspaces", body.dump(), "application/json");
        REQUIRE(res != nullptr);
        CHECK(res->status == 201);

        auto j = nlohmann::json::parse(res->body);
        CHECK(j["id"] > 0);
        created_id = j["id"].get<int64_t>();
        CHECK(j["name"] == "My Test Project");
        CHECK(j["status"] == "idle");

        // List workspaces
        auto list_res = env.client->Get("/api/v1/workspaces");
        REQUIRE(list_res != nullptr);
        CHECK(list_res->status == 200);
        auto list_json = nlohmann::json::parse(list_res->body);
        CHECK(list_json["total"] >= 1);

        // Get single workspace
        auto get_res = env.client->Get("/api/v1/workspaces/" + std::to_string(created_id));
        REQUIRE(get_res != nullptr);
        CHECK(get_res->status == 200);
        auto get_json = nlohmann::json::parse(get_res->body);
        CHECK(get_json["id"] == created_id);

        // Patch workspace
        nlohmann::json patch_body = {{"name", "Renamed Project"}};
        auto patch_res = env.client->Patch("/api/v1/workspaces/" + std::to_string(created_id),
                                           patch_body.dump(), "application/json");
        REQUIRE(patch_res != nullptr);
        CHECK(patch_res->status == 200);
        auto patch_json = nlohmann::json::parse(patch_res->body);
        CHECK(patch_json["name"] == "Renamed Project");

        // Delete workspace
        auto del_res = env.client->Delete("/api/v1/workspaces/" + std::to_string(created_id));
        REQUIRE(del_res != nullptr);
        CHECK(del_res->status == 200);

        // Confirm deleted
        auto after_res = env.client->Get("/api/v1/workspaces/" + std::to_string(created_id));
        REQUIRE(after_res != nullptr);
        CHECK(after_res->status == 404);
    }

    SECTION("Path traversal and invalid workspace root rejected (F-10)") {
        nlohmann::json bad_body = {
            {"rootPath", "/path/that/does/not/exist/anywhere"},
            {"name", "Invalid WS"},
        };
        auto res = env.client->Post("/api/v1/workspaces", bad_body.dump(), "application/json");
        REQUIRE(res != nullptr);
        CHECK(res->status == 400);
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["code"] == "invalid_workspace_root");
    }
}

TEST_CASE("Tree navigation and lazy loading (F-06, F-10)", "[server][tree]") {
    TestHttpServerEnv env;

    // Create workspace
    nlohmann::json create_body = {
        {"rootPath", env.root.string()},
        {"name", "Tree Test"},
    };
    auto ws_res = env.client->Post("/api/v1/workspaces", create_body.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

    SECTION("Root tree returns directories and files with directories first") {
        auto res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree");
        REQUIRE(res != nullptr);
        CHECK(res->status == 200);

        auto j = nlohmann::json::parse(res->body);
        CHECK(j["workspaceId"] == ws_id);
        auto entries = j["entries"];
        REQUIRE(entries.size() >= 2);
        CHECK(entries[0]["type"] == "directory");
        CHECK(entries[0]["name"] == "src");
    }

    SECTION("Subdirectory tree returns files in that directory") {
        auto res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
        REQUIRE(res != nullptr);
        CHECK(res->status == 200);

        auto j = nlohmann::json::parse(res->body);
        CHECK(j["path"] == "src");
        auto entries = j["entries"];
        REQUIRE(entries.size() == 1);
        CHECK(entries[0]["name"] == "main.c");
        CHECK(entries[0]["type"] == "file");
    }

    SECTION("Path traversal escaping workspace root is rejected (F-10)") {
        auto res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=../../etc");
        REQUIRE(res != nullptr);
        CHECK(res->status == 400);
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["code"] == "path_traversal");
    }
}

TEST_CASE("Indexing lifecycle, status, jobs, and file content (F-05, F-06)", "[server][pipeline]") {
    TestHttpServerEnv env;

    // Create workspace
    nlohmann::json create_body = {
        {"rootPath", env.root.string()},
        {"name", "Index Test"},
    };
    auto ws_res = env.client->Post("/api/v1/workspaces", create_body.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

    SECTION("Trigger indexing job and inspect status") {
        nlohmann::json index_req = {{"jobType", "full"}, {"forceFull", true}};
        auto post_idx = env.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                         index_req.dump(), "application/json");
        REQUIRE(post_idx != nullptr);
        CHECK(post_idx->status == 202);

        auto job_json = nlohmann::json::parse(post_idx->body);
        int64_t job_id = job_json["id"].get<int64_t>();
        CHECK(job_id > 0);

        // Wait for job to complete
        bool completed = false;
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto job_res = env.client->Get("/api/v1/jobs/" + std::to_string(job_id));
            if (job_res && job_res->status == 200) {
                auto j = nlohmann::json::parse(job_res->body);
                if (j["status"] == "completed") {
                    completed = true;
                    break;
                }
            }
        }
        CHECK(completed);

        // Inspect workspace status
        auto st_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
        REQUIRE(st_res != nullptr);
        CHECK(st_res->status == 200);
        auto st_json = nlohmann::json::parse(st_res->body);
        CHECK(st_json["fileCount"] >= 1);
        CHECK(st_json["symbolCount"] >= 1);

        // Find indexed main.c file ID
        auto tree_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
        REQUIRE(tree_res != nullptr);
        auto tree_json = nlohmann::json::parse(tree_res->body);
        int64_t file_id = tree_json["entries"][0]["fileId"].get<int64_t>();
        REQUIRE(file_id > 0);

        // File metadata
        auto file_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                        std::to_string(file_id));
        REQUIRE(file_res != nullptr);
        CHECK(file_res->status == 200);
        auto file_json = nlohmann::json::parse(file_res->body);
        CHECK(file_json["relativePath"] == "src/main.c");
        CHECK((file_json["language"] == "C" || file_json["language"] == "c"));

        // File content (full)
        auto content_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/files/" + std::to_string(file_id) + "/content");
        REQUIRE(content_res != nullptr);
        CHECK(content_res->status == 200);
        auto content_json = nlohmann::json::parse(content_res->body);
        CHECK(content_json["totalLines"] >= 5);
        std::string full_text = content_json["content"];
        CHECK(full_text.find("int add(int a, int b)") != std::string::npos);

        // File content with line range
        auto range_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                            std::to_string(file_id) + "/content?startLine=0&endLine=2");
        REQUIRE(range_res != nullptr);
        CHECK(range_res->status == 200);
        auto range_json = nlohmann::json::parse(range_res->body);
        CHECK(range_json["startLine"] == 0);
        CHECK(range_json["endLine"] == 2);
        CHECK(range_json["content"] == "int add(int a, int b) {\n    return a + b;\n}");

        // Highlights
        auto hl_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                      std::to_string(file_id) + "/highlights");
        REQUIRE(hl_res != nullptr);
        CHECK(hl_res->status == 200);
        auto hl_json = nlohmann::json::parse(hl_res->body);
        CHECK(hl_json.contains("legend"));
        CHECK(hl_json["tokens"].is_array());
        CHECK(hl_json["tokens"].size() >= 1);

        // Symbols in file
        auto sym_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                       std::to_string(file_id) + "/symbols");
        REQUIRE(sym_res != nullptr);
        CHECK(sym_res->status == 200);
        auto sym_json = nlohmann::json::parse(sym_res->body);
        CHECK(sym_json["total"] >= 2); // 'add' and 'main'

        // Outline
        auto outline_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/files/" + std::to_string(file_id) + "/outline");
        REQUIRE(outline_res != nullptr);
        CHECK(outline_res->status == 200);
        auto outline_json = nlohmann::json::parse(outline_res->body);
        CHECK(outline_json["outline"].size() >= 2);

        // Occurrences
        auto occ_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                       std::to_string(file_id) + "/occurrences");
        REQUIRE(occ_res != nullptr);
        CHECK(occ_res->status == 200);
        auto occ_json = nlohmann::json::parse(occ_res->body);
        CHECK(occ_json["occurrences"].is_array());

        // Workspace symbols search / list
        auto ws_syms_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols?query=add");
        REQUIRE(ws_syms_res != nullptr);
        CHECK(ws_syms_res->status == 200);
        auto ws_syms_json = nlohmann::json::parse(ws_syms_res->body);
        REQUIRE(ws_syms_json["items"].size() >= 1);
        int64_t add_symbol_id = ws_syms_json["items"][0]["id"].get<int64_t>();

        // Symbol Detail
        auto sym_det_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/symbols/" + std::to_string(add_symbol_id));
        REQUIRE(sym_det_res != nullptr);
        CHECK(sym_det_res->status == 200);
        auto sym_det_json = nlohmann::json::parse(sym_det_res->body);
        CHECK(sym_det_json["symbol"]["name"] == "add");

        // Definitions
        auto defs_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols/" +
                            std::to_string(add_symbol_id) + "/definitions");
        REQUIRE(defs_res != nullptr);
        CHECK(defs_res->status == 200);
        auto defs_json = nlohmann::json::parse(defs_res->body);
        CHECK(defs_json["definitions"].size() >= 1);

        // References
        auto refs_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols/" +
                            std::to_string(add_symbol_id) + "/references");
        REQUIRE(refs_res != nullptr);
        CHECK(refs_res->status == 200);

        // Callers
        auto callers_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols/" +
                            std::to_string(add_symbol_id) + "/callers");
        REQUIRE(callers_res != nullptr);
        CHECK(callers_res->status == 200);

        // Callees
        auto callees_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols/" +
                            std::to_string(add_symbol_id) + "/callees");
        REQUIRE(callees_res != nullptr);
        CHECK(callees_res->status == 200);

        // Graph
        auto graph_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                         "/symbols/" + std::to_string(add_symbol_id) + "/graph");
        REQUIRE(graph_res != nullptr);
        CHECK(graph_res->status == 200);
        auto graph_json = nlohmann::json::parse(graph_res->body);
        CHECK(graph_json["rootSymbolId"] == add_symbol_id);
        CHECK(graph_json["nodes"].size() >= 1);

        // FTS Symbol Search
        auto fts_sym_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/search/symbols?query=add");
        REQUIRE(fts_sym_res != nullptr);
        CHECK(fts_sym_res->status == 200);

        // FTS Source Search
        auto fts_src_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/search?query=return");
        REQUIRE(fts_src_res != nullptr);
        CHECK(fts_src_res->status == 200);
    }
}
