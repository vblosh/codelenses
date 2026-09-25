#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/filesystem/file_capture.hpp"
#include "codelenses/filesystem/path.hpp"

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::filesystem;

namespace {

struct TempWorkspace {
    fs::path root;
    fs::path outside;

    TempWorkspace() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::temp_directory_path() / ("codelenses_test_disc_" + pid + "_" + now);
        outside = fs::temp_directory_path() / ("codelenses_test_outside_" + pid + "_" + now);
        fs::remove_all(root);
        fs::remove_all(outside);
        fs::create_directories(root);
        fs::create_directories(outside);
    }

    ~TempWorkspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove_all(outside, ec);
    }

    void write_file(const fs::path& rel_path, const std::string& content) {
        auto full = root / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
    }
};

} // namespace

TEST_CASE("Workspace path canonicalization and containment checks (D-01)", "[filesystem][path]") {
    TempWorkspace ws;
    ws.write_file("src/main.cpp", "int main() {}");

    SECTION("Canonicalize valid workspace root") {
        auto res = canonicalize_workspace_root(ws.root);
        REQUIRE(res.has_value());
        REQUIRE(*res == fs::canonical(ws.root));
    }

    SECTION("Reject non-existent workspace root") {
        auto bad_path = ws.root / "does_not_exist";
        auto res = canonicalize_workspace_root(bad_path);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::not_found);
    }

    SECTION("Reject file as workspace root") {
        auto file_path = ws.root / "src/main.cpp";
        auto res = canonicalize_workspace_root(file_path);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::invalid_argument);
    }

    SECTION("Containment check prevents directory traversal") {
        auto canonical_root = *canonicalize_workspace_root(ws.root);
        REQUIRE(is_contained_in(canonical_root, canonical_root / "src/main.cpp"));
        REQUIRE(is_contained_in(canonical_root, canonical_root));

        // Outside path
        REQUIRE_FALSE(is_contained_in(canonical_root, ws.outside));
        REQUIRE_FALSE(is_contained_in(canonical_root, canonical_root.parent_path()));

        // Resolve workspace path safely
        auto valid_res = resolve_workspace_path(canonical_root, "src/main.cpp");
        REQUIRE(valid_res.has_value());

        // Relative traversal escaping root is rejected
        auto traversal_res = resolve_workspace_path(canonical_root, "../../etc/passwd");
        REQUIRE_FALSE(traversal_res.has_value());
        REQUIRE(traversal_res.error().code == ErrorCode::invalid_argument);
    }

    SECTION("Symlink escaping workspace root is rejected") {
        auto canonical_root = *canonicalize_workspace_root(ws.root);
        auto secret_file = ws.outside / "secret.txt";
        {
            std::ofstream out(secret_file);
            out << "secret";
        }

        fs::create_symlink(secret_file, ws.root / "src/symlink_out.txt");

        auto res = resolve_workspace_path(canonical_root, "src/symlink_out.txt");
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::invalid_argument);
    }

    SECTION("Workspace-relative path formatting") {
        auto canonical_root = *canonicalize_workspace_root(ws.root);
        auto rel = to_workspace_relative(canonical_root, canonical_root / "src/main.cpp");
        REQUIRE(rel == "src/main.cpp");
    }
}

TEST_CASE("Recursive file discovery with include/exclude and default ignores (D-02, D-03)",
          "[filesystem][discovery]") {
    TempWorkspace ws;
    ws.write_file("src/main.cpp", "int main() {}");
    ws.write_file("src/app.py", "print('hello')");
    ws.write_file("src/client.ts", "const x = 1;");
    ws.write_file("include/header.h", "int val;");
    ws.write_file("build/artifact.o", "binary");
    ws.write_file("node_modules/pkg/index.js", "console.log();");
    ws.write_file(".git/config", "repo config");
    ws.write_file("dist/bundle.js", "bundled");
    ws.write_file("docs/readme.txt", "readme text");

    SECTION("Default ignores skip build, node_modules, .git, dist") {
        FileDiscovery discovery;
        auto res = discovery.discover(ws.root);
        REQUIRE(res.has_value());
        const auto& files = *res;

        auto has_path = [&](const std::string& path) {
            for (const auto& f : files) {
                if (f.relative_path == path)
                    return true;
            }
            return false;
        };

        REQUIRE(has_path("src/main.cpp"));
        REQUIRE(has_path("src/app.py"));
        REQUIRE(has_path("src/client.ts"));
        REQUIRE(has_path("include/header.h"));

        // Default ignores excluded
        REQUIRE_FALSE(has_path("build/artifact.o"));
        REQUIRE_FALSE(has_path("node_modules/pkg/index.js"));
        REQUIRE_FALSE(has_path(".git/config"));
        REQUIRE_FALSE(has_path("dist/bundle.js"));
    }

    SECTION("Include patterns restrict discovery") {
        DiscoveryOptions opts;
        opts.include_patterns = {"src/**"};
        FileDiscovery discovery(opts);
        auto res = discovery.discover(ws.root);
        REQUIRE(res.has_value());
        const auto& files = *res;

        for (const auto& f : files) {
            REQUIRE(f.relative_path.starts_with("src/"));
        }
    }

    SECTION("Exclude patterns filter specific paths") {
        DiscoveryOptions opts;
        opts.exclude_patterns = {"src/*.py"};
        FileDiscovery discovery(opts);
        auto res = discovery.discover(ws.root);
        REQUIRE(res.has_value());
        const auto& files = *res;

        for (const auto& f : files) {
            REQUIRE_FALSE(f.relative_path == "src/app.py");
        }
    }

    SECTION(".gitignore and .codelensignore parsing") {
        ws.write_file("vendor/lib.cpp", "int lib;");
        ws.write_file("logs/app.log", "log");
        ws.write_file("logs/important.log", "important");
        ws.write_file(".gitignore", "*.log\n!logs/important.log\n");
        ws.write_file(".codelensignore", "vendor/\n");

        DiscoveryOptions opts;
        opts.extension_overrides[".log"] = Language::cpp;
        FileDiscovery discovery(opts);
        auto res = discovery.discover(ws.root);
        REQUIRE(res.has_value());
        const auto& files = *res;

        auto has_path = [&](const std::string& path) {
            for (const auto& f : files) {
                if (f.relative_path == path)
                    return true;
            }
            return false;
        };

        REQUIRE_FALSE(has_path("vendor/lib.cpp"));
        REQUIRE_FALSE(has_path("logs/app.log"));
        REQUIRE(has_path("logs/important.log"));
    }

    SECTION("Cyclic symlinks are detected without infinite recursion") {
        fs::create_directory_symlink(ws.root / "src", ws.root / "src/loop");
        FileDiscovery discovery;
        auto res = discovery.discover(ws.root);
        REQUIRE(res.has_value());
    }
}

TEST_CASE("Language and binary detection (D-04)", "[filesystem][detection]") {
    TempWorkspace ws;
    ws.write_file("script_bash", "#!/bin/bash\necho 1");
    ws.write_file("script_env_sh", "#!/usr/bin/env sh\necho 1");
    ws.write_file("script_python", "#!/usr/bin/env python3\nprint(1)");
    ws.write_file("binary.bin", std::string("text\0with\0null\0bytes", 20));
    ws.write_file("utf8.txt", "Hello, 世界! 🌍");
    ws.write_file("include/common.h", "int a;");

    SECTION("Shebang parsing for extensionless files") {
        REQUIRE(FileDiscovery::parse_shebang_line("#!/bin/bash") == Language::bash);
        REQUIRE(FileDiscovery::parse_shebang_line("#!/usr/bin/env bash") == Language::bash);
        REQUIRE(FileDiscovery::parse_shebang_line("#!/bin/sh") == Language::shell);
        REQUIRE(FileDiscovery::parse_shebang_line("#!/usr/bin/env python3") == Language::python);
        REQUIRE(FileDiscovery::parse_shebang_line("#!/usr/bin/env node") == Language::javascript);
    }

    SECTION("Detect shebang on disk") {
        REQUIRE(FileDiscovery::detect_shebang_language(ws.root / "script_bash") == Language::bash);
        REQUIRE(FileDiscovery::detect_shebang_language(ws.root / "script_env_sh") ==
                Language::shell);
        REQUIRE(FileDiscovery::detect_shebang_language(ws.root / "script_python") ==
                Language::python);
    }

    SECTION("Binary detection identifies null bytes") {
        auto capture_bin = capture_file(ws.root / "binary.bin");
        REQUIRE(capture_bin.has_value());
        REQUIRE(capture_bin->is_binary);

        auto capture_txt = capture_file(ws.root / "utf8.txt");
        REQUIRE(capture_txt.has_value());
        REQUIRE_FALSE(capture_txt->is_binary);
        REQUIRE(capture_txt->is_valid_utf8);
    }

    SECTION("Ambiguous header override") {
        DiscoveryOptions default_opts;
        FileDiscovery def_disc(default_opts);
        auto def_res = def_disc.discover(ws.root);
        REQUIRE(def_res.has_value());
        for (const auto& f : *def_res) {
            if (f.relative_path == "include/common.h") {
                REQUIRE(f.language == Language::c);
            }
        }

        DiscoveryOptions cpp_opts;
        cpp_opts.ambiguous_header_mode = Language::cpp;
        FileDiscovery cpp_disc(cpp_opts);
        auto cpp_res = cpp_disc.discover(ws.root);
        REQUIRE(cpp_res.has_value());
        for (const auto& f : *cpp_res) {
            if (f.relative_path == "include/common.h") {
                REQUIRE(f.language == Language::cpp);
            }
        }
    }
}

TEST_CASE("Content hashing and file capture (D-05)", "[filesystem][capture]") {
    TempWorkspace ws;
    std::string test_data = "int add(int a, int b) { return a + b; }\n";
    ws.write_file("src/math.cpp", test_data);

    SECTION("SHA-256 calculation matches standard test vector") {
        // SHA-256 of empty string: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        auto empty_span = std::span<const std::byte>{};
        REQUIRE(sha256_hex(empty_span) ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        // SHA-256 of "abc": ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
        std::string abc = "abc";
        auto abc_span = std::as_bytes(std::span(abc));
        REQUIRE(sha256_hex(abc_span) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    SECTION("Capture file reads content, hash, and metadata accurately") {
        auto res = capture_file(ws.root / "src/math.cpp");
        REQUIRE(res.has_value());
        REQUIRE(res->byte_size == test_data.size());
        REQUIRE(res->as_string_view() == test_data);
        REQUIRE(res->content_hash == sha256_hex(std::as_bytes(std::span(test_data))));
        REQUIRE(res->modified_ns > 0);
        REQUIRE_FALSE(res->is_binary);
    }

    SECTION("Capture file rechecks workspace containment") {
        TempWorkspace outside_ws;
        outside_ws.write_file("secret.txt", "secret content");

        // Try capturing file outside workspace root
        auto res = capture_file(outside_ws.root / "secret.txt", 1024, ws.root);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::invalid_argument);

        // Symlink pointing outside workspace root
        std::error_code ec;
        fs::create_symlink(outside_ws.root / "secret.txt", ws.root / "sym_outside.txt", ec);
        auto sym_res = capture_file(ws.root / "sym_outside.txt", 1024, ws.root);
        REQUIRE_FALSE(sym_res.has_value());
        REQUIRE(sym_res.error().code == ErrorCode::invalid_argument);
    }
}

TEST_CASE("Incomplete directory scan and depth limits are treated as errors", "[filesystem][discovery]") {
    TempWorkspace ws;
    ws.write_file("sub/nested/file.txt", "content");

    SECTION("Discovery depth limit exceeded returns out_of_range error") {
        DiscoveryOptions opts;
        opts.max_discovery_depth = 1; // sub/nested requires depth 2
        FileDiscovery discovery(opts);

        auto res = discovery.discover(ws.root);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::out_of_range);
    }

    SECTION("Unreadable directory returns failure error") {
        ws.write_file("protected/secret.txt", "secret");
        auto prot_dir = ws.root / "protected";

        // Remove read & execute permissions from directory
        std::error_code ec;
        fs::permissions(prot_dir, fs::perms::none, fs::perm_options::replace, ec);

        DiscoveryOptions opts;
        FileDiscovery discovery(opts);
        auto res = discovery.discover(ws.root);

        // Restore permissions for cleanup
        fs::permissions(prot_dir, fs::perms::all, fs::perm_options::replace, ec);

        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::failed);
    }
}
