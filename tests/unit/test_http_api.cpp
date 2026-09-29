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
        fs::create_directories(root / "z_crlf");

        // Write a test source file
        std::ofstream f1(root / "src" / "main.c");
        f1 << "int add(int a, int b) {\n    return a + b;\n}\n\nint main() {\n    return add(1, "
              "2);\n}\n";
        f1.close();

        // Write a test CRLF file
        std::ofstream fcrlf(root / "z_crlf" / "crlf.c", std::ios::binary);
        fcrlf << "int mult(int x, int y) {\r\n    return x * y;\r\n}\r\n";
        fcrlf.close();

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

    SECTION("Unmatched route 404 sets identical X-Request-ID in header and JSON body") {
        auto res = env.client->Get("/api/v1/nonexistent_route_for_test");
        REQUIRE(res != nullptr);
        CHECK(res->status == 404);
        CHECK(res->has_header("X-Request-ID"));
        std::string header_id = res->get_header_value("X-Request-ID");
        CHECK(!header_id.empty());
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["requestId"] == header_id);
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

    SECTION("Forbidden system directories like / and /etc rejected with 403 (F-10)") {
        nlohmann::json root_body = {
            {"rootPath", "/"},
            {"name", "Root WS"},
        };
        auto res1 = env.client->Post("/api/v1/workspaces", root_body.dump(), "application/json");
        REQUIRE(res1 != nullptr);
        CHECK(res1->status == 403);
        auto j1 = nlohmann::json::parse(res1->body);
        CHECK(j1["code"] == "forbidden_workspace_root");

        nlohmann::json etc_body = {
            {"rootPath", "/etc"},
            {"name", "Etc WS"},
        };
        auto res2 = env.client->Post("/api/v1/workspaces", etc_body.dump(), "application/json");
        REQUIRE(res2 != nullptr);
        CHECK(res2->status == 403);
        auto j2 = nlohmann::json::parse(res2->body);
        CHECK(j2["code"] == "forbidden_workspace_root");
    }

    SECTION("Symlink workspace root rejected when allow_external_symlinks is disabled") {
        auto sym_target = env.root / "sym_target";
        auto sym_link = env.root / "sym_link";
        fs::create_directories(sym_target);
        std::error_code ec;
        fs::create_directory_symlink(sym_target, sym_link, ec);
        if (!ec) {
            nlohmann::json sym_body = {
                {"rootPath", sym_link.string()},
                {"name", "Symlink WS"},
            };
            auto res = env.client->Post("/api/v1/workspaces", sym_body.dump(), "application/json");
            REQUIRE(res != nullptr);
            CHECK(res->status == 403);
            auto j = nlohmann::json::parse(res->body);
            CHECK(j["code"] == "forbidden_workspace_root");
        }
    }
}

TEST_CASE("HTTP C# library profile creation, attachment, and access revocation",
          "[server][library][csharp]") {
    TestHttpServerEnv env;

    const auto library_root = env.root / "csharp_library";
    fs::create_directories(library_root);
    std::ofstream(library_root / "Widget.cs")
        << "namespace Example; public class Widget { }\n";

    nlohmann::json library_body = {
        {"name", "Example C# source"},
        {"language", "csharp"},
        {"provider", "custom"},
        {"targetFramework", "net8.0"},
        {"sourceRoots", nlohmann::json::array({library_root.string()})},
    };
    auto missing_framework_body = library_body;
    missing_framework_body.erase("targetFramework");
    auto missing_framework_res = env.client->Post(
        "/api/v1/libraries", missing_framework_body.dump(), "application/json");
    REQUIRE(missing_framework_res != nullptr);
    CHECK(missing_framework_res->status == 400);
    CHECK(nlohmann::json::parse(missing_framework_res->body)["code"] == "missing_field");

    auto library_res =
        env.client->Post("/api/v1/libraries", library_body.dump(), "application/json");
    REQUIRE(library_res != nullptr);
    REQUIRE(library_res->status == 201);
    auto library_json = nlohmann::json::parse(library_res->body);
    const auto library_id = library_json["id"].get<int64_t>();
    const auto library_workspace_id = library_json["workspaceId"].get<int64_t>();
    REQUIRE(library_id > 0);
    CHECK(library_json["language"] == "csharp");
    CHECK(library_json["targetFramework"] == "net8.0");

    auto wait_for_job = [&](int64_t job_id) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto job_res = env.client->Get("/api/v1/jobs/" + std::to_string(job_id));
            if (job_res && job_res->status == 200) {
                const auto job = nlohmann::json::parse(job_res->body);
                if (job["status"] == "completed") return true;
                if (job["status"] == "failed" || job["status"] == "canceled" ||
                    job["status"] == "cancelled") return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    };

    auto library_index_res = env.client->Post(
        "/api/v1/libraries/" + std::to_string(library_id) + "/index", "{}", "application/json");
    REQUIRE(library_index_res != nullptr);
    REQUIRE(library_index_res->status == 202);
    REQUIRE(wait_for_job(nlohmann::json::parse(library_index_res->body)["id"].get<int64_t>()));

    const auto project_root = env.root / "csharp_project";
    fs::create_directories(project_root);
    std::ofstream(project_root / "Program.cs")
        << "using Example; public class App { Widget value; }\n";
    nlohmann::json workspace_body = {
        {"rootPath", project_root.string()},
        {"name", "C# consumer"},
    };
    auto workspace_res = env.client->Post("/api/v1/workspaces", workspace_body.dump(),
                                          "application/json");
    REQUIRE(workspace_res != nullptr);
    REQUIRE(workspace_res->status == 201);
    const auto workspace_id = nlohmann::json::parse(workspace_res->body)["id"].get<int64_t>();

    auto attach_res = env.client->Post(
        "/api/v1/workspaces/" + std::to_string(workspace_id) + "/libraries",
        nlohmann::json{{"profileId", library_id}}.dump(), "application/json");
    REQUIRE(attach_res != nullptr);
    CHECK(attach_res->status == 201);

    nlohmann::json index_body = {{"jobType", "full"}, {"forceFull", true}};
    auto project_index_res = env.client->Post(
        "/api/v1/workspaces/" + std::to_string(workspace_id) + "/index", index_body.dump(),
        "application/json");
    REQUIRE(project_index_res != nullptr);
    REQUIRE(project_index_res->status == 202);
    REQUIRE(wait_for_job(nlohmann::json::parse(project_index_res->body)["id"].get<int64_t>()));

    const auto library_files = env.db->files().list_by_workspace(library_workspace_id, false);
    REQUIRE(library_files.size() == 1);
    const auto file_id = library_files.front().id;
    const auto file_url = "/api/v1/workspaces/" + std::to_string(workspace_id) + "/files/" +
                          std::to_string(file_id);
    auto file_res = env.client->Get(file_url);
    REQUIRE(file_res != nullptr);
    REQUIRE(file_res->status == 200);
    const auto file_json = nlohmann::json::parse(file_res->body);
    CHECK(file_json["origin"] == "library");
    CHECK(file_json["libraryProfileId"] == library_id);
    CHECK(file_json["targetFramework"] == "net8.0");

    auto detach_res = env.client->Delete("/api/v1/workspaces/" + std::to_string(workspace_id) +
                                         "/libraries/" + std::to_string(library_id));
    REQUIRE(detach_res != nullptr);
    CHECK(detach_res->status == 200);
    auto revoked_file_res = env.client->Get(file_url);
    REQUIRE(revoked_file_res != nullptr);
    CHECK(revoked_file_res->status == 404);
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

        // Test CRLF file content and accurate byte range
        auto tree_crlf_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=z_crlf");
        REQUIRE(tree_crlf_res != nullptr);
        auto tree_crlf_json = nlohmann::json::parse(tree_crlf_res->body);
        int64_t crlf_file_id = -1;
        for (const auto& entry : tree_crlf_json["entries"]) {
            if (entry["name"] == "crlf.c") {
                crlf_file_id = entry["fileId"].get<int64_t>();
                break;
            }
        }
        REQUIRE(crlf_file_id > 0);

        // Whole CRLF file preserves \r\n
        auto crlf_full_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/files/" + std::to_string(crlf_file_id) + "/content");
        REQUIRE(crlf_full_res != nullptr);
        CHECK(crlf_full_res->status == 200);
        auto crlf_full_json = nlohmann::json::parse(crlf_full_res->body);
        std::string crlf_full_text = crlf_full_json["content"];
        CHECK(crlf_full_text.find("\r\n") != std::string::npos);

        // Line-range CRLF file preserves \r\n and bounds endByte at line boundary
        auto crlf_range_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                            std::to_string(crlf_file_id) + "/content?startLine=0&endLine=1");
        REQUIRE(crlf_range_res != nullptr);
        CHECK(crlf_range_res->status == 200);
        auto crlf_range_json = nlohmann::json::parse(crlf_range_res->body);
        std::string crlf_range_text = crlf_range_json["content"];
        CHECK(crlf_range_text.find("\r\n") != std::string::npos);
        CHECK(crlf_range_json["startByte"] == 0);
        CHECK(crlf_range_json["endByte"].get<int64_t>() <
              crlf_full_json["totalSizeBytes"].get<int64_t>());

        // Cross-workspace file access prevention in list_symbols
        auto cross_sym_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/symbols?fileId=999999");
        REQUIRE(cross_sym_res != nullptr);
        CHECK(cross_sym_res->status == 404);

        // Graph bounds validation (negative values rejected)
        auto bad_graph_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols/" +
                            std::to_string(add_symbol_id) + "/graph?depth=-1");
        REQUIRE(bad_graph_res != nullptr);
        CHECK(bad_graph_res->status == 400);

        // Graph kinds filtering
        auto filtered_graph_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/symbols/" +
                            std::to_string(add_symbol_id) + "/graph?kinds=calls");
        REQUIRE(filtered_graph_res != nullptr);
        CHECK(filtered_graph_res->status == 200);

        // Symbol search pagination
        auto paged_sym_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/search/symbols?query=add&limit=1&offset=0");
        REQUIRE(paged_sym_res != nullptr);
        CHECK(paged_sym_res->status == 200);
        auto paged_sym_json = nlohmann::json::parse(paged_sym_res->body);
        CHECK(paged_sym_json["items"].size() == 1);
        CHECK(paged_sym_json["limit"] == 1);
        CHECK(paged_sym_json["offset"] == 0);
        CHECK(paged_sym_json["total"] >= 1);

        auto paged_sym_oob = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/search/symbols?query=add&limit=1&offset=1000");
        REQUIRE(paged_sym_oob != nullptr);
        CHECK(paged_sym_oob->status == 200);
        auto paged_oob_json = nlohmann::json::parse(paged_sym_oob->body);
        CHECK(paged_oob_json["items"].empty());
        CHECK(paged_oob_json["hasMore"] == false);

        // Source search pagination
        auto paged_src_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/search?query=return&limit=1&offset=0");
        REQUIRE(paged_src_res != nullptr);
        CHECK(paged_src_res->status == 200);
        auto paged_src_json = nlohmann::json::parse(paged_src_res->body);
        CHECK(paged_src_json["limit"] == 1);
        CHECK(paged_src_json["offset"] == 0);

        // Job cancellation
        auto cancel_res = env.client->Post("/api/v1/jobs/" + std::to_string(job_id) + "/cancel", "",
                                           "application/json");
        REQUIRE(cancel_res != nullptr);
        CHECK(cancel_res->status == 200);
        auto cancel_json = nlohmann::json::parse(cancel_res->body);
        std::string st = cancel_json["status"];
        CHECK((st == "completed" || st == "canceled"));
    }
}

TEST_CASE("Compile command endpoints and workspace compilation database status (H1-06, UI)",
          "[server][api][compile_commands]") {
    TestHttpServerEnv env;

    // Create compile_commands.json in workspace root
    std::string cdb_content = R"json([
        {
            "directory": ".",
            "file": "src/main.c",
            "output": "build/main.o",
            "arguments": [
                "gcc",
                "-Iinclude",
                "-DBUILD_VERSION=42",
                "-std=c17",
                "-c",
                "src/main.c"
            ]
        }
    ])json";
    std::ofstream cdb_file(env.root / "compile_commands.json");
    cdb_file << cdb_content;
    cdb_file.close();

    // Create workspace
    nlohmann::json ws_req{
        {"rootPath", env.root.string()},
        {"name", "CDB Test Workspace"},
    };
    auto ws_res = env.client->Post("/api/v1/workspaces", ws_req.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    REQUIRE(ws_res->status == 201);
    auto ws_json = nlohmann::json::parse(ws_res->body);
    int64_t ws_id = ws_json["id"];

    // 1. Verify workspace compile-commands summary (auto-detected)
    auto cdb_status_res =
        env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/compile-commands");
    REQUIRE(cdb_status_res != nullptr);
    CHECK(cdb_status_res->status == 200);
    auto cdb_status_json = nlohmann::json::parse(cdb_status_res->body);
    CHECK(cdb_status_json["exists"] == true);
    CHECK(cdb_status_json["isAutoDetected"] == true);
    CHECK(cdb_status_json["totalCommands"] == 1);
    CHECK(cdb_status_json["effectivePath"].is_string());

    // 2. Index workspace
    auto idx_res = env.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index", "{}",
                                    "application/json");
    REQUIRE(idx_res != nullptr);
    REQUIRE(idx_res->status == 202);

    // Wait for indexing completion
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto st_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
        if (st_res && st_res->status == 200) {
            auto st_json = nlohmann::json::parse(st_res->body);
            if (st_json["status"] == "idle") {
                break;
            }
        }
    }

    // Find main.c and README.md file IDs from tree
    auto tree_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree");
    REQUIRE(tree_res != nullptr);
    auto tree_json = nlohmann::json::parse(tree_res->body);
    int64_t readme_id = 0;
    for (const auto& entry : tree_json["entries"]) {
        if (entry["name"] == "README.md") {
            readme_id = entry["fileId"];
        }
    }

    auto src_tree_res =
        env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
    REQUIRE(src_tree_res != nullptr);
    auto src_tree_json = nlohmann::json::parse(src_tree_res->body);
    int64_t main_id = 0;
    for (const auto& entry : src_tree_json["entries"]) {
        if (entry["name"] == "main.c") {
            main_id = entry["fileId"];
        }
    }
    REQUIRE(main_id > 0);

    // 3. Query compile-command for main.c
    auto cmd_res = env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                   std::to_string(main_id) + "/compile-command");
    REQUIRE(cmd_res != nullptr);
    CHECK(cmd_res->status == 200);
    auto cmd_json = nlohmann::json::parse(cmd_res->body);
    CHECK(cmd_json["hasCompileCommand"] == true);
    CHECK(cmd_json["isAutoDetected"] == true);
    REQUIRE(cmd_json["compileCommand"].is_object());
    CHECK(cmd_json["compileCommand"]["languageStandard"] == "c17");
    CHECK(cmd_json["compileCommand"]["output"] == "build/main.o");

    bool has_build_version = false;
    for (const auto& def : cmd_json["compileCommand"]["defines"]) {
        if (def == "BUILD_VERSION=42") {
            has_build_version = true;
        }
    }
    CHECK(has_build_version);

    // 4. Query compile-command for file without command (README.md)
    if (readme_id > 0) {
        auto no_cmd_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                            std::to_string(readme_id) + "/compile-command");
        REQUIRE(no_cmd_res != nullptr);
        CHECK(no_cmd_res->status == 200);
        auto no_cmd_json = nlohmann::json::parse(no_cmd_res->body);
        CHECK(no_cmd_json["hasCompileCommand"] == false);
        CHECK(no_cmd_json["compileCommand"].is_null());
    }

    // 5. Update workspace with explicit compileCommandsPath
    nlohmann::json patch_req{
        {"compileCommandsPath", "custom/compile_commands.json"},
    };
    auto patch_res = env.client->Patch("/api/v1/workspaces/" + std::to_string(ws_id),
                                       patch_req.dump(), "application/json");
    REQUIRE(patch_res != nullptr);
    CHECK(patch_res->status == 200);

    auto custom_status_res =
        env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/compile-commands");
    REQUIRE(custom_status_res != nullptr);
    CHECK(custom_status_res->status == 200);
    auto custom_status_json = nlohmann::json::parse(custom_status_res->body);
    CHECK(custom_status_json["isAutoDetected"] == false);
    CHECK(custom_status_json["configuredPath"] == "custom/compile_commands.json");
    CHECK(custom_status_json["exists"] == false);

    // 6. Set workspace default compile command and test precedence
    nlohmann::json def_cmd_patch{
        {"defaultCompileCommand", "clang -std=c99 -Iinclude -DGLOBAL_FALLBACK=1"},
    };
    auto def_patch_res = env.client->Patch("/api/v1/workspaces/" + std::to_string(ws_id),
                                           def_cmd_patch.dump(), "application/json");
    REQUIRE(def_patch_res != nullptr);
    CHECK(def_patch_res->status == 200);

    // Summary endpoint reflects default compile command
    auto ws_summary_res =
        env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/compile-commands");
    REQUIRE(ws_summary_res != nullptr);
    CHECK(ws_summary_res->status == 200);
    auto ws_summary_json = nlohmann::json::parse(ws_summary_res->body);
    CHECK(ws_summary_json["defaultCompileCommand"] ==
          "clang -std=c99 -Iinclude -DGLOBAL_FALLBACK=1");

    // Unlisted file (README.md) now gets workspace default compile command
    if (readme_id > 0) {
        auto fallback_res =
            env.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                            std::to_string(readme_id) + "/compile-command");
        REQUIRE(fallback_res != nullptr);
        CHECK(fallback_res->status == 200);
        auto fallback_json = nlohmann::json::parse(fallback_res->body);
        CHECK(fallback_json["hasCompileCommand"] == true);
        CHECK(fallback_json["isWorkspaceDefault"] == true);
        REQUIRE(fallback_json["compileCommand"].is_object());
        CHECK(fallback_json["compileCommand"]["languageStandard"] == "c99");
        bool has_fallback_flag = false;
        for (const auto& def : fallback_json["compileCommand"]["defines"]) {
            if (def == "GLOBAL_FALLBACK=1") {
                has_fallback_flag = true;
            }
        }
        CHECK(has_fallback_flag);
    }
}
