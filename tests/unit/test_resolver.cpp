#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/resolver/compile_commands.hpp"
#include "codelenses/resolver/dependency_resolver.hpp"
#include "codelenses/resolver/relationship_builder.hpp"
#include "codelenses/resolver/resolver.hpp"
#include "codelenses/resolver/symbol_key.hpp"
#include "codelenses/resolver/symbol_resolver.hpp"
#include "codelenses/resolver/types.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::resolver;
using namespace codelenses::index;

namespace {

struct TestWorkspaceEnv {
    fs::path root;
    fs::path db_file;
    std::unique_ptr<Database> db;

    TestWorkspaceEnv() {
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::temp_directory_path() / ("codelenses_test_res_" + now);
        db_file = fs::temp_directory_path() / ("codelenses_test_res_" + now + ".db");
        fs::remove_all(root);
        fs::remove(db_file);
        fs::create_directories(root);
        db = Database::open(db_file.string(), true);
    }

    ~TestWorkspaceEnv() {
        db.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove(db_file, ec);
        fs::remove(db_file.string() + "-wal", ec);
        fs::remove(db_file.string() + "-shm", ec);
    }

    void write_file(const fs::path& rel_path, const std::string& content) {
        auto full = root / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
    }

    int64_t create_workspace(const std::string& name = "Resolver WS") {
        Workspace ws{
            .root_path = root.string(),
            .name = name,
        };
        return db->workspaces().create(ws);
    }
};

} // namespace

// =============================================================================
// Task E-01: Scope & Symbol-Candidate Lookup Interfaces
// =============================================================================

TEST_CASE("Scope hierarchy and candidate lookup interfaces (E-01)", "[resolver][scope]") {
    SECTION("C/C++ scope hierarchy with :: delimiter") {
        auto scopes = get_scope_hierarchy("Outer::Inner::Deep", "::");
        REQUIRE(scopes.size() == 4);
        REQUIRE(scopes[0] == "Outer::Inner::Deep");
        REQUIRE(scopes[1] == "Outer::Inner");
        REQUIRE(scopes[2] == "Outer");
        REQUIRE(scopes[3] == ""); // global scope
    }

    SECTION("Dot delimiter for Python, Java, C#, TS") {
        auto scopes = get_scope_hierarchy("com.example.service.UserService", ".");
        REQUIRE(scopes.size() == 5);
        REQUIRE(scopes[0] == "com.example.service.UserService");
        REQUIRE(scopes[1] == "com.example.service");
        REQUIRE(scopes[2] == "com.example");
        REQUIRE(scopes[3] == "com");
        REQUIRE(scopes[4] == "");
    }

    SECTION("Empty or null scope returns single global scope") {
        auto scopes_null = get_scope_hierarchy(std::nullopt);
        REQUIRE(scopes_null.size() == 1);
        REQUIRE(scopes_null[0] == "");

        auto scopes_empty = get_scope_hierarchy("");
        REQUIRE(scopes_empty.size() == 1);
        REQUIRE(scopes_empty[0] == "");
    }

    SECTION("Language compatibility and SymbolResolver candidate lookups") {
        REQUIRE(languages_compatible(Language::c, Language::cpp));
        REQUIRE(languages_compatible(Language::cpp, Language::c));
        REQUIRE(languages_compatible(Language::typescript, Language::javascript));
        REQUIRE(languages_compatible(Language::shell, Language::bash));
        REQUIRE_FALSE(languages_compatible(Language::cpp, Language::python));

        SymbolResolver resolver;
        resolver.add_symbol(SymbolCandidate{
            .symbol_id = 42,
            .file_id = 1,
            .file_path = "src/main.cpp",
            .symbol_key = "cpp:src/main.cpp#function#foo#()",
            .name = "foo",
            .qualified_name = "foo",
            .kind = "function",
            .language = "cpp",
        });

        REQUIRE(resolver.find_symbol_by_id(42) != nullptr);
        REQUIRE(resolver.find_symbol_by_key("cpp:src/main.cpp#function#foo#()") != nullptr);
        REQUIRE(resolver.find_symbol_by_key("nonexistent") == nullptr);
    }
}

// =============================================================================
// Task E-02: Deterministic Symbol-Key Generation
// =============================================================================

TEST_CASE("Deterministic symbol-key generation (E-02)", "[resolver][symbol_key]") {
    SECTION("Signature normalization extracts types and strips variable names") {
        std::string sig = "int calculate(int x, const double& factor, char* buffer)";
        std::string norm = normalize_signature(sig);
        REQUIRE(norm == "(int, const double&, char*)");

        std::string multi_sig = "void set_mask(unsigned int, const char*)";
        REQUIRE(normalize_signature(multi_sig) == "(unsigned int, const char*)");

        std::string template_sig = "void process(std::vector<int> items, Map<std::string, int> m)";
        std::string norm_tpl = normalize_signature(template_sig);
        REQUIRE(norm_tpl == "(std::vector<int>, Map<std::string, int>)");

        std::string void_sig = "void run(void)";
        REQUIRE(normalize_signature(void_sig) == "()");

        std::string empty_sig = "void reset()";
        REQUIRE(normalize_signature(empty_sig) == "()");
    }

    SECTION("Deterministic key generation across reindexing") {
        auto key1 = generate_symbol_key("cpp", "src/math.cpp", "function", "math::add",
                                        "int add(int a, int b)");
        auto key2 = generate_symbol_key("cpp", "src/math.cpp", "function", "math::add",
                                        "int add(int x, int y)");
        // Same function signature with different param names generates identical stable key!
        REQUIRE(key1 == key2);
        REQUIRE(key1 == "cpp:src/math.cpp#function#math::add#(int, int)");
    }

    SECTION("Overloads generate distinct deterministic keys") {
        auto key_int = generate_symbol_key("cpp", "src/math.cpp", "function", "math::add",
                                           "int add(int a, int b)");
        auto key_double = generate_symbol_key("cpp", "src/math.cpp", "function", "math::add",
                                              "double add(double a, double b)");
        REQUIRE(key_int != key_double);
    }

    SECTION("Disambiguator byte appends @offset when needed") {
        auto key = generate_symbol_key("python", "app.py", "variable", "temp", "", 120);
        REQUIRE(key == "python:app.py#variable#temp@120");
    }
}

// =============================================================================
// Task E-03: Same-File Lexical/Scope Resolution
// =============================================================================

TEST_CASE("Same-file lexical/scope resolution with shadowing (E-03)", "[resolver][lexical]") {
    SymbolResolver resolver;

    // Global variable x (id=1)
    resolver.add_symbol(SymbolCandidate{
        .symbol_id = 1,
        .file_id = 10,
        .file_path = "src/main.cpp",
        .symbol_key = "cpp:src/main.cpp#variable#x#1",
        .name = "x",
        .qualified_name = "x",
        .kind = "variable",
        .language = "cpp",
        .enclosing_scope = std::nullopt,
    });

    // Outer::x (id=2)
    resolver.add_symbol(SymbolCandidate{
        .symbol_id = 2,
        .file_id = 10,
        .file_path = "src/main.cpp",
        .symbol_key = "cpp:src/main.cpp#variable#Outer::x#2",
        .name = "x",
        .qualified_name = "Outer::x",
        .kind = "variable",
        .language = "cpp",
        .enclosing_scope = "Outer",
    });

    // Enclosing function Outer::Inner::compute (id=3)
    resolver.add_symbol(SymbolCandidate{
        .symbol_id = 3,
        .file_id = 10,
        .file_path = "src/main.cpp",
        .symbol_key = "cpp:src/main.cpp#function#Outer::Inner::compute",
        .name = "compute",
        .qualified_name = "Outer::Inner::compute",
        .kind = "function",
        .language = "cpp",
        .enclosing_scope = "Outer::Inner",
        .range = {.start_byte = 50, .end_byte = 150},
    });

    SECTION("Innermost enclosing scope Outer shadows global x") {
        Occurrence occ{
            .id = 101,
            .file_id = 10,
            .name = "x",
            .range = {.start_byte = 80, .end_byte = 81},
        };

        auto res = resolver.resolve_occurrence(occ, "src/main.cpp", Language::cpp);
        REQUIRE(res.resolution == Resolution::resolved);
        REQUIRE(res.confidence == 1.0);
        REQUIRE(res.enclosing_symbol_id == 3);
        REQUIRE(res.candidates.size() == 1);
        REQUIRE(res.candidates[0].target_symbol_id == 2); // Outer::x, not global x
        REQUIRE(res.candidates[0].reason == "lexical_scope");
    }

    SECTION("Explicit qualification resolves directly") {
        Occurrence occ{
            .id = 102,
            .file_id = 10,
            .name = "Outer::x",
            .range = {.start_byte = 85, .end_byte = 93},
        };

        auto res = resolver.resolve_occurrence(occ, "src/main.cpp", Language::cpp);
        REQUIRE(res.resolution == Resolution::resolved);
        REQUIRE(res.candidates.size() == 1);
        REQUIRE(res.candidates[0].target_symbol_id == 2);
        REQUIRE(res.candidates[0].reason == "explicit_qualification");
    }

    SECTION("Missing symbol returns unresolved and never selects unrelated symbol") {
        Occurrence occ{
            .id = 103,
            .file_id = 10,
            .name = "non_existent_symbol",
            .range = {.start_byte = 95, .end_byte = 114},
        };

        auto res = resolver.resolve_occurrence(occ, "src/main.cpp", Language::cpp);
        REQUIRE(res.resolution == Resolution::unresolved);
        REQUIRE(res.confidence == 0.0);
        REQUIRE(res.candidates.empty());
    }
}

// =============================================================================
// Task E-04: Dependency Path Resolution
// =============================================================================

TEST_CASE("Dependency path resolution across languages (E-04)", "[resolver][dependency]") {
    DependencyResolver dep_resolver;
    dep_resolver.set_workspace_root("/workspace");

    SECTION("C/C++ quote, system, and external include resolution") {
        dep_resolver.register_file(1, "src/main.cpp", Language::cpp);
        dep_resolver.register_file(2, "src/local_header.h", Language::c);
        dep_resolver.register_file(3, "include/shared.hpp", Language::cpp);

        dep_resolver.set_include_directories("src/main.cpp", {}, {"include"});

        // 1. Quote include in same directory
        auto target1 =
            dep_resolver.resolve_dependency(Language::cpp, "src/main.cpp", "\"local_header.h\"");
        REQUIRE(target1.target_file_path.has_value());
        REQUIRE(*target1.target_file_path == "src/local_header.h");
        REQUIRE(target1.reason == "include");

        // 2. System include from include directory
        auto target2 =
            dep_resolver.resolve_dependency(Language::cpp, "src/main.cpp", "<shared.hpp>");
        REQUIRE(target2.target_file_path.has_value());
        REQUIRE(*target2.target_file_path == "include/shared.hpp");

        // 3. External system header (<iostream>)
        auto target3 = dep_resolver.resolve_dependency(Language::cpp, "src/main.cpp", "<iostream>");
        REQUIRE_FALSE(target3.target_file_path.has_value());
        REQUIRE(target3.reason == "external_or_missing");

        // 4. Empty and all-whitespace raw_name does not crash or cause undefined behavior
        auto target_empty = dep_resolver.resolve_dependency(Language::cpp, "src/main.cpp", "");
        REQUIRE_FALSE(target_empty.target_file_path.has_value());
        REQUIRE(target_empty.reason == "unresolved");

        auto target_ws = dep_resolver.resolve_dependency(Language::cpp, "src/main.cpp", "   ");
        REQUIRE_FALSE(target_ws.target_file_path.has_value());
        REQUIRE(target_ws.reason == "unresolved");
    }

    SECTION("Python relative and absolute imports") {
        dep_resolver.register_file(1, "pkg/app.py", Language::python);
        dep_resolver.register_file(2, "pkg/utils.py", Language::python);
        dep_resolver.register_file(3, "top_module.py", Language::python);

        // 1. Relative sibling import
        auto target1 = dep_resolver.resolve_dependency(Language::python, "pkg/app.py", ".utils");
        REQUIRE(target1.target_file_path.has_value());
        REQUIRE(*target1.target_file_path == "pkg/utils.py");

        // 2. Absolute top-level import
        auto target2 =
            dep_resolver.resolve_dependency(Language::python, "pkg/app.py", "top_module");
        REQUIRE(target2.target_file_path.has_value());
        REQUIRE(*target2.target_file_path == "top_module.py");

        // 3. External package (os, sys)
        auto target3 = dep_resolver.resolve_dependency(Language::python, "pkg/app.py", "os");
        REQUIRE_FALSE(target3.target_file_path.has_value());
        REQUIRE(target3.reason == "external_or_missing");
    }

    SECTION("TypeScript relative imports and tsconfig path aliases") {
        dep_resolver.register_file(1, "src/index.ts", Language::typescript);
        dep_resolver.register_file(2, "src/components/Button.tsx", Language::typescript);
        dep_resolver.register_file(3, "src/lib/math.ts", Language::typescript);

        dep_resolver.set_tsconfig_paths("", {{"@components/*", {"src/components/*"}}});

        // 1. Relative import without extension
        auto target1 =
            dep_resolver.resolve_dependency(Language::typescript, "src/index.ts", "./lib/math");
        REQUIRE(target1.target_file_path.has_value());
        REQUIRE(*target1.target_file_path == "src/lib/math.ts");

        // 2. Tsconfig path alias
        auto target2 = dep_resolver.resolve_dependency(Language::typescript, "src/index.ts",
                                                       "@components/Button");
        REQUIRE(target2.target_file_path.has_value());
        REQUIRE(*target2.target_file_path == "src/components/Button.tsx");

        // 3. External npm package
        auto target3 =
            dep_resolver.resolve_dependency(Language::typescript, "src/index.ts", "react");
        REQUIRE_FALSE(target3.target_file_path.has_value());
        REQUIRE(target3.reason == "external_or_missing");
    }

    SECTION("Go module imports") {
        dep_resolver.set_go_module("github.com/org/repo");
        dep_resolver.register_file(1, "cmd/main.go", Language::go);
        dep_resolver.register_file(2, "pkg/server/server.go", Language::go);

        // 1. Internal module package
        auto target1 = dep_resolver.resolve_dependency(Language::go, "cmd/main.go",
                                                       "github.com/org/repo/pkg/server");
        REQUIRE(target1.target_file_path.has_value());
        REQUIRE(*target1.target_file_path == "pkg/server");

        // 2. Standard library (fmt)
        auto target2 = dep_resolver.resolve_dependency(Language::go, "cmd/main.go", "fmt");
        REQUIRE_FALSE(target2.target_file_path.has_value());
        REQUIRE(target2.reason == "external_or_missing");
    }

    SECTION("Java package and wildcard imports") {
        dep_resolver.register_file(1, "src/com/example/Main.java", Language::java);
        dep_resolver.register_file(2, "src/com/example/model/User.java", Language::java);

        // 1. Class import
        auto target1 = dep_resolver.resolve_dependency(Language::java, "src/com/example/Main.java",
                                                       "com.example.model.User");
        REQUIRE(target1.target_file_path.has_value());
        REQUIRE(*target1.target_file_path == "src/com/example/model/User.java");

        // 2. Wildcard import
        auto target2 = dep_resolver.resolve_dependency(Language::java, "src/com/example/Main.java",
                                                       "com.example.model.*");
        REQUIRE(target2.target_file_path.has_value());
        REQUIRE(*target2.target_file_path == "src/com/example/model");

        // 3. External JDK
        auto target3 = dep_resolver.resolve_dependency(Language::java, "src/com/example/Main.java",
                                                       "java.util.List");
        REQUIRE_FALSE(target3.target_file_path.has_value());
        REQUIRE(target3.reason == "external_or_missing");
    }

    SECTION("Shell literal source vs dynamic expansion") {
        dep_resolver.register_file(1, "bin/run.sh", Language::shell);
        dep_resolver.register_file(2, "bin/common.sh", Language::shell);

        // 1. Literal source
        auto target1 =
            dep_resolver.resolve_dependency(Language::shell, "bin/run.sh", "./common.sh");
        REQUIRE(target1.target_file_path.has_value());
        REQUIRE(*target1.target_file_path == "bin/common.sh");

        // 2. Dynamic variable expansion ($DIR/common.sh) -> must NOT fabricate target!
        auto target2 =
            dep_resolver.resolve_dependency(Language::shell, "bin/run.sh", "$DIR/common.sh");
        REQUIRE_FALSE(target2.target_file_path.has_value());
        REQUIRE(target2.reason == "dynamic_expansion");
    }
}

// =============================================================================
// Task E-05 & E-06: Cross-File Resolution, Confidence & Ambiguity Handling
// =============================================================================

TEST_CASE("Cross-file name resolution baseline, confidence and ambiguity (E-05, E-06)",
          "[resolver][cross_file]") {
    SymbolResolver resolver;

    // File 1 (math.hpp): int add(int, int)
    resolver.add_symbol(SymbolCandidate{
        .symbol_id = 1,
        .file_id = 1,
        .file_path = "include/math.hpp",
        .name = "add",
        .qualified_name = "math::add",
        .kind = "function",
        .language = "cpp",
        .signature = "int add(int a, int b)",
    });

    // File 2 (math.cpp): double add(double, double) overload
    resolver.add_symbol(SymbolCandidate{
        .symbol_id = 2,
        .file_id = 2,
        .file_path = "src/math.cpp",
        .name = "add",
        .qualified_name = "math::add",
        .kind = "function",
        .language = "cpp",
        .signature = "double add(double a, double b)",
    });

    // File 3 (app.cpp): references "add"
    SECTION("Overload ambiguity preserves all candidates and sets confidence 0.5") {
        Occurrence occ{
            .id = 101,
            .file_id = 3,
            .name = "add",
            .range = {.start_byte = 10, .end_byte = 13},
        };

        auto res = resolver.resolve_occurrence(occ, "src/app.cpp", Language::cpp, {1, 2});
        REQUIRE(res.resolution == Resolution::ambiguous);
        REQUIRE(res.confidence == 0.5);
        REQUIRE(res.candidates.size() == 2);
        for (const auto& cand : res.candidates) {
            REQUIRE(cand.reason == "overload");
        }
    }

    SECTION("Explicit import priority over unrelated files") {
        // Add another unrelated 'add' function in another package
        resolver.add_symbol(SymbolCandidate{
            .symbol_id = 3,
            .file_id = 4,
            .file_path = "src/other.cpp",
            .name = "compute",
            .qualified_name = "other::compute",
            .kind = "function",
            .language = "cpp",
        });

        Occurrence occ{
            .id = 102,
            .file_id = 3,
            .name = "compute",
            .range = {.start_byte = 20, .end_byte = 27},
        };

        // When file 4 is imported, confidence is 0.9
        auto res_imported = resolver.resolve_occurrence(occ, "src/app.cpp", Language::cpp, {4});
        REQUIRE(res_imported.resolution == Resolution::resolved);
        REQUIRE(res_imported.confidence == 0.9);
        REQUIRE(res_imported.candidates[0].target_symbol_id == 3);

        // When no import, falls back to workspace global with confidence 0.8
        auto res_fallback = resolver.resolve_occurrence(occ, "src/app.cpp", Language::cpp, {});
        REQUIRE(res_fallback.resolution == Resolution::resolved);
        REQUIRE(res_fallback.confidence == 0.8);
        REQUIRE(res_fallback.candidates[0].target_symbol_id == 3);
    }
}

// =============================================================================
// Task E-07 & E-08: Relations (contains, calls, inherits, implements, overrides)
// =============================================================================

TEST_CASE("Building symbol relations (E-07, E-08)", "[resolver][relations]") {
    SymbolResolver resolver;
    RelationshipBuilder builder;

    // Class Base
    Symbol base_class{
        .id = 1,
        .workspace_id = 1,
        .file_id = 1,
        .name = "Base",
        .kind = "class",
        .range = {.start_byte = 0, .end_byte = 100},
    };
    resolver.add_symbol(base_class, "src/base.hpp");

    // Base method: virtual void execute()
    Symbol base_method{
        .id = 2,
        .workspace_id = 1,
        .file_id = 1,
        .name = "execute",
        .kind = "method",
        .container_name = "Base",
        .scope_symbol_id = 1,
        .range = {.start_byte = 20, .end_byte = 50},
    };
    resolver.add_symbol(base_method, "src/base.hpp");

    // Derived class
    Symbol derived_class{
        .id = 3,
        .workspace_id = 1,
        .file_id = 2,
        .name = "Derived",
        .kind = "class",
        .range = {.start_byte = 0, .end_byte = 120},
    };
    resolver.add_symbol(derived_class, "src/derived.hpp");

    // Derived method: void execute() override
    Symbol derived_method{
        .id = 4,
        .workspace_id = 1,
        .file_id = 2,
        .name = "execute",
        .kind = "method",
        .container_name = "Derived",
        .scope_symbol_id = 3,
        .range = {.start_byte = 30, .end_byte = 60},
    };
    resolver.add_symbol(derived_method, "src/derived.hpp");

    // Caller function: void main()
    Symbol main_func{
        .id = 5,
        .workspace_id = 1,
        .file_id = 3,
        .name = "main",
        .kind = "function",
        .range = {.start_byte = 0, .end_byte = 50},
    };
    resolver.add_symbol(main_func, "src/main.cpp");

    std::vector<Symbol> all_symbols = {base_class, base_method, derived_class, derived_method,
                                       main_func};

    // Derived inherits Base occurrence
    Occurrence inherit_occ{
        .id = 10,
        .workspace_id = 1,
        .file_id = 2,
        .symbol_id = 1, // Base
        .occurrence_kind = "inherits",
        .name = "Base",
        .range = {.start_byte = 10, .end_byte = 14}, // inside Derived class range (0..120)
        .confidence = 1.0,
        .resolution = "resolved",
    };

    // Call from main to derived_method.execute
    ReferenceOccurrence call_ref{
        .id = 20,
        .workspace_id = 1,
        .source_file_id = 3,
        .source_symbol_id = 5, // main
        .target_symbol_id = 4, // derived_method
        .name = "execute",
        .reference_kind = "call",
        .range = {.start_byte = 15, .end_byte = 22},
        .resolution = "resolved",
        .confidence = 1.0,
    };

    auto relations =
        builder.build_relations(1, all_symbols, {inherit_occ}, {call_ref}, {}, resolver);

    bool has_containment = false;
    bool has_calls = false;
    bool has_inherits = false;
    bool has_overrides = false;

    for (const auto& rel : relations) {
        if (rel.relation_kind == "contains" && rel.source_symbol_id == 1 &&
            rel.target_symbol_id == 2) {
            has_containment = true;
        }
        if (rel.relation_kind == "calls" && rel.source_symbol_id == 5 &&
            rel.target_symbol_id == 4) {
            has_calls = true;
        }
        if (rel.relation_kind == "inherits" && rel.source_symbol_id == 3 &&
            rel.target_symbol_id == 1) {
            has_inherits = true;
        }
        if (rel.relation_kind == "overrides" && rel.source_symbol_id == 4 &&
            rel.target_symbol_id == 2) {
            has_overrides = true;
        }
    }

    REQUIRE(has_containment);
    REQUIRE(has_calls);
    REQUIRE(has_inherits);
    REQUIRE(has_overrides);
}

// =============================================================================
// Task E-09: Optional C/C++ Compile-Command Configuration Model
// =============================================================================

TEST_CASE("Compilation command configuration model and safe tokenization (E-09)",
          "[resolver][compile_commands]") {
    SECTION("Safe command tokenization handles quotes and escapes without shell execution") {
        std::string cmd =
            R"cmd(clang++ -I"my include/dir" -isystem 'sys/inc' -DFOO=\"bar\" -c src/main.cpp -o main.o)cmd";
        auto tokens_res = CompilationDatabase::tokenize_command_safely(cmd);
        REQUIRE(tokens_res.has_value());
        const auto& tokens = *tokens_res;
        REQUIRE(tokens.size() == 9);
        REQUIRE(tokens[0] == "clang++");
        REQUIRE(tokens[1] == "-Imy include/dir");
        REQUIRE(tokens[2] == "-isystem");
        REQUIRE(tokens[3] == "sys/inc");
        REQUIRE(tokens[4] == "-DFOO=\"bar\"");
        REQUIRE(tokens[5] == "-c");
        REQUIRE(tokens[6] == "src/main.cpp");
        REQUIRE(tokens[7] == "-o");
        REQUIRE(tokens[8] == "main.o");
    }

    SECTION("Parsing compilation database JSON") {
        std::string json = R"json([
            {
                "directory": "build",
                "file": "../src/main.cpp",
                "arguments": [
                    "/usr/bin/clang++",
                    "-std=c++20",
                    "-I../include",
                    "-isystem", "../sys_inc",
                    "-DFOO=1",
                    "-UOLD",
                    "-o", "main.o",
                    "-c", "../src/main.cpp"
                ]
            }
        ])json";

        auto cdb_res = CompilationDatabase::parse_json(json, "/workspace");
        REQUIRE(cdb_res.has_value());
        const auto& cdb = *cdb_res;
        REQUIRE(cdb.size() == 1);

        const auto* entry = cdb.find_for_file("src/main.cpp");
        REQUIRE(entry != nullptr);
        REQUIRE(entry->language_standard.has_value());
        REQUIRE(entry->language_standard.value() == "c++20");
        REQUIRE(entry->defines.size() == 2);
        REQUIRE(entry->defines[0] == "FOO=1");
        REQUIRE(entry->defines[1] == "-UOLD");
        REQUIRE(entry->include_dirs.size() >= 2);
    }

    SECTION("Parsing standalone command string into CompileCommand") {
        std::string cmd = "clang -std=c17 -Iinclude -isystem /usr/include -DFEATURE_X=1 -UDEBUG -o bin/app src/main.c";
        auto parsed = CompilationDatabase::parse_command_string(cmd, "/workspace", "src/main.c", "/workspace");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->language_standard.has_value());
        REQUIRE(parsed->language_standard.value() == "c17");
        REQUIRE(parsed->defines.size() == 2);
        REQUIRE(parsed->defines[0] == "FEATURE_X=1");
        REQUIRE(parsed->defines[1] == "-UDEBUG");
        REQUIRE(parsed->include_dirs.size() == 2);
        REQUIRE(parsed->include_dirs[0] == "include");
        REQUIRE(parsed->output.has_value());
        REQUIRE(parsed->output.value() == "bin/app");
    }
}

// =============================================================================
// Task E-10 & E-11: Integration Tests, Queries, and Resolved/Unresolved Labels
// =============================================================================

TEST_CASE("Resolver integration, navigation queries, and label completeness (E-10, E-11)",
          "[resolver][integration]") {
    TestWorkspaceEnv env;

    // Header defining interface and function
    env.write_file("include/service.h", R"C(
int process_data(int val);
)C");

    // Source defining function and caller
    env.write_file("src/service.c", R"C(
#include "service.h"
#include <stdio.h>

int process_data(int val) {
    return val * 2;
}

int run_task(int x) {
    return process_data(x);
}
)C");

    int64_t ws_id = env.create_workspace("Full Pipeline WS");
    REQUIRE(ws_id > 0);

    IndexingPipeline pipeline(*env.db);
    auto res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res.has_value());
    REQUIRE(res->status == "completed");
    REQUIRE(res->files_processed == 2);

    // 1. Verify file dependencies are resolved
    auto files = env.db->files().list_by_workspace(ws_id, false);
    REQUIRE(files.size() == 2);

    auto deps = env.db->dependencies().list_by_workspace(ws_id);
    bool has_resolved_dep = false;
    bool has_external_dep = false;
    for (const auto& d : deps) {
        if (d.raw_name.find("service.h") != std::string::npos) {
            REQUIRE(d.resolution == "resolved");
            REQUIRE(d.target_file_id.has_value());
            has_resolved_dep = true;
        }
        if (d.raw_name.find("stdio.h") != std::string::npos) {
            REQUIRE(d.resolution == "external");
            has_external_dep = true;
        }
    }
    REQUIRE(has_resolved_dep);
    REQUIRE(has_external_dep);

    // 2. Verify symbol queries
    auto symbols = env.db->symbols().list_by_workspace(ws_id);
    int64_t process_data_id = 0;
    int64_t run_task_id = 0;
    for (const auto& s : symbols) {
        if (s.name == "process_data") {
            process_data_id = s.id;
        }
        if (s.name == "run_task") {
            run_task_id = s.id;
        }
    }
    REQUIRE(process_data_id > 0);
    REQUIRE(run_task_id > 0);

    // 3. Navigation queries: referencers
    auto referencers = env.db->references().find_referencers(ws_id, process_data_id);
    REQUIRE_FALSE(referencers.empty());
    REQUIRE(referencers[0].name == "process_data");
    REQUIRE(referencers[0].resolution == "resolved");
    REQUIRE(referencers[0].confidence >= 0.8);
    REQUIRE(referencers[0].containing_symbol_id == run_task_id);

    // 4. Navigation queries: callers and callees
    auto callers = env.db->references().find_callers(process_data_id);
    REQUIRE(callers.size() == 1);
    REQUIRE(callers[0].symbol_id == run_task_id);
    REQUIRE(callers[0].name == "run_task");

    auto callees = env.db->references().find_callees(run_task_id);
    REQUIRE(callees.size() == 1);
    REQUIRE(callees[0].symbol_id == process_data_id);
    REQUIRE(callees[0].name == "process_data");

    // 5. Relations: calls relation exists
    auto relations = env.db->relations().find_by_source_symbol(run_task_id, "calls");
    REQUIRE(relations.size() == 1);
    REQUIRE(relations[0].target_symbol_id == process_data_id);
    REQUIRE(relations[0].resolution == "resolved");

    // 6. Completeness: unresolved references are preserved and never hidden
    auto occurrences = env.db->occurrences().list_by_workspace(ws_id);
    for (const auto& occ : occurrences) {
        REQUIRE_FALSE(occ.resolution.empty());
        REQUIRE((occ.resolution == "resolved" || occ.resolution == "unresolved" ||
                 occ.resolution == "ambiguous" || occ.resolution == "external"));
    }
}
