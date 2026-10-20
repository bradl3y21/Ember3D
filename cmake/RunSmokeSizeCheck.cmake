# RunSmokeSizeCheck.cmake — the bounded-run size-agreement assertion.
# Owned by Kiln · Platform (cmake/ is Build's path).
#
# Runs bin/Ember3D.exe --frames 60 --exit with EMBER_SMOKE_RESIZE set (the
# platform hook resizes the window mid-run) and FAILS unless:
#
#   1. the run exits 0 and completes the frame budget;
#   2. at least TWO size lines appear (the resize actually happened, so the
#      callback path was exercised — otherwise this test is vacuous);
#   3. no "STALE viewport rect" line (a size change was not resynced —
#      the defect-1 regression);
#   4. no "MISMATCH" line (input rect != window size);
#   5. the last size line parses and its parts agree:
#        rect == window   (D17: the input rect lives in window coordinates)
#        fb ~ window * scale  (framebuffer = renderer's resize input)
#        rect captured at the current window size (not a stale capture)
#   6. "renderer ready (WxH)" — the renderer's initial framebuffer size —
#      equals the fb in the size line, so renderer and input side agree.
#
# Any failure is FATAL_ERROR => non-zero => the CTest case fails => the next
# mismatch is caught by the test, not by a human wiggling a mouse.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED EMBER_SMOKE_EXE)
    message(FATAL_ERROR "RunSmokeSizeCheck: -DEMBER_SMOKE_EXE=<path> is required")
endif()

set(_timeout 120)
if(DEFINED EMBER_SMOKE_TIMEOUT)
    set(_timeout "${EMBER_SMOKE_TIMEOUT}")
endif()

execute_process(
    COMMAND "${EMBER_SMOKE_EXE}" --frames 60 --exit
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT ${_timeout}
)
set(_log "${_out}${_err}")

message(STATUS "---- bounded run: ${EMBER_SMOKE_EXE} --frames 60 --exit ----")
message(STATUS "${_log}")
message(STATUS "---- assertions ----")

# 1. exit code + frame budget
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "smoke run failed: exit code ${_rc}")
endif()
if(NOT _log MATCHES "frame loop ended after 60 frames")
    message(FATAL_ERROR "frame budget not met: 'frame loop ended after 60 frames' missing")
endif()

# 2. the resize hook must have fired (else nothing below is exercised)
string(REGEX MATCHALL "size: window=" _size_lines "${_log}")
list(LENGTH _size_lines _n_size)
if(_n_size LESS 2)
    message(FATAL_ERROR "expected >=2 size lines (EMBER_SMOKE_RESIZE fired?), got ${_n_size}")
endif()

# 3. / 4. regression markers must be absent
if(_log MATCHES "STALE viewport rect")
    message(FATAL_ERROR "DEFECT-1 REGRESSION: a size change was not resynced "
                        "('STALE viewport rect' appeared in the bounded run)")
endif()
if(_log MATCHES "MISMATCH")
    message(FATAL_ERROR "input rect disagrees with the window size ('MISMATCH' in output)")
endif()
if(NOT _log MATCHES "rect=window OK")
    message(FATAL_ERROR "'rect=window OK' never appeared — size line missing or failing")
endif()

# 5. parse the LAST size line (after the resize) and compare its parts.
#    CMake math() is integer-only, so scale (printed %.2f) becomes percent:
#    "1.00" -> 100, "1.50" -> 150; fb must equal round(window * pct / 100).
string(REGEX MATCHALL "size: window=[0-9]+x[0-9]+ fb=[0-9]+x[0-9]+ scale=[0-9]+\\.[0-9]+ rect=[0-9]+x[0-9]+\\(captured [0-9]+x[0-9]+\\)"
       _size_all "${_log}")
list(LENGTH _size_all _n_full)
if(_n_full EQUAL 0)
    message(FATAL_ERROR "no parseable size line — format changed? update this checker")
endif()
list(GET _size_all -1 _last)
# NOTE: CMake regex supports at most 9 capture groups — capture scale as ONE
# group ("1.50") and derive the integer percent below, or group 10 goes empty.
string(REGEX MATCH "window=([0-9]+)x([0-9]+) fb=([0-9]+)x([0-9]+) scale=([0-9]+\\.[0-9]+) rect=([0-9]+)x([0-9]+)\\(captured ([0-9]+)x([0-9]+)\\)"
     _m "${_last}")
set(_winW ${CMAKE_MATCH_1})
set(_winH ${CMAKE_MATCH_2})
set(_fbW  ${CMAKE_MATCH_3})
set(_fbH  ${CMAKE_MATCH_4})
set(_scaleRaw "${CMAKE_MATCH_5}")      # save BEFORE any other regex runs:
set(_rectW ${CMAKE_MATCH_6})           # string(REGEX REPLACE ...) clobbers
set(_rectH ${CMAKE_MATCH_7})           # CMAKE_MATCH_* with ITS own groups
set(_capW  ${CMAKE_MATCH_8})
set(_capH  ${CMAKE_MATCH_9})
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)" "\\1\\2" _scalePct "${_scaleRaw}")  # "1.00" -> 100

math(EXPR _expectFbW "(${_winW} * ${_scalePct} + 50) / 100")
math(EXPR _expectFbH "(${_winH} * ${_scalePct} + 50) / 100")

if(NOT _rectW EQUAL _winW OR NOT _rectH EQUAL _winH)
    message(FATAL_ERROR "input rect ${_rectW}x${_rectH} != window ${_winW}x${_winH}")
endif()
if(NOT _capW EQUAL _winW OR NOT _capH EQUAL _winH)
    message(FATAL_ERROR "rect captured at ${_capW}x${_capH}, window is ${_winW}x${_winH} — stale capture")
endif()
if(NOT _fbW EQUAL _expectFbW OR NOT _fbH EQUAL _expectFbH)
    message(FATAL_ERROR "framebuffer ${_fbW}x${_fbH} != window*scale ${_expectFbW}x${_expectFbH} "
                        "(renderer resize input and input rect disagree)")
endif()

# 6. renderer's initial size vs the input side's framebuffer
if(_log MATCHES "renderer ready \\(([0-9]+)x([0-9]+)\\)")
    set(_rrW ${CMAKE_MATCH_1})
    set(_rrH ${CMAKE_MATCH_2})
    # renderer-ready is logged at CREATE size; the run then resizes, so compare
    # against the FIRST size line's fb, not the last.
    string(REGEX MATCH "size: window=([0-9]+)x([0-9]+) fb=([0-9]+)x([0-9]+)" _first "${_log}")
    set(_firstFbW ${CMAKE_MATCH_3})
    set(_firstFbH ${CMAKE_MATCH_4})
    if(NOT _rrW EQUAL _firstFbW OR NOT _rrH EQUAL _firstFbH)
        message(FATAL_ERROR "renderer ready ${_rrW}x${_rrH} disagrees with framebuffer ${_firstFbW}x${_firstFbH}")
    endif()
else()
    message(WARNING "'renderer ready (WxH)' not found — renderer comparison skipped "
                    "(renderer module missing?)")
endif()

message(STATUS "PASS: rect ${_rectW}x${_rectH} == window, fb ${_fbW}x${_fbH} == window*${_scalePct}/100, "
               "renderer agrees, ${_n_size} size transitions, no STALE/MISMATCH")
