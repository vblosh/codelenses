#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codelenses/language.hpp"
#include "codelenses/resolver/compile_commands.hpp"
#include "codelenses/resolver/types.hpp"

namespace codelenses::resolver {

class DependencyResolver {
public:
    DependencyResolver() = default;

    void set_workspace_root(std::filesystem::path root);

    void register_file(int64_t file_id, std::filesystem::path rel_path, Language lang);
    void unregister_file(const std::filesystem::path& rel_path);
    void clear_files();

    void set_compilation_database(const CompilationDatabase& cdb);

    void set_include_directories(std::filesystem::path file_path,
                                 std::vector<std::filesystem::path> quote_dirs,
                                 std::vector<std::filesystem::path> system_dirs);

    void set_tsconfig_paths(std::filesystem::path base_url,
                            std::unordered_map<std::string, std::vector<std::string>> paths);

    void set_go_module(std::string module_path);

    void set_allow_suffix_fallback(bool allow) noexcept { allow_suffix_fallback_ = allow; }
    [[nodiscard]] bool allow_suffix_fallback() const noexcept { return allow_suffix_fallback_; }

    [[nodiscard]] CandidateTarget resolve_dependency(Language lang,
                                                     const std::filesystem::path& source_file,
                                                     std::string_view raw_name) const;

    [[nodiscard]] std::vector<CandidateTarget>
    resolve_dependency_candidates(Language lang, const std::filesystem::path& source_file,
                                  std::string_view raw_name) const;

private:
    struct RegisteredFile {
        int64_t file_id{0};
        std::filesystem::path relative_path;
        Language language{Language::unknown};
    };

    struct IncludeDirs {
        std::vector<std::filesystem::path> quote_dirs;
        std::vector<std::filesystem::path> system_dirs;
    };

    [[nodiscard]] std::filesystem::path to_absolute(const std::filesystem::path& p) const;
    [[nodiscard]] std::filesystem::path to_relative(const std::filesystem::path& p) const;
    [[nodiscard]] const RegisteredFile* find_file(const std::filesystem::path& p) const;
    [[nodiscard]] bool is_directory_in_workspace(const std::filesystem::path& dir) const;

    [[nodiscard]] std::vector<CandidateTarget>
    resolve_c_cpp(const std::filesystem::path& source_file, std::string_view raw_name) const;

    [[nodiscard]] std::vector<CandidateTarget>
    resolve_python(const std::filesystem::path& source_file, std::string_view raw_name) const;

    [[nodiscard]] std::vector<CandidateTarget>
    resolve_typescript(const std::filesystem::path& source_file, std::string_view raw_name) const;

    [[nodiscard]] std::vector<CandidateTarget> resolve_go(const std::filesystem::path& source_file,
                                                          std::string_view raw_name) const;

    [[nodiscard]] std::vector<CandidateTarget>
    resolve_java(const std::filesystem::path& source_file, std::string_view raw_name) const;

    [[nodiscard]] std::vector<CandidateTarget>
    resolve_shell(const std::filesystem::path& source_file, std::string_view raw_name) const;

    std::filesystem::path workspace_root_;
    std::unordered_map<std::string, RegisteredFile> files_by_rel_path_;
    std::unordered_map<int64_t, std::string> file_id_to_rel_path_;

    CompilationDatabase compilation_db_;
    IncludeDirs default_include_dirs_;
    std::unordered_map<std::string, IncludeDirs> file_include_dirs_;

    std::filesystem::path ts_base_url_;
    std::unordered_map<std::string, std::vector<std::string>> ts_paths_;

    std::string go_module_;
    bool allow_suffix_fallback_{true};
};

} // namespace codelenses::resolver
