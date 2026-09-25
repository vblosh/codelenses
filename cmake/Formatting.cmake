# Clang-format targets

find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-19 clang-format-18 clang-format-17)

if(CLANG_FORMAT_EXE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXE}")

    file(GLOB_RECURSE ALL_SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i -style=file ${ALL_SOURCE_FILES}
        COMMENT "Running clang-format on all source files..."
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror -style=file ${ALL_SOURCE_FILES}
        COMMENT "Checking code formatting with clang-format..."
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    message(STATUS "clang-format not found; formatting targets disabled")
endif()
