#include "codelenses/db/database.hpp"

#include <unordered_map>

#include "codelenses/db/error.hpp"
#include "codelenses/db/statement.hpp"

namespace codelenses {

std::unique_ptr<Database> Database::open(const std::string& path, bool apply_migrations) {
    auto conn = Connection::open(path);
    auto db = std::make_unique<Database>(std::move(conn));
    if (apply_migrations) {
        db->migration_runner().apply_pending(db->connection());
    }
    return db;
}

std::unique_ptr<Database> Database::open_reader(const std::string& path) {
    return std::make_unique<Database>(Connection::open_readonly(path));
}

std::unique_ptr<Database> Database::open_reader() const {
    const auto& path = conn_->path();
    if (path.empty()) {
        return nullptr;
    }
    return Database::open_reader(path);
}

std::unique_ptr<Database> Database::open_memory(bool apply_migrations) {
    auto conn = Connection::open_memory();
    auto db = std::make_unique<Database>(std::move(conn));
    if (apply_migrations) {
        db->migration_runner().apply_pending(db->connection());
    }
    return db;
}

Database::Database(std::unique_ptr<Connection> conn)
    : conn_(std::move(conn)), workspaces_(*conn_), files_(*conn_), symbols_(*conn_),
      occurrences_(*conn_), references_(*conn_), relations_(*conn_), dependencies_(*conn_),
      diagnostics_(*conn_), jobs_(*conn_), fts_(*conn_), workspace_settings_(*conn_) {}

void Database::replace_file_index(int64_t file_id, const FileIndexData& data) {
    // Section 6: Transactional file replacement
    Transaction tx(*conn_, TransactionType::immediate);

    // 1. Update file metadata
    Statement update_file(conn_->handle(), R"SQL(
        UPDATE file
        SET size_bytes = ?,
            modified_ns = ?,
            content_hash = ?,
            parse_hash = ?,
            language = ?,
            is_binary = ?,
            is_deleted = 0,
            last_index_job_id = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");
    update_file.bind_int64(1, data.size_bytes);
    update_file.bind_int64(2, data.modified_ns);
    update_file.bind_optional_text(3, data.content_hash);
    update_file.bind_optional_text(4, data.parse_hash);
    update_file.bind_text(5, data.language);
    update_file.bind_bool(6, data.is_binary);
    update_file.bind_optional_int64(7, data.last_index_job_id);
    update_file.bind_int64(8, file_id);
    update_file.execute();

    // 2. Remove derived records before inserting the new extraction
    diagnostics_.delete_by_file(file_id);

    Statement del_occ(conn_->handle(), "DELETE FROM occurrence WHERE file_id = ?;");
    del_occ.bind_int64(1, file_id);
    del_occ.execute();

    Statement del_ref(conn_->handle(),
                      "DELETE FROM reference_occurrence WHERE source_file_id = ?;");
    del_ref.bind_int64(1, file_id);
    del_ref.execute();

    Statement del_dep(conn_->handle(), "DELETE FROM file_dependency WHERE source_file_id = ?;");
    del_dep.bind_int64(1, file_id);
    del_dep.execute();

    Statement del_rel(conn_->handle(), R"SQL(
        DELETE FROM symbol_relation
        WHERE source_symbol_id IN (SELECT id FROM symbol WHERE file_id = ?)
           OR target_symbol_id IN (SELECT id FROM symbol WHERE file_id = ?);
    )SQL");
    del_rel.bind_int64(1, file_id);
    del_rel.bind_int64(2, file_id);
    del_rel.execute();

    Statement del_sym(conn_->handle(), "DELETE FROM symbol WHERE file_id = ?;");
    del_sym.bind_int64(1, file_id);
    del_sym.execute();

    // 3. Insert symbols and record old_id -> new_id mapping
    std::unordered_map<int64_t, int64_t> symbol_id_map;
    std::vector<int64_t> new_symbol_ids;
    new_symbol_ids.reserve(data.symbols.size());

    for (size_t i = 0; i < data.symbols.size(); ++i) {
        Symbol sym = data.symbols[i];
        int64_t old_id = sym.id;
        sym.scope_symbol_id = std::nullopt; // defer scope linkage until all symbols are inserted
        int64_t new_id = symbols_.insert(sym);
        new_symbol_ids.push_back(new_id);
        if (old_id != 0) {
            symbol_id_map[old_id] = new_id;
        } else {
            symbol_id_map[static_cast<int64_t>(i)] = new_id;
        }
    }

    // Remap and update scope_symbol_id on symbols that have a scope
    Statement update_scope(conn_->handle(), "UPDATE symbol SET scope_symbol_id = ? WHERE id = ?;");
    for (size_t i = 0; i < data.symbols.size(); ++i) {
        if (data.symbols[i].scope_symbol_id.has_value()) {
            int64_t old_scope = *data.symbols[i].scope_symbol_id;
            auto it = symbol_id_map.find(old_scope);
            int64_t new_scope = (it != symbol_id_map.end()) ? it->second : old_scope;

            update_scope.reset();
            update_scope.clear_bindings();
            update_scope.bind_int64(1, new_scope);
            update_scope.bind_int64(2, new_symbol_ids[i]);
            update_scope.execute();
        }
    }

    // Remap and insert occurrences
    auto occurrences = data.occurrences;
    for (auto& occ : occurrences) {
        if (occ.symbol_id.has_value()) {
            auto it = symbol_id_map.find(*occ.symbol_id);
            if (it != symbol_id_map.end()) {
                occ.symbol_id = it->second;
            }
        }
    }
    occurrences_.insert_batch(occurrences);

    // Remap and insert references
    auto references = data.references;
    for (auto& ref : references) {
        if (ref.source_symbol_id.has_value()) {
            auto it = symbol_id_map.find(*ref.source_symbol_id);
            if (it != symbol_id_map.end()) {
                ref.source_symbol_id = it->second;
            }
        }
        if (ref.target_symbol_id.has_value()) {
            auto it = symbol_id_map.find(*ref.target_symbol_id);
            if (it != symbol_id_map.end()) {
                ref.target_symbol_id = it->second;
            }
        }
    }
    references_.insert_batch(references);

    // Remap and insert relations
    auto relations = data.relations;
    for (auto& rel : relations) {
        auto it_src = symbol_id_map.find(rel.source_symbol_id);
        if (it_src != symbol_id_map.end()) {
            rel.source_symbol_id = it_src->second;
        }
        if (rel.target_symbol_id.has_value()) {
            auto it_tgt = symbol_id_map.find(*rel.target_symbol_id);
            if (it_tgt != symbol_id_map.end()) {
                rel.target_symbol_id = it_tgt->second;
            }
        }
    }
    relations_.insert_batch(relations);

    dependencies_.insert_batch(data.dependencies);
    diagnostics_.insert_batch(data.diagnostics);

    // 4. Update file indexed_at
    Statement finish_file(conn_->handle(), R"SQL(
        UPDATE file
        SET indexed_at = CURRENT_TIMESTAMP,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");
    finish_file.bind_int64(1, file_id);
    finish_file.execute();

    tx.commit();
}

} // namespace codelenses
