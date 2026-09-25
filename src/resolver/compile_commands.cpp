#include "codelenses/resolver/compile_commands.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace codelenses::resolver {

Result<std::vector<std::string>>
CompilationDatabase::tokenize_command_safely(std::string_view command) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];

        if (escaped) {
            current += c;
            escaped = false;
            continue;
        }

        if (c == '\\' && !in_single_quote) {
            escaped = true;
            continue;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !in_single_quote && !in_double_quote) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current += c;
    }

    if (in_single_quote || in_double_quote) {
        return unexpected_result<std::vector<std::string>>(ErrorCode::invalid_argument,
                                                           "unclosed quote in command string");
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

void CompilationDatabase::add_entry(CompileCommand entry) {
    entries_.push_back(std::move(entry));
}

const CompileCommand*
CompilationDatabase::find_for_file(const std::filesystem::path& file_path) const {
    auto target_filename = file_path.filename();
    auto target_norm = file_path.lexically_normal();

    // 1. Exact match on normalized path
    for (const auto& entry : entries_) {
        if (entry.file.lexically_normal() == target_norm) {
            return &entry;
        }
    }

    // 2. Suffix match (e.g. "src/main.cpp" matches "/abs/path/src/main.cpp")
    auto target_str = target_norm.generic_string();
    for (const auto& entry : entries_) {
        auto entry_str = entry.file.lexically_normal().generic_string();
        if (entry_str == target_str ||
            (entry_str.size() > target_str.size() && entry_str.ends_with("/" + target_str))) {
            return &entry;
        }
        if (target_str.size() > entry_str.size() && target_str.ends_with("/" + entry_str)) {
            return &entry;
        }
    }

    // 3. Fallback: match by filename only if unique to avoid mis-attributing commands
    const CompileCommand* unique_filename_match = nullptr;
    std::size_t filename_matches = 0;
    for (const auto& entry : entries_) {
        if (entry.file.filename() == target_filename) {
            unique_filename_match = &entry;
            filename_matches++;
        }
    }
    if (filename_matches == 1) {
        return unique_filename_match;
    }

    return nullptr;
}

Result<CompilationDatabase>
CompilationDatabase::parse_json(std::string_view json_content,
                                const std::filesystem::path& workspace_root) {
    nlohmann::json root_json;
    try {
        root_json = nlohmann::json::parse(json_content);
    } catch (const std::exception& e) {
        return unexpected_result<CompilationDatabase>(
            ErrorCode::invalid_argument, std::string("failed to parse JSON: ") + e.what());
    }

    if (!root_json.is_array()) {
        return unexpected_result<CompilationDatabase>(
            ErrorCode::invalid_argument,
            "compilation database JSON must be an array of command objects");
    }

    CompilationDatabase db;

    for (const auto& item : root_json) {
        if (!item.is_object())
            continue;

        CompileCommand entry;

        std::filesystem::path dir_path;
        if (item.contains("directory") && item["directory"].is_string()) {
            dir_path = item["directory"].get<std::string>();
            if (dir_path.is_relative() && !workspace_root.empty()) {
                dir_path = (workspace_root / dir_path).lexically_normal();
            }
        } else if (!workspace_root.empty()) {
            dir_path = workspace_root;
        }
        entry.directory = dir_path;

        if (item.contains("file") && item["file"].is_string()) {
            std::filesystem::path f = item["file"].get<std::string>();
            if (f.is_relative() && !dir_path.empty()) {
                f = (dir_path / f).lexically_normal();
            }
            if (!workspace_root.empty()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(f, workspace_root, ec);
                if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                    entry.file = rel;
                } else {
                    entry.file = f;
                }
            } else {
                entry.file = f;
            }
        }

        if (item.contains("output") && item["output"].is_string()) {
            std::filesystem::path out_p = item["output"].get<std::string>();
            if (out_p.is_relative() && !dir_path.empty()) {
                out_p = (dir_path / out_p).lexically_normal();
            }
            if (!workspace_root.empty()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(out_p, workspace_root, ec);
                if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                    entry.output = rel;
                } else {
                    entry.output = out_p;
                }
            } else {
                entry.output = out_p;
            }
        }

        std::vector<std::string> args;
        if (item.contains("arguments") && item["arguments"].is_array()) {
            entry.parsed_from_arguments = true;
            for (const auto& arg : item["arguments"]) {
                if (arg.is_string()) {
                    args.push_back(arg.get<std::string>());
                }
            }
        } else if (item.contains("command") && item["command"].is_string()) {
            entry.parsed_from_arguments = false;
            auto tokenized = tokenize_command_safely(item["command"].get<std::string>());
            if (tokenized) {
                args = std::move(*tokenized);
            }
        }
        entry.arguments = args;

        // Parse GCC/Clang flags from arguments
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];

            // Include directories
            if (arg == "-I" || arg == "-isystem" || arg == "-iquote" || arg == "-idirafter") {
                if (i + 1 < args.size()) {
                    std::filesystem::path inc_path = args[++i];
                    if (inc_path.is_relative() && !dir_path.empty()) {
                        inc_path = (dir_path / inc_path).lexically_normal();
                    }
                    if (!workspace_root.empty()) {
                        std::error_code ec;
                        auto rel = std::filesystem::relative(inc_path, workspace_root, ec);
                        if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                            entry.include_dirs.push_back(rel);
                        } else {
                            entry.include_dirs.push_back(inc_path);
                        }
                    } else {
                        entry.include_dirs.push_back(inc_path);
                    }
                }
            } else if (arg.starts_with("-I") && arg.size() > 2) {
                std::filesystem::path inc_path = arg.substr(2);
                if (inc_path.is_relative() && !dir_path.empty()) {
                    inc_path = (dir_path / inc_path).lexically_normal();
                }
                if (!workspace_root.empty()) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(inc_path, workspace_root, ec);
                    if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                        entry.include_dirs.push_back(rel);
                    } else {
                        entry.include_dirs.push_back(inc_path);
                    }
                } else {
                    entry.include_dirs.push_back(inc_path);
                }
            } else if (arg.starts_with("-isystem") && arg.size() > 8) {
                std::filesystem::path inc_path = arg.substr(8);
                if (inc_path.is_relative() && !dir_path.empty()) {
                    inc_path = (dir_path / inc_path).lexically_normal();
                }
                if (!workspace_root.empty()) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(inc_path, workspace_root, ec);
                    if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                        entry.include_dirs.push_back(rel);
                    } else {
                        entry.include_dirs.push_back(inc_path);
                    }
                } else {
                    entry.include_dirs.push_back(inc_path);
                }
            } else if (arg.starts_with("-iquote") && arg.size() > 7) {
                std::filesystem::path inc_path = arg.substr(7);
                if (inc_path.is_relative() && !dir_path.empty()) {
                    inc_path = (dir_path / inc_path).lexically_normal();
                }
                if (!workspace_root.empty()) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(inc_path, workspace_root, ec);
                    if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                        entry.include_dirs.push_back(rel);
                    } else {
                        entry.include_dirs.push_back(inc_path);
                    }
                } else {
                    entry.include_dirs.push_back(inc_path);
                }
            } else if (arg.starts_with("-idirafter") && arg.size() > 10) {
                std::filesystem::path inc_path = arg.substr(10);
                if (inc_path.is_relative() && !dir_path.empty()) {
                    inc_path = (dir_path / inc_path).lexically_normal();
                }
                if (!workspace_root.empty()) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(inc_path, workspace_root, ec);
                    if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                        entry.include_dirs.push_back(rel);
                    } else {
                        entry.include_dirs.push_back(inc_path);
                    }
                } else {
                    entry.include_dirs.push_back(inc_path);
                }
            }
            // Defines (-D, -U)
            else if (arg == "-D") {
                if (i + 1 < args.size()) {
                    entry.defines.push_back(args[++i]);
                }
            } else if (arg.starts_with("-D") && arg.size() > 2) {
                entry.defines.push_back(arg.substr(2));
            } else if (arg == "-U") {
                if (i + 1 < args.size()) {
                    entry.defines.push_back("-U" + args[++i]);
                }
            } else if (arg.starts_with("-U") && arg.size() > 2) {
                entry.defines.push_back("-U" + arg.substr(2));
            }
            // Language standard
            else if (arg.starts_with("-std=")) {
                entry.language_standard = arg.substr(5);
            } else if (arg.starts_with("--std=")) {
                entry.language_standard = arg.substr(6);
            }
            // Output (-o)
            else if (arg == "-o") {
                if (i + 1 < args.size() && !entry.output.has_value()) {
                    std::filesystem::path out_file = args[++i];
                    if (out_file.is_relative() && !dir_path.empty()) {
                        out_file = (dir_path / out_file).lexically_normal();
                    }
                    if (!workspace_root.empty()) {
                        std::error_code ec;
                        auto rel = std::filesystem::relative(out_file, workspace_root, ec);
                        if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
                            entry.output = rel;
                        } else {
                            entry.output = out_file;
                        }
                    } else {
                        entry.output = out_file;
                    }
                }
            }
        }

        db.add_entry(std::move(entry));
    }

    return db;
}

Result<CompilationDatabase>
CompilationDatabase::load_file(const std::filesystem::path& file_path,
                               const std::filesystem::path& workspace_root) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return unexpected_result<CompilationDatabase>(ErrorCode::not_found,
                                                      "could not open compilation database file: " +
                                                          file_path.string());
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_json(content, workspace_root);
}

} // namespace codelenses::resolver
