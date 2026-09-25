#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/result.hpp"

namespace codelenses::resolver {

using CompileCommand = adapters::CompileCommandContext;

class CompilationDatabase {
public:
    CompilationDatabase() = default;

    // Adds a parsed compile command entry.
    void add_entry(CompileCommand entry);

    [[nodiscard]] const std::vector<CompileCommand>& entries() const noexcept { return entries_; }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Finds a compile command matching the given file path (relative or absolute).
    [[nodiscard]] const CompileCommand* find_for_file(const std::filesystem::path& file_path) const;

    // Safely tokenizes a command string without shell execution or expansions.
    [[nodiscard]] static Result<std::vector<std::string>>
    tokenize_command_safely(std::string_view command);

    // Parses JSON compilation database content.
    [[nodiscard]] static Result<CompilationDatabase>
    parse_json(std::string_view json_content, const std::filesystem::path& workspace_root = {});

    // Loads compile_commands.json from disk.
    [[nodiscard]] static Result<CompilationDatabase>
    load_file(const std::filesystem::path& file_path,
              const std::filesystem::path& workspace_root = {});

private:
    std::vector<CompileCommand> entries_;
};

} // namespace codelenses::resolver
