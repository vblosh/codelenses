cmake_minimum_required(VERSION 3.22)

# Auto-detect vcpkg if needed before project()
include(cmake/Vcpkg.cmake)

project(codelenses
    VERSION 0.1.0
    DESCRIPTION "Local-first code indexer and browser backend"
    LANGUAGES C CXX
)

# Standard C++23 configuration (required by codelens headers for std::expected)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Debug if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()

# Build options
option(CODELENSES_BUILD_TESTS "Build test suite" ON)
option(CODELENSES_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CODELENSES_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CODELENSES_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# Include custom CMake modules
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/Dependencies.cmake)
include(cmake/TreeSitterGrammar.cmake)
include(cmake/Formatting.cmake)
include(cmake/StaticAnalysis.cmake)

# Register selected grammars (A-02)
# C grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME c
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_TAG v0.24.2
)

# C++ grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME cpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG v0.23.4
)

# C# grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME c_sharp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c-sharp.git
    GIT_TAG v0.23.1
)

# Python grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME python
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_TAG v0.23.6
)

# JavaScript grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME javascript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    GIT_TAG v0.23.1
)

# TypeScript grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME typescript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    GIT_TAG v0.23.2
    SUBDIR typescript
)

# TSX grammar baseline (shares repository with typescript)
if(NOT DEFINED treesitter_grammar_typescript_SOURCE_DIR OR "${treesitter_grammar_typescript_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "tsx grammar requires treesitter_grammar_typescript_SOURCE_DIR to be defined; register 'typescript' grammar before 'tsx'")
endif()
codelenses_add_tree_sitter_grammar(
    NAME tsx
    SOURCE_DIR "${treesitter_grammar_typescript_SOURCE_DIR}"
    SUBDIR tsx
)

# Bash grammar baseline
codelenses_add_tree_sitter_grammar(
    NAME bash
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
    GIT_TAG v0.23.3
)

# Core library
set(CODELENSES_CORE_SOURCES
    src/app/config.cpp
    src/db/connection.cpp
    src/db/statement.cpp
    src/db/transaction.cpp
    src/db/migration.cpp
    src/db/workspace_repository.cpp
    src/db/file_repository.cpp
    src/db/symbol_repository.cpp
    src/db/occurrence_repository.cpp
    src/db/reference_repository.cpp
    src/db/relation_repository.cpp
    src/db/dependency_repository.cpp
    src/db/diagnostic_repository.cpp
    src/db/job_repository.cpp
    src/db/fts_repository.cpp
    src/db/database.cpp
    src/treesitter/parser.cpp
    src/treesitter/grammars.cpp
    src/adapters/adapter.cpp
    src/adapters/registry.cpp
    src/adapters/reference_adapter.cpp
    src/adapters/c_adapter.cpp
    src/adapters/cpp_adapter.cpp
    src/adapters/csharp_adapter.cpp
    src/adapters/go_adapter.cpp
    src/adapters/java_adapter.cpp
    src/adapters/python_adapter.cpp
    src/adapters/shell_adapter.cpp
    src/adapters/ts_adapter.cpp
    src/modules/domain.cpp
    src/filesystem/path.cpp
    src/filesystem/file_capture.cpp
    src/filesystem/discovery.cpp
    src/index/thread_pool.cpp
    src/index/incremental_planner.cpp
    src/index/indexer.cpp
    src/resolver/symbol_key.cpp
    src/resolver/compile_commands.cpp
    src/resolver/dependency_resolver.cpp
    src/resolver/symbol_resolver.cpp
    src/resolver/relationship_builder.cpp
    src/resolver/resolver.cpp
    src/server/service.cpp
    src/server/http_server.cpp
)

add_library(codelenses_core STATIC
    ${CODELENSES_CORE_SOURCES}
)

target_include_directories(codelenses_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(codelenses_core PUBLIC
    codelenses_external_deps
    codelenses_grammar_c
    codelenses_grammar_cpp
    codelenses_grammar_c_sharp
    codelenses_grammar_python
    codelenses_grammar_javascript
    codelenses_grammar_typescript
    codelenses_grammar_tsx
    codelenses_grammar_bash
)

codelenses_set_compiler_warnings(codelenses_core)
codelenses_set_sanitizers(codelenses_core)

# Main executable
add_executable(codelenses
    src/app/main.cpp
)

target_link_libraries(codelenses PRIVATE
    codelenses_core
)

codelenses_set_compiler_warnings(codelenses)
codelenses_set_sanitizers(codelenses)

# Testing
if(CODELENSES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation and Release Packaging (I-09)
include(GNUInstallDirs)

install(TARGETS codelenses
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS codelenses_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/codelenses
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/web/dist")
    install(DIRECTORY web/dist/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/codelenses/web
    )
endif()

install(FILES
    config/codelenses.sample.json
    config/codelenses.service
    config/.codelensignore.sample
    DESTINATION ${CMAKE_INSTALL_DATADIR}/codelenses
)

install(FILES README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# CPack configuration
set(CPACK_PACKAGE_NAME "codelenses")
set(CPACK_PACKAGE_VENDOR "CodeLenses")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local-first code indexer and browser backend")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "support@codelenses.local")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_GENERATOR "TGZ;STGZ;DEB")
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "CodeLenses Maintainers <support@codelenses.local>")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.34), libsqlite3-0 (>= 3.35.0)")
set(CPACK_SOURCE_IGNORE_FILES
    "/.git/"
    "/build/"
    "node_modules"
    "\\\\.db$"
    "\\\\.db-wal$"
    "\\\\.db-shm$"
)
include(CPack)

