# BuildGate.cmake — the runner behind the build.gate test. Kiln · Platform.
#
# WHY: Verity caught ctest reporting 22/22 GREEN while the tree did not compile,
# because bin/Ember3D.exe was older than the sources (item 92). A green ctest on
# a red tree is a lie; only the build exit code is authoritative. This gate
# rebuilds the SAME binary dir ctest is running from, so every test afterwards
# measures a binary that matches the sources — or the gate fails and everything
# else is marked Not Run.
#
# One retry: concurrent agents hold .exe locks now and then (transient LNK1104,
# seen repeatedly). One retry shrugs that off WITHOUT masking a real error -
# a genuinely red tree fails both attempts and the gate fails loudly.
# (Two short pauses between three attempts: the shared bin/ has several
# concurrent builders; a red tree still fails all three and fails loudly.)

if(NOT DEFINED GATE_BINARY_DIR OR NOT DEFINED GATE_CONFIG)
    message(FATAL_ERROR "BuildGate: GATE_BINARY_DIR and GATE_CONFIG are required")
endif()

set(_cmd "${CMAKE_COMMAND}" --build "${GATE_BINARY_DIR}" --config "${GATE_CONFIG}")

set(_attempt 0)
set(_rc 1)
set(_out "")
set(_err "")
while(_attempt LESS 3)
    math(EXPR _attempt "${_attempt} + 1")
    execute_process(COMMAND ${_cmd}
                    RESULT_VARIABLE _rc
                    OUTPUT_VARIABLE _out
                    ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        break()
    endif()
    if(_attempt LESS 3)
        message(STATUS "build.gate: build exited ${_rc} on attempt ${_attempt} - retrying "
                       "(transient file locks happen with concurrent builds)")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3)
    endif()
endwhile()

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "build.gate: THE TREE IS RED - ctest refuses to run any test against it.\n"
        "cmake --build ${GATE_BINARY_DIR} --config ${GATE_CONFIG} exited ${_rc} after 3 attempts.\n"
        "A green ctest on a red tree is the failure this gate exists to make impossible.\n"
        "${_out}${_err}")
endif()

message(STATUS "build.gate: tree builds clean (cmake --build ${GATE_BINARY_DIR} "
               "--config ${GATE_CONFIG} exit 0) - the suite below measures current binaries")
