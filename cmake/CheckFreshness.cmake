# CheckFreshness.cmake — fail loudly when no binary reflects the newest
# source edit. Kiln · Platform. Pairs with the build gate (BuildGate.cmake):
#
#   build.gate      rebuilds before the suite; MSBuild relinks per-target, so
#                   after a GREEN gate every exe is up to date WITH ITS OWN
#                   dependencies by construction. That is why this check does
#                   NOT demand every exe newer than every source — that shape
#                   false-positives the moment one module relinks and its
#                   siblings legitimately stay older.
#   build.freshness (this script) catches the two holes no build can:
#       (a) the tree was edited and NEVER rebuilt since — the newest source is
#           newer than even the most recently built exe. This is Verity's
#           item-92 trap: ctest reporting green against binaries that predate
#           the sources.
#       (b) a source file wired into NO CMake target — nothing rebuilds for it
#           ever, so no exe ever gets newer than it. The __has_include /
#           orphan-file trap, caught by mtime instead of by inspection.
#
# Comparing against the NEWEST exe (not each exe individually) is the precise
# test for both: if any build has happened since the newest source edit, the
# affected targets were relinked and the check passes; if nothing has, it fails.
#
# Compilable sources only (.cpp/.c/.h): a README or a CMake script edit does
# not relink anything and must not fail the gate — measured false-positive on
# 2026-10-16 (editing cmake/BuildGate.cmake flagged every exe).
#
# Timestamps: file(TIMESTAMP) with a fixed-width UTC format → plain string
# compare. One command per line — CMake parses two commands on one line as one.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BIN_DIR)
    message(FATAL_ERROR "CheckFreshness: SOURCE_DIR and BIN_DIR are required")
endif()

set(_fmt "%Y%m%d%H%M%S")

file(GLOB_RECURSE _sources
     "${SOURCE_DIR}/src/*.cpp"
     "${SOURCE_DIR}/src/*.h"
     "${SOURCE_DIR}/src/*.c"
     "${SOURCE_DIR}/tests/*.cpp"
     "${SOURCE_DIR}/tests/*.h"
     "${SOURCE_DIR}/tests/*.c"
     "${SOURCE_DIR}/vendor/*.cpp"
     "${SOURCE_DIR}/vendor/*.h"
     "${SOURCE_DIR}/vendor/*.c")

list(LENGTH _sources _nsrc)
if(_nsrc EQUAL 0)
    message(FATAL_ERROR "CheckFreshness: no compilable sources found under '${SOURCE_DIR}' - wrong SOURCE_DIR?")
endif()

set(_newest "")
set(_newest_file "")
foreach(f IN LISTS _sources)
    file(TIMESTAMP "${f}" _ts ${_fmt} UTC)
    if(_ts STREQUAL "")
        continue()
    endif()
    if(_newest STREQUAL "" OR _ts STRGREATER _newest)
        set(_newest "${_ts}")
        set(_newest_file "${f}")
    endif()
endforeach()

if(_newest STREQUAL "")
    message(FATAL_ERROR "CheckFreshness: could not timestamp any source - cannot judge freshness")
endif()

file(GLOB _exes "${BIN_DIR}/*.exe")
list(LENGTH _exes _nexe)
if(_nexe EQUAL 0)
    message(FATAL_ERROR "CheckFreshness: no executables in '${BIN_DIR}' - nothing has been built")
endif()

set(_newest_exe "")
set(_newest_exe_file "")
foreach(e IN LISTS _exes)
    file(TIMESTAMP "${e}" _ets ${_fmt} UTC)
    if(_ets STREQUAL "")
        continue()
    endif()
    if(_newest_exe STREQUAL "" OR _ets STRGREATER _newest_exe)
        set(_newest_exe "${_ets}")
        set(_newest_exe_file "${e}")
    endif()
endforeach()

if(_newest_exe STREQUAL "")
    message(FATAL_ERROR "CheckFreshness: could not timestamp any executable - cannot judge freshness")
endif()

if(_newest STRGREATER _newest_exe)
    message(FATAL_ERROR
        "STALE BUILD — no binary reflects the newest source edit.\n"
        "  newest source: ${_newest_file}\n"
        "    edited:      ${_newest}\n"
        "  newest binary: ${_newest_exe_file}\n"
        "    built:       ${_newest_exe}\n"
        "ctest would be measuring code that PREDATES the sources — the item-92 trap.\n"
        "THREE CAUSES, in the order they actually happen:\n"
        "  1. You edited and did not rebuild:            cmake --build <build-dir> --config <config>\n"
        "  2. A build was INTERRUPTED or truncated       (early-terminating pipeline, Ctrl-C, killed job),\n"
        "     relinking only some targets.               NOTE: this check does NOT reliably detect that\n"
        "                                                 mixed state (it passes as soon as one relinked\n"
        "                                                 binary postdates the newest source) — that needs\n"
        "                                                 the per-target version, not yet landed. If you\n"
        "                                                 suspect a truncated build, just rebuild.\n"
        "  3. The newest source is wired into NO target  (orphan file — the __has_include trap class):\n"
        "     a rebuild will NOT move any binary past it — wire it into its module's CMakeLists or delete it.\n"
        "If a full rebuild leaves this test red, cause 3 is what you are looking at.")
endif()

message(STATUS "build.freshness OK: newest binary (${_newest_exe_file}, ${_newest_exe}) "
               "postdates the newest of ${_nsrc} compilable sources "
               "(${_newest_file}, ${_newest})")
