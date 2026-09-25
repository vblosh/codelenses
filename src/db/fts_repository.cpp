#include "codelenses/db/fts_repository.hpp"

#include "codelenses/db/connection.hpp"
#include "codelenses/db/error.hpp"
#include "codelenses/db/statement.hpp"

namespace codelenses {

FtsRepository::FtsRepository(Connection& conn) : conn_(conn) {}

std::vector<SymbolSearchResult> FtsRepository::search_symbols(int64_t workspace_id,
                                                              const std::string& query,
                                                              int64_t limit, int64_t offset) {
    if (query.empty()) {
        return {};
    }

    // Section 5.1: Symbol search query, excluding tombstoned files
    Statement stmt(conn_.handle(), R"SQL(
        SELECT
            s.id,
            s.file_id,
            s.name,
            s.qualified_name,
            s.kind,
            bm25(symbol_search) AS rank
        FROM symbol_search
        JOIN symbol AS s ON s.id = symbol_search.rowid
        JOIN file AS fl ON fl.id = s.file_id
        WHERE symbol_search MATCH ?
          AND s.workspace_id = ?
          AND fl.is_deleted = 0
        ORDER BY rank, s.name
        LIMIT ? OFFSET ?;
    )SQL");

    stmt.bind_text(1, query);
    stmt.bind_int64(2, workspace_id);
    stmt.bind_int64(3, limit);
    stmt.bind_int64(4, offset);

    std::vector<SymbolSearchResult> results;
    try {
        while (stmt.step()) {
            SymbolSearchResult res;
            res.id = stmt.column_int64(0);
            res.file_id = stmt.column_int64(1);
            res.name = stmt.column_text(2);
            res.qualified_name = stmt.column_optional_text(3);
            res.kind = stmt.column_text(4);
            res.rank = stmt.column_double(5);
            results.push_back(res);
        }
    } catch (const DbError&) {
        // If raw FTS syntax fails (e.g. unclosed quotes or syntax error), reset and try phrase
        // search
        stmt.reset();
        stmt.clear_bindings();
        std::string escaped_query;
        escaped_query.reserve(query.size() + 2);
        escaped_query.push_back('"');
        for (char c : query) {
            if (c == '"') {
                escaped_query.push_back('"');
            }
            escaped_query.push_back(c);
        }
        escaped_query.push_back('"');

        stmt.bind_text(1, escaped_query);
        stmt.bind_int64(2, workspace_id);
        stmt.bind_int64(3, limit);
        stmt.bind_int64(4, offset);

        try {
            while (stmt.step()) {
                SymbolSearchResult res;
                res.id = stmt.column_int64(0);
                res.file_id = stmt.column_int64(1);
                res.name = stmt.column_text(2);
                res.qualified_name = stmt.column_optional_text(3);
                res.kind = stmt.column_text(4);
                res.rank = stmt.column_double(5);
                results.push_back(res);
            }
        } catch (const DbError&) {
            return {};
        }
    }

    return results;
}

void FtsRepository::insert_or_update_file_content(const FileContent& content) {
    Statement stmt(conn_.handle(), R"SQL(
        INSERT INTO file_content (file_id, workspace_id, content, content_hash, updated_at)
        VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(file_id) DO UPDATE SET
            content = excluded.content,
            content_hash = excluded.content_hash,
            updated_at = CURRENT_TIMESTAMP;
    )SQL");

    stmt.bind_int64(1, content.file_id);
    stmt.bind_int64(2, content.workspace_id);
    stmt.bind_text(3, content.content);
    stmt.bind_text(4, content.content_hash);
    stmt.execute();
}

void FtsRepository::delete_file_content(int64_t file_id) {
    Statement stmt(conn_.handle(), "DELETE FROM file_content WHERE file_id = ?;");
    stmt.bind_int64(1, file_id);
    stmt.execute();
}

std::vector<FileSearchResult> FtsRepository::search_files(int64_t workspace_id,
                                                          const std::string& query, int64_t limit,
                                                          int64_t offset) {
    if (query.empty()) {
        return {};
    }

    // Section 5.2: File search query, excluding tombstoned files (fl.is_deleted = 0)
    Statement stmt(conn_.handle(), R"SQL(
        SELECT
            f.file_id,
            snippet(file_search, 0, '<b>', '</b>', '...', 10) AS snippet,
            bm25(file_search) AS rank
        FROM file_search
        JOIN file_content AS f ON f.file_id = file_search.rowid
        JOIN file AS fl ON fl.id = f.file_id
        WHERE file_search MATCH ?
          AND f.workspace_id = ?
          AND fl.is_deleted = 0
        ORDER BY rank
        LIMIT ? OFFSET ?;
    )SQL");

    stmt.bind_text(1, query);
    stmt.bind_int64(2, workspace_id);
    stmt.bind_int64(3, limit);
    stmt.bind_int64(4, offset);

    std::vector<FileSearchResult> results;
    try {
        while (stmt.step()) {
            FileSearchResult res;
            res.file_id = stmt.column_int64(0);
            res.snippet = stmt.column_text(1);
            res.rank = stmt.column_double(2);
            results.push_back(res);
        }
    } catch (const DbError&) {
        // Fallback to phrase search
        stmt.reset();
        stmt.clear_bindings();
        std::string escaped_query;
        escaped_query.reserve(query.size() + 2);
        escaped_query.push_back('"');
        for (char c : query) {
            if (c == '"') {
                escaped_query.push_back('"');
            }
            escaped_query.push_back(c);
        }
        escaped_query.push_back('"');

        stmt.bind_text(1, escaped_query);
        stmt.bind_int64(2, workspace_id);
        stmt.bind_int64(3, limit);
        stmt.bind_int64(4, offset);

        try {
            while (stmt.step()) {
                FileSearchResult res;
                res.file_id = stmt.column_int64(0);
                res.snippet = stmt.column_text(1);
                res.rank = stmt.column_double(2);
                results.push_back(res);
            }
        } catch (const DbError&) {
            return {};
        }
    }

    return results;
}

void FtsRepository::rebuild_symbol_index() {
    conn_.execute("INSERT INTO symbol_search(symbol_search) VALUES('rebuild');");
}

void FtsRepository::rebuild_file_index() {
    conn_.execute("INSERT INTO file_search(file_search) VALUES('rebuild');");
}

} // namespace codelenses
