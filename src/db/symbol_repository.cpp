#include "codelenses/db/symbol_repository.hpp"

#include "codelenses/db/connection.hpp"
#include "codelenses/db/statement.hpp"

namespace codelenses {

namespace {

Symbol read_symbol_row(Statement& stmt) {
    Symbol s;
    s.id = stmt.column_int64(0);
    s.workspace_id = stmt.column_int64(1);
    s.file_id = stmt.column_int64(2);
    s.symbol_key = stmt.column_text(3);
    s.name = stmt.column_text(4);
    s.qualified_name = stmt.column_optional_text(5);
    s.display_name = stmt.column_optional_text(6);
    s.kind = stmt.column_text(7);
    s.language = stmt.column_text(8);
    s.signature = stmt.column_optional_text(9);
    s.documentation = stmt.column_optional_text(10);
    s.container_name = stmt.column_optional_text(11);
    s.scope_symbol_id = stmt.column_optional_int64(12);
    s.visibility = stmt.column_optional_text(13);
    s.is_definition = stmt.column_bool(14);
    s.is_declaration = stmt.column_bool(15);
    s.is_generated = stmt.column_bool(16);
    s.range.start_byte = stmt.column_int64(17);
    s.range.end_byte = stmt.column_int64(18);
    s.range.start_line = stmt.column_int64(19);
    s.range.start_column = stmt.column_int64(20);
    s.range.end_line = stmt.column_int64(21);
    s.range.end_column = stmt.column_int64(22);
    s.selection_start_byte = stmt.column_optional_int64(23);
    s.selection_end_byte = stmt.column_optional_int64(24);
    s.signature_hash = stmt.column_optional_text(25);
    s.parser_version = stmt.column_optional_text(26);
    s.created_at = stmt.column_text(27);
    s.updated_at = stmt.column_text(28);
    return s;
}

const char* kSymbolSelectFields =
    "id, workspace_id, file_id, symbol_key, name, qualified_name, display_name, kind, language, "
    "signature, documentation, container_name, scope_symbol_id, visibility, is_definition, "
    "is_declaration, is_generated, start_byte, end_byte, start_line, start_column, end_line, "
    "end_column, selection_start_byte, selection_end_byte, signature_hash, parser_version, "
    "created_at, updated_at";

const char* kSymbolInsertSql = R"SQL(
    INSERT INTO symbol (
        workspace_id, file_id, symbol_key, name, qualified_name, display_name,
        kind, language, signature, documentation, container_name, scope_symbol_id,
        visibility, is_definition, is_declaration, is_generated, start_byte, end_byte,
        start_line, start_column, end_line, end_column, selection_start_byte,
        selection_end_byte, signature_hash, parser_version
    ) VALUES (
        ?, ?, ?, ?, ?, ?,
        ?, ?, ?, ?, ?, ?,
        ?, ?, ?, ?, ?, ?,
        ?, ?, ?, ?, ?,
        ?, ?, ?
    );
)SQL";

void bind_symbol_params(Statement& stmt, const Symbol& symbol) {
    stmt.bind_int64(1, symbol.workspace_id);
    stmt.bind_int64(2, symbol.file_id);
    stmt.bind_text(3, symbol.symbol_key);
    stmt.bind_text(4, symbol.name);
    stmt.bind_optional_text(5, symbol.qualified_name);
    stmt.bind_optional_text(6, symbol.display_name);
    stmt.bind_text(7, symbol.kind);
    stmt.bind_text(8, symbol.language);
    stmt.bind_optional_text(9, symbol.signature);
    stmt.bind_optional_text(10, symbol.documentation);
    stmt.bind_optional_text(11, symbol.container_name);
    stmt.bind_optional_int64(12, symbol.scope_symbol_id);
    stmt.bind_optional_text(13, symbol.visibility);
    stmt.bind_bool(14, symbol.is_definition);
    stmt.bind_bool(15, symbol.is_declaration);
    stmt.bind_bool(16, symbol.is_generated);
    stmt.bind_int64(17, symbol.range.start_byte);
    stmt.bind_int64(18, symbol.range.end_byte);
    stmt.bind_int64(19, symbol.range.start_line);
    stmt.bind_int64(20, symbol.range.start_column);
    stmt.bind_int64(21, symbol.range.end_line);
    stmt.bind_int64(22, symbol.range.end_column);
    stmt.bind_optional_int64(23, symbol.selection_start_byte);
    stmt.bind_optional_int64(24, symbol.selection_end_byte);
    stmt.bind_optional_text(25, symbol.signature_hash);
    stmt.bind_optional_text(26, symbol.parser_version);
}

} // namespace

SymbolRepository::SymbolRepository(Connection& conn) : conn_(conn) {}

int64_t SymbolRepository::insert(const Symbol& symbol) {
    Statement stmt(conn_.handle(), kSymbolInsertSql);
    bind_symbol_params(stmt, symbol);
    stmt.execute();
    return conn_.last_insert_rowid();
}

std::vector<int64_t> SymbolRepository::insert_batch(const std::vector<Symbol>& symbols) {
    std::vector<int64_t> ids;
    ids.reserve(symbols.size());
    Statement stmt(conn_.handle(), kSymbolInsertSql);
    for (const auto& symbol : symbols) {
        stmt.reset();
        stmt.clear_bindings();
        bind_symbol_params(stmt, symbol);
        stmt.execute();
        ids.push_back(conn_.last_insert_rowid());
    }
    return ids;
}

std::optional<Symbol> SymbolRepository::get_by_id(int64_t id) {
    std::string sql = "SELECT " + std::string(kSymbolSelectFields) + " FROM symbol WHERE id = ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, id);

    if (stmt.step()) {
        return read_symbol_row(stmt);
    }
    return std::nullopt;
}

std::vector<Symbol> SymbolRepository::get_by_key(int64_t workspace_id,
                                                 const std::string& symbol_key) {
    // Section 8: Definitions and declarations ordered by is_definition DESC, file_id, start_byte
    std::string sql = "SELECT " + std::string(kSymbolSelectFields) +
                      " FROM symbol WHERE workspace_id = ? AND symbol_key = ? "
                      " ORDER BY is_definition DESC, file_id, start_byte;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);
    stmt.bind_text(2, symbol_key);

    std::vector<Symbol> results;
    while (stmt.step()) {
        results.push_back(read_symbol_row(stmt));
    }
    return results;
}

std::vector<Symbol> SymbolRepository::list_by_file(int64_t file_id) {
    // Section 8: Symbols in a file outline ordered by start_byte, end_byte DESC, name
    std::string sql = "SELECT " + std::string(kSymbolSelectFields) +
                      " FROM symbol WHERE file_id = ? ORDER BY start_byte, end_byte DESC, name;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);

    std::vector<Symbol> results;
    while (stmt.step()) {
        results.push_back(read_symbol_row(stmt));
    }
    return results;
}

std::optional<Symbol> SymbolRepository::find_at_offset(int64_t file_id, int64_t byte_offset) {
    // Section 8: Symbol under a source position
    std::string sql = "SELECT " + std::string(kSymbolSelectFields) +
                      " FROM symbol WHERE file_id = ? AND start_byte <= ? AND end_byte >= ?"
                      " ORDER BY (end_byte - start_byte) ASC LIMIT 1;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, file_id);
    stmt.bind_int64(2, byte_offset);
    stmt.bind_int64(3, byte_offset);

    if (stmt.step()) {
        return read_symbol_row(stmt);
    }
    return std::nullopt;
}

std::vector<Symbol> SymbolRepository::find_by_name(int64_t workspace_id, const std::string& name) {
    std::string sql = "SELECT " + std::string(kSymbolSelectFields) +
                      " FROM symbol WHERE workspace_id = ? AND name = ? ORDER BY name;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);
    stmt.bind_text(2, name);

    std::vector<Symbol> results;
    while (stmt.step()) {
        results.push_back(read_symbol_row(stmt));
    }
    return results;
}

bool SymbolRepository::delete_by_file(int64_t file_id) {
    Statement stmt(conn_.handle(), "DELETE FROM symbol WHERE file_id = ?;");
    stmt.bind_int64(1, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
