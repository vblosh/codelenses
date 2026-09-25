#include "codelenses/db/workspace_repository.hpp"

#include "codelenses/db/connection.hpp"
#include "codelenses/db/statement.hpp"
#include <nlohmann/json.hpp>

namespace codelenses {

namespace {

std::vector<std::string> parse_json_string_array(const std::string& str) {
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

std::string serialize_json_string_array(const std::vector<std::string>& vec) {
    return nlohmann::json(vec).dump();
}

Workspace read_workspace_row(Statement& stmt) {
    Workspace ws;
    ws.id = stmt.column_int64(0);
    ws.root_path = stmt.column_text(1);
    ws.name = stmt.column_text(2);
    ws.include_patterns = parse_json_string_array(stmt.column_text(3));
    ws.exclude_patterns = parse_json_string_array(stmt.column_text(4));
    ws.default_ignores = parse_json_string_array(stmt.column_text(5));
    ws.compile_commands_path = stmt.column_optional_text(6);
    ws.created_at = stmt.column_text(7);
    ws.updated_at = stmt.column_text(8);
    ws.revision = stmt.column_int64(9);
    ws.status = workspace_status_from_string(stmt.column_text(10));
    ws.last_error = stmt.column_optional_text(11);
    return ws;
}

} // namespace

WorkspaceRepository::WorkspaceRepository(Connection& conn) : conn_(conn) {}

int64_t WorkspaceRepository::create(const Workspace& ws) {
    Statement stmt(conn_.handle(), R"SQL(
        INSERT INTO workspace (
            root_path, name, include_json, exclude_json, default_ignores_json,
            compile_commands_path, revision, status, last_error
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL");

    stmt.bind_text(1, ws.root_path);
    stmt.bind_text(2, ws.name);
    stmt.bind_text(3, serialize_json_string_array(ws.include_patterns));
    stmt.bind_text(4, serialize_json_string_array(ws.exclude_patterns));
    stmt.bind_text(5, serialize_json_string_array(ws.default_ignores));
    stmt.bind_optional_text(6, ws.compile_commands_path);
    stmt.bind_int64(7, ws.revision);
    stmt.bind_text(8, to_string(ws.status));
    stmt.bind_optional_text(9, ws.last_error);

    stmt.execute();
    return conn_.last_insert_rowid();
}

std::optional<Workspace> WorkspaceRepository::get_by_id(int64_t id) {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT id, root_path, name, include_json, exclude_json, default_ignores_json,
               compile_commands_path, created_at, updated_at, revision, status, last_error
        FROM workspace
        WHERE id = ?;
    )SQL");
    stmt.bind_int64(1, id);

    if (stmt.step()) {
        return read_workspace_row(stmt);
    }
    return std::nullopt;
}

std::optional<Workspace> WorkspaceRepository::get_by_root_path(const std::string& root_path) {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT id, root_path, name, include_json, exclude_json, default_ignores_json,
               compile_commands_path, created_at, updated_at, revision, status, last_error
        FROM workspace
        WHERE root_path = ?;
    )SQL");
    stmt.bind_text(1, root_path);

    if (stmt.step()) {
        return read_workspace_row(stmt);
    }
    return std::nullopt;
}

std::vector<Workspace> WorkspaceRepository::list_all() {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT id, root_path, name, include_json, exclude_json, default_ignores_json,
               compile_commands_path, created_at, updated_at, revision, status, last_error
        FROM workspace
        ORDER BY id ASC;
    )SQL");

    std::vector<Workspace> results;
    while (stmt.step()) {
        results.push_back(read_workspace_row(stmt));
    }
    return results;
}

bool WorkspaceRepository::update(const Workspace& ws) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE workspace
        SET name = ?,
            include_json = ?,
            exclude_json = ?,
            default_ignores_json = ?,
            compile_commands_path = ?,
            status = ?,
            last_error = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");

    stmt.bind_text(1, ws.name);
    stmt.bind_text(2, serialize_json_string_array(ws.include_patterns));
    stmt.bind_text(3, serialize_json_string_array(ws.exclude_patterns));
    stmt.bind_text(4, serialize_json_string_array(ws.default_ignores));
    stmt.bind_optional_text(5, ws.compile_commands_path);
    stmt.bind_text(6, to_string(ws.status));
    stmt.bind_optional_text(7, ws.last_error);
    stmt.bind_int64(8, ws.id);

    stmt.execute();
    return conn_.changes() > 0;
}

bool WorkspaceRepository::update_status(int64_t id, WorkspaceStatus status,
                                        const std::optional<std::string>& last_error) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE workspace
        SET status = ?,
            last_error = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");

    stmt.bind_text(1, to_string(status));
    stmt.bind_optional_text(2, last_error);
    stmt.bind_int64(3, id);

    stmt.execute();
    return conn_.changes() > 0;
}

int64_t WorkspaceRepository::increment_revision(int64_t id) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE workspace
        SET revision = revision + 1,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");
    stmt.bind_int64(1, id);
    stmt.execute();

    Statement fetch(conn_.handle(), "SELECT revision FROM workspace WHERE id = ?;");
    fetch.bind_int64(1, id);
    if (fetch.step()) {
        return fetch.column_int64(0);
    }
    return 0;
}

bool WorkspaceRepository::delete_by_id(int64_t id) {
    Statement stmt(conn_.handle(), "DELETE FROM workspace WHERE id = ?;");
    stmt.bind_int64(1, id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
