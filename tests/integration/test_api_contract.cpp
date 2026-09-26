#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "codelenses/app/version.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/dto.hpp"
#include "codelenses/server/error.hpp"
#include "codelenses/server/http_server.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::server;

namespace {

struct ApiContractFixture {
    fs::path temp_dir;
    fs::path db_path;
    fs::path workspace_root;

    std::unique_ptr<Database> db;
    std::unique_ptr<index::IndexingPipeline> pipeline;
    std::unique_ptr<ApiService> service;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
    uint16_t port{0};

    ApiContractFixture() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = fs::temp_directory_path() / ("codelenses_api_contract_" + pid + "_" + now);
        db_path = temp_dir / "codelenses_contract.db";
        workspace_root = temp_dir / "workspace";

        fs::create_directories(workspace_root / "src");
        fs::create_directories(workspace_root / "include");

        // Write C source files for indexing contract validation
        std::ofstream h1(workspace_root / "include" / "calc.h");
        h1 << "#ifndef CALC_H\n#define CALC_H\nint compute(int x, int y);\n#endif\n";
        h1.close();

        std::ofstream c1(workspace_root / "src" / "calc.c");
        c1 << "#include \"calc.h\"\n"
              "int compute(int x, int y) {\n"
              "    return x + y;\n"
              "}\n"
              "int run() {\n"
              "    return compute(10, 20);\n"
              "}\n";
        c1.close();

        std::ofstream md(workspace_root / "README.md");
        md << "# Contract Test Workspace\nDocumentation file for API contract tests.\n";
        md.close();

        start_server();
    }

    void start_server() {
        db = Database::open(db_path.string(), true);
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

    void stop_server() {
        if (server) {
            server->stop();
            server.reset();
        }
        client.reset();
        service.reset();
        pipeline.reset();
        db.reset();
    }

    void restart_server() {
        stop_server();
        start_server();
    }

    ~ApiContractFixture() {
        stop_server();
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void wait_for_job(int64_t job_id) {
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto res = client->Get("/api/v1/jobs/" + std::to_string(job_id));
            if (res && res->status == 200) {
                auto j = nlohmann::json::parse(res->body);
                std::string st = j["status"];
                if (st == "completed" || st == "failed" || st == "cancelled") {
                    break;
                }
            }
        }
    }
};

} // namespace

TEST_CASE("API Contract: Temporary SQLite database and persistence lifecycle (I-02)", "[contract][sqlite]") {
    ApiContractFixture fixture;

    SECTION("Temporary SQLite database file is created on disk with WAL mode enabled") {
        REQUIRE(fs::exists(fixture.db_path));
        REQUIRE(fs::is_regular_file(fixture.db_path));

        // Query PRAGMA journal_mode directly via SQLite connection handle
        Statement pragma_stmt(fixture.db->connection().handle(), "PRAGMA journal_mode;");
        REQUIRE(pragma_stmt.step());
        std::string mode = pragma_stmt.column_text(0);
        // SQLite WAL mode should be active
        CHECK(mode == "wal");
    }

    SECTION("Data written via API survives server restart and persists in SQLite DB") {
        nlohmann::json create_body = {
            {"rootPath", fixture.workspace_root.string()},
            {"name", "Persisted Workspace"},
        };
        auto post_res = fixture.client->Post("/api/v1/workspaces", create_body.dump(), "application/json");
        REQUIRE(post_res != nullptr);
        REQUIRE(post_res->status == 201);
        auto post_json = nlohmann::json::parse(post_res->body);
        int64_t ws_id = post_json["id"].get<int64_t>();

        // Index the workspace
        auto idx_res = fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                            "{}", "application/json");
        REQUIRE(idx_res != nullptr);
        REQUIRE(idx_res->status == 202);
        int64_t job_id = nlohmann::json::parse(idx_res->body)["id"].get<int64_t>();
        fixture.wait_for_job(job_id);

        // Verify status before restart
        auto status_before = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
        REQUIRE(status_before != nullptr);
        auto sb_json = nlohmann::json::parse(status_before->body);
        int64_t file_count_before = sb_json["fileCount"].get<int64_t>();
        int64_t sym_count_before = sb_json["symbolCount"].get<int64_t>();
        CHECK(file_count_before >= 3);
        CHECK(sym_count_before >= 2);

        // Restart server against the same SQLite database file
        fixture.restart_server();

        // Query workspace list and workspace status after restart
        auto list_res = fixture.client->Get("/api/v1/workspaces");
        REQUIRE(list_res != nullptr);
        REQUIRE(list_res->status == 200);
        auto list_json = nlohmann::json::parse(list_res->body);
        CHECK(list_json["total"].get<int64_t>() >= 1);

        auto ws_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id));
        REQUIRE(ws_res != nullptr);
        REQUIRE(ws_res->status == 200);
        auto ws_json = nlohmann::json::parse(ws_res->body);
        CHECK(ws_json["name"] == "Persisted Workspace");

        auto status_after = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
        REQUIRE(status_after != nullptr);
        REQUIRE(status_after->status == 200);
        auto sa_json = nlohmann::json::parse(status_after->body);
        CHECK(sa_json["fileCount"].get<int64_t>() == file_count_before);
        CHECK(sa_json["symbolCount"].get<int64_t>() == sym_count_before);
    }
}

TEST_CASE("API Contract: Complete Section 6 Endpoint Schemas and Error Contracts (I-02)", "[contract][api]") {
    ApiContractFixture fixture;

    SECTION("1. Health and Version contract") {
        // Health
        auto health_res = fixture.client->Get("/api/v1/health");
        REQUIRE(health_res != nullptr);
        CHECK(health_res->status == 200);
        CHECK(health_res->get_header_value("Content-Type") == "application/json");
        auto h_json = nlohmann::json::parse(health_res->body);
        CHECK(h_json["status"] == "ok");
        CHECK(h_json["version"].is_string());

        // Version
        auto ver_res = fixture.client->Get("/api/v1/version");
        REQUIRE(ver_res != nullptr);
        CHECK(ver_res->status == 200);
        auto v_json = nlohmann::json::parse(ver_res->body);
        CHECK(v_json["name"] == "codelenses");
        CHECK(v_json["version"].is_string());
        CHECK(v_json["major"].is_number_integer());
        CHECK(v_json["minor"].is_number_integer());
        CHECK(v_json["patch"].is_number_integer());
    }

    SECTION("2. Error Envelope and Request-ID propagation contract") {
        // Send request with custom X-Request-ID
        httplib::Headers headers = {{"X-Request-ID", "req-contract-test-42"}};
        auto res = fixture.client->Get("/api/v1/workspaces/999999", headers);
        REQUIRE(res != nullptr);
        CHECK(res->status == 404);
        CHECK(res->get_header_value("X-Request-ID") == "req-contract-test-42");
        CHECK(res->get_header_value("Content-Type") == "application/json");

        auto err_json = nlohmann::json::parse(res->body);
        REQUIRE(err_json.contains("error"));
        const auto& err = err_json["error"];
        CHECK(err["code"].is_string());
        CHECK(err["message"].is_string());
        CHECK(err["requestId"] == "req-contract-test-42");

        // Malformed JSON body returns 400 with invalid_json code
        auto bad_json_res = fixture.client->Post("/api/v1/workspaces", "{invalid json syntax", "application/json");
        REQUIRE(bad_json_res != nullptr);
        CHECK(bad_json_res->status == 400);
        auto bad_json_obj = nlohmann::json::parse(bad_json_res->body);
        CHECK(bad_json_obj["error"]["code"] == "invalid_json");

        // Empty body returns 400 with empty_body code
        auto empty_body_res = fixture.client->Post("/api/v1/workspaces", "", "application/json");
        REQUIRE(empty_body_res != nullptr);
        CHECK(empty_body_res->status == 400);
        auto empty_json_obj = nlohmann::json::parse(empty_body_res->body);
        CHECK(empty_json_obj["error"]["code"] == "empty_body");

        // Non-positive ID returns 400 with invalid_id code
        auto zero_id_res = fixture.client->Get("/api/v1/workspaces/0");
        REQUIRE(zero_id_res != nullptr);
        CHECK(zero_id_res->status == 400);
        auto zero_id_obj = nlohmann::json::parse(zero_id_res->body);
        CHECK(zero_id_obj["error"]["code"] == "invalid_id");

        // Non-integer ID does not match route and returns 404 with standard error envelope
        auto not_found_route_res = fixture.client->Get("/api/v1/workspaces/notanid");
        REQUIRE(not_found_route_res != nullptr);
        CHECK(not_found_route_res->status == 404);
        auto not_found_route_obj = nlohmann::json::parse(not_found_route_res->body);
        CHECK(not_found_route_obj["error"]["code"] == "not_found");
    }

    SECTION("3. CORS preflight contract") {
        auto opt_res = fixture.client->Options("/api/v1/workspaces");
        REQUIRE(opt_res != nullptr);
        CHECK(opt_res->status == 204);
        CHECK(opt_res->get_header_value("Access-Control-Allow-Origin") == "*");
        CHECK(opt_res->has_header("Access-Control-Allow-Methods"));
        CHECK(opt_res->has_header("Access-Control-Allow-Headers"));
    }

    SECTION("4. Full Resource Hierarchy Contract Test") {
        // Workspace CRUD
        nlohmann::json create_body = {
            {"rootPath", fixture.workspace_root.string()},
            {"name", "Contract Workspace"},
            {"include", {"src/**", "include/**", "*.md"}},
            {"exclude", {".git/**"}},
        };
        auto ws_res = fixture.client->Post("/api/v1/workspaces", create_body.dump(), "application/json");
        REQUIRE(ws_res != nullptr);
        REQUIRE(ws_res->status == 201);
        auto ws_json = nlohmann::json::parse(ws_res->body);
        int64_t ws_id = ws_json["id"].get<int64_t>();
        CHECK(ws_json["name"] == "Contract Workspace");
        CHECK(ws_json["rootPath"] == fixture.workspace_root.string());
        CHECK(ws_json["includePatterns"].is_array());
        CHECK(ws_json["excludePatterns"].is_array());
        CHECK(ws_json["status"] == "idle");
        CHECK(ws_json["revision"].is_number_integer());
        CHECK(ws_json.contains("createdAt"));
        CHECK(ws_json.contains("updatedAt"));

        // GET /workspaces
        auto list_res = fixture.client->Get("/api/v1/workspaces");
        REQUIRE(list_res != nullptr);
        CHECK(list_res->status == 200);
        auto list_json = nlohmann::json::parse(list_res->body);
        CHECK(list_json["workspaces"].is_array());
        CHECK(list_json["total"].get<int64_t>() >= 1);

        // GET /workspaces/{id}
        auto get_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id));
        REQUIRE(get_res != nullptr);
        CHECK(get_res->status == 200);
        auto get_json = nlohmann::json::parse(get_res->body);
        CHECK(get_json["id"] == ws_id);

        // PATCH /workspaces/{id}
        nlohmann::json patch_body = {
            {"name", "Updated Contract Workspace"},
            {"compileCommandsPath", "compile_commands.json"}
        };
        auto patch_res = fixture.client->Patch("/api/v1/workspaces/" + std::to_string(ws_id),
                                               patch_body.dump(), "application/json");
        REQUIRE(patch_res != nullptr);
        CHECK(patch_res->status == 200);
        auto patch_json = nlohmann::json::parse(patch_res->body);
        CHECK(patch_json["name"] == "Updated Contract Workspace");
        CHECK(patch_json["compileCommandsPath"] == "compile_commands.json");

        // GET /workspaces/{id}/compile-commands
        auto cdb_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/compile-commands");
        REQUIRE(cdb_res != nullptr);
        CHECK(cdb_res->status == 200);
        auto cdb_json = nlohmann::json::parse(cdb_res->body);
        CHECK(cdb_json.contains("exists"));
        CHECK(cdb_json.contains("isAutoDetected"));

        // POST /workspaces/{id}/index
        nlohmann::json index_req = {{"jobType", "full"}};
        auto index_res = fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                              index_req.dump(), "application/json");
        REQUIRE(index_res != nullptr);
        CHECK(index_res->status == 202);
        auto job_json = nlohmann::json::parse(index_res->body);
        int64_t job_id = job_json["id"].get<int64_t>();
        CHECK(job_json["workspaceId"] == ws_id);
        CHECK(job_json["jobType"] == "full");
        CHECK(job_json.contains("queuedAt"));

        // GET /jobs/{id}
        auto job_res = fixture.client->Get("/api/v1/jobs/" + std::to_string(job_id));
        REQUIRE(job_res != nullptr);
        CHECK(job_res->status == 200);
        fixture.wait_for_job(job_id);

        // POST /jobs/{id}/cancel on completed/cancelled job returns 200
        auto cancel_res = fixture.client->Post("/api/v1/jobs/" + std::to_string(job_id) + "/cancel",
                                               "", "application/json");
        REQUIRE(cancel_res != nullptr);
        CHECK(cancel_res->status == 200);

        // GET /workspaces/{id}/status
        auto status_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
        REQUIRE(status_res != nullptr);
        CHECK(status_res->status == 200);
        auto status_json = nlohmann::json::parse(status_res->body);
        CHECK(status_json["workspaceId"] == ws_id);
        CHECK(status_json["fileCount"].get<int64_t>() >= 3);
        CHECK(status_json["symbolCount"].get<int64_t>() >= 2);

        // GET /workspaces/{id}/tree
        auto tree_root_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree");
        REQUIRE(tree_root_res != nullptr);
        CHECK(tree_root_res->status == 200);
        auto tree_root_json = nlohmann::json::parse(tree_root_res->body);
        CHECK(tree_root_json["workspaceId"] == ws_id);
        CHECK(tree_root_json["entries"].is_array());
        CHECK(tree_root_json["entries"].size() >= 2);

        // GET /workspaces/{id}/tree?path=src
        auto tree_src_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
        REQUIRE(tree_src_res != nullptr);
        CHECK(tree_src_res->status == 200);
        auto tree_src_json = nlohmann::json::parse(tree_src_res->body);
        CHECK(tree_src_json["path"] == "src");
        REQUIRE(!tree_src_json["entries"].empty());
        int64_t calc_file_id = tree_src_json["entries"][0]["fileId"].get<int64_t>();
        REQUIRE(calc_file_id > 0);

        // GET /workspaces/{id}/files/{fileId}
        auto file_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                            "/files/" + std::to_string(calc_file_id));
        REQUIRE(file_res != nullptr);
        CHECK(file_res->status == 200);
        auto file_json = nlohmann::json::parse(file_res->body);
        CHECK(file_json["id"] == calc_file_id);
        CHECK(file_json["workspaceId"] == ws_id);
        CHECK(file_json["relativePath"] == "src/calc.c");
        CHECK(file_json["isBinary"] == false);
        CHECK(file_json["sizeBytes"].get<int64_t>() > 0);

        // GET /workspaces/{id}/files/{fileId}/content
        auto content_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                               "/files/" + std::to_string(calc_file_id) + "/content");
        REQUIRE(content_res != nullptr);
        CHECK(content_res->status == 200);
        auto content_json = nlohmann::json::parse(content_res->body);
        CHECK(content_json["fileId"] == calc_file_id);
        CHECK(content_json["totalLines"].get<int64_t>() >= 6);
        CHECK(content_json["content"].get<std::string>().find("int compute") != std::string::npos);

        // Line-bounded content query
        auto slice_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/files/" + std::to_string(calc_file_id) +
                                             "/content?startLine=1&endLine=3");
        REQUIRE(slice_res != nullptr);
        CHECK(slice_res->status == 200);
        auto slice_json = nlohmann::json::parse(slice_res->body);
        CHECK(slice_json["startLine"] == 1);
        CHECK(slice_json["endLine"] == 3);

        // GET /workspaces/{id}/files/{fileId}/compile-command
        auto file_cmd_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                                "/files/" + std::to_string(calc_file_id) + "/compile-command");
        REQUIRE(file_cmd_res != nullptr);
        CHECK(file_cmd_res->status == 200);
        auto file_cmd_json = nlohmann::json::parse(file_cmd_res->body);
        CHECK(file_cmd_json.contains("hasCompileCommand"));

        // GET /workspaces/{id}/files/{fileId}/highlights
        auto hl_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                          "/files/" + std::to_string(calc_file_id) + "/highlights");
        REQUIRE(hl_res != nullptr);
        CHECK(hl_res->status == 200);
        auto hl_json = nlohmann::json::parse(hl_res->body);
        CHECK(hl_json["fileId"] == calc_file_id);
        CHECK(hl_json["legend"].is_object());
        CHECK(hl_json["legend"].contains("tokenTypes"));
        CHECK(hl_json["tokens"].is_array());

        // GET /workspaces/{id}/files/{fileId}/symbols
        auto file_syms_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                                 "/files/" + std::to_string(calc_file_id) + "/symbols");
        REQUIRE(file_syms_res != nullptr);
        CHECK(file_syms_res->status == 200);
        auto file_syms_json = nlohmann::json::parse(file_syms_res->body);
        CHECK(file_syms_json["fileId"] == calc_file_id);
        CHECK(file_syms_json["total"].get<int64_t>() >= 2);

        // GET /workspaces/{id}/files/{fileId}/outline
        auto outline_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                               "/files/" + std::to_string(calc_file_id) + "/outline");
        REQUIRE(outline_res != nullptr);
        CHECK(outline_res->status == 200);
        auto outline_json = nlohmann::json::parse(outline_res->body);
        CHECK(outline_json["fileId"] == calc_file_id);
        CHECK(outline_json["outline"].is_array());

        // GET /workspaces/{id}/files/{fileId}/occurrences
        auto occ_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/files/" + std::to_string(calc_file_id) + "/occurrences");
        REQUIRE(occ_res != nullptr);
        CHECK(occ_res->status == 200);
        auto occ_json = nlohmann::json::parse(occ_res->body);
        CHECK(occ_json["fileId"] == calc_file_id);
        CHECK(occ_json["occurrences"].is_array());

        // GET /workspaces/{id}/diagnostics
        auto ws_diags_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/diagnostics");
        REQUIRE(ws_diags_res != nullptr);
        CHECK(ws_diags_res->status == 200);
        auto ws_diags_json = nlohmann::json::parse(ws_diags_res->body);
        CHECK(ws_diags_json["workspaceId"] == ws_id);
        CHECK(ws_diags_json["diagnostics"].is_array());

        // GET /workspaces/{id}/files/{fileId}/diagnostics
        auto file_diags_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                                  "/files/" + std::to_string(calc_file_id) + "/diagnostics");
        REQUIRE(file_diags_res != nullptr);
        CHECK(file_diags_res->status == 200);
        auto file_diags_json = nlohmann::json::parse(file_diags_res->body);
        CHECK(file_diags_json["fileId"] == calc_file_id);
        CHECK(file_diags_json["diagnostics"].is_array());

        // GET /workspaces/{id}/symbols (listing with pagination)
        auto sym_list_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                                "/symbols?limit=10&offset=0");
        REQUIRE(sym_list_res != nullptr);
        CHECK(sym_list_res->status == 200);
        auto sym_list_json = nlohmann::json::parse(sym_list_res->body);
        CHECK(sym_list_json["items"].is_array());
        CHECK(sym_list_json["limit"] == 10);
        CHECK(sym_list_json["offset"] == 0);
        CHECK(sym_list_json["total"].get<int64_t>() >= 2);
        CHECK(sym_list_json.contains("hasMore"));

        int64_t compute_sym_id = 0;
        for (const auto& item : sym_list_json["items"]) {
            if (item["name"] == "compute") {
                compute_sym_id = item["id"].get<int64_t>();
                break;
            }
        }
        REQUIRE(compute_sym_id > 0);

        // GET /workspaces/{id}/symbols/{symbolId}
        auto sym_det_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                               "/symbols/" + std::to_string(compute_sym_id));
        REQUIRE(sym_det_res != nullptr);
        CHECK(sym_det_res->status == 200);
        auto sym_det_json = nlohmann::json::parse(sym_det_res->body);
        CHECK(sym_det_json["symbol"]["id"] == compute_sym_id);
        CHECK(sym_det_json["symbol"]["name"] == "compute");

        // GET /workspaces/{id}/symbols/{symbolId}/definitions
        auto defs_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                            "/symbols/" + std::to_string(compute_sym_id) + "/definitions");
        REQUIRE(defs_res != nullptr);
        CHECK(defs_res->status == 200);
        auto defs_json = nlohmann::json::parse(defs_res->body);
        CHECK(defs_json["symbolId"] == compute_sym_id);
        CHECK(defs_json["definitions"].is_array());
        CHECK(defs_json["total"].get<int64_t>() >= 1);

        // GET /workspaces/{id}/symbols/{symbolId}/references
        auto refs_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                            "/symbols/" + std::to_string(compute_sym_id) + "/references");
        REQUIRE(refs_res != nullptr);
        CHECK(refs_res->status == 200);
        auto refs_json = nlohmann::json::parse(refs_res->body);
        CHECK(refs_json["items"].is_array());
        CHECK(refs_json.contains("total"));

        // GET /workspaces/{id}/symbols/{symbolId}/callers
        auto callers_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                               "/symbols/" + std::to_string(compute_sym_id) + "/callers");
        REQUIRE(callers_res != nullptr);
        CHECK(callers_res->status == 200);
        auto callers_json = nlohmann::json::parse(callers_res->body);
        CHECK(callers_json["symbolId"] == compute_sym_id);
        CHECK(callers_json["callers"].is_array());

        // GET /workspaces/{id}/symbols/{symbolId}/callees
        auto callees_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                               "/symbols/" + std::to_string(compute_sym_id) + "/callees");
        REQUIRE(callees_res != nullptr);
        CHECK(callees_res->status == 200);
        auto callees_json = nlohmann::json::parse(callees_res->body);
        CHECK(callees_json["symbolId"] == compute_sym_id);
        CHECK(callees_json["callees"].is_array());

        // GET /workspaces/{id}/symbols/{symbolId}/graph
        auto graph_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/symbols/" + std::to_string(compute_sym_id) + "/graph?depth=2");
        REQUIRE(graph_res != nullptr);
        CHECK(graph_res->status == 200);
        auto graph_json = nlohmann::json::parse(graph_res->body);
        CHECK(graph_json["rootSymbolId"] == compute_sym_id);
        CHECK(graph_json["nodes"].is_array());
        CHECK(graph_json["edges"].is_array());

        // GET /workspaces/{id}/search (source text)
        auto search_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                              "/search?query=compute");
        REQUIRE(search_res != nullptr);
        CHECK(search_res->status == 200);
        auto search_json = nlohmann::json::parse(search_res->body);
        CHECK(search_json["items"].is_array());
        CHECK(search_json["limit"] == 50);
        CHECK(search_json.contains("total"));
        CHECK(search_json.contains("hasMore"));

        // GET /workspaces/{id}/search/symbols (symbol search)
        auto sym_search_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                                  "/search/symbols?query=compute");
        REQUIRE(sym_search_res != nullptr);
        CHECK(sym_search_res->status == 200);
        auto sym_search_json = nlohmann::json::parse(sym_search_res->body);
        CHECK(sym_search_json["items"].is_array());
        CHECK(sym_search_json["limit"] == 50);
        CHECK(sym_search_json["total"].get<int64_t>() >= 1);
        CHECK(sym_search_json["hasMore"].is_boolean());

        // DELETE /workspaces/{id}
        auto del_res = fixture.client->Delete("/api/v1/workspaces/" + std::to_string(ws_id));
        REQUIRE(del_res != nullptr);
        CHECK(del_res->status == 200);
        auto del_json = nlohmann::json::parse(del_res->body);
        CHECK(del_json["status"] == "deleted");
        CHECK(del_json["id"] == ws_id);

        // Subsequent GET /workspaces/{id} returns 404
        auto get_deleted_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id));
        REQUIRE(get_deleted_res != nullptr);
        CHECK(get_deleted_res->status == 404);
    }
}
