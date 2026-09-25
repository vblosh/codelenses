# Helper function to fetch and build Tree-sitter language grammars

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

include(FetchContent)

function(codelenses_add_tree_sitter_grammar)
    set(options "")
    set(oneValueArgs NAME GIT_REPOSITORY GIT_TAG SOURCE_DIR)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_NAME)
        message(FATAL_ERROR "codelenses_add_tree_sitter_grammar requires NAME")
    endif()

    set(TARGET_NAME "codelenses_grammar_${ARG_NAME}")

    if(TARGET ${TARGET_NAME})
        return()
    endif()

    if(ARG_GIT_REPOSITORY)
        set(FETCH_NAME "treesitter_grammar_${ARG_NAME}")
        FetchContent_Declare(
            ${FETCH_NAME}
            GIT_REPOSITORY ${ARG_GIT_REPOSITORY}
            GIT_TAG        ${ARG_GIT_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_GetProperties(${FETCH_NAME})
        if(NOT ${FETCH_NAME}_POPULATED)
            FetchContent_Populate(${FETCH_NAME})
        endif()
        set(GRAMMAR_SRC_DIR "${${FETCH_NAME}_SOURCE_DIR}")
    elseif(ARG_SOURCE_DIR)
        set(GRAMMAR_SRC_DIR ${ARG_SOURCE_DIR})
    else()
        message(FATAL_ERROR "Must specify GIT_REPOSITORY or SOURCE_DIR for grammar ${ARG_NAME}")
    endif()

    # Locate source files (typically src/parser.c and optional src/scanner.c or scanner.cc)
    set(SOURCES "")
    if(EXISTS "${GRAMMAR_SRC_DIR}/src/parser.c")
        list(APPEND SOURCES "${GRAMMAR_SRC_DIR}/src/parser.c")
    endif()
    if(EXISTS "${GRAMMAR_SRC_DIR}/src/scanner.c")
        list(APPEND SOURCES "${GRAMMAR_SRC_DIR}/src/scanner.c")
    elseif(EXISTS "${GRAMMAR_SRC_DIR}/src/scanner.cc")
        list(APPEND SOURCES "${GRAMMAR_SRC_DIR}/src/scanner.cc")
    endif()

    if(NOT SOURCES)
        message(FATAL_ERROR "No grammar sources found in ${GRAMMAR_SRC_DIR}/src for ${ARG_NAME}")
    endif()

    add_library(${TARGET_NAME} STATIC ${SOURCES})
    target_include_directories(${TARGET_NAME} PUBLIC
        "$<BUILD_INTERFACE:${GRAMMAR_SRC_DIR}/src>"
    )
    target_link_libraries(${TARGET_NAME} PUBLIC unofficial::tree-sitter::tree-sitter)

    # Disable warnings on third-party grammar code
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /w)
    else()
        target_compile_options(${TARGET_NAME} PRIVATE -w)
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        C_STANDARD 11
        POSITION_INDEPENDENT_CODE ON
    )
    message(STATUS "Registered Tree-sitter grammar: ${ARG_NAME} (target: ${TARGET_NAME})")
endfunction()
