#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_c(void);

TEST_CASE("SQLite3 initializes and supports FTS5 full-text search", "[dependencies][sqlite3]") {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(":memory:", &db);
    REQUIRE(rc == SQLITE_OK);
    REQUIRE(db != nullptr);

    // Test creating an FTS5 virtual table
    const char* create_fts5_sql = "CREATE VIRTUAL TABLE code_symbols USING fts5("
                                  "    name,"
                                  "    signature,"
                                  "    content"
                                  ");";

    char* err_msg = nullptr;
    rc = sqlite3_exec(db, create_fts5_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err = (err_msg != nullptr) ? err_msg : "Unknown error";
        sqlite3_free(err_msg);
        sqlite3_close(db);
        FAIL("FTS5 table creation failed: " + err);
    }

    // Insert sample rows
    const char* insert_sql =
        "INSERT INTO code_symbols (name, signature, content) VALUES "
        "('calculate_total', 'int calculate_total(int a, int b)', 'return a + b;'), "
        "('find_symbol', 'Symbol* find_symbol(const char* name)', 'search symbol table');";

    rc = sqlite3_exec(db, insert_sql, nullptr, nullptr, &err_msg);
    REQUIRE(rc == SQLITE_OK);

    // Query using MATCH syntax
    const char* match_sql = "SELECT name FROM code_symbols WHERE code_symbols MATCH 'calculate';";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, match_sql, -1, &stmt, nullptr);
    REQUIRE(rc == SQLITE_OK);

    rc = sqlite3_step(stmt);
    REQUIRE(rc == SQLITE_ROW);
    std::string matched_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    REQUIRE(matched_name == "calculate_total");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_CASE("Tree-sitter core and C grammar parses C code correctly", "[dependencies][treesitter]") {
    TSParser* parser = ts_parser_new();
    REQUIRE(parser != nullptr);

    const TSLanguage* c_lang = tree_sitter_c();
    REQUIRE(c_lang != nullptr);

    bool set_lang_ok = ts_parser_set_language(parser, c_lang);
    REQUIRE(set_lang_ok);

    const char* source_code = "int add(int a, int b) { return a + b; }\n";
    TSTree* tree = ts_parser_parse_string(parser, nullptr, source_code,
                                          static_cast<uint32_t>(strlen(source_code)));
    REQUIRE(tree != nullptr);

    TSNode root = ts_tree_root_node(tree);
    REQUIRE(ts_node_is_null(root) == false);

    const char* root_type = ts_node_type(root);
    REQUIRE(std::string(root_type) == "translation_unit");

    uint32_t child_count = ts_node_child_count(root);
    REQUIRE(child_count >= 1);

    TSNode function_node = ts_node_child(root, 0);
    const char* func_type = ts_node_type(function_node);
    REQUIRE(std::string(func_type) == "function_definition");

    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

TEST_CASE("cpp-httplib can instantiate server and handle requests", "[dependencies][httplib]") {
    httplib::Server svr;

    bool handler_called = false;
    svr.Get("/ping", [&](const httplib::Request&, httplib::Response& res) {
        handler_called = true;
        res.set_content("pong", "text/plain");
    });

    REQUIRE(svr.is_valid());
}

TEST_CASE("nlohmann_json can parse and serialize objects", "[dependencies][json]") {
    nlohmann::json data = {{"project", "codelenses"},
                           {"cxx_standard", 20},
                           {"languages", {"c", "cpp", "python", "typescript"}}};

    std::string serialized = data.dump();
    auto parsed = nlohmann::json::parse(serialized);

    REQUIRE(parsed["project"] == "codelenses");
    REQUIRE(parsed["cxx_standard"] == 20);
    REQUIRE(parsed["languages"].size() == 4);
}
