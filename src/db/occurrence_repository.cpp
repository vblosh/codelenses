#include "codelenses/db/occurrence_repository.h"

#include "codelenses/db/connection.h"
#include "codelenses/db/statement.h"

namespace codelenses {

namespace {

Occurrence read_occurrence_row(Statement& stmt) {
    Occurrence occ;
    occ.id = stmt.column_int64(0);
    occ.workspace_id = stmt.column_int64(1);
    occ.file_id = stmt.column_int64(2);
    occ.symbol_id = stmt.column_optional_int64(3);
    occ.occurrence_kind = stmt.column_text(4);
    occ.name = stmt.column_text(5);
    occ.range.start_byte = stmt.column_int64(6);
    occ.range.end_byte = stmt.column_int64(7);
    occ.range.start_line = stmt.column_int64(8);
    occ.range.start_column = stmt.column_int64(9);
    occ.range.end_line = stmt.column_int64(10);
    occ.range.end_column = stmt.column_int64(11);
    occ.confidence = stmt.column_double(12);
    occ.resolution = stmt.column_text(13);
    occ.metadata_json = stmt.column_optional_text(14);
    return occ;
}

const char* kOccurrenceSelectFields =
    "id, workspace_id, file_id, symbol_id, occurrence_kind, name, "
    "start_byte, end_byte, start_line, start_column, end_line, end_column, "
    "confidence, resolution, metadata_json";

const char* kOccurrenceInsertSql = R"SQL(
    INSERT INTO occurrence (
        workspace_id, file_id, symbol_id, occurrence_kind, name,
        start_byte, end_byte, start_line, start_column, end_line, end_column,
        confidence, resolution, metadata_json
    ) VALUES (
        ?, ?, ?, ?, ?,
        ?, ?, ?, ?, ?, ?,
        ?, ?, ?
    );
)SQL";

void bind_occurrence_params(Statement& stmt, const Occurrence& occ) {
    stmt.bind_int64(1, occ.workspace_id);
    stmt.bind_int64(2, occ.file_id);
    stmt.bind_optional_int64(3, occ.symbol_id);
    stmt.bind_text(4, occ.occurrence_kind);
    stmt.bind_text(5, occ.name);
    stmt.bind_int64(6, occ.range.start_byte);
    stmt.bind_int64(7, occ.range.end_byte);
    stmt.bind_int64(8, occ.range.start_line);
    stmt.bind_int64(9, occ.range.start_column);
    stmt.bind_int64(10, occ.range.end_line);
    stmt.bind_int64(11, occ.range.end_column);
    stmt.bind_double(12, occ.confidence);
    stmt.bind_text(13, occ.resolution);
    stmt.bind_optional_text(14, occ.metadata_json);
}

} // namespace

OccurrenceRepository::OccurrenceRepository(Connection& conn) : conn_(conn) {}

int64_t OccurrenceRepository::insert(const Occurrence& occ) {
    Statement stmt(conn_.handle(), kOccurrenceInsertSql);
    bind_occurrence_params(stmt, occ);
    stmt.execute();
    return conn_.last_insert_rowid();
}

void OccurrenceRepository::insert_batch(const std::vector<Occurrence>& occurrences) {
    Statement stmt(conn_.handle(), kOccurrenceInsertSql);
    for (const auto& occ : occurrences) {
        stmt.reset();
        stmt.clear_bindings();
        bind_occurrence_params(stmt, occ);
        stmt.execute();
    }
}

std::optional<Occurrence> OccurrenceRepository::get_by_id(int64_t id) {
    std::string sql =
        "SELECT " + std::string(kOccurrenceSelectFields) + " FROM occurrence WHERE id = ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, id);

    if (stmt.step()) {
        return read_occurrence_row(stmt);
    }
    return std::nullopt;
}

std::vector<Occurrence> OccurrenceRepository::list_by_file(int64_t file_id) {
    std::string sql = "SELECT " + std::string(kOccurrenceSelectFields) +
                      " FROM occurrence WHERE file_id = ? ORDER BY start_byte, end_byte DESC;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);

    std::vector<Occurrence> results;
    while (stmt.step()) {
        results.push_back(read_occurrence_row(stmt));
    }
    return results;
}

std::vector<Occurrence> OccurrenceRepository::list_by_symbol(int64_t symbol_id) {
    std::string sql = "SELECT " + std::string(kOccurrenceSelectFields) +
                      " FROM occurrence WHERE symbol_id = ? ORDER BY file_id, start_byte;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, symbol_id);

    std::vector<Occurrence> results;
    while (stmt.step()) {
        results.push_back(read_occurrence_row(stmt));
    }
    return results;
}

std::vector<Occurrence> OccurrenceRepository::find_at_range(int64_t file_id, int64_t start_byte,
                                                            int64_t end_byte) {
    std::string sql = "SELECT " + std::string(kOccurrenceSelectFields) +
                      " FROM occurrence WHERE file_id = ? AND start_byte >= ? AND end_byte <= ?"
                      " ORDER BY start_byte ASC;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);
    stmt.bind_int64(2, start_byte);
    stmt.bind_int64(3, end_byte);

    std::vector<Occurrence> results;
    while (stmt.step()) {
        results.push_back(read_occurrence_row(stmt));
    }
    return results;
}

bool OccurrenceRepository::delete_by_file(int64_t file_id) {
    Statement stmt(conn_.handle(), "DELETE FROM occurrence WHERE file_id = ?;");
    stmt.bind_int64(1, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
