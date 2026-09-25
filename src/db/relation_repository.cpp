#include "codelenses/db/relation_repository.h"

#include "codelenses/db/connection.h"
#include "codelenses/db/statement.h"

namespace codelenses {

namespace {

SymbolRelation read_relation_row(Statement& stmt) {
    SymbolRelation rel;
    rel.id = stmt.column_int64(0);
    rel.workspace_id = stmt.column_int64(1);
    rel.source_symbol_id = stmt.column_int64(2);
    rel.target_symbol_id = stmt.column_optional_int64(3);
    rel.relation_kind = stmt.column_text(4);
    rel.target_name = stmt.column_optional_text(5);
    rel.resolution = stmt.column_text(6);
    rel.confidence = stmt.column_double(7);
    rel.metadata_json = stmt.column_optional_text(8);
    return rel;
}

const char* kRelationSelectFields =
    "id, workspace_id, source_symbol_id, target_symbol_id, relation_kind, "
    "target_name, resolution, confidence, metadata_json";

const char* kRelationInsertSql = R"SQL(
    INSERT OR REPLACE INTO symbol_relation (
        workspace_id, source_symbol_id, target_symbol_id, relation_kind,
        target_name, resolution, confidence, metadata_json
    ) VALUES (
        ?, ?, ?, ?,
        ?, ?, ?, ?
    );
)SQL";

void bind_relation_params(Statement& stmt, const SymbolRelation& rel) {
    stmt.bind_int64(1, rel.workspace_id);
    stmt.bind_int64(2, rel.source_symbol_id);
    stmt.bind_optional_int64(3, rel.target_symbol_id);
    stmt.bind_text(4, rel.relation_kind);
    stmt.bind_optional_text(5, rel.target_name);
    stmt.bind_text(6, rel.resolution);
    stmt.bind_double(7, rel.confidence);
    stmt.bind_optional_text(8, rel.metadata_json);
}

} // namespace

RelationRepository::RelationRepository(Connection& conn) : conn_(conn) {}

int64_t RelationRepository::insert(const SymbolRelation& rel) {
    Statement stmt(conn_.handle(), kRelationInsertSql);
    bind_relation_params(stmt, rel);
    stmt.execute();
    return conn_.last_insert_rowid();
}

void RelationRepository::insert_batch(const std::vector<SymbolRelation>& relations) {
    Statement stmt(conn_.handle(), kRelationInsertSql);
    for (const auto& rel : relations) {
        stmt.reset();
        stmt.clear_bindings();
        bind_relation_params(stmt, rel);
        stmt.execute();
    }
}

std::vector<SymbolRelation>
RelationRepository::find_by_source_symbol(int64_t source_symbol_id,
                                          const std::optional<std::string>& relation_kind) {
    std::string sql = "SELECT " + std::string(kRelationSelectFields) +
                      " FROM symbol_relation WHERE source_symbol_id = ?";
    if (relation_kind.has_value()) {
        sql += " AND relation_kind = ?";
    }
    sql += " ORDER BY relation_kind, target_name;";

    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, source_symbol_id);
    if (relation_kind.has_value()) {
        stmt.bind_text(2, *relation_kind);
    }

    std::vector<SymbolRelation> results;
    while (stmt.step()) {
        results.push_back(read_relation_row(stmt));
    }
    return results;
}

std::vector<SymbolRelation>
RelationRepository::find_by_target_symbol(int64_t target_symbol_id,
                                          const std::optional<std::string>& relation_kind) {
    std::string sql = "SELECT " + std::string(kRelationSelectFields) +
                      " FROM symbol_relation WHERE target_symbol_id = ?";
    if (relation_kind.has_value()) {
        sql += " AND relation_kind = ?";
    }
    sql += " ORDER BY relation_kind, source_symbol_id;";

    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, target_symbol_id);
    if (relation_kind.has_value()) {
        stmt.bind_text(2, *relation_kind);
    }

    std::vector<SymbolRelation> results;
    while (stmt.step()) {
        results.push_back(read_relation_row(stmt));
    }
    return results;
}

bool RelationRepository::delete_by_file(int64_t file_id) {
    Statement stmt(conn_.handle(), R"SQL(
        DELETE FROM symbol_relation
        WHERE source_symbol_id IN (SELECT id FROM symbol WHERE file_id = ?)
           OR target_symbol_id IN (SELECT id FROM symbol WHERE file_id = ?);
    )SQL");
    stmt.bind_int64(1, file_id);
    stmt.bind_int64(2, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
