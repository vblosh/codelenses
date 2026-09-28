#include "codelenses/db/library_repository.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <span>
#include "codelenses/db/connection.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/filesystem/file_capture.hpp"
#include <nlohmann/json.hpp>

namespace codelenses {

std::string compute_library_fingerprint(const LibraryProfile& profile) {
    // Deterministic configuration fingerprint
    std::ostringstream ss;
    ss << profile.language << "|"
       << profile.provider << "|"
       << profile.sdk_version.value_or("") << "|"
       << profile.target_environment.value_or("") << "|"
       << profile.language_standard.value_or("") << "|"
       << profile.target_framework.value_or("") << "|"
       << profile.sysroot.value_or("") << "|";
    for (const auto& sr : profile.source_roots) {
        ss << sr << ",";
    }
    ss << "|";
    for (const auto& dir : profile.default_include_roots) {
        ss << dir << ",";
    }
    ss << "|";
    for (const auto& def : profile.defines) {
        ss << def << ",";
    }
    ss << "|";
    for (const auto& inc : profile.include_patterns) {
        ss << inc << ",";
    }
    ss << "|";
    for (const auto& exc : profile.exclude_patterns) {
        ss << exc << ",";
    }

    std::string s = ss.str();
    return filesystem::sha256_hex(std::as_bytes(std::span(s.data(), s.size())));
}

namespace {

std::vector<std::string> parse_str_array(const std::string& str) {
    if (str.empty()) {
        return {};
    }
    try {
        auto j = nlohmann::json::parse(str);
        if (j.is_array()) {
            return j.get<std::vector<std::string>>();
        }
    } catch (...) {
    }
    return {};
}

std::string serialize_str_array(const std::vector<std::string>& vec) {
    return nlohmann::json(vec).dump();
}

LibraryProfile read_profile_row(Statement& stmt) {
    LibraryProfile p;
    p.id = stmt.column_int64(0);
    p.workspace_id = stmt.column_int64(1);
    p.name = stmt.column_text(2);
    p.language = stmt.column_text(3);
    p.provider = stmt.column_text(4);
    p.sdk_version = stmt.column_optional_text(5);
    p.target_environment = stmt.column_optional_text(6);
    p.language_standard = stmt.column_optional_text(7);
    p.target_framework = stmt.column_optional_text(8);
    p.sysroot = stmt.column_optional_text(9);
    p.source_roots = parse_str_array(stmt.column_text(10));
    p.default_include_roots = parse_str_array(stmt.column_text(11));
    p.defines = parse_str_array(stmt.column_text(12));
    p.include_patterns = parse_str_array(stmt.column_text(13));
    p.exclude_patterns = parse_str_array(stmt.column_text(14));
    p.fingerprint = stmt.column_text(15);
    p.created_at = stmt.column_text(16);
    p.updated_at = stmt.column_text(17);
    return p;
}

constexpr const char* kProfileColumns =
    "id, workspace_id, name, language, provider, sdk_version, target_environment, "
    "language_standard, target_framework, sysroot, source_roots_json, default_include_roots_json, "
    "defines_json, include_json, exclude_json, fingerprint, created_at, updated_at";

} // namespace

LibraryRepository::LibraryRepository(Connection& conn) : conn_(conn) {}

int64_t LibraryRepository::create_profile(const LibraryProfile& profile) {
    Statement stmt(conn_.handle(), R"SQL(
        INSERT INTO library_profile (
            workspace_id, name, language, provider, sdk_version, target_environment,
            language_standard, target_framework, sysroot, source_roots_json, default_include_roots_json,
            defines_json, include_json, exclude_json, fingerprint
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL");
    stmt.bind_int64(1, profile.workspace_id);
    stmt.bind_text(2, profile.name);
    stmt.bind_text(3, profile.language);
    stmt.bind_text(4, profile.provider);
    stmt.bind_optional_text(5, profile.sdk_version);
    stmt.bind_optional_text(6, profile.target_environment);
    stmt.bind_optional_text(7, profile.language_standard);
    stmt.bind_optional_text(8, profile.target_framework);
    stmt.bind_optional_text(9, profile.sysroot);
    stmt.bind_text(10, serialize_str_array(profile.source_roots));
    stmt.bind_text(11, serialize_str_array(profile.default_include_roots));
    stmt.bind_text(12, serialize_str_array(profile.defines));
    stmt.bind_text(13, serialize_str_array(profile.include_patterns));
    stmt.bind_text(14, serialize_str_array(profile.exclude_patterns));
    std::string fp = profile.fingerprint.empty() ? compute_library_fingerprint(profile) : profile.fingerprint;
    stmt.bind_text(15, fp);
    stmt.execute();
    const auto profile_id = conn_.last_insert_rowid();
    Statement root_stmt(conn_.handle(), R"SQL(
        INSERT INTO library_source_root (profile_id, ordinal, root_path)
        VALUES (?, ?, ?);
    )SQL");
    for (std::size_t i = 0; i < profile.source_roots.size(); ++i) {
        root_stmt.reset();
        root_stmt.clear_bindings();
        root_stmt.bind_int64(1, profile_id);
        root_stmt.bind_int64(2, static_cast<int64_t>(i));
        root_stmt.bind_text(3, profile.source_roots[i]);
        root_stmt.execute();
    }
    return profile_id;
}

std::vector<LibrarySourceRoot> LibraryRepository::list_source_roots(int64_t profile_id) {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT id, profile_id, ordinal, root_path
        FROM library_source_root WHERE profile_id = ? ORDER BY ordinal, id;
    )SQL");
    stmt.bind_int64(1, profile_id);
    std::vector<LibrarySourceRoot> roots;
    while (stmt.step()) {
        roots.push_back(LibrarySourceRoot{
            .id = stmt.column_int64(0),
            .profile_id = stmt.column_int64(1),
            .ordinal = stmt.column_int64(2),
            .path = stmt.column_text(3),
        });
    }
    return roots;
}

std::optional<LibraryProfile> LibraryRepository::get_profile(int64_t id) {
    Statement stmt(conn_.handle(), std::string("SELECT ") + kProfileColumns +
                                       " FROM library_profile WHERE id = ?;");
    stmt.bind_int64(1, id);
    if (stmt.step()) {
        return read_profile_row(stmt);
    }
    return std::nullopt;
}

std::optional<LibraryProfile>
LibraryRepository::get_profile_by_workspace(int64_t workspace_id) {
    Statement stmt(conn_.handle(), std::string("SELECT ") + kProfileColumns +
                                       " FROM library_profile WHERE workspace_id = ?;");
    stmt.bind_int64(1, workspace_id);
    if (stmt.step()) {
        return read_profile_row(stmt);
    }
    return std::nullopt;
}

std::vector<LibraryProfile> LibraryRepository::list_profiles() {
    Statement stmt(conn_.handle(), std::string("SELECT ") + kProfileColumns +
                                       " FROM library_profile ORDER BY id ASC;");
    std::vector<LibraryProfile> results;
    while (stmt.step()) {
        results.push_back(read_profile_row(stmt));
    }
    return results;
}

bool LibraryRepository::update_profile(const LibraryProfile& profile) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE library_profile
        SET name = ?,
            language = ?,
            provider = ?,
            sdk_version = ?,
            target_environment = ?,
            language_standard = ?,
            target_framework = ?,
            sysroot = ?,
            source_roots_json = ?,
            default_include_roots_json = ?,
            defines_json = ?,
            include_json = ?,
            exclude_json = ?,
            fingerprint = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");
    stmt.bind_text(1, profile.name);
    stmt.bind_text(2, profile.language);
    stmt.bind_text(3, profile.provider);
    stmt.bind_optional_text(4, profile.sdk_version);
    stmt.bind_optional_text(5, profile.target_environment);
    stmt.bind_optional_text(6, profile.language_standard);
    stmt.bind_optional_text(7, profile.target_framework);
    stmt.bind_optional_text(8, profile.sysroot);
    stmt.bind_text(9, serialize_str_array(profile.source_roots));
    stmt.bind_text(10, serialize_str_array(profile.default_include_roots));
    stmt.bind_text(11, serialize_str_array(profile.defines));
    stmt.bind_text(12, serialize_str_array(profile.include_patterns));
    stmt.bind_text(13, serialize_str_array(profile.exclude_patterns));
    std::string fp = profile.fingerprint.empty() ? compute_library_fingerprint(profile) : profile.fingerprint;
    stmt.bind_text(14, fp);
    stmt.bind_int64(15, profile.id);
    stmt.execute();
    const bool profile_updated = conn_.changes() > 0;
    auto old_roots = list_source_roots(profile.id);
    Statement shift_roots(conn_.handle(),
                          "UPDATE library_source_root SET ordinal = ordinal + 1000000000 "
                          "WHERE profile_id = ?;");
    shift_roots.bind_int64(1, profile.id);
    shift_roots.execute();
    Statement root_stmt(conn_.handle(), R"SQL(
        INSERT INTO library_source_root (profile_id, ordinal, root_path)
        VALUES (?, ?, ?)
        ON CONFLICT(profile_id, root_path) DO UPDATE SET ordinal = excluded.ordinal;
    )SQL");
    for (std::size_t i = 0; i < profile.source_roots.size(); ++i) {
        root_stmt.reset();
        root_stmt.clear_bindings();
        root_stmt.bind_int64(1, profile.id);
        root_stmt.bind_int64(2, static_cast<int64_t>(i));
        root_stmt.bind_text(3, profile.source_roots[i]);
        root_stmt.execute();
    }
    for (const auto& old_root : old_roots) {
        if (std::find(profile.source_roots.begin(), profile.source_roots.end(), old_root.path) ==
            profile.source_roots.end()) {
            Statement delete_root(conn_.handle(),
                                  "DELETE FROM library_source_root WHERE id = ?;");
            delete_root.bind_int64(1, old_root.id);
            delete_root.execute();
        }
    }
    return profile_updated;
}

bool LibraryRepository::delete_profile(int64_t id) {
    Statement stmt(conn_.handle(), "DELETE FROM library_profile WHERE id = ?;");
    stmt.bind_int64(1, id);
    stmt.execute();
    return conn_.changes() > 0;
}

bool LibraryRepository::attach(int64_t workspace_id, int64_t profile_id) {
    Statement stmt(conn_.handle(), R"SQL(
        INSERT OR IGNORE INTO workspace_library (workspace_id, profile_id)
        VALUES (?, ?);
    )SQL");
    stmt.bind_int64(1, workspace_id);
    stmt.bind_int64(2, profile_id);
    stmt.execute();
    return true;
}

bool LibraryRepository::detach(int64_t workspace_id, int64_t profile_id) {
    Statement stmt(
        conn_.handle(),
        "DELETE FROM workspace_library WHERE workspace_id = ? AND profile_id = ?;");
    stmt.bind_int64(1, workspace_id);
    stmt.bind_int64(2, profile_id);
    stmt.execute();
    return conn_.changes() > 0;
}

bool LibraryRepository::is_attached(int64_t workspace_id, int64_t profile_id) {
    Statement stmt(conn_.handle(),
                   "SELECT 1 FROM workspace_library WHERE workspace_id = ? AND profile_id = ?;");
    stmt.bind_int64(1, workspace_id);
    stmt.bind_int64(2, profile_id);
    return stmt.step();
}

std::vector<LibraryProfile> LibraryRepository::list_attached(int64_t workspace_id) {
    Statement stmt(conn_.handle(), std::string(R"SQL(
        SELECT p.id, p.workspace_id, p.name, p.language, p.provider,
               p.sdk_version, p.target_environment, p.language_standard, p.target_framework, p.sysroot,
               p.source_roots_json, p.default_include_roots_json, p.defines_json,
               p.include_json, p.exclude_json, p.fingerprint,
               p.created_at, p.updated_at
        FROM workspace_library wl
        JOIN library_profile p ON p.id = wl.profile_id
        WHERE wl.workspace_id = ?
        ORDER BY wl.attached_at ASC, p.id ASC;
    )SQL"));
    stmt.bind_int64(1, workspace_id);
    std::vector<LibraryProfile> results;
    while (stmt.step()) {
        results.push_back(read_profile_row(stmt));
    }
    return results;
}

std::vector<int64_t> LibraryRepository::list_consumers(int64_t profile_id) {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT workspace_id FROM workspace_library
        WHERE profile_id = ?
        ORDER BY attached_at ASC, workspace_id ASC;
    )SQL");
    stmt.bind_int64(1, profile_id);
    std::vector<int64_t> results;
    while (stmt.step()) {
        results.push_back(stmt.column_int64(0));
    }
    return results;
}

} // namespace codelenses
