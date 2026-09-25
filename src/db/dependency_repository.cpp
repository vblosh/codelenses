#include "codelenses/db/dependency_repository.hpp"

#include "codelenses/db/connection.hpp"
#include "codelenses/db/statement.hpp"

namespace codelenses {

namespace {

FileDependency read_dependency_row(Statement& stmt) {
    FileDependency dep;
    dep.id = stmt.column_int64(0);
    dep.workspace_id = stmt.column_int64(1);
    dep.source_file_id = stmt.column_int64(2);
    dep.target_file_id = stmt.column_optional_int64(3);
    dep.dependency_kind = stmt.column_text(4);
    dep.raw_name = stmt.column_text(5);
    dep.resolved_path = stmt.column_optional_text(6);
    dep.resolution = stmt.column_text(7);
    dep.start_byte = stmt.column_optional_int64(8);
    dep.end_byte = stmt.column_optional_int64(9);
    dep.metadata_json = stmt.column_optional_text(10);
    return dep;
}

const char* kDependencySelectFields =
    "id, workspace_id, source_file_id, target_file_id, dependency_kind, "
    "raw_name, resolved_path, resolution, start_byte, end_byte, metadata_json";

const char* kDependencyInsertSql = R"SQL(
    INSERT OR REPLACE INTO file_dependency (
        workspace_id, source_file_id, target_file_id, dependency_kind,
        raw_name, resolved_path, resolution, start_byte, end_byte, metadata_json
    ) VALUES (
        ?, ?, ?, ?,
        ?, ?, ?, ?, ?, ?
    );
)SQL";

void bind_dependency_params(Statement& stmt, const FileDependency& dep) {
    stmt.bind_int64(1, dep.workspace_id);
    stmt.bind_int64(2, dep.source_file_id);
    stmt.bind_optional_int64(3, dep.target_file_id);
    stmt.bind_text(4, dep.dependency_kind);
    stmt.bind_text(5, dep.raw_name);
    stmt.bind_optional_text(6, dep.resolved_path);
    stmt.bind_text(7, dep.resolution);
    stmt.bind_optional_int64(8, dep.start_byte);
    stmt.bind_optional_int64(9, dep.end_byte);
    stmt.bind_optional_text(10, dep.metadata_json);
}

} // namespace

DependencyRepository::DependencyRepository(Connection& conn) : conn_(conn) {}

int64_t DependencyRepository::insert(const FileDependency& dep) {
    Statement stmt(conn_.handle(), kDependencyInsertSql);
    bind_dependency_params(stmt, dep);
    stmt.execute();
    return conn_.last_insert_rowid();
}

void DependencyRepository::insert_batch(const std::vector<FileDependency>& dependencies) {
    Statement stmt(conn_.handle(), kDependencyInsertSql);
    for (const auto& dep : dependencies) {
        stmt.reset();
        stmt.clear_bindings();
        bind_dependency_params(stmt, dep);
        stmt.execute();
    }
}

std::vector<FileDependency> DependencyRepository::list_by_source_file(int64_t file_id) {
    std::string sql =
        "SELECT " + std::string(kDependencySelectFields) +
        " FROM file_dependency WHERE source_file_id = ? ORDER BY dependency_kind, raw_name;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);

    std::vector<FileDependency> results;
    while (stmt.step()) {
        results.push_back(read_dependency_row(stmt));
    }
    return results;
}

std::vector<FileDependency> DependencyRepository::list_by_target_file(int64_t file_id) {
    std::string sql = "SELECT " + std::string(kDependencySelectFields) +
                      " FROM file_dependency WHERE target_file_id = ? ORDER BY source_file_id;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);

    std::vector<FileDependency> results;
    while (stmt.step()) {
        results.push_back(read_dependency_row(stmt));
    }
    return results;
}

bool DependencyRepository::delete_by_file(int64_t file_id) {
    Statement stmt(conn_.handle(), "DELETE FROM file_dependency WHERE source_file_id = ?;");
    stmt.bind_int64(1, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
