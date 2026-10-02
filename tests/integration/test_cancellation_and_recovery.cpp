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
#include "codelenses/db/transaction.hpp"
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

struct RecoveryFixture {
    fs::path temp_dir;
    fs::path db_path;
    fs::path workspace_root;

    std::unique_ptr<Database> db;
    std::unique_ptr<index::IndexingPipeline> pipeline;
    std::unique_ptr<ApiService> service;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<httplib::Client> client;
    uint16_t port{0};

    RecoveryFixture() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = fs::temp_directory_path() / ("codelenses_recovery_" + pid + "_" + now);
        db_path = temp_dir / "recovery.db";
        workspace_root = temp_dir / "workspace";

        fs::create_directories(workspace_root / "src");

        // Generate 40 distinct C source files so indexing takes measurable time
        for (int i = 0; i < 40; ++i) {
            std::ofstream f(workspace_root / "src" / ("mod_" + std::to_string(i) + ".c"));
            f << "int worker_" << i << "(int val) {\n"
              << "    return val + " << i << ";\n"
              << "}\n"
              << "int dispatch_" << i << "() {\n"
              << "    return worker_" << i << "(" << i * 10 << ");\n"
              << "}\n";
            f.close();
        }

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

    void restart_server() {
        stop_server();
        start_server();
    }

    ~RecoveryFixture() {
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

TEST_CASE("Pipeline cancellation and database integrity (I-05)", "[pipeline][cancellation]") {
    RecoveryFixture fixture;

    // 1. Create workspace
    nlohmann::json ws_req = {
        {"rootPath", fixture.workspace_root.string()},
        {"name", "Cancellation Test Workspace"},
    };
    auto ws_res = fixture.client->Post("/api/v1/workspaces", ws_req.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    REQUIRE(ws_res->status == 201);
    int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

    SECTION("Cancelling an active indexing job leaves workspace in idle status and DB consistent") {
        // Trigger full indexing
        nlohmann::json idx_req = {{"jobType", "full"}};
        auto post_idx =
            fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                 idx_req.dump(), "application/json");
        REQUIRE(post_idx != nullptr);
        REQUIRE(post_idx->status == 202);
        int64_t job_id = nlohmann::json::parse(post_idx->body)["id"].get<int64_t>();

        // Post cancellation immediately
        auto cancel_res = fixture.client->Post("/api/v1/jobs/" + std::to_string(job_id) + "/cancel",
                                               "", "application/json");
        REQUIRE(cancel_res != nullptr);
        CHECK(cancel_res->status == 200);

        fixture.wait_for_job(job_id);

        // Verify job status
        auto job_res = fixture.client->Get("/api/v1/jobs/" + std::to_string(job_id));
        REQUIRE(job_res != nullptr);
        auto job_json = nlohmann::json::parse(job_res->body);
        std::string job_st = job_json["status"];
        CHECK((job_st == "cancelled" || job_st == "completed"));

        // Workspace status must be idle
        auto ws_status_res =
            fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
        REQUIRE(ws_status_res != nullptr);
        auto status_json = nlohmann::json::parse(ws_status_res->body);
        CHECK(status_json["status"] == "idle");

        // Verify SQLite database integrity
        Statement integrity(fixture.db->connection().handle(), "PRAGMA integrity_check;");
        REQUIRE(integrity.step());
        CHECK(integrity.column_text(0) == "ok");

        // Verify foreign key integrity
        Statement fk_check(fixture.db->connection().handle(), "PRAGMA foreign_key_check;");
        CHECK_FALSE(fk_check.step()); // No rows means no foreign key violations
    }
}

TEST_CASE("Restart recovery and incremental catch-up after cancellation (I-05)",
          "[pipeline][recovery]") {
    RecoveryFixture fixture;

    // 1. Create workspace
    nlohmann::json ws_req = {
        {"rootPath", fixture.workspace_root.string()},
        {"name", "Restart Recovery Workspace"},
    };
    auto ws_res = fixture.client->Post("/api/v1/workspaces", ws_req.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

    // 2. Start full indexing and cancel
    auto idx_res = fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                        "{\"jobType\": \"full\"}", "application/json");
    REQUIRE(idx_res != nullptr);
    int64_t first_job_id = nlohmann::json::parse(idx_res->body)["id"].get<int64_t>();

    // Cancel job
    fixture.client->Post("/api/v1/jobs/" + std::to_string(first_job_id) + "/cancel", "",
                         "application/json");
    fixture.wait_for_job(first_job_id);

    // Record partially indexed file count
    auto status_mid =
        fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
    REQUIRE(status_mid != nullptr);
    int64_t files_mid = nlohmann::json::parse(status_mid->body)["fileCount"].get<int64_t>();

    // 3. Simulate application crash / restart
    fixture.restart_server();

    // Check database integrity on reopen
    {
        Statement pragma_wal(fixture.db->connection().handle(), "PRAGMA journal_mode;");
        REQUIRE(pragma_wal.step());
        CHECK(pragma_wal.column_text(0) == "wal");

        Statement pragma_check(fixture.db->connection().handle(), "PRAGMA integrity_check;");
        REQUIRE(pragma_check.step());
        CHECK(pragma_check.column_text(0) == "ok");
    }

    // 4. Trigger incremental indexing on the reopened workspace
    auto resume_res = fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                           "{\"jobType\": \"incremental\"}", "application/json");
    REQUIRE(resume_res != nullptr);
    REQUIRE(resume_res->status == 202);
    int64_t resume_job_id = nlohmann::json::parse(resume_res->body)["id"].get<int64_t>();
    fixture.wait_for_job(resume_job_id);

    // Verify recovery job completed successfully
    auto resume_job_res = fixture.client->Get("/api/v1/jobs/" + std::to_string(resume_job_id));
    REQUIRE(resume_job_res != nullptr);
    auto resume_job_json = nlohmann::json::parse(resume_job_res->body);
    CHECK(resume_job_json["status"] == "completed");

    // Incremental planner should have skipped files that were already indexed before restart!
    int64_t files_skipped = resume_job_json["filesSkipped"].get<int64_t>();
    int64_t files_processed = resume_job_json["filesProcessed"].get<int64_t>();
    CHECK(files_skipped >= files_mid);
    CHECK(files_skipped + files_processed >= 40);

    // 5. Final workspace status verifies all 40 files and symbols are present
    auto final_status =
        fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
    REQUIRE(final_status != nullptr);
    auto fs_json = nlohmann::json::parse(final_status->body);
    CHECK(fs_json["fileCount"].get<int64_t>() >= 40);
    CHECK(fs_json["symbolCount"].get<int64_t>() >= 80); // 2 functions per file * 40 files

    // Verify symbol from file 0 and file 39 are both searchable
    auto sym0_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                        "/symbols?query=worker_0");
    REQUIRE(sym0_res != nullptr);
    CHECK(nlohmann::json::parse(sym0_res->body)["total"].get<int64_t>() >= 1);

    auto sym39_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                         "/symbols?query=worker_39");
    REQUIRE(sym39_res != nullptr);
    CHECK(nlohmann::json::parse(sym39_res->body)["total"].get<int64_t>() >= 1);
}

TEST_CASE("Transaction rollback prevents partial state corruption (I-05)", "[pipeline][rollback]") {
    RecoveryFixture fixture;

    nlohmann::json ws_req = {
        {"rootPath", fixture.workspace_root.string()},
        {"name", "Rollback Test"},
    };
    auto ws_res = fixture.client->Post("/api/v1/workspaces", ws_req.dump(), "application/json");
    REQUIRE(ws_res != nullptr);
    int64_t ws_id = nlohmann::json::parse(ws_res->body)["id"].get<int64_t>();

    // Index fully first
    auto idx_res = fixture.client->Post("/api/v1/workspaces/" + std::to_string(ws_id) + "/index",
                                        "{\"jobType\": \"full\"}", "application/json");
    REQUIRE(idx_res != nullptr);
    int64_t job_id = nlohmann::json::parse(idx_res->body)["id"].get<int64_t>();
    fixture.wait_for_job(job_id);

    auto before_status =
        fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
    REQUIRE(before_status != nullptr);
    int64_t before_syms = nlohmann::json::parse(before_status->body)["symbolCount"].get<int64_t>();

    // Simulate an aborted / crashed transaction
    {
        Transaction tx(fixture.db->connection(), TransactionType::immediate);
        fixture.db->connection().execute(
            "INSERT INTO symbol (workspace_id, file_id, symbol_key, name, kind, language, "
            "is_definition, "
            "start_byte, end_byte, start_line, start_column, end_line, end_column) "
            "VALUES (" +
            std::to_string(ws_id) +
            ", 1, 'orphan_key', 'orphan_sym', 'function', 'c', 1, 0, 10, 0, 0, 1, 0);");
        // Intentionally NOT calling tx.commit() -> destructor will roll back!
    }

    // Reopen database to verify recovery
    fixture.restart_server();

    // Verify symbol count was NOT corrupted by the aborted transaction
    auto after_status =
        fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) + "/status");
    REQUIRE(after_status != nullptr);
    int64_t after_syms = nlohmann::json::parse(after_status->body)["symbolCount"].get<int64_t>();
    CHECK(after_syms == before_syms);

    // Orphan symbol must not exist
    auto orphan_res = fixture.client->Get("/api/v1/workspaces/" + std::to_string(ws_id) +
                                          "/symbols?query=orphan_sym");
    REQUIRE(orphan_res != nullptr);
    CHECK(nlohmann::json::parse(orphan_res->body)["total"].get<int64_t>() == 0);
}
