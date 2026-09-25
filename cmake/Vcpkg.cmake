# Auto-detect vcpkg toolchain if not already specified
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg toolchain file")
        message(STATUS "Using vcpkg toolchain from VCPKG_ROOT: ${CMAKE_TOOLCHAIN_FILE}")
    else()
        find_program(VCPKG_EXECUTABLE NAMES vcpkg)
        if(VCPKG_EXECUTABLE)
            get_filename_component(VCPKG_DIR "${VCPKG_EXECUTABLE}" DIRECTORY)
            if(EXISTS "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
                set(CMAKE_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake"
                    CACHE FILEPATH "vcpkg toolchain file")
                message(STATUS "Using vcpkg toolchain found via PATH: ${CMAKE_TOOLCHAIN_FILE}")
            endif()
        endif()
    endif()
endif()
