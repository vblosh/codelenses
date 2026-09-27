include(CTest)
include(Catch)

set(UNIT_TEST_SOURCES
    unit/test_config.cpp
    unit/test_dependencies.cpp
    unit/test_migration.cpp
    unit/test_persistence.cpp
    unit/test_adapters_sdk.cpp
    unit/test_path_and_discovery.cpp
    unit/test_bounded_queue_and_pool.cpp
    unit/test_indexing_pipeline.cpp
    unit/test_resolver.cpp
    unit/test_library_indexing.cpp
    unit/test_http_api.cpp
    unit/test_c_adapter.cpp
    unit/test_cpp_adapter.cpp
    unit/test_csharp_adapter.cpp
    unit/test_python_adapter.cpp
    unit/test_typescript_adapter.cpp
    unit/test_javascript_adapter.cpp
    unit/test_java_adapter.cpp
    unit/test_shell_adapter.cpp
    unit/test_go_adapter.cpp
    integration/test_sample_workspace.cpp
    integration/test_api_contract.cpp
    integration/test_security_and_malformed.cpp
    integration/test_cancellation_and_recovery.cpp
    benchmarks/benchmark_indexing.cpp
    benchmarks/benchmark_queries.cpp
    benchmarks/benchmark_footprint.cpp
)

add_executable(codelenses_unit_tests
    ${UNIT_TEST_SOURCES}
)

target_link_libraries(codelenses_unit_tests PRIVATE
    codelenses_core
    Catch2::Catch2WithMain
    codelenses_grammar_c
    codelenses_grammar_cpp
    codelenses_grammar_c_sharp
    codelenses_grammar_python
    codelenses_grammar_javascript
    codelenses_grammar_typescript
    codelenses_grammar_tsx
    codelenses_grammar_bash
    codelenses_grammar_java
)

target_compile_definitions(codelenses_unit_tests PRIVATE
    CODELENSES_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

codelenses_set_compiler_warnings(codelenses_unit_tests)
codelenses_set_sanitizers(codelenses_unit_tests)

catch_discover_tests(codelenses_unit_tests)
