#include "codelenses/db/reference_repository.hpp"

#include "codelenses/db/connection.hpp"
#include "codelenses/db/statement.hpp"

namespace codelenses {

namespace {

ReferenceOccurrence read_reference_row(Statement& stmt) {
    ReferenceOccurrence ref;
    ref.id = stmt.column_int64(0);
    ref.workspace_id = stmt.column_int64(1);
    ref.source_file_id = stmt.column_int64(2);
    ref.source_symbol_id = stmt.column_optional_int64(3);
    ref.target_symbol_id = stmt.column_optional_int64(4);
    ref.name = stmt.column_text(5);
    ref.reference_kind = stmt.column_text(6);
    ref.range.start_byte = stmt.column_int64(7);
    ref.range.end_byte = stmt.column_int64(8);
    ref.range.start_line = stmt.column_int64(9);
    ref.range.start_column = stmt.column_int64(10);
    ref.range.end_line = stmt.column_int64(11);
    ref.range.end_column = stmt.column_int64(12);
    ref.resolution = stmt.column_text(13);
    ref.confidence = stmt.column_double(14);
    ref.metadata_json = stmt.column_optional_text(15);
    return ref;
}

const char* kReferenceSelectFields =
    "id, workspace_id, source_file_id, source_symbol_id, target_symbol_id, name, "
    "reference_kind, start_byte, end_byte, start_line, start_column, end_line, end_column, "
    "resolution, confidence, metadata_json";

const char* kReferenceInsertSql = R"SQL(
    INSERT INTO reference_occurrence (
        workspace_id, source_file_id, source_symbol_id, target_symbol_id, name,
        reference_kind, start_byte, end_byte, start_line, start_column, end_line,
        end_column, resolution, confidence, metadata_json
    ) VALUES (
        ?, ?, ?, ?, ?,
        ?, ?, ?, ?, ?, ?,
        ?, ?, ?, ?
    );
)SQL";

void bind_reference_params(Statement& stmt, const ReferenceOccurrence& ref) {
    stmt.bind_int64(1, ref.workspace_id);
    stmt.bind_int64(2, ref.source_file_id);
    stmt.bind_optional_int64(3, ref.source_symbol_id);
    stmt.bind_optional_int64(4, ref.target_symbol_id);
    stmt.bind_text(5, ref.name);
    stmt.bind_text(6, ref.reference_kind);
    stmt.bind_int64(7, ref.range.start_byte);
    stmt.bind_int64(8, ref.range.end_byte);
    stmt.bind_int64(9, ref.range.start_line);
    stmt.bind_int64(10, ref.range.start_column);
    stmt.bind_int64(11, ref.range.end_line);
    stmt.bind_int64(12, ref.range.end_column);
    stmt.bind_text(13, ref.resolution);
    stmt.bind_double(14, ref.confidence);
    stmt.bind_optional_text(15, ref.metadata_json);
}

} // namespace

ReferenceRepository::ReferenceRepository(Connection& conn) : conn_(conn) {}

int64_t ReferenceRepository::insert(const ReferenceOccurrence& ref) {
    Statement stmt(conn_.handle(), kReferenceInsertSql);
    bind_reference_params(stmt, ref);
    stmt.execute();
    return conn_.last_insert_rowid();
}

void ReferenceRepository::insert_batch(const std::vector<ReferenceOccurrence>& references) {
    Statement stmt(conn_.handle(), kReferenceInsertSql);
    for (const auto& ref : references) {
        stmt.reset();
        stmt.clear_bindings();
        bind_reference_params(stmt, ref);
        stmt.execute();
    }
}

std::optional<ReferenceOccurrence> ReferenceRepository::get_by_id(int64_t id) {
    std::string sql = "SELECT " + std::string(kReferenceSelectFields) +
                      " FROM reference_occurrence WHERE id = ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, id);

    if (stmt.step()) {
        return read_reference_row(stmt);
    }
    return std::nullopt;
}

std::vector<ReferenceOccurrence> ReferenceRepository::list_by_source_file(int64_t source_file_id) {
    std::string sql = "SELECT " + std::string(kReferenceSelectFields) +
                      " FROM reference_occurrence WHERE source_file_id = ? ORDER BY start_byte;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, source_file_id);

    std::vector<ReferenceOccurrence> results;
    while (stmt.step()) {
        results.push_back(read_reference_row(stmt));
    }
    return results;
}

std::vector<ReferenceOccurrence> ReferenceRepository::list_by_workspace(int64_t workspace_id) {
    std::string sql = "SELECT " + std::string(kReferenceSelectFields) +
                      " FROM reference_occurrence WHERE workspace_id = ? ORDER BY source_file_id, "
                      "start_byte;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);

    std::vector<ReferenceOccurrence> results;
    while (stmt.step()) {
        results.push_back(read_reference_row(stmt));
    }
    return results;
}

bool ReferenceRepository::update_resolution(int64_t id, std::optional<int64_t> source_symbol_id,
                                            std::optional<int64_t> target_symbol_id,
                                            const std::string& resolution, double confidence,
                                            const std::optional<std::string>& metadata_json) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE reference_occurrence
        SET source_symbol_id = ?,
            target_symbol_id = ?,
            resolution = ?,
            confidence = ?,
            metadata_json = ?
        WHERE id = ?;
    )SQL");
    stmt.bind_optional_int64(1, source_symbol_id);
    stmt.bind_optional_int64(2, target_symbol_id);
    stmt.bind_text(3, resolution);
    stmt.bind_double(4, confidence);
    stmt.bind_optional_text(5, metadata_json);
    stmt.bind_int64(6, id);
    stmt.execute();
    return conn_.changes() > 0;
}

std::vector<ReferencerResult> ReferenceRepository::find_referencers(int64_t workspace_id,
                                                                    int64_t symbol_id,
                                                                    int64_t limit, int64_t offset) {
    // Section 8: Referencers of a symbol
    Statement stmt(conn_.handle(), R"SQL(
        SELECT
            r.id,
            r.reference_kind,
            r.name,
            r.resolution,
            r.confidence,
            r.start_line,
            r.start_column,
            r.end_line,
            r.end_column,
            f.id AS file_id,
            f.relative_path,
            ss.id AS containing_symbol_id,
            ss.name AS containing_symbol_name,
            ss.qualified_name AS containing_qualified_name
        FROM reference_occurrence AS r
        JOIN file AS f ON f.id = r.source_file_id
        LEFT JOIN symbol AS ss ON ss.id = r.source_symbol_id
        WHERE r.workspace_id = ?
          AND r.target_symbol_id = ?
        ORDER BY f.relative_path, r.start_line, r.start_column
        LIMIT ? OFFSET ?;
    )SQL");

    stmt.bind_int64(1, workspace_id);
    stmt.bind_int64(2, symbol_id);
    stmt.bind_int64(3, limit);
    stmt.bind_int64(4, offset);

    std::vector<ReferencerResult> results;
    while (stmt.step()) {
        ReferencerResult res;
        res.id = stmt.column_int64(0);
        res.reference_kind = stmt.column_text(1);
        res.name = stmt.column_text(2);
        res.resolution = stmt.column_text(3);
        res.confidence = stmt.column_double(4);
        res.start_line = stmt.column_int64(5);
        res.start_column = stmt.column_int64(6);
        res.end_line = stmt.column_int64(7);
        res.end_column = stmt.column_int64(8);
        res.file_id = stmt.column_int64(9);
        res.relative_path = stmt.column_text(10);
        res.containing_symbol_id = stmt.column_optional_int64(11);
        res.containing_symbol_name = stmt.column_optional_text(12);
        res.containing_qualified_name = stmt.column_optional_text(13);
        results.push_back(res);
    }
    return results;
}

std::vector<CallerCalleeResult> ReferenceRepository::find_callers(int64_t symbol_id) {
    // Section 8: Callers: symbols containing references to the selected function.
    Statement stmt(conn_.handle(), R"SQL(
        SELECT DISTINCT
            r.source_symbol_id,
            s.name,
            s.qualified_name,
            s.file_id
        FROM reference_occurrence AS r
        JOIN symbol AS s ON s.id = r.source_symbol_id
        WHERE r.target_symbol_id = ?
          AND r.reference_kind IN ('call', 'invocation')
        ORDER BY s.name;
    )SQL");

    stmt.bind_int64(1, symbol_id);

    std::vector<CallerCalleeResult> results;
    while (stmt.step()) {
        CallerCalleeResult res;
        res.symbol_id = stmt.column_optional_int64(0);
        res.name = stmt.column_text(1);
        res.qualified_name = stmt.column_optional_text(2);
        res.file_id = stmt.column_int64(3);
        results.push_back(res);
    }
    return results;
}

std::vector<CallerCalleeResult> ReferenceRepository::find_callees(int64_t symbol_id) {
    // Section 8: Callees: symbols referenced by the selected function.
    Statement stmt(conn_.handle(), R"SQL(
        SELECT DISTINCT
            r.target_symbol_id,
            s.name,
            s.qualified_name,
            s.file_id
        FROM reference_occurrence AS r
        JOIN symbol AS s ON s.id = r.target_symbol_id
        WHERE r.source_symbol_id = ?
          AND r.reference_kind IN ('call', 'invocation')
        ORDER BY s.name;
    )SQL");

    stmt.bind_int64(1, symbol_id);

    std::vector<CallerCalleeResult> results;
    while (stmt.step()) {
        CallerCalleeResult res;
        res.symbol_id = stmt.column_optional_int64(0);
        res.name = stmt.column_text(1);
        res.qualified_name = stmt.column_optional_text(2);
        res.file_id = stmt.column_int64(3);
        results.push_back(res);
    }
    return results;
}

bool ReferenceRepository::delete_by_file(int64_t file_id) {
    Statement stmt(conn_.handle(), "DELETE FROM reference_occurrence WHERE source_file_id = ?;");
    stmt.bind_int64(1, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
