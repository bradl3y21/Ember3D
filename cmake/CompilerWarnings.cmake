# ember_enable_warnings(<target>) — warnings for Ember3D-owned targets only.
# Vendor code builds quiet on purpose.

function(ember_enable_warnings target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "ember_enable_warnings: no such target '${target}'")
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4            # our code is warning-clean at /W4; vendor is excluded
            /permissive-   # standards-conforming
            /Zc:__cplusplus
            /utf-8         # source is UTF-8
            /MP            # parallel cl
            /wd4100        # 'unreferenced formal parameter' - interface stubs are legal
        )
        # Multi-processor compilation is also useful in Debug.
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
        )
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
