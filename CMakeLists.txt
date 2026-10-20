# Ember3D — root build. Owned by Kiln · Platform [Build].
#   cmake -B build
#   cmake --build build --config Release
#   bin/Ember3D.exe
# Each module owns src/<module>/CMakeLists.txt; the root only wires them.

cmake_minimum_required(VERSION 3.25)

project(Ember3D
        VERSION 0.1.0
        DESCRIPTION "A Blender-like 3D editor"
        LANGUAGES C CXX)

# --------------------------------------------------------------------------
# Language / toolchain defaults
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Single-config generators default to Release (ARCHITECTURE §3).
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Everything runtime-ish lands in <repo>/bin, for every config. Do not build
# multiple build trees in parallel against this shared directory. A running
# Ember3D.exe is diagnosed by the lock preflight below. If you need separate
# build trees, give each its own directory:
#   cmake -B build-gpu -DEMBER_OUTPUT_DIR=<path>
set(EMBER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin" CACHE PATH
    "Directory for runtime outputs (Ember3D.exe, test exes)")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${EMBER_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${cfg}" cfgU)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfgU} "${EMBER_OUTPUT_DIR}")
endforeach()

set(CMAKE_VS_STARTUP_PROJECT Ember3D)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# --------------------------------------------------------------------------
# Third-party: exactly one copy of each, committed source, offline builds.
# --------------------------------------------------------------------------
add_subdirectory(vendor)

# --------------------------------------------------------------------------
# Modules — each declares its own sources; the root never lists them.
# A module without a CMakeLists.txt yet is skipped (it is still being born).
# core goes LAST: src/core/CMakeLists.txt defines the Ember3D executable and
# links every ember_* target that exists at that moment.
# --------------------------------------------------------------------------
set(EMBER_MODULES platform render scene editor core)
foreach(mod IN LISTS EMBER_MODULES)
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/${mod}/CMakeLists.txt")
        add_subdirectory("src/${mod}")
    else()
        message(STATUS "[Ember3D] src/${mod}/CMakeLists.txt not present yet - skipping")
    endif()
endforeach()

# Warnings and flags are root policy (ARCHITECTURE §5) - applied to every
# module target that exists, whatever directory defined it.
foreach(tgt ember_core ember_platform ember_render ember_scene ember_editor Ember3D)
    if(TARGET ${tgt})
        ember_enable_warnings(${tgt})
    endif()
endforeach()

# QA owns tests/; the root only enables CTest and includes it.
enable_testing()
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
else()
    message(STATUS "[Ember3D] tests/CMakeLists.txt not present yet - skipping")
endif()

# Runtime smoke test — WHAT-REMAINS.md: the one test that can see runtime/GL
# bugs (shader compile, context, framebuffer). It runs the real app for 60
# frames and exits non-zero if the frame path breaks. Registered from the root
# because it exercises the Ember3D executable, and gated with the same option
# Verity's GPU tests use so the default ctest run stays context-free.
if(TARGET Ember3D)
    # Size-agreement smoke (backlog item 37): registered for the DEFAULT ctest
    # invocation, not only under the GPU option — but self-skipping where there
    # is no GL context (scripts/smoke_gate.ps1 exits 77 -> SKIP_RETURN_CODE),
    # so tests/README.md's context-free default run stays green. It runs the
    # bounded smoke WITH a mid-run resize (EMBER_SMOKE_RESIZE) and asserts
    # rect == window, fb == window*scale, renderer-ready == fb, >=2 size
    # transitions, and no STALE/MISMATCH — see cmake/RunSmokeSizeCheck.cmake.
    add_test(NAME app.smoke_size_agree
             COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                     -File "${CMAKE_SOURCE_DIR}/scripts/smoke_gate.ps1"
                     -Exe "$<TARGET_FILE:Ember3D>"
                     -Check "${CMAKE_SOURCE_DIR}/cmake/RunSmokeSizeCheck.cmake"
                     -Resize 1600x900)
    set_tests_properties(app.smoke_size_agree PROPERTIES
        TIMEOUT 150
        SKIP_RETURN_CODE 77)
endif()

if(EMBER_ENABLE_GPU_TESTS AND TARGET Ember3D)
    add_test(NAME app.smoke_frames COMMAND Ember3D --frames 60 --exit)
    set_tests_properties(app.smoke_frames PROPERTIES TIMEOUT 120)

    message(STATUS "[Ember3D] runtime smoke test registered: app.smoke_frames (GPU option) + app.smoke_size_agree (default)")
else()
    message(STATUS "[Ember3D] runtime smoke test app.smoke_size_agree is on by default; "
                   "app.smoke_frames needs -DEMBER_ENABLE_GPU_TESTS=ON")
endif()

# A running Windows executable holds an image section that prevents the linker
# from replacing it. Diagnose that before any project starts compiling, rather
# than letting LINK report a confusing LNK1104 after the rest of the solution
# has built. This target is intentionally always out-of-date, and is a
# prerequisite of every build target so it also runs on incremental builds.
if(WIN32 AND TARGET Ember3D)
    add_custom_target(ember_executable_lock_preflight
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_executable_lock.ps1"
                -Exe "${EMBER_OUTPUT_DIR}/Ember3D.exe"
        COMMENT "Checking that Ember3D.exe can be replaced"
        VERBATIM)

    function(ember_require_executable_lock_preflight directory)
        get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            if(NOT _target STREQUAL "ember_executable_lock_preflight")
                get_target_property(_imported "${_target}" IMPORTED)
                if(NOT _imported)
                    add_dependencies("${_target}" ember_executable_lock_preflight)
                endif()
            endif()
        endforeach()

        get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(_subdirectory IN LISTS _subdirectories)
            ember_require_executable_lock_preflight("${_subdirectory}")
        endforeach()
    endfunction()

    ember_require_executable_lock_preflight("${CMAKE_SOURCE_DIR}")
endif()

# --------------------------------------------------------------------------
# The build gate (item 102): a GREEN ctest ON A RED TREE is made impossible.
#
# Verity caught ctest reporting 22/22 green while the tree did not compile,
# because bin/Ember3D.exe was older than the sources - only the build exit
# code is authoritative. Two mechanisms, both tests:
#
#   build.gate      FIXTURES_SETUP  - rebuilds this binary dir first; if the
#                   tree is red the gate FAILS and every dependent test is
#                   marked Not Run (ctest exits non-zero). CTest runs the
#                   setup fixture even under -R/-L filters, so no invocation
#                   can skip it.
#   build.freshness FIXTURES_REQUIRED - after a green gate, asserts every
#                   exe in bin/ is newer than every compilable source. Catches
#                   the holes a build cannot: an orphan source wired into no
#                   target (nothing rebuilds for it - the __has_include trap
#                   class) or an exe that belongs to no target.
#
# Every other test gets FIXTURES_REQUIRED ember_build below, INCLUDING the
# tests Verity registers in tests/ (set from here via the cross-directory
# TEST property - tests/CMakeLists.txt stays hers).
# --------------------------------------------------------------------------
set(EMBER_BUILD_FIXTURE "ember_build")

add_test(NAME build.gate
         COMMAND ${CMAKE_COMMAND}
                 "-DGATE_BINARY_DIR=${CMAKE_BINARY_DIR}"
                 "-DGATE_CONFIG=$<CONFIG>"
                 -P "${CMAKE_SOURCE_DIR}/cmake/BuildGate.cmake")
set_tests_properties(build.gate PROPERTIES
    FIXTURES_SETUP "${EMBER_BUILD_FIXTURE}"
    TIMEOUT 900)

add_test(NAME build.freshness
         COMMAND ${CMAKE_COMMAND}
                 "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                 "-DBIN_DIR=${EMBER_OUTPUT_DIR}"
                 -P "${CMAKE_SOURCE_DIR}/cmake/CheckFreshness.cmake")
set_tests_properties(build.freshness PROPERTIES
    FIXTURES_REQUIRED "${EMBER_BUILD_FIXTURE}"
    TIMEOUT 120)

# Attach the fixture to every test known at this point: this directory's own
# (app.smoke_*, build.*) and tests/ (Verity's). Skip the gate itself - it IS
# the setup - and freshness, which already carries the property above.
foreach(_dir "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests")
    if(EXISTS "${_dir}/CMakeLists.txt")
        get_property(_dir_tests DIRECTORY "${_dir}" PROPERTY TESTS)
        foreach(_t IN LISTS _dir_tests)
            if(NOT _t STREQUAL "build.gate" AND NOT _t STREQUAL "build.freshness")
                set_property(TEST ${_t} DIRECTORY "${_dir}" APPEND
                             PROPERTY FIXTURES_REQUIRED "${EMBER_BUILD_FIXTURE}")
            endif()
        endforeach()
    endif()
endforeach()
unset(_dir_tests)
unset(_t)
unset(_dir)

# --------------------------------------------------------------------------
# Per-item evidence verification (item 106, Track A): one ctest case per
# declaration in cmake/evidence/*.evidence, run under the same build fixture.
# Each verifies the declaration mechanically in D33's order (freshness first,
# then artifact, test, symbol, residue) and fails with the exact missing
# thing. Declarations are additive - add a file, get a check.
# --------------------------------------------------------------------------
file(GLOB _evidence_decls "${CMAKE_SOURCE_DIR}/cmake/evidence/*.evidence")
foreach(_decl IN LISTS _evidence_decls)
    get_filename_component(_decl_name "${_decl}" NAME_WE)
    add_test(NAME "evidence.${_decl_name}"
             COMMAND ${CMAKE_COMMAND}
                     "-DEVIDENCE_FILE=${_decl}"
                     "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                     "-DBIN_DIR=${EMBER_OUTPUT_DIR}"
                     "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
                     "-DCTEST_CONFIG=$<CONFIG>"
                     -P "${CMAKE_SOURCE_DIR}/cmake/VerifyEvidence.cmake")
    set_tests_properties("evidence.${_decl_name}" PROPERTIES
        FIXTURES_REQUIRED "${EMBER_BUILD_FIXTURE}"
        TIMEOUT 300)
endforeach()
unset(_decl)
unset(_decl_name)
unset(_evidence_decls)

# The executable bin/Ember3D.exe is defined by src/core (composition root);
# the root only guarantees it lands in <repo>/bin for every config.

message(STATUS "[Ember3D] configured: ${CMAKE_PROJECT_VERSION}, compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
