# Compiler warnings and policy for CodeLenses

function(codelenses_set_compiler_warnings TARGET_NAME)
    set(GCC_CLANG_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wformat=2
        -Wimplicit-fallthrough
    )

    set(CLANG_ONLY_WARNINGS
        -Wextra-semi
    )

    set(GCC_ONLY_WARNINGS
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
    )

    set(MSVC_WARNINGS
        /W4
        /permissive-
        /w14242
        /w14254
        /w14263
        /w14265
        /w14287
        /we4289
        /w14296
        /w14311
        /w14545
        /w14546
        /w14547
        /w14549
        /w14555
        /w14619
        /w14640
        /w14826
        /w14905
        /w14906
        /w14928
    )

    set(TARGET_WARNINGS "")

    if(MSVC)
        set(TARGET_WARNINGS ${MSVC_WARNINGS})
        if(CODELENSES_WARNINGS_AS_ERRORS)
            list(APPEND TARGET_WARNINGS /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
        set(TARGET_WARNINGS ${GCC_CLANG_WARNINGS} ${CLANG_ONLY_WARNINGS})
        if(CODELENSES_WARNINGS_AS_ERRORS)
            list(APPEND TARGET_WARNINGS -Werror)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(TARGET_WARNINGS ${GCC_CLANG_WARNINGS} ${GCC_ONLY_WARNINGS})
        # GCC 12/13/14 have known false positive null-dereference warnings in libstdc++ headers at -O3
        list(REMOVE_ITEM TARGET_WARNINGS -Wnull-dereference)
        if(CODELENSES_WARNINGS_AS_ERRORS)
            list(APPEND TARGET_WARNINGS -Werror)
        endif()
    endif()

    target_compile_options(${TARGET_NAME} PRIVATE ${TARGET_WARNINGS})
endfunction()
