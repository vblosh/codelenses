# Dependency management for CodeLenses via vcpkg

find_package(Threads REQUIRED)

# 1. cpp-httplib
find_package(httplib CONFIG REQUIRED)
message(STATUS "Found cpp-httplib")

# 2. SQLite3
find_package(unofficial-sqlite3 CONFIG REQUIRED)
message(STATUS "Found unofficial-sqlite3")

# 3. Tree-sitter core
find_package(unofficial-tree-sitter CONFIG REQUIRED)
message(STATUS "Found unofficial-tree-sitter")

# 4. nlohmann_json
find_package(nlohmann_json CONFIG REQUIRED)
message(STATUS "Found nlohmann_json")

# 5. Catch2 (for unit tests)
if(CODELENSES_BUILD_TESTS)
    find_package(Catch2 CONFIG REQUIRED)
    message(STATUS "Found Catch2 v${Catch2_VERSION}")
endif()

# Interface target grouping external dependencies
add_library(codelenses_external_deps INTERFACE)
target_link_libraries(codelenses_external_deps INTERFACE
    httplib::httplib
    unofficial::sqlite3::sqlite3
    unofficial::tree-sitter::tree-sitter
    nlohmann_json::nlohmann_json
    Threads::Threads
)
