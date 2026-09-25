cmake_minimum_required(VERSION 3.22)

# Auto-detect vcpkg if needed before project()
include(cmake/Vcpkg.cmake)

project(codelenses
    VERSION 0.1.0
    DESCRIPTION "Local-first code indexer and browser backend"
    LANGUAGES C CXX
)

# Standard C++20 configuration
set(CMAKE_CXX_STANDARD 20)
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
