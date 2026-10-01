#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/db/migration.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::index;
using namespace codelenses::server;

namespace {

struct TempWorkspaceRoots {
    fs::path path;

    explicit TempWorkspaceRoots(const std::string& prefix) {
        const auto nonce =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / (prefix + "_" + nonce);
        fs::create_directories(path);
    }

    ~TempWorkspaceRoots() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const fs::path& relative_path, const std::string& contents) {
        const auto full_path = path / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream output(full_path, std::ios::binary);
        output << contents;
    }
};

} // namespace

TEST_CASE("A linked to B cannot observe B's outgoing C targets", "[workspace-links][isolation]") {
    TempWorkspaceRoots roots("workspace_link_isolation");
    roots.write("a/App.cs", "public class App : Hidden.Secret { }\n");
    roots.write("b/Bridge.cs", "public class Bridge : Hidden.Secret { }\n");
    roots.write("c/Secret.cs", "namespace Hidden; public class Secret { }\n");

    auto db = Database::open_memory();
    IndexingPipeline pipeline(*db);
    ApiService service(*db, pipeline);
    auto create_workspace = [&](const std::string& directory) {
        CreateWorkspaceRequest request;
        request.root_path = (roots.path / directory).string();
        request.name = directory;
        return service.create_workspace(request).id;
    };

    const auto a = create_workspace("a");
    const auto b = create_workspace("b");
    const auto c = create_workspace("c");
    REQUIRE(pipeline.run_indexing(c).has_value());

    service.link_workspace(b, c);
    REQUIRE(pipeline.run_indexing(b).has_value());
    service.link_workspace(a, b);
    REQUIRE(pipeline.run_indexing(a).has_value());

    const auto c_symbols = db->symbols().list_by_workspace(c);
    const auto secret = std::find_if(c_symbols.begin(), c_symbols.end(),
                                     [](const auto& symbol) { return symbol.name == "Secret"; });
    REQUIRE(secret != c_symbols.end());
    const auto b_symbols = db->symbols().list_by_workspace(b);
    const auto bridge = std::find_if(b_symbols.begin(), b_symbols.end(),
                                     [](const auto& symbol) { return symbol.name == "Bridge"; });
    REQUIRE(bridge != b_symbols.end());

    const auto b_occurrences = db->occurrences().list_by_workspace(b);
    const auto inherited_secret =
        std::find_if(b_occurrences.begin(), b_occurrences.end(), [](const auto& occurrence) {
            return occurrence.occurrence_kind == "inheritance" &&
                   occurrence.name.find("Secret") != std::string::npos;
        });
    REQUIRE(inherited_secret != b_occurrences.end());
    CHECK(inherited_secret->resolution == "resolved");
    REQUIRE(inherited_secret->symbol_id == secret->id);

    const auto b_file = db->files().get_by_id(inherited_secret->file_id);
    REQUIRE(b_file.has_value());
    const auto b_view = service.get_file_occurrences(b, b_file->id);
    const auto b_visible_occurrence =
        std::find_if(b_view.begin(), b_view.end(),
                     [&](const auto& occurrence) { return occurrence.id == inherited_secret->id; });
    REQUIRE(b_visible_occurrence != b_view.end());
    CHECK(b_visible_occurrence->symbol_id == secret->id);
    CHECK(b_visible_occurrence->resolution == "resolved");

    const auto b_graph = service.get_symbol_graph(b, bridge->id, 1, 20, 50, {});
    CHECK(std::any_of(b_graph.edges.begin(), b_graph.edges.end(),
                      [&](const auto& edge) { return edge.target_symbol_id == secret->id; }));

    const auto a_view = service.get_file_occurrences(a, b_file->id);
    const auto hidden_occurrence =
        std::find_if(a_view.begin(), a_view.end(),
                     [&](const auto& occurrence) { return occurrence.id == inherited_secret->id; });
    REQUIRE(hidden_occurrence != a_view.end());
    CHECK_FALSE(hidden_occurrence->symbol_id.has_value());
    CHECK(hidden_occurrence->resolution == "unresolved");
    const auto hidden_occurrence_json = nlohmann::json(*hidden_occurrence);
    CHECK_FALSE(hidden_occurrence_json.contains("metadata"));
    CHECK_FALSE(hidden_occurrence_json.contains("metadataJson"));

    const auto a_graph = service.get_symbol_graph(a, bridge->id, 1, 20, 50, {});
    CHECK(std::none_of(a_graph.nodes.begin(), a_graph.nodes.end(),
                       [&](const auto& node) { return node.id == secret->id; }));
    CHECK(std::none_of(a_graph.edges.begin(), a_graph.edges.end(),
                       [&](const auto& edge) { return edge.target_symbol_id == secret->id; }));

    const auto a_occurrences = db->occurrences().list_by_workspace(a);
    const auto a_inheritance =
        std::find_if(a_occurrences.begin(), a_occurrences.end(), [](const auto& occurrence) {
            return occurrence.occurrence_kind == "inheritance" &&
                   occurrence.name.find("Secret") != std::string::npos;
        });
    REQUIRE(a_inheritance != a_occurrences.end());
    CHECK(a_inheritance->resolution == "unresolved");
    CHECK_FALSE(a_inheritance->symbol_id.has_value());
    const auto a_search_hits = service.search_symbols(a, "Secret", 20, 0).items;
    CHECK(std::none_of(a_search_hits.begin(), a_search_hits.end(), [&](const auto& hit) {
        return hit.origin_metadata.owner_workspace_id == c;
    }));
    CHECK(std::none_of(a_search_hits.begin(), a_search_hits.end(),
                       [&](const auto& hit) { return hit.id == secret->id; }));
    CHECK(service.get_symbol_references(a, bridge->id).total == 0);
}

TEST_CASE("Linked-workspace migration preserves indexed identities and search entries",
          "[migration][workspace-links]") {
    auto db = Database::open_memory(false);
    auto& connection = db->connection();
    auto& runner = db->migration_runner();
    runner.ensure_migration_table(connection);
    connection.execute("PRAGMA foreign_keys = OFF;");
    for (const auto& migration : runner.registered_migrations()) {
        if (migration.version >= 5)
            break;
        connection.execute(migration.up_sql);
        Statement record(connection.handle(),
                         "INSERT INTO schema_migration(version, name) VALUES (?, ?);");
        record.bind_int64(1, migration.version);
        record.bind_text(2, migration.name);
        record.execute();
    }

    connection.execute(R"SQL(
        INSERT INTO workspace(id, root_path, name, kind) VALUES
            (1, '/legacy/app', 'App', 'project'),
            (2, '/legacy/sdk', 'SDK', 'library');
        INSERT INTO library_profile(
            id, workspace_id, name, language, source_roots_json, target_framework
        ) VALUES (10, 2, 'SDK sources', 'csharp', '["/legacy/sdk", "/legacy/contracts"]', 'net8.0');
        INSERT INTO library_source_root(id, profile_id, ordinal, root_path) VALUES
            (20, 10, 0, '/legacy/sdk'),
            (21, 10, 1, '/legacy/contracts');
        INSERT INTO workspace_library(workspace_id, profile_id) VALUES (1, 10);
        INSERT INTO file(
            id, workspace_id, path, relative_path, name, extension, language,
            size_bytes, modified_ns, content_hash
        ) VALUES (
            30, 2, '/legacy/sdk/Widget.cs', 'root-20/Widget.cs', 'Widget.cs',
            '.cs', 'csharp', 12, 1234, 'legacy-file-hash'
        );
        INSERT INTO symbol(
            id, workspace_id, file_id, symbol_key, name, qualified_name,
            kind, language, is_definition, start_byte, end_byte,
            start_line, start_column, end_line, end_column
        ) VALUES (
            40, 2, 30, 'Legacy.Widget', 'Widget', 'Legacy.Widget',
            'class', 'csharp', 1, 0, 6, 1, 1, 1, 7
        );
        INSERT INTO occurrence(
            id, workspace_id, file_id, symbol_id, occurrence_kind, name,
            start_byte, end_byte, start_line, start_column, end_line, end_column,
            confidence, resolution
        ) VALUES (
            50, 2, 30, 40, 'definition', 'Widget',
            0, 6, 1, 1, 1, 7, 1.0, 'resolved'
        );
    )SQL");

    runner.apply_pending(connection);

    REQUIRE(db->workspaces().linked_ids(1) == std::vector<int64_t>{2});
    const auto settings = db->workspace_settings().get_profile_by_workspace(2);
    REQUIRE(settings.has_value());
    CHECK(settings->id == 10);
    const auto roots_after_migration = db->workspace_settings().list_source_roots(10);
    REQUIRE(roots_after_migration.size() == 2);
    CHECK(roots_after_migration[0].id == 20);
    CHECK(roots_after_migration[1].id == 21);

    const auto file = db->files().get_by_id(30);
    REQUIRE(file.has_value());
    CHECK(file->workspace_id == 2);
    CHECK(file->relative_path == "root-20/Widget.cs");
    const auto symbol = db->symbols().get_by_id(40);
    REQUIRE(symbol.has_value());
    CHECK(symbol->workspace_id == 2);
    CHECK(symbol->file_id == 30);
    CHECK(symbol->symbol_key == "Legacy.Widget");
    const auto occurrence = db->occurrences().get_by_id(50);
    REQUIRE(occurrence.has_value());
    CHECK(occurrence->workspace_id == 2);
    CHECK(occurrence->file_id == 30);
    CHECK(occurrence->symbol_id == 40);

    const auto indexed_symbols = db->fts().search_symbols(2, "Widget", 10, 0);
    CHECK(std::any_of(indexed_symbols.begin(), indexed_symbols.end(),
                      [](const auto& hit) { return hit.id == 40; }));
    Statement foreign_keys(connection.handle(), "PRAGMA foreign_key_check;");
    CHECK_FALSE(foreign_keys.step());
}
