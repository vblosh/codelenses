#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/range.hpp"
#include "codelenses/result.hpp"
#include <tree_sitter/api.h>

namespace codelenses::treesitter {

class Tree;
class Parser;
class Query;
class QueryCursor;

// Thin C++ value wrapper around TSNode.
// TSNode is a value type containing a pointer to its tree and internal identifiers.
class Node {
public:
    constexpr Node() noexcept : node_{} {}
    constexpr explicit Node(TSNode raw) noexcept : node_(raw) {}

    [[nodiscard]] bool is_null() const noexcept { return ts_node_is_null(node_); }
    [[nodiscard]] explicit operator bool() const noexcept { return !is_null(); }

    [[nodiscard]] std::string_view type() const noexcept {
        if (is_null())
            return "";
        const char* t = ts_node_type(node_);
        return t ? std::string_view(t) : std::string_view{};
    }

    [[nodiscard]] TSSymbol symbol() const noexcept { return is_null() ? 0 : ts_node_symbol(node_); }

    [[nodiscard]] bool is_named() const noexcept { return !is_null() && ts_node_is_named(node_); }

    [[nodiscard]] bool is_error() const noexcept { return !is_null() && ts_node_is_error(node_); }

    [[nodiscard]] bool has_error() const noexcept { return !is_null() && ts_node_has_error(node_); }

    [[nodiscard]] bool is_missing() const noexcept {
        return !is_null() && ts_node_is_missing(node_);
    }

    [[nodiscard]] bool is_extra() const noexcept { return !is_null() && ts_node_is_extra(node_); }

    [[nodiscard]] uint32_t start_byte() const noexcept {
        return is_null() ? 0 : ts_node_start_byte(node_);
    }

    [[nodiscard]] uint32_t end_byte() const noexcept {
        return is_null() ? 0 : ts_node_end_byte(node_);
    }

    [[nodiscard]] ByteRange byte_range() const noexcept {
        return ByteRange{start_byte(), end_byte()};
    }

    [[nodiscard]] TSPoint start_point() const noexcept {
        return is_null() ? TSPoint{0, 0} : ts_node_start_point(node_);
    }

    [[nodiscard]] TSPoint end_point() const noexcept {
        return is_null() ? TSPoint{0, 0} : ts_node_end_point(node_);
    }

    // Display coordinates are one-based (row + 1, column + 1).
    [[nodiscard]] DisplayPosition start_position() const noexcept {
        const auto pt = start_point();
        return DisplayPosition{pt.row + 1, pt.column + 1};
    }

    [[nodiscard]] DisplayPosition end_position() const noexcept {
        const auto pt = end_point();
        return DisplayPosition{pt.row + 1, pt.column + 1};
    }

    [[nodiscard]] DisplayRange display_range() const noexcept {
        const auto sp = start_position();
        const auto ep = end_position();
        return DisplayRange{sp.line, sp.column, ep.line, ep.column};
    }

    [[nodiscard]] uint32_t child_count() const noexcept {
        return is_null() ? 0 : ts_node_child_count(node_);
    }

    [[nodiscard]] Node child(uint32_t index) const noexcept {
        return is_null() ? Node{} : Node{ts_node_child(node_, index)};
    }

    [[nodiscard]] uint32_t named_child_count() const noexcept {
        return is_null() ? 0 : ts_node_named_child_count(node_);
    }

    [[nodiscard]] Node named_child(uint32_t index) const noexcept {
        return is_null() ? Node{} : Node{ts_node_named_child(node_, index)};
    }

    [[nodiscard]] Node child_by_field_name(std::string_view field_name) const noexcept {
        if (is_null() || field_name.empty())
            return Node{};
        return Node{ts_node_child_by_field_name(node_, field_name.data(),
                                                static_cast<uint32_t>(field_name.size()))};
    }

    [[nodiscard]] Node parent() const noexcept {
        return is_null() ? Node{} : Node{ts_node_parent(node_)};
    }

    [[nodiscard]] Node next_sibling() const noexcept {
        return is_null() ? Node{} : Node{ts_node_next_sibling(node_)};
    }

    [[nodiscard]] Node prev_sibling() const noexcept {
        return is_null() ? Node{} : Node{ts_node_prev_sibling(node_)};
    }

    [[nodiscard]] Node next_named_sibling() const noexcept {
        return is_null() ? Node{} : Node{ts_node_next_named_sibling(node_)};
    }

    [[nodiscard]] Node prev_named_sibling() const noexcept {
        return is_null() ? Node{} : Node{ts_node_prev_named_sibling(node_)};
    }

    [[nodiscard]] std::string_view text(std::string_view source) const noexcept {
        const auto sb = start_byte();
        const auto eb = end_byte();
        if (sb <= eb && eb <= source.size()) {
            return source.substr(sb, eb - sb);
        }
        return {};
    }

    [[nodiscard]] TSNode raw() const noexcept { return node_; }

    friend bool operator==(const Node& lhs, const Node& rhs) noexcept {
        return ts_node_eq(lhs.node_, rhs.node_);
    }

private:
    TSNode node_{};
};

// RAII wrapper managing TSTree* lifetime.
class Tree {
public:
    Tree() noexcept = default;
    explicit Tree(TSTree* tree) noexcept : tree_(tree) {}

    ~Tree() {
        if (tree_) {
            ts_tree_delete(tree_);
        }
    }

    Tree(const Tree& other) : tree_(other.tree_ ? ts_tree_copy(other.tree_) : nullptr) {}

    Tree& operator=(const Tree& other) {
        if (this != &other) {
            if (tree_) {
                ts_tree_delete(tree_);
            }
            tree_ = other.tree_ ? ts_tree_copy(other.tree_) : nullptr;
        }
        return *this;
    }

    Tree(Tree&& other) noexcept : tree_(other.tree_) { other.tree_ = nullptr; }

    Tree& operator=(Tree&& other) noexcept {
        if (this != &other) {
            if (tree_) {
                ts_tree_delete(tree_);
            }
            tree_ = other.tree_;
            other.tree_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return tree_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] Node root_node() const noexcept {
        if (!tree_)
            return Node{};
        return Node{ts_tree_root_node(tree_)};
    }

    [[nodiscard]] const TSLanguage* language() const noexcept {
        return tree_ ? ts_tree_language(tree_) : nullptr;
    }

    [[nodiscard]] TSTree* raw() const noexcept { return tree_; }

    [[nodiscard]] TSTree* release() noexcept {
        TSTree* result = tree_;
        tree_ = nullptr;
        return result;
    }

private:
    TSTree* tree_{nullptr};
};

// RAII wrapper managing TSQuery* lifetime.
class Query {
public:
    Query() noexcept = default;
    explicit Query(TSQuery* query) noexcept : query_(query) {}

    ~Query() {
        if (query_) {
            ts_query_delete(query_);
        }
    }

    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    Query(Query&& other) noexcept : query_(other.query_) { other.query_ = nullptr; }

    Query& operator=(Query&& other) noexcept {
        if (this != &other) {
            if (query_) {
                ts_query_delete(query_);
            }
            query_ = other.query_;
            other.query_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] static Result<Query> create(const TSLanguage* language, std::string_view source);

    [[nodiscard]] bool valid() const noexcept { return query_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] uint32_t pattern_count() const noexcept {
        return query_ ? ts_query_pattern_count(query_) : 0;
    }

    [[nodiscard]] uint32_t capture_count() const noexcept {
        return query_ ? ts_query_capture_count(query_) : 0;
    }

    [[nodiscard]] uint32_t string_count() const noexcept {
        return query_ ? ts_query_string_count(query_) : 0;
    }

    [[nodiscard]] std::string_view capture_name(uint32_t index) const noexcept {
        if (!query_)
            return {};
        uint32_t length = 0;
        const char* name = ts_query_capture_name_for_id(query_, index, &length);
        return (name && length > 0) ? std::string_view(name, length) : std::string_view{};
    }

    [[nodiscard]] TSQuery* raw() const noexcept { return query_; }

private:
    TSQuery* query_{nullptr};
};

// RAII wrapper managing TSQueryCursor* lifetime.
class QueryCursor {
public:
    QueryCursor() : cursor_(ts_query_cursor_new()) {}
    explicit QueryCursor(TSQueryCursor* cursor) noexcept : cursor_(cursor) {}

    ~QueryCursor() {
        if (cursor_) {
            ts_query_cursor_delete(cursor_);
        }
    }

    QueryCursor(const QueryCursor&) = delete;
    QueryCursor& operator=(const QueryCursor&) = delete;

    QueryCursor(QueryCursor&& other) noexcept : cursor_(other.cursor_) { other.cursor_ = nullptr; }

    QueryCursor& operator=(QueryCursor&& other) noexcept {
        if (this != &other) {
            if (cursor_) {
                ts_query_cursor_delete(cursor_);
            }
            cursor_ = other.cursor_;
            other.cursor_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return cursor_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    void exec(const Query& query, const Node& node) {
        if (cursor_ && query.valid() && node) {
            ts_query_cursor_exec(cursor_, query.raw(), node.raw());
        }
    }

    [[nodiscard]] bool next_match(TSQueryMatch& match) {
        if (!cursor_)
            return false;
        return ts_query_cursor_next_match(cursor_, &match);
    }

    [[nodiscard]] bool next_capture(TSQueryMatch& match, uint32_t& capture_index) {
        if (!cursor_)
            return false;
        return ts_query_cursor_next_capture(cursor_, &match, &capture_index);
    }

    [[nodiscard]] bool set_byte_range(uint32_t start_byte, uint32_t end_byte) {
        if (!cursor_)
            return false;
        return ts_query_cursor_set_byte_range(cursor_, start_byte, end_byte);
    }

    [[nodiscard]] bool set_point_range(TSPoint start_point, TSPoint end_point) {
        if (!cursor_)
            return false;
        return ts_query_cursor_set_point_range(cursor_, start_point, end_point);
    }

    [[nodiscard]] TSQueryCursor* raw() const noexcept { return cursor_; }

private:
    TSQueryCursor* cursor_{nullptr};
};

// RAII wrapper managing TSParser* lifetime and parsing execution.
class Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    Parser(Parser&& other) noexcept;
    Parser& operator=(Parser&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return parser_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] Result<void> set_language(const TSLanguage* language);
    [[nodiscard]] const TSLanguage* language() const noexcept;

    void reset() noexcept;

    // Set explicit cancellation flag pointer (polled periodically by Tree-sitter).

    // Parse source string with optional cooperative cancellation via std::stop_token.
    [[nodiscard]] Result<Tree> parse_string(std::string_view source, const Tree* old_tree = nullptr,
                                            const std::stop_token& stop_token = {});

    [[nodiscard]] TSParser* raw() const noexcept { return parser_; }

private:
    TSParser* parser_{nullptr};
};

} // namespace codelenses::treesitter
