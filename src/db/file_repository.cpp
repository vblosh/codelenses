#include "codelenses/db/file_repository.hpp"

#include <sstream>

#include "codelenses/db/connection.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/db/transaction.hpp"

namespace codelenses {

namespace {

FileRecord read_file_row(Statement& stmt) {
    FileRecord f;
    f.id = stmt.column_int64(0);
    f.workspace_id = stmt.column_int64(1);
    f.path = stmt.column_text(2);
    f.relative_path = stmt.column_text(3);
    f.name = stmt.column_text(4);
    f.extension = stmt.column_optional_text(5);
    f.language = stmt.column_text(6);
    f.encoding = stmt.column_text(7);
    f.size_bytes = stmt.column_int64(8);
    f.modified_ns = stmt.column_int64(9);
    f.content_hash = stmt.column_optional_text(10);
    f.parse_hash = stmt.column_optional_text(11);
    f.language_version = stmt.column_optional_text(12);
    f.is_binary = stmt.column_bool(13);
    f.is_generated = stmt.column_bool(14);
    f.is_deleted = stmt.column_bool(15);
    f.last_index_job_id = stmt.column_optional_int64(16);
    f.indexed_at = stmt.column_optional_text(17);
    f.created_at = stmt.column_text(18);
    f.updated_at = stmt.column_text(19);
    return f;
}

const char* kFileSelectFields =
    "id, workspace_id, path, relative_path, name, extension, language, encoding, "
    "size_bytes, modified_ns, content_hash, parse_hash, language_version, is_binary, "
    "is_generated, is_deleted, last_index_job_id, indexed_at, created_at, updated_at";

} // namespace

FileRepository::FileRepository(Connection& conn) : conn_(conn) {}

int64_t FileRepository::insert(const FileRecord& file) {
    Statement stmt(conn_.handle(), R"SQL(
        INSERT INTO file (
            workspace_id, path, relative_path, name, extension, language, encoding,
            size_bytes, modified_ns, content_hash, parse_hash, language_version,
            is_binary, is_generated, is_deleted, last_index_job_id, indexed_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL");

    stmt.bind_int64(1, file.workspace_id);
    stmt.bind_text(2, file.path);
    stmt.bind_text(3, file.relative_path);
    stmt.bind_text(4, file.name);
    stmt.bind_optional_text(5, file.extension);
    stmt.bind_text(6, file.language);
    stmt.bind_text(7, file.encoding);
    stmt.bind_int64(8, file.size_bytes);
    stmt.bind_int64(9, file.modified_ns);
    stmt.bind_optional_text(10, file.content_hash);
    stmt.bind_optional_text(11, file.parse_hash);
    stmt.bind_optional_text(12, file.language_version);
    stmt.bind_bool(13, file.is_binary);
    stmt.bind_bool(14, file.is_generated);
    stmt.bind_bool(15, file.is_deleted);
    stmt.bind_optional_int64(16, file.last_index_job_id);
    stmt.bind_optional_text(17, file.indexed_at);

    stmt.execute();
    return conn_.last_insert_rowid();
}

bool FileRepository::update(const FileRecord& file) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE file
        SET path = ?,
            relative_path = ?,
            name = ?,
            extension = ?,
            language = ?,
            encoding = ?,
            size_bytes = ?,
            modified_ns = ?,
            content_hash = ?,
            parse_hash = ?,
            language_version = ?,
            is_binary = ?,
            is_generated = ?,
            is_deleted = ?,
            last_index_job_id = ?,
            indexed_at = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");

    stmt.bind_text(1, file.path);
    stmt.bind_text(2, file.relative_path);
    stmt.bind_text(3, file.name);
    stmt.bind_optional_text(4, file.extension);
    stmt.bind_text(5, file.language);
    stmt.bind_text(6, file.encoding);
    stmt.bind_int64(7, file.size_bytes);
    stmt.bind_int64(8, file.modified_ns);
    stmt.bind_optional_text(9, file.content_hash);
    stmt.bind_optional_text(10, file.parse_hash);
    stmt.bind_optional_text(11, file.language_version);
    stmt.bind_bool(12, file.is_binary);
    stmt.bind_bool(13, file.is_generated);
    stmt.bind_bool(14, file.is_deleted);
    stmt.bind_optional_int64(15, file.last_index_job_id);
    stmt.bind_optional_text(16, file.indexed_at);
    stmt.bind_int64(17, file.id);

    stmt.execute();
    return conn_.changes() > 0;
}

std::optional<FileRecord> FileRepository::get_by_id(int64_t id) {
    std::string sql = "SELECT " + std::string(kFileSelectFields) + " FROM file WHERE id = ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, id);

    if (stmt.step()) {
        return read_file_row(stmt);
    }
    return std::nullopt;
}

std::optional<FileRecord> FileRepository::get_by_path(int64_t workspace_id,
                                                      const std::string& relative_path) {
    std::string sql = "SELECT " + std::string(kFileSelectFields) +
                      " FROM file WHERE workspace_id = ? AND relative_path = ?;";
    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);
    stmt.bind_text(2, relative_path);

    if (stmt.step()) {
        return read_file_row(stmt);
    }
    return std::nullopt;
}

std::vector<FileRecord> FileRepository::list_by_workspace(int64_t workspace_id,
                                                          bool include_deleted) {
    std::string sql =
        "SELECT " + std::string(kFileSelectFields) + " FROM file WHERE workspace_id = ?";
    if (!include_deleted) {
        sql += " AND is_deleted = 0";
    }
    sql += " ORDER BY relative_path ASC;";

    Statement stmt(conn_.handle(), sql);
    stmt.bind_int64(1, workspace_id);

    std::vector<FileRecord> results;
    while (stmt.step()) {
        results.push_back(read_file_row(stmt));
    }
    return results;
}

int64_t FileRepository::count_by_workspace(int64_t workspace_id) {
    Statement stmt(conn_.handle(),
                   "SELECT COUNT(*) FROM file WHERE workspace_id = ? AND is_deleted = 0;");
    stmt.bind_int64(1, workspace_id);
    return stmt.step() ? stmt.column_int64(0) : 0;
}

std::vector<FileLanguageCount> FileRepository::count_by_language(int64_t workspace_id) {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT language, COUNT(*) AS file_count
        FROM file
        WHERE workspace_id = ? AND is_deleted = 0
        GROUP BY language
        ORDER BY file_count DESC, language ASC;
    )SQL");
    stmt.bind_int64(1, workspace_id);

    std::vector<FileLanguageCount> results;
    while (stmt.step()) {
        results.push_back(FileLanguageCount{
            .language = stmt.column_text(0),
            .file_count = stmt.column_int64(1),
        });
    }
    return results;
}

std::vector<FileStateItem> FileRepository::get_file_states(int64_t workspace_id) {
    Statement stmt(conn_.handle(), R"SQL(
        SELECT id, relative_path, size_bytes, modified_ns, content_hash, is_deleted, language
        FROM file
        WHERE workspace_id = ?
        ORDER BY relative_path ASC;
    )SQL");
    stmt.bind_int64(1, workspace_id);

    std::vector<FileStateItem> results;
    while (stmt.step()) {
        FileStateItem item;
        item.id = stmt.column_int64(0);
        item.relative_path = stmt.column_text(1);
        item.size_bytes = stmt.column_int64(2);
        item.modified_ns = stmt.column_int64(3);
        item.content_hash = stmt.column_optional_text(4);
        item.is_deleted = stmt.column_bool(5);
        item.language = stmt.column_text(6);
        results.push_back(item);
    }
    return results;
}

bool FileRepository::mark_deleted(int64_t file_id) {
    Statement stmt(conn_.handle(), R"SQL(
        UPDATE file
        SET is_deleted = 1,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");
    stmt.bind_int64(1, file_id);
    stmt.execute();
    return conn_.changes() > 0;
}

int64_t FileRepository::mark_missing_as_deleted(int64_t workspace_id,
                                                const std::vector<int64_t>& keep_file_ids) {
    Transaction tx(conn_, TransactionType::immediate);

    // Section 7: Use a temporary table for discovered_file_ids rather than a large SQL parameter
    // list.
    conn_.execute("CREATE TEMP TABLE IF NOT EXISTS temp_discovered_ids (id INTEGER PRIMARY KEY);");
    conn_.execute("DELETE FROM temp_discovered_ids;");

    {
        Statement insert_stmt(conn_.handle(),
                              "INSERT OR IGNORE INTO temp_discovered_ids (id) VALUES (?);");
        for (int64_t id : keep_file_ids) {
            insert_stmt.bind_int64(1, id);
            insert_stmt.execute();
            insert_stmt.reset();
        }
    }

    Statement update_stmt(conn_.handle(), R"SQL(
        UPDATE file
        SET is_deleted = 1,
            updated_at = CURRENT_TIMESTAMP
        WHERE workspace_id = ?
          AND id NOT IN (SELECT id FROM temp_discovered_ids)
          AND is_deleted = 0;
    )SQL");
    update_stmt.bind_int64(1, workspace_id);
    update_stmt.execute();

    int modified = conn_.changes();

    conn_.execute("DELETE FROM temp_discovered_ids;");
    tx.commit();

    return modified;
}

int64_t FileRepository::cleanup_deleted_files_derived_data(int64_t workspace_id) {
    // Section 7: Deleted-file cleanup
    Transaction tx(conn_, TransactionType::immediate);

    Statement del_occ(conn_.handle(), R"SQL(
        DELETE FROM occurrence
        WHERE file_id IN (
            SELECT id FROM file
            WHERE workspace_id = ? AND is_deleted = 1
        );
    )SQL");
    del_occ.bind_int64(1, workspace_id);
    del_occ.execute();

    Statement del_ref(conn_.handle(), R"SQL(
        DELETE FROM reference_occurrence
        WHERE source_file_id IN (
            SELECT id FROM file
            WHERE workspace_id = ? AND is_deleted = 1
        );
    )SQL");
    del_ref.bind_int64(1, workspace_id);
    del_ref.execute();

    Statement del_dep(conn_.handle(), R"SQL(
        DELETE FROM file_dependency
        WHERE source_file_id IN (
            SELECT id FROM file
            WHERE workspace_id = ? AND is_deleted = 1
        );
    )SQL");
    del_dep.bind_int64(1, workspace_id);
    del_dep.execute();

    Statement del_rel(conn_.handle(), R"SQL(
        DELETE FROM symbol_relation
        WHERE source_symbol_id IN (
            SELECT s.id FROM symbol s
            JOIN file f ON f.id = s.file_id
            WHERE f.workspace_id = ? AND f.is_deleted = 1
        ) OR target_symbol_id IN (
            SELECT s.id FROM symbol s
            JOIN file f ON f.id = s.file_id
            WHERE f.workspace_id = ? AND f.is_deleted = 1
        );
    )SQL");
    del_rel.bind_int64(1, workspace_id);
    del_rel.execute();

    Statement del_sym(conn_.handle(), R"SQL(
        DELETE FROM symbol
        WHERE workspace_id = ? AND file_id IN (
            SELECT id FROM file
            WHERE workspace_id = ? AND is_deleted = 1
        );
    )SQL");
    del_sym.bind_int64(1, workspace_id);
    del_sym.bind_int64(2, workspace_id);
    del_sym.execute();

    Statement del_content(conn_.handle(), R"SQL(
        DELETE FROM file_content
        WHERE file_id IN (
            SELECT id FROM file
            WHERE workspace_id = ? AND is_deleted = 1
        );
    )SQL");
    del_content.bind_int64(1, workspace_id);
    del_content.execute();

    tx.commit();
    return 1;
}

int64_t FileRepository::delete_tombstones(int64_t workspace_id) {
    Statement stmt(conn_.handle(), "DELETE FROM file WHERE workspace_id = ? AND is_deleted = 1;");
    stmt.bind_int64(1, workspace_id);
    stmt.execute();
    return conn_.changes();
}

bool FileRepository::delete_by_id(int64_t id) {
    Statement stmt(conn_.handle(), "DELETE FROM file WHERE id = ?;");
    stmt.bind_int64(1, id);
    stmt.execute();
    return conn_.changes() > 0;
}

} // namespace codelenses
