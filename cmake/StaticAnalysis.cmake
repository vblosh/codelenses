# Clang-tidy integration

find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-19 clang-tidy-18 clang-tidy-17)

option(CODELENSES_ENABLE_CLANG_TIDY "Enable clang-tidy during compilation" OFF)

if(CLANG_TIDY_EXE)
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXE}")

    if(CODELENSES_ENABLE_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}" CACHE STRING "" FORCE)
        message(STATUS "clang-tidy enabled for C++ targets")
    endif()

    file(GLOB_RECURSE PROJECT_CXX_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )

    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
            ${PROJECT_CXX_SOURCES}
        COMMENT "Running clang-tidy on source files..."
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    message(STATUS "clang-tidy not found; static analysis targets disabled")
endif()
