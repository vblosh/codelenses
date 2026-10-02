#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "codelenses/app/version.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/filesystem/file_capture.hpp"
#include "codelenses/filesystem/path.hpp"
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

struct SecurityTestFixture {
    fs::path temp_dir;
    fs::path db_path;
    fs::path workspace_root;
    fs::path outside_dir;

    std::unique_ptr<Database> db;
    std::unique_ptr<index::IndexingPipeline> pipeline;
    std::unique_ptr<ApiService> service;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
    uint16_t port{0};

    SecurityTestFixture() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = fs::temp_directory_path() / ("codelenses_sec_" + pid + "_" + now);
        db_path = temp_dir / "security_test.db";
        workspace_root = temp_dir / "workspace";
        outside_dir = temp_dir / "outside";

        fs::create_directories(workspace_root / "src");
        fs::create_directories(outside_dir);

        // Outside secret file that must NEVER be leaked
        std::ofstream secret(outside_dir / "secret.txt");
        secret << "TOP_SECRET_CREDENTIALS_DO_NOT_READ\n";
        secret.close();

        // Valid file inside workspace
        std::ofstream valid_c(workspace_root / "src" / "valid.c");
        valid_c << "int valid_func(int a) {\n    return a * 2;\n}\n";
        valid_c.close();

        // Valid Python file
        std::ofstream valid_py(workspace_root / "src" / "valid.py");
        valid_py << "def valid_python():\n    return 42\n";
        valid_py.close();

        // 1. Syntax-error C file (unbalanced braces, random tokens, junk)
        std::ofstream broken_c(workspace_root / "src" / "broken.c");
        broken_c << "int broken( {\n    return @#$%^&*;\n    void ??? !!!\n";
        broken_c.close();

        // 2. Syntax-error C++ file (malformed templates)
        std::ofstream broken_cpp(workspace_root / "src" / "broken.cpp");
        broken_cpp << "template<class class class >>> struct Foo< { ::? void >>>\n";
        broken_cpp.close();

        // 3. Syntax-error Python file (bad indentation and unclosed literal)
        std::ofstream broken_py(workspace_root / "src" / "broken.py");
        broken_py << "def bad_py(\n  x = \"unterminated string\n\t\t bad indent\n";
        broken_py.close();

        // 4. Binary file disguised as source (.c extension with null bytes and invalid UTF-8)
        std::ofstream bin_c(workspace_root / "src" / "fake_binary.c", std::ios::binary);
        const char bin_bytes[] = {'\x00', '\x01', '\x02', '\xFF', '\xFE', '\x00',
                                  '\x00', '\x7F', 'E',    'L',    'F'};
        bin_c.write(bin_bytes, sizeof(bin_bytes));
        bin_c.close();

        // 5. Corrupted UTF-8 file (truncated multi-byte sequence)
        std::ofstream utf8_c(workspace_root / "src" / "corrupted_utf8.c", std::ios::binary);
        const char utf8_bytes[] = {'i', 'n', 't', ' ', 'x', ' ', '=', ' ', '\xC3', ';', '\n'};
        utf8_c.write(utf8_bytes, sizeof(utf8_bytes));
        utf8_c.close();

        // 6. 0-byte empty file
        std::ofstream empty_c(workspace_root / "src" / "empty.c");
        empty_c.close();

        // 7. Whitespace-only file
        std::ofstream ws_c(workspace_root / "src" / "whitespace_only.c");
        ws_c << "   \n\t\t\n   \r\n   \n";
        ws_c.close();

        // 8. Single huge minified line (100,000 characters)
        std::ofstream mini_c(workspace_root / "src" / "minified.c");
        mini_c << "int dummy=0;";
        for (int i = 0; i < 10000; ++i) {
            mini_c << "dummy++;";
        }
        mini_c << "\n";
        mini_c.close();

        // 9. Deeply nested expressions (200 levels of nested parens)
        std::ofstream deep_c(workspace_root / "src" / "deeply_nested.c");
        deep_c << "int deep_calc() { return ";
        for (int i = 0; i < 200; ++i) {
            deep_c << "(";
        }
        deep_c << "1";
        for (int i = 0; i < 200; ++i) {
            deep_c << ")";
        }
        deep_c << "; }\n";
        deep_c.close();

        // 10. Symlink attempting to escape workspace root
        std::error_code ec;
        fs::create_directory_symlink(outside_dir, workspace_root / "symlink_escape", ec);

        start_server();
    }

    void start_server() {
        db = Database::open(db_path.string(), true);
        pipeline = std::make_unique<index::IndexingPipeline>(*db);
        service = std::make_unique<ApiService>(*db, *pipeline);

        HttpServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
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

    ~SecurityTestFixture() {
        stop_server();
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void wait_for_job(int64_t job_id, int max_checks = 600) {
        for (int i = 0; i < max_checks; ++i) {
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

TEST_CASE("Hostile-path and security containment validation (I-04)", "[security][path]") {
    SecurityTestFixture fixture;

    SECTION("Reject non-existent workspace root path") {
        nlohmann::json req = {
            {"rootPath", (fixture.temp_dir / "does_not_exist").string()},
            {"name", "Invalid Path Workspace"},
        };
        auto res = fixture.client->Post("/api/v1/workspaces", req.dump(), "application/json");
        REQUIRE(res != nullptr);
        CHECK(res->status == 400);
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["error"]["code"] == "invalid_workspace_root");
    }

    SECTION("Reject regular file as workspace root path") {
        fs::path file_root = fixture.workspace_root / "src" / "valid.c";
        nlohmann::json req = {
            {"rootPath", file_root.string()},
            {"name", "File As Root"},
        };
        auto res = fixture.client->Post("/api/v1/workspaces", req.dump(), "application/json");
        REQUIRE(res != nullptr);
        CHECK(res->status == 400);
        auto j = nlohmann::json::parse(res->body);
        CHECK(j["error"]["code"] == "invalid_workspace_root");
    }

    SECTION("Reject path traversal in tree query") {
        // Create valid workspace
        nlohmann::json req = {
            {"rootPath", fixture.workspace_root.string()},
            {"name", "Traversal Test Workspace"},
        };
        auto ws_res = fixture.client->Post("/api/v1/workspaces", req.dump(), "application/json");
        REQUIRE(ws_res != nullptr);
        REQUIRE(ws_res->status == 201);
        int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

        // Path traversal attempts
        std::vector<std::string> hostile_paths = {
            "../",
            "../../",
            "../../../etc",
            "../../outside",
            "src/../../outside",
            "src/../../../etc/passwd",
            "/etc",
            "/etc/passwd",
        };

        for (const auto& hp : hostile_paths) {
            httplib::Params params = {{"path", hp}};
            auto res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree",
                                           params, httplib::Headers{});
            REQUIRE(res != nullptr);
            CHECK(res->status == 400);
            auto j = nlohmann::json::parse(res->body);
            CHECK(j["error"]["code"] == "path_traversal");
        }
    }

    SECTION("Containment helper prevents directory traversal and symlink escapes") {
        auto canonical_root = filesystem::canonicalize_workspace_root(fixture.workspace_root);
        REQUIRE(canonical_root.has_value());

        // Check relative traversal
        auto outside_res =
            filesystem::resolve_workspace_path(*canonical_root, "../outside/secret.txt");
        CHECK(!outside_res.has_value());

        // Check symlink escape resolution
        if (fs::exists(fixture.workspace_root / "symlink_escape")) {
            auto symlink_res =
                filesystem::resolve_workspace_path(*canonical_root, "symlink_escape/secret.txt");
            CHECK(!symlink_res.has_value());
        }

        // Direct containment check
        CHECK(!filesystem::is_contained_in(*canonical_root, fixture.outside_dir));
        CHECK(!filesystem::is_contained_in(*canonical_root, fixture.outside_dir / "secret.txt"));
        CHECK(filesystem::is_contained_in(*canonical_root,
                                          fixture.workspace_root / "src" / "valid.c"));
    }

    SECTION("Cross-workspace file isolation prevents accessing foreign workspace resources") {
        // Create Workspace A
        nlohmann::json req_a = {
            {"rootPath", fixture.workspace_root.string()},
            {"name", "Workspace A"},
        };
        auto res_a = fixture.client->Post("/api/v1/workspaces", req_a.dump(), "application/json");
        REQUIRE(res_a != nullptr);
        REQUIRE(res_a->status == 201);
        int64_t ws_a_id = nlohmann::json::parse(res_a->body)["id"].get<int64_t>();

        // Create Workspace B in a separate directory
        fs::path ws_b_root = fixture.temp_dir / "workspace_b";
        fs::create_directories(ws_b_root);
        std::ofstream b_file(ws_b_root / "b.c");
        b_file << "int b_code() { return 1; }\n";
        b_file.close();

        nlohmann::json req_b = {
            {"rootPath", ws_b_root.string()},
            {"name", "Workspace B"},
        };
        auto res_b = fixture.client->Post("/api/v1/workspaces", req_b.dump(), "application/json");
        REQUIRE(res_b != nullptr);
        REQUIRE(res_b->status == 201);
        int64_t ws_b_id = nlohmann::json::parse(res_b->body)["id"].get<int64_t>();

        // Index both
        auto idx_a = fixture.client->Post(
            "/api/v1/workspaces/" + std::to_string(ws_a_id) + "/index", "{}", "application/json");
        auto idx_b = fixture.client->Post(
            "/api/v1/workspaces/" + std::to_string(ws_b_id) + "/index", "{}", "application/json");
        REQUIRE(idx_a != nullptr);
        REQUIRE(idx_b != nullptr);
        fixture.wait_for_job(nlohmann::json::parse(idx_a->body)["id"].get<int64_t>());
        fixture.wait_for_job(nlohmann::json::parse(idx_b->body)["id"].get<int64_t>());

        // Find file ID in Workspace B
        auto tree_b =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_b_id) + "/tree");
        REQUIRE(tree_b != nullptr);
        auto tb_json = nlohmann::json::parse(tree_b->body);
        REQUIRE(!tb_json["entries"].empty());
        int64_t file_b_id = tb_json["entries"][0]["fileId"].get<int64_t>();

        // Requesting Workspace B file via Workspace A must be rejected with 404
        auto cross_file = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_a_id) +
                                              "/files/" + std::to_string(file_b_id));
        REQUIRE(cross_file != nullptr);
        CHECK(cross_file->status == 404);

        auto cross_content =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_a_id) + "/files/" +
                                std::to_string(file_b_id) + "/content");
        REQUIRE(cross_content != nullptr);
        CHECK(cross_content->status == 404);

        auto cross_syms = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_a_id) +
                                              "/symbols?fileId=" + std::to_string(file_b_id));
        REQUIRE(cross_syms != nullptr);
        CHECK(cross_syms->status == 404);
    }
}

TEST_CASE("Malformed-source resilience and parser error diagnostics (I-04)",
          "[security][malformed]") {
    SecurityTestFixture fixture;

    // Create workspace with malformed files
    nlohmann::json req = {
        {"rootPath", fixture.workspace_root.string()},
        {"name", "Malformed & Robustness Workspace"},
    };
    auto ws_res = fixture.client->Post("/api/v1/workspaces", req.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    REQUIRE(ws_res->status == 201);
    int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

    // Index the workspace
    auto idx_res = fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                        "{}", "application/json");
    REQUIRE(idx_res != nullptr);
    REQUIRE(idx_res->status == 202);
    int64_t job_id = nlohmann::json::parse(idx_res->body)["id"].get<int64_t>();
    fixture.wait_for_job(job_id);

    SECTION("Malformed files do not abort the job: job completes successfully") {
        auto job_res = fixture.client->Get("/api/v1/jobs/" + std::to_string(job_id));
        REQUIRE(job_res != nullptr);
        CHECK(job_res->status == 200);
        auto job_json = nlohmann::json::parse(job_res->body);

        // Core Requirement: A malformed source file must not abort the entire job
        CHECK(job_json["status"] == "completed");
        CHECK(job_json["filesProcessed"].get<int64_t>() >= 5);
    }

    SECTION("Valid files in the same workspace are properly parsed and indexed") {
        // Query symbols for valid_func in valid.c
        auto sym_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/symbols?query=valid_func");
        REQUIRE(sym_res != nullptr);
        CHECK(sym_res->status == 200);
        auto sym_json = nlohmann::json::parse(sym_res->body);
        CHECK(sym_json["total"].get<int64_t>() >= 1);
        CHECK(sym_json["items"][0]["name"] == "valid_func");

        // Query symbols for valid_python in valid.py
        auto py_sym_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                              "/symbols?query=valid_python");
        REQUIRE(py_sym_res != nullptr);
        CHECK(py_sym_res->status == 200);
        auto py_sym_json = nlohmann::json::parse(py_sym_res->body);
        CHECK(py_sym_json["total"].get<int64_t>() >= 1);
        CHECK(py_sym_json["items"][0]["name"] == "valid_python");
    }

    SECTION("Binary file disguised with .c extension is detected and safely handled") {
        auto tree_res =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
        REQUIRE(tree_res != nullptr);
        auto tree_json = nlohmann::json::parse(tree_res->body);

        int64_t bin_file_id = 0;
        for (const auto& entry : tree_json["entries"]) {
            if (entry["name"] == "fake_binary.c") {
                bin_file_id = entry["fileId"].get<int64_t>();
                break;
            }
        }
        REQUIRE(bin_file_id > 0);

        // Verify file metadata indicates binary
        auto file_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                            "/files/" + std::to_string(bin_file_id));
        REQUIRE(file_res != nullptr);
        CHECK(file_res->status == 200);
        auto file_json = nlohmann::json::parse(file_res->body);
        CHECK(file_json["isBinary"] == true);

        // Content request returns is_binary = true
        auto content_res =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                std::to_string(bin_file_id) + "/content");
        REQUIRE(content_res != nullptr);
        CHECK(content_res->status == 200);
        auto content_json = nlohmann::json::parse(content_res->body);
        CHECK(content_json["isBinary"] == true);
    }

    SECTION("Empty and whitespace-only files are indexed cleanly with 0 symbols") {
        auto tree_res =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
        REQUIRE(tree_res != nullptr);
        auto tree_json = nlohmann::json::parse(tree_res->body);

        int64_t empty_file_id = 0;
        int64_t ws_file_id = 0;
        for (const auto& entry : tree_json["entries"]) {
            if (entry["name"] == "empty.c") {
                empty_file_id = entry["fileId"].get<int64_t>();
            } else if (entry["name"] == "whitespace_only.c") {
                ws_file_id = entry["fileId"].get<int64_t>();
            }
        }
        REQUIRE(empty_file_id > 0);
        REQUIRE(ws_file_id > 0);

        // Symbols in empty file
        auto empty_syms =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                std::to_string(empty_file_id) + "/symbols");
        REQUIRE(empty_syms != nullptr);
        CHECK(empty_syms->status == 200);
        CHECK(nlohmann::json::parse(empty_syms->body)["total"] == 0);

        // Symbols in whitespace file
        auto ws_syms = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                           "/files/" + std::to_string(ws_file_id) + "/symbols");
        REQUIRE(ws_syms != nullptr);
        CHECK(ws_syms->status == 200);
        CHECK(nlohmann::json::parse(ws_syms->body)["total"] == 0);
    }

    SECTION("Minified single-line and deeply nested files do not crash the server") {
        auto tree_res =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/tree?path=src");
        REQUIRE(tree_res != nullptr);
        auto tree_json = nlohmann::json::parse(tree_res->body);

        int64_t mini_file_id = 0;
        int64_t deep_file_id = 0;
        for (const auto& entry : tree_json["entries"]) {
            if (entry["name"] == "minified.c") {
                mini_file_id = entry["fileId"].get<int64_t>();
            } else if (entry["name"] == "deeply_nested.c") {
                deep_file_id = entry["fileId"].get<int64_t>();
            }
        }
        REQUIRE(mini_file_id > 0);
        REQUIRE(deep_file_id > 0);

        // Minified file content can be retrieved without buffer overflow
        auto mini_content =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/files/" +
                                std::to_string(mini_file_id) + "/content");
        REQUIRE(mini_content != nullptr);
        CHECK(mini_content->status == 200);
        auto mini_json = nlohmann::json::parse(mini_content->body);
        CHECK(mini_json["totalSizeBytes"].get<int64_t>() > 50000);

        // Deeply nested file extracted its symbol safely
        auto deep_syms = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                             "/files/" + std::to_string(deep_file_id) + "/symbols");
        REQUIRE(deep_syms != nullptr);
        CHECK(deep_syms->status == 200);
        auto deep_json = nlohmann::json::parse(deep_syms->body);
        CHECK(deep_json["total"].get<int64_t>() >= 1);
    }

    SECTION("Parser diagnostics are stored for broken files") {
        auto diags_res =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/diagnostics");
        REQUIRE(diags_res != nullptr);
        CHECK(diags_res->status == 200);
        auto diags_json = nlohmann::json::parse(diags_res->body);
        CHECK(diags_json["workspaceId"] == ws_id);
        // Diagnostics array exists and is valid
        CHECK(diags_json["diagnostics"].is_array());
    }
}
