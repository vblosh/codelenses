#include "codelenses/db/job_repository.h"

#include "codelenses/db/connection.h"
#include "codelenses/db/statement.h"

namespace codelenses {

namespace {

IndexJob read_job_row(Statement& stmt) {
    IndexJob j;
    j.id = stmt.column_int64(0);
    j.workspace_id = stmt.column_int64(1);
    j.job_type = stmt.column_text(2);
    j.status = stmt.column_text(3);
    j.requested_mode = stmt.column_optional_text(4);
    j.queued_at = stmt.column_text(5);
    j.started_at = stmt.column_optional_text(6);
    j.finished_at = stmt.column_optional_text(7);
    j.files_total = stmt.column_int64(8);
    j.files_processed = stmt.column_int64(9);
    j.files_skipped = stmt.column_int64(10);
    j.error_count = stmt.column_int64(11);
    j.warning_count = stmt.column_int64(12);
    j.workspace_revision = stmt.column_optional_int64(13);
    j.error_message = stmt.column_optional_text(14);
    return j;
}

const char* kJobSelectFields =
    "id, workspace_id, job_type, status, requested_mode, queued_at, started_at, "
    "finished_at, files_total, files_processed, files_skipped, error_count, "
    "warning_count, workspace_revision, error_message";

} // namespace

JobRepository::JobRepository(Connection& conn) : conn_(conn) {}

int64_t JobRepository::create(const IndexJob& job) {
    Statement stmt(conn_.handle(), R"SQL(
        INSERT INTO index_job (
            workspace_id, job_type, status, requested_mode, files_total,
            files_processed, files_skipped, error_count, warning_count,
            workspace_revision, error_message
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL");

    stmt.bind_int64(1, job.workspace_id);
    stmt.bind_text(2, job.job_type);
    stmt.bind_text(3, job.status);
    stmt.bind_optional_text(4, job.requested_mode);
    stmt.bind_int64(5, job.files_total);
    stmt.bind_int64(6, job.files_processed);
    stmt.bind_int64(7, job.files_skipped);
    stmt.bind_int64(8, job.error_count);
    stmt.bind_int64(9, job.warning_count);
    stmt.bind_optional_int64(10, job.workspace_revision);
    stmt.bind_optional_text(11, job.error_message);

    stmt.execute();
    return conn_.last_insert_rowid();
}

std::optional<IndexJob> JobRepository::get_by_id(int64_t id) {
    std::string sql = "SELECT " + std::string(kJobSelectFields) + " FROM index_job WHERE id = ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, id);

    if (stmt.step()) {
        return read_job_row(stmt);
    }
    return std::nullopt;
}

bool JobRepository::update_status(int64_t id, const std::string& status,
                                  const std::optional<std::string>& error_message) {
    std::string sql = "UPDATE index_job SET status = ?, error_message = ?";
    if (status == "running") {
        sql += ", started_at = CURRENT_TIMESTAMP";
    }
    sql += " WHERE id = ?;";

    Statement stmt(conn_.handle(), sql);
    stmt.bind_text(1, status);
    stmt.bind_optional_text(2, error_message);
    stmt.bind_int64(3, id);

    stmt.execute();
    return conn_.changes() > 0;
}

bool JobRepository::update_progress(int64_t id, int64_t processed, int64_t skipped, int64_t errors,
                                    int64_t warnings) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE index_job
        SET files_processed = ?,
            files_skipped = ?,
            error_count = ?,
            warning_count = ?
        WHERE id = ?;
    )SQL");

    stmt.bind_int64(1, processed);
    stmt.bind_int64(2, skipped);
    stmt.bind_int64(3, errors);
    stmt.bind_int64(4, warnings);
    stmt.bind_int64(5, id);

    stmt.execute();
    return conn_.changes() > 0;
}

bool JobRepository::finish_job(int64_t id, const std::string& status,
                               const std::optional<int64_t>& revision,
                               const std::optional<std::string>& error_message) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE index_job
        SET status = ?,
            finished_at = CURRENT_TIMESTAMP,
            workspace_revision = ?,
            error_message = ?
        WHERE id = ?;
    )SQL");

    stmt.bind_text(1, status);
    stmt.bind_optional_int64(2, revision);
    stmt.bind_optional_text(3, error_message);
    stmt.bind_int64(4, id);

    stmt.execute();
    return conn_.changes() > 0;
}

std::vector<IndexJob> JobRepository::list_by_workspace(int64_t workspace_id, int64_t limit) {
    std::string sql = "SELECT " + std::string(kJobSelectFields) +
                      " FROM index_job WHERE workspace_id = ? ORDER BY queued_at DESC LIMIT ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);
    stmt.bind_int64(2, limit);

    std::vector<IndexJob> results;
    while (stmt.step()) {
        results.push_back(read_job_row(stmt));
    }
    return results;
}

} // namespace codelenses
