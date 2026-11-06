# mcpkit_apply_sanitizers()
# Appends sanitizer flags to CMAKE_C_FLAGS and CMAKE_C_LINKER_FLAGS so every
# target in the build inherits them. ASan and TSan are mutually exclusive.
# UBSan uses -fno-sanitize-recover so the first UB aborts the build.
function(mcpkit_apply_sanitizers)
    if(MCPKIT_ENABLE_ASAN AND MCPKIT_ENABLE_TSAN)
        message(FATAL_ERROR "MCPKIT_ENABLE_ASAN and MCPKIT_ENABLE_TSAN are mutually exclusive")
    endif()
    if(MCPKIT_ENABLE_ASAN)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
        # Leak detection on at exit (LSan integrated with ASan on Linux).
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer")
    endif()
    if(MCPKIT_ENABLE_UBSAN)
        # -fno-sanitize-recover=... makes any UB abort instead of print+continue.
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
        add_link_options(-fsanitize=undefined)
    endif()
    if(MCPKIT_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()
endfunction()
