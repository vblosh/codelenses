#include "codelenses/treesitter/parser.hpp"

#include <cstring>
#include <stop_token>

namespace codelenses::treesitter {

Result<Query> Query::create(const TSLanguage* language, std::string_view source) {
    if (!language) {
        return unexpected_result<Query>(ErrorCode::invalid_argument,
                                        "cannot create query with null language");
    }

    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;

    TSQuery* query = ts_query_new(language, source.data(), static_cast<uint32_t>(source.size()),
                                  &error_offset, &error_type);
    if (!query) {
        std::string err_desc;
        switch (error_type) {
        case TSQueryErrorSyntax:
            err_desc = "syntax error";
            break;
        case TSQueryErrorNodeType:
            err_desc = "invalid node type";
            break;
        case TSQueryErrorField:
            err_desc = "invalid field name";
            break;
        case TSQueryErrorCapture:
            err_desc = "invalid capture name";
            break;
        case TSQueryErrorStructure:
            err_desc = "invalid structure";
            break;
        case TSQueryErrorLanguage:
            err_desc = "incompatible language";
            break;
        default:
            err_desc = "unknown query error";
            break;
        }
        return unexpected_result<Query>(ErrorCode::invalid_argument,
                                        "failed to create query at byte offset " +
                                            std::to_string(error_offset) + ": " + err_desc);
    }

    return Query(query);
}

Parser::Parser() : parser_(ts_parser_new()) {}

Parser::~Parser() {
    if (parser_) {
        ts_parser_delete(parser_);
    }
}

Parser::Parser(Parser&& other) noexcept : parser_(other.parser_) {
    other.parser_ = nullptr;
}

Parser& Parser::operator=(Parser&& other) noexcept {
    if (this != &other) {
        if (parser_) {
            ts_parser_delete(parser_);
        }
        parser_ = other.parser_;
        other.parser_ = nullptr;
    }
    return *this;
}

Result<void> Parser::set_language(const TSLanguage* language) {
    if (!parser_) {
        return unexpected_result<void>(ErrorCode::failed, "parser is null");
    }
    if (!language) {
        return unexpected_result<void>(ErrorCode::invalid_argument, "language is null");
    }
    if (!ts_parser_set_language(parser_, language)) {
        return unexpected_result<void>(
            ErrorCode::invalid_argument,
            "tree-sitter language version mismatch or incompatible grammar");
    }
    return {};
}

const TSLanguage* Parser::language() const noexcept {
    return parser_ ? ts_parser_language(parser_) : nullptr;
}

void Parser::reset() noexcept {
    if (parser_) {
        ts_parser_reset(parser_);
    }
}

namespace {

struct StopContext {
    const std::stop_token* stop_token{nullptr};
};

bool progress_callback(TSParseState* state) {
    if (state && state->payload) {
        auto* ctx = static_cast<StopContext*>(state->payload);
        if (ctx->stop_token && ctx->stop_token->stop_requested()) {
            return true; // cancel parse
        }
    }
    return false;
}

struct SourcePayload {
    std::string_view source;
};

const char* source_read(void* payload, uint32_t byte_index, TSPoint /*position*/,
                        uint32_t* bytes_read) {
    auto* src = static_cast<SourcePayload*>(payload);
    if (byte_index >= src->source.size()) {
        *bytes_read = 0;
        return nullptr;
    }
    *bytes_read = static_cast<uint32_t>(src->source.size() - byte_index);
    return src->source.data() + byte_index;
}

} // namespace

Result<Tree> Parser::parse_string(std::string_view source, const Tree* old_tree,
                                  const std::stop_token& stop_token) {
    if (!parser_) {
        return unexpected_result<Tree>(ErrorCode::failed, "parser is null");
    }
    if (!language()) {
        return unexpected_result<Tree>(ErrorCode::invalid_argument, "no language set on parser");
    }

    if (stop_token.stop_requested()) {
        return unexpected_result<Tree>(ErrorCode::cancelled, "parsing cancelled before start");
    }

    StopContext stop_ctx{.stop_token = &stop_token};
    TSParseOptions options{
        .payload = &stop_ctx,
        .progress_callback = progress_callback,
    };

    SourcePayload payload{.source = source};
    TSInput input{
        .payload = &payload,
        .read = source_read,
        .encoding = TSInputEncodingUTF8,
        .decode = nullptr,
    };

    TSTree* raw_tree =
        ts_parser_parse_with_options(parser_, old_tree ? old_tree->raw() : nullptr, input, options);

    if (stop_token.stop_requested()) {
        if (raw_tree) {
            ts_tree_delete(raw_tree);
        }
        ts_parser_reset(parser_);
        return unexpected_result<Tree>(ErrorCode::cancelled, "parsing was cancelled");
    }

    if (!raw_tree) {
        ts_parser_reset(parser_);
        return unexpected_result<Tree>(ErrorCode::failed,
                                       "tree-sitter failed to produce syntax tree");
    }

    return Tree(raw_tree);
}

} // namespace codelenses::treesitter
