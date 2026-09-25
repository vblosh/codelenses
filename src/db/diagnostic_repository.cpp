#include "codelenses/db/diagnostic_repository.h"

#include "codelenses/db/connection.h"
#include "codelenses/db/statement.h"

namespace codelenses {

namespace {

Diagnostic read_diagnostic_row(Statement& stmt) {
    Diagnostic d;
    d.id = stmt.column_int64(0);
    d.workspace_id = stmt.column_int64(1);
    d.job_id = stmt.column_optional_int64(2);
    d.file_id = stmt.column_optional_int64(3);
    d.severity = stmt.column_text(4);
    d.source = stmt.column_text(5);
    d.code = stmt.column_text(6);
    d.message = stmt.column_text(7);
    d.start_byte = stmt.column_optional_int64(8);
    d.end_byte = stmt.column_optional_int64(9);
    d.start_line = stmt.column_optional_int64(10);
    d.start_column = stmt.column_optional_int64(11);
    d.end_line = stmt.column_optional_int64(12);
    d.end_column = stmt.column_optional_int64(13);
    d.metadata_json = stmt.column_optional_text(14);
    d.created_at = stmt.column_text(15);
    return d;
}

const char* kDiagnosticSelectFields =
    "id, workspace_id, job_id, file_id, severity, source, code, message, "
    "start_byte, end_byte, start_line, start_column, end_line, end_column, metadata_json, "
    "created_at";

const char* kDiagnosticInsertSql = R"SQL(
    INSERT INTO diagnostic (
        workspace_id, job_id, file_id, severity, source, code, message,
        start_byte, end_byte, start_line, start_column, end_line, end_column, metadata_json
    ) VALUES (
        ?, ?, ?, ?, ?, ?, ?,
        ?, ?, ?, ?, ?, ?, ?
    );
)SQL";

void bind_diagnostic_params(Statement& stmt, const Diagnostic& diag) {
    stmt.bind_int64(1, diag.workspace_id);
    stmt.bind_optional_int64(2, diag.job_id);
    stmt.bind_optional_int64(3, diag.file_id);
    stmt.bind_text(4, diag.severity);
    stmt.bind_text(5, diag.source);
    stmt.bind_text(6, diag.code);
    stmt.bind_text(7, diag.message);
    stmt.bind_optional_int64(8, diag.start_byte);
    stmt.bind_optional_int64(9, diag.end_byte);
    stmt.bind_optional_int64(10, diag.start_line);
    stmt.bind_optional_int64(11, diag.start_column);
    stmt.bind_optional_int64(12, diag.end_line);
    stmt.bind_optional_int64(13, diag.end_column);
    stmt.bind_optional_text(14, diag.metadata_json);
}

} // namespace

DiagnosticRepository::DiagnosticRepository(Connection& conn) : conn_(conn) {}

int64_t DiagnosticRepository::insert(const Diagnostic& diag) {
    Statement stmt(conn_.handle(), kDiagnosticInsertSql);
    bind_diagnostic_params(stmt, diag);
    stmt.execute();
    return conn_.last_insert_rowid();
}

void DiagnosticRepository::insert_batch(const std::vector<Diagnostic>& diagnostics) {
    Statement stmt(conn_.handle(), kDiagnosticInsertSql);
    for (const auto& diag : diagnostics) {
        stmt.reset();
        stmt.clear_bindings();
        bind_diagnostic_params(stmt, diag);
        stmt.execute();
    }
}

std::vector<Diagnostic>
DiagnosticRepository::list_by_workspace(int64_t workspace_id,
                                        const std::optional<std::string>& severity, int64_t limit,
                                        int64_t offset) {
    std::string sql = "SELECT " + std::string(kDiagnosticSelectFields) +
                      " FROM diagnostic WHERE workspace_id = ?";
    if (severity.has_value()) {
        sql += " AND severity = ?";
    }
    sql += " ORDER BY created_at DESC LIMIT ? OFFSET ?;";

    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);
    int next_idx = 2;
    if (severity.has_value()) {
        stmt.bind_text(next_idx++, *severity);
    }
    stmt.bind_int64(next_idx++, limit);
    stmt.bind_int64(next_idx, offset);

    std::vector<Diagnostic> results;
    while (stmt.step()) {
        results.push_back(read_diagnostic_row(stmt));
    }
    return results;
}

std::vector<Diagnostic> DiagnosticRepository::list_by_file(int64_t file_id) {
    std::string sql = "SELECT " + std::string(kDiagnosticSelectFields) +
                      " FROM diagnostic WHERE file_id = ? ORDER BY start_line, start_column;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);

    std::vector<Diagnostic> results;
    while (stmt.step()) {
        results.push_back(read_diagnostic_row(stmt));
    }
    return results;
}

std::vector<Diagnostic> DiagnosticRepository::list_by_job(int64_t job_id) {
    std::string sql = "SELECT " + std::string(kDiagnosticSelectFields) +
                      " FROM diagnostic WHERE job_id = ? ORDER BY id ASC;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, job_id);

    std::vector<Diagnostic> results;
    while (stmt.step()) {
        results.push_back(read_diagnostic_row(stmt));
    }
    return results;
}

bool DiagnosticRepository::delete_by_file(int64_t file_id) {
    Statement stmt(conn_.handle(), "DELETE FROM diagnostic WHERE file_id = ?;");
    stmt.bind_int64(1, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
