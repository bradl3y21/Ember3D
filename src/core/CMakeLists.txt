# core — owned by Ash · Lead.  Contract: ARCHITECTURE.md §5.
#
# Two targets:
#   ember_core  — the shared vocabulary + app shell. Links nothing of ours.
#   Ember3D     — the composition root (src/core/main.cpp), which is the one
#                 place that is allowed to link every module. Per §5:
#                 "core/App links everything".
#
# The executable is created only if it does not already exist, so the root
# CMakeLists.txt (Kiln) may also define it — first definition wins, and never
# two. The root still owns the output directory (bin/Ember3D.exe).

# Resolve src/ from this file's location rather than the top-level project, so
# the module builds identically whether the root includes it or a scratch
# tree does.
set(EMBER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(ember_core STATIC
    App.cpp
)

target_include_directories(ember_core PUBLIC ${EMBER_SRC_DIR})

# Header-only vocabulary, listed so IDEs and installs see them.
target_sources(ember_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${EMBER_SRC_DIR}
    FILES Math.h Types.h Log.h App.h
)

target_compile_features(ember_core PUBLIC cxx_std_20)

# App.cpp is the composition root — the one TU allowed to include every module
# (ARCHITECTURE §5: "core/App links everything"). Its includes pull in module
# headers (which live under src/, already on our include path) and those pull in
# vendor headers (glad today; glfw/imgui once editor lands), so this target
# needs the vendor usage requirements to *compile*.
#
# Vendor targets only, and PRIVATE: linking the ember_* modules here would make
# a cycle, because each of them already links ember_core PUBLIC. Core's own
# headers (Math.h/Types.h/Log.h) stay dependency-free — this changes nothing for
# anything that merely consumes ember_core.
# The root adds vendor/ first and core last, so these targets exist.
foreach(vendor_dep glad glfw imgui)
    if(TARGET ${vendor_dep})
        target_link_libraries(ember_core PRIVATE ${vendor_dep})
    endif()
endforeach()

# --------------------------------------------------------------------------
# Module presence, as REAL compile definitions (not __has_include).
#
# App.cpp picks its wiring with #if defined(EMBER_HAS_*). CMake cannot see
# __has_include: when editor/Editor.h appeared, nothing recompiled App.cpp and
# the exe shipped with the editor silently compiled out while the build reported
# green (operator-verified 2026-10-16). A definition changes the command line, so
# a module landing forces the rebuild.
#
# PUBLIC so consumers (tests included) can also #ifdef on these, and at
# ember_core scope — not inside the if(NOT TARGET Ember3D) guard below — so it
# holds even if the root defines the executable.
#
# The definitions carry the same implications as App.cpp's nested wiring: render
# needs a window (platform), scene needs render, editor needs scene + render.
# Without that, a partial configuration could define EMBER_HAS_RENDER while
# EMBER_HAS_PLATFORM is absent and App.cpp would stop compiling.
#
# Always `=1`. A value-less define makes `#if EMBER_HAS_X` a hard error and is
# easy to misuse; `#if defined(...)` works either way, but one form has to win.
#
# ONE definition site. This reconciles Kiln's patch (thread
# cmsg_mvahk5ol5cLk8uLQ1oTv) with the block that was already here: two sites
# emitted both `EMBER_HAS_X` and `EMBER_HAS_X=1`, which resolved by ordering and
# would diverge silently.
# --------------------------------------------------------------------------
foreach(module platform render scene editor)
    if(NOT TARGET ember_${module})
        continue()
    endif()
    if(module STREQUAL "render" AND NOT TARGET ember_platform)
        continue()
    endif()
    if(module STREQUAL "scene" AND NOT TARGET ember_render)
        continue()
    endif()
    if(module STREQUAL "editor" AND (NOT TARGET ember_render OR NOT TARGET ember_scene))
        continue()
    endif()
    string(TOUPPER ${module} _ember_module_upper)
    target_compile_definitions(ember_core PUBLIC "EMBER_HAS_${_ember_module_upper}=1")
endforeach()

if(NOT TARGET Ember3D)
    add_executable(Ember3D main.cpp)
    set_target_properties(Ember3D PROPERTIES OUTPUT_NAME Ember3D)
    target_compile_features(Ember3D PRIVATE cxx_std_20)
    target_link_libraries(Ember3D PRIVATE ember_core)

    # Modules link downward only. Each is added as it lands; until then the app
    # builds core-only and logs that the window is not available (M0).
    foreach(module platform render scene editor)
        if(TARGET ember_${module})
            target_link_libraries(Ember3D PRIVATE ember_${module})
        endif()
    endforeach()
endif()
