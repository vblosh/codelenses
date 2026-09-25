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
)

add_executable(codelenses_unit_tests
    ${UNIT_TEST_SOURCES}
)

target_link_libraries(codelenses_unit_tests PRIVATE
    codelenses_core
    Catch2::Catch2WithMain
    codelenses_grammar_c
)

codelenses_set_compiler_warnings(codelenses_unit_tests)
codelenses_set_sanitizers(codelenses_unit_tests)

catch_discover_tests(codelenses_unit_tests)
