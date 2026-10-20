# VerifyEvidence.cmake — the per-item evidence verifier (item 106, Track A).
# Owner: Kiln · Platform (cmake/ is Build's). Contract: D33 (ARCHITECTURE §7 /
# TEAM §10), D36 for the attribution half (§11).
#
# Turns a CLAIM into a CHECK. An item declares its evidence in a plain
# `key: value` file (see cmake/evidence/*.evidence for the format in use);
# this script then verifies the declaration mechanically, in D33's order —
# the order is load-bearing: if the build/freshness check fails, NOTHING else
# about the item is admissible, because every other artifact would be evidence
# about some previous tree.
#
#   1. BUILD FIRST  newest source vs newest binary (CheckFreshness semantics);
#                  aborts everything on failure.
#   2. artifact     exists and is non-empty (per class).
#   3. artifact     postdates the sources it declares it depends on.
#   4. test         named case appears in `ctest -N` AND exits 0.
#   5. symbol       exists in the NAMED file; the test references it; EDITOR
#                  items additionally need a NON-TEST call site (catches
#                  'present is not reachable').
#   6. residue      the claim's named residue is present and non-empty, and is
#                  REPRINTED on success so a reader always sees what was not
#                  proven.
#
# Per-class extras (D33 table): RENDER needs `shader built: <name>` in a run
# log + a measured property with two numbers; DOC greps the WHOLE tree for the
# new sentence present and the stale claim absent; DECISION's network half is
# NOT implemented (CMake cannot set HTTP headers) — named as residue.
#
# D36 attribution: a declared Blender port must carry, verbatim, in its first
# lines: SPDX-FileCopyrightText, SPDX-License-Identifier, `Origin: blender/
# blender <path> @ <sha|tag>`, and `Modifications:`. RESIDUE, stated in this
# script's own output: an UNDECLARED port cannot be detected by grep — this
# check verifies DECLARED ports only; undeclared ones need a human or a diff.
#
# Usage:
#   cmake -DEVIDENCE_FILE=<file.evidence> -DSOURCE_DIR=<repo> -DBIN_DIR=<bin>
#         [-DBUILD_DIR=<build>] [-DCTEST_CONFIG=Release]
#         -P cmake/VerifyEvidence.cmake
#
# One command per line — two commands on one line is a CMake parse error
# (paid for once already). string(FIND) over regex wherever possible.

cmake_minimum_required(VERSION 3.25)

# ---- arguments -------------------------------------------------------------
foreach(_req EVIDENCE_FILE SOURCE_DIR BIN_DIR)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "VerifyEvidence: -D${_req}=... is required")
    endif()
endforeach()
if(NOT DEFINED BUILD_DIR)
    set(BUILD_DIR "${SOURCE_DIR}/build")
endif()
if(NOT DEFINED CTEST_CONFIG)
    set(CTEST_CONFIG "Release")
endif()
# CMAKE_CTEST_COMMAND is not guaranteed in -P script mode; derive it.
if(NOT DEFINED CMAKE_CTEST_COMMAND OR CMAKE_CTEST_COMMAND STREQUAL "")
    get_filename_component(_cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
    find_program(CMAKE_CTEST_COMMAND NAMES ctest ctest.exe HINTS "${_cmake_bin_dir}")
endif()
if(NOT CMAKE_CTEST_COMMAND)
    message(FATAL_ERROR "VerifyEvidence: ctest executable not found")
endif()
if(NOT EXISTS "${EVIDENCE_FILE}")
    message(FATAL_ERROR "VerifyEvidence: declaration not found: ${EVIDENCE_FILE}")
endif()

set(_fmt "%Y%m%d%H%M%S")

# ---- parse the declaration (key: value, repeatable keys append) ------------
# ENCODING UTF-8 is load-bearing: without it file(STRINGS) splits comment
# lines at non-ASCII bytes (em-dashes) and hands the parser a leading-space
# fragment that fails the key test. Strip every line defensively anyway.
file(STRINGS "${EVIDENCE_FILE}" _lines ENCODING UTF-8)
set(_item "")
set(_class "")
set(_claim "")
set(_residue "")
set(_symbol "")
set(_symbol_file "")
set(_test "")
set(_test_file "")
set(_call_site_file "")
set(_artifacts "")
set(_artifact_depends "")
set(_run_log "")
set(_shader "")
set(_measured "")
set(_port_file "")
set(_origins "")
set(_doc_must_contain "")
set(_doc_must_not_contain "")
set(_doc_must_not_contain_regex "")
set(_decision_id "")

foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line MATCHES "^#" OR _line STREQUAL "")
        continue()
    endif()
    if(NOT _line MATCHES "^([a-z_]+):[ ]?(.*)$")
        message(FATAL_ERROR "Parse error in ${EVIDENCE_FILE}: line is not 'key: value':\n  ${_line}")
    endif()
    set(_k "${CMAKE_MATCH_1}")
    set(_v "${CMAKE_MATCH_2}")
    string(STRIP "${_v}" _v)
    if(_k STREQUAL "item")
        set(_item "${_v}")
    elseif(_k STREQUAL "class")
        set(_class "${_v}")
    elseif(_k STREQUAL "claim")
        set(_claim "${_v}")
    elseif(_k STREQUAL "residue")
        set(_residue "${_v}")
    elseif(_k STREQUAL "symbol")
        set(_symbol "${_v}")
    elseif(_k STREQUAL "symbol_file")
        set(_symbol_file "${_v}")
    elseif(_k STREQUAL "test")
        set(_test "${_v}")
    elseif(_k STREQUAL "test_file")
        set(_test_file "${_v}")
    elseif(_k STREQUAL "call_site_file")
        set(_call_site_file "${_v}")
    elseif(_k STREQUAL "artifact")
        list(APPEND _artifacts "${_v}")
    elseif(_k STREQUAL "depends")
        set(_artifact_depends "${_v}")
    elseif(_k STREQUAL "artifact_depends_on")
        # deprecated alias for `depends` (D38 names the key `depends`)
        set(_artifact_depends "${_v}")
    elseif(_k STREQUAL "run_log")
        set(_run_log "${_v}")
    elseif(_k STREQUAL "shader")
        set(_shader "${_v}")
    elseif(_k STREQUAL "measured")
        set(_measured "${_v}")
    elseif(_k STREQUAL "port_file")
        set(_port_file "${_v}")
    elseif(_k STREQUAL "origin")
        list(APPEND _origins "${_v}")
    elseif(_k STREQUAL "doc_must_contain")
        list(APPEND _doc_must_contain "${_v}")          # phrase SET (repeatable)
    elseif(_k STREQUAL "doc_must_not_contain")
        list(APPEND _doc_must_not_contain "${_v}")      # phrase SET (repeatable)
    elseif(_k STREQUAL "doc_must_not_contain_regex")
        list(APPEND _doc_must_not_contain_regex "${_v}") # CMake regex, repeatable
    elseif(_k STREQUAL "decision_id")
        set(_decision_id "${_v}")
    else()
        message(FATAL_ERROR "Unknown key '${_k}' in ${EVIDENCE_FILE}. Known keys: item, class, claim, residue, symbol, symbol_file, test, test_file, call_site_file, artifact, depends, run_log, shader, measured, port_file, origin, doc_must_contain, doc_must_not_contain, doc_must_not_contain_regex, decision_id")
    endif()
endforeach()

message(STATUS "==== evidence verification: item '${_item}' (class ${_class}) ====")
message(STATUS "claim:   ${_claim}")

# ---- strip C++ comments (item-107 hole 1) ---------------------------------
# A symbol that appears ONLY in a comment satisfied CHECK 5: string(FIND) had
# no code-vs-comment awareness, and 'present in a comment' is exactly the
# 'present is not reachable' failure this check exists to catch (Verity's
# mutation: definition removed, name kept in a comment -> passed).
# Line-based, stateful over /* */ blocks. One command per line.
# RESIDUE, deliberately: string literals are NOT stripped, so a symbol inside
# a quoted string still matches, and a '//' inside a string (a URL) truncates
# that line — a false failure, not a false pass, which is the safe direction.
function(ember_strip_comments text out_var)
    string(REPLACE "\r\n" "\n" _rest "${text}")
    set(_out "")
    set(_in_block FALSE)
    set(_guard 0)
    while(NOT _rest STREQUAL "" AND _guard LESS 200000)
        math(EXPR _guard "${_guard} + 1")
        string(FIND "${_rest}" "\n" _nl)
        if(_nl LESS 0)
            set(_line "${_rest}")
            set(_rest "")
        else()
            string(SUBSTRING "${_rest}" 0 ${_nl} _line)
            math(EXPR _start "${_nl} + 1")
            string(SUBSTRING "${_rest}" ${_start} -1 _rest)
        endif()
        if(_in_block)
            string(FIND "${_line}" "*/" _endc)
            if(_endc LESS 0)
                set(_line "")
            else()
                math(EXPR _cs "${_endc} + 2")
                string(SUBSTRING "${_line}" ${_cs} -1 _line)
                set(_in_block FALSE)
            endif()
        endif()
        if(NOT _in_block)
            set(_keep_going TRUE)
            while(_keep_going)
                set(_keep_going FALSE)
                string(FIND "${_line}" "//" _lpos)
                string(FIND "${_line}" "/*" _bpos)
                if(_bpos GREATER -1 AND (_lpos LESS 0 OR _bpos LESS _lpos))
                    string(SUBSTRING "${_line}" 0 ${_bpos} _before)
                    math(EXPR _after "${_bpos} + 2")
                    string(SUBSTRING "${_line}" ${_after} -1 _seg)
                    string(FIND "${_seg}" "*/" _close)
                    if(_close LESS 0)
                        set(_line "${_before}")
                        set(_in_block TRUE)
                    else()
                        math(EXPR _cc "${_close} + 2")
                        string(SUBSTRING "${_seg}" ${_cc} -1 _afterc)
                        set(_line "${_before}${_afterc}")
                        set(_keep_going TRUE)
                    endif()
                elseif(_lpos GREATER -1)
                    string(SUBSTRING "${_line}" 0 ${_lpos} _line)
                endif()
            endwhile()
        endif()
        set(_out "${_out}${_line}\n")
    endwhile()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

# ---- required header fields (checked first: a declaration without them is
#      not a claim this script can verify at all) -----------------------------
if(_item STREQUAL "")
    message(FATAL_ERROR "MISSING: 'item:' — the declaration does not name an item")
endif()
if(_class STREQUAL "")
    message(FATAL_ERROR "MISSING: 'class:' — declare BUILD|SCENE|RENDER|EDITOR|DECISION|DOC")
endif()
if(_claim STREQUAL "")
    message(FATAL_ERROR "MISSING: 'claim:' — a declaration with no claim verifies nothing")
endif()

# ---- check 1: BUILD FIRST — freshness gates all other evidence -------------
message(STATUS "check 1/6 BUILD-FRESHNESS: newest source vs newest binary in ${BIN_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}" "-DBIN_DIR=${BIN_DIR}"
            -P "${SOURCE_DIR}/cmake/CheckFreshness.cmake"
    RESULT_VARIABLE _fresh_rc
    OUTPUT_VARIABLE _fresh_out
    ERROR_VARIABLE _fresh_err)
if(NOT _fresh_rc EQUAL 0)
    message(FATAL_ERROR
        "CHECK 1/6 FAILED — NOTHING ELSE ABOUT THIS ITEM IS ADMISSIBLE.\n"
        "The tree is not fresh: ${_fresh_out}${_fresh_err}\n"
        "Every artifact below would be evidence about a PREVIOUS tree. Rebuild, "
        "re-take the artifacts, then re-run this verifier. (Residue: under ctest "
        "build.gate already guarantees the build exit code; run standalone, this "
        "check verifies freshness only, not the exit code.)")
endif()
message(STATUS "  [ok] sources are not newer than the newest binary")

# ---- check 2: artifact exists and is non-empty -----------------------------
foreach(_a IN LISTS _artifacts)
    set(_apath "${_a}")
    if(NOT IS_ABSOLUTE "${_apath}")
        set(_apath "${SOURCE_DIR}/${_a}")
    endif()
    if(NOT EXISTS "${_apath}")
        message(FATAL_ERROR
            "CHECK 2/6 FAILED — artifact does not exist.\n"
            "  declared artifact: ${_a}\n"
            "  looked at:        ${_apath}\n"
            "The claim names this file as its evidence; the claim cannot stand without it.")
    endif()
    if(IS_DIRECTORY "${_apath}")
        message(FATAL_ERROR "CHECK 2/6 FAILED — artifact '${_a}' is a directory, not a file")
    endif()
    file(SIZE "${_apath}" _asize)
    if(_asize EQUAL 0)
        message(FATAL_ERROR
            "CHECK 2/6 FAILED — artifact '${_a}' exists but is EMPTY (0 bytes).\n"
            "An empty artifact is the shape of a run that crashed before producing output.")
    endif()
    message(STATUS "  [ok] artifact ${_a} exists (${_asize} bytes)")
endforeach()

# ---- check 3: artifact postdates its declared sources ----------------------
if(NOT _artifacts STREQUAL "")
    # D38: `depends:` is REQUIRED with `artifact:`. No fallback glob — a
    # guessed dependency set is a guessed contract, and a check that rejects
    # correct input trains its readers to ignore it. (The previous fallback
    # globbed tests/ against bin/Ember3D.exe, so editing ANY test invalidated
    # evidence about the app — Verity found it, Ash ruled it out, D38.)
    #
    # The two freshness checks differ BY DESIGN and must not be described as
    # agreeing: build.freshness (CheckFreshness) compares the newest source
    # against the newest binary ANYWHERE (did anything rebuild?); CHECK 3
    # compares a NAMED artifact against the sources the declaration says it
    # consumes (is THIS evidence about THIS tree?). Different questions.
    #
    # Entry grammar, per D38: a file path, or a DIRECTORY meaning 'everything
    # under it, recursively'. The declaration names the scope; expansion is
    # mechanical and infers nothing (literal globs like 'src/**' are rejected
    # by EXISTS — directory entries are the shape that works with this parser).
    if(_artifact_depends STREQUAL "")
        message(FATAL_ERROR
            "CHECK 3/6 FAILED — 'artifact:' declared without 'depends:'.\n"
            "D38: depends is REQUIRED whenever artifact is present. Name what the "
            "artifact consumes, e.g.\n"
            "  artifact: bin/Ember3D.exe\n"
            "  depends: src/, vendor/\n"
            "Directory entries expand recursively; file entries are taken as-is. "
            "The verifier will not guess a dependency set for you — a guessed "
            "contract is not a contract.")
    endif()
    set(_deps "")
    foreach(_d IN LISTS _artifact_depends)
        string(REPLACE "," ";" _d "${_d}")
        foreach(_one IN LISTS _d)
            string(STRIP "${_one}" _one)
            if(_one STREQUAL "")
                continue()
            endif()
            if(IS_ABSOLUTE "${_one}")
                set(_resolved "${_one}")
            else()
                string(REGEX REPLACE "/+$" "" _one_trimmed "${_one}")
                set(_resolved "${SOURCE_DIR}/${_one_trimmed}")
            endif()
            if(NOT EXISTS "${_resolved}")
                message(FATAL_ERROR
                    "CHECK 3/6 FAILED — declared dependency does not exist: ${_one}\n"
                    "  looked at: ${_resolved}\n"
                    "depends entries are files or directories; this is neither "
                    "(literal globs are not supported — name the directory).")
            endif()
            if(IS_DIRECTORY "${_resolved}")
                file(GLOB_RECURSE _sub_files LIST_DIRECTORIES false "${_resolved}/*")
                if(_sub_files STREQUAL "")
                    message(FATAL_ERROR
                        "CHECK 3/6 FAILED — depends directory is empty: ${_one}\n"
                        "An empty scope means the declaration asserts nothing about "
                        "what the artifact consumed; name a directory that has files.")
                endif()
                list(APPEND _deps ${_sub_files})
            else()
                list(APPEND _deps "${_resolved}")
            endif()
        endforeach()
    endforeach()
    set(_newest_dep "")
    set(_newest_dep_file "")
    foreach(_d IN LISTS _deps)
        if(NOT EXISTS "${_d}")
            message(FATAL_ERROR "CHECK 3/6 FAILED — declared dependency does not exist: ${_d}")
        endif()
        file(TIMESTAMP "${_d}" _ts ${_fmt} UTC)
        if(_newest_dep STREQUAL "" OR _ts STRGREATER _newest_dep)
            set(_newest_dep "${_ts}")
            set(_newest_dep_file "${_d}")
        endif()
    endforeach()
    foreach(_a IN LISTS _artifacts)
        set(_apath "${_a}")
        if(NOT IS_ABSOLUTE "${_apath}")
            set(_apath "${SOURCE_DIR}/${_a}")
        endif()
        file(TIMESTAMP "${_apath}" _ats ${_fmt} UTC)
        if(_ats STRLESS _newest_dep)
            message(FATAL_ERROR
                "CHECK 3/6 FAILED — artifact is OLDER than a source it depends on.\n"
                "  artifact:    ${_a}  (produced ${_ats})\n"
                "  newer source: ${_newest_dep_file}  (edited  ${_newest_dep})\n"
                "The artifact is evidence about the PREVIOUS tree (stale-ctest trap, "
                "generalised). Rebuild, re-take the artifact, re-run.")
        endif()
    endforeach()
    message(STATUS "  [ok] artifact(s) postdate the newest of the declared sources (newest: ${_newest_dep_file})")
endif()

# ---- check 4: the named test exists in ctest AND exits 0 -------------------
if(NOT _test STREQUAL "")
    execute_process(
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" -C "${CTEST_CONFIG}" -N -R "^${_test}$"
        RESULT_VARIABLE _n_rc
        OUTPUT_VARIABLE _n_out
        ERROR_VARIABLE _n_err)
    string(FIND "${_n_out}" "${_test}" _npos)
    if(NOT _npos GREATER -1)
        message(FATAL_ERROR
            "CHECK 4/6 FAILED — no ctest case named '${_test}'.\n"
            "command: ctest --test-dir '${BUILD_DIR}' -C ${CTEST_CONFIG} -N -R '^${_test}$'\n"
            "exit:    ${_n_rc}\n"
            "stdout:  ${_n_out}\n"
            "stderr:  ${_n_err}\n"
            "The declaration names a test that does not exist; the claim rests on nothing. "
            "(If the test was never REGISTERED — ctest -N lists registrations, not files.)")
    endif()
    message(STATUS "  [ok] ctest case '${_test}' is registered")
    execute_process(
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" -C "${CTEST_CONFIG}" -R "^${_test}$"
        RESULT_VARIABLE _t_rc
        OUTPUT_VARIABLE _t_out
        ERROR_VARIABLE _t_err)
    if(NOT _t_rc EQUAL 0)
        message(FATAL_ERROR
            "CHECK 4/6 FAILED — ctest case '${_test}' did not pass (exit ${_t_rc}).\n"
            "${_t_out}${_t_err}")
    endif()
    message(STATUS "  [ok] ctest case '${_test}' passed")
endif()

# ---- check 5: symbol in the named file; test references it; EDITOR call site
if(NOT _symbol STREQUAL "")
    if(_symbol_file STREQUAL "")
        message(FATAL_ERROR "CHECK 5/6 FAILED — 'symbol:' declared without 'symbol_file:'. "
                            "The contract requires the symbol in the NAMED file, not 'somewhere'.")
    endif()
    set(_sfp "${_symbol_file}")
    if(NOT IS_ABSOLUTE "${_sfp}")
        set(_sfp "${SOURCE_DIR}/${_symbol_file}")
    endif()
    if(NOT EXISTS "${_sfp}")
        message(FATAL_ERROR "CHECK 5/6 FAILED — named symbol file does not exist: ${_symbol_file}")
    endif()
    file(READ "${_sfp}" _sf_raw)
    ember_strip_comments("${_sf_raw}" _sf_content)
    string(FIND "${_sf_content}" "${_symbol}" _spos)
    if(_spos LESS 0)
        message(FATAL_ERROR
            "CHECK 5/6 FAILED — symbol '${_symbol}' NOT FOUND in ${_symbol_file}.\n"
            "Searched the CODE (comments stripped): if it appears only in a comment, "
            "it does not count — a comment is not a definition (item-107 hole 1). "
            "Either the symbol is not there or the path is wrong.")
    endif()
    message(STATUS "  [ok] symbol '${_symbol}' present in ${_symbol_file}")

    if(NOT _test_file STREQUAL "")
        set(_tfp "${_test_file}")
        if(NOT IS_ABSOLUTE "${_tfp}")
            set(_tfp "${SOURCE_DIR}/${_test_file}")
        endif()
        if(NOT EXISTS "${_tfp}")
            message(FATAL_ERROR "CHECK 5/6 FAILED — named test file does not exist: ${_test_file}")
        endif()
        file(READ "${_tfp}" _tf_raw)
        ember_strip_comments("${_tf_raw}" _tf_content)
        string(FIND "${_tf_content}" "${_symbol}" _tpos)
        if(_tpos LESS 0)
            message(FATAL_ERROR
                "CHECK 5/6 FAILED — test file ${_test_file} does NOT reference symbol '${_symbol}' in its code.\n"
                "Comments are stripped before matching (item-107 hole 1): a test that only "
                "MENTIONS the symbol in a comment does not exercise it. For this class the "
                "test IS the call site that makes 'defined' mean 'reachable'.")
        endif()
        message(STATUS "  [ok] test file ${_test_file} references '${_symbol}'")
    endif()

    if(_class STREQUAL "EDITOR")
        if(_call_site_file STREQUAL "")
            message(FATAL_ERROR
                "CHECK 5/6 FAILED — EDITOR class REQUIRES 'call_site_file:'.\n"
                "The contract: the symbol must have a NON-TEST call site — the test drives "
                "the real code path, not a copy of its maths. Name the production file that "
                "calls it.")
        endif()
        set(_csp "${_call_site_file}")
        if(NOT IS_ABSOLUTE "${_csp}")
            set(_csp "${SOURCE_DIR}/${_call_site_file}")
        endif()
        if(NOT EXISTS "${_csp}")
            message(FATAL_ERROR "CHECK 5/6 FAILED — named call-site file does not exist: ${_call_site_file}")
        endif()
        string(FIND "${_call_site_file}" "tests/" _cs_in_tests)
        if(NOT _cs_in_tests LESS 0)
            message(FATAL_ERROR
                "CHECK 5/6 FAILED — call_site_file '${_call_site_file}' is under tests/.\n"
                "A test cannot be its own non-test call site; that is the 'present is not "
                "reachable' hole this check exists to close.")
        endif()
        file(READ "${_csp}" _cs_raw)
        ember_strip_comments("${_cs_raw}" _cs_content)
        string(FIND "${_cs_content}" "${_symbol}" _cspos)
        if(_cspos LESS 0)
            message(FATAL_ERROR
                "CHECK 5/6 FAILED — non-test call site: '${_symbol}' NOT FOUND in ${_call_site_file} (code, comments stripped).\n"
                "The symbol exists and a test touches it, but no production code calls it — "
                "'present is not reachable'. Either wire it or narrow the claim.")
        endif()
        message(STATUS "  [ok] non-test call site: ${_call_site_file} references '${_symbol}'")
    endif()
endif()

# ---- check 6: the claim names its residue ----------------------------------
if(_residue STREQUAL "")
    message(FATAL_ERROR
        "CHECK 6/6 FAILED — no 'residue:' line.\n"
        "D33 rule 1: every claim names what it does NOT prove. A claim with no stated "
        "residue is either trivial or unexamined; this verifier refuses it.")
endif()
message(STATUS "  [ok] residue named: ${_residue}")

# ---- per-class extras ------------------------------------------------------
if(_class STREQUAL "RENDER")
    if(_run_log STREQUAL "" OR _shader STREQUAL "")
        message(FATAL_ERROR "RENDER class needs 'run_log:' and 'shader:' — the artifact is the run reporting 'shader built: <name>'.")
    endif()
    set(_rlp "${_run_log}")
    if(NOT IS_ABSOLUTE "${_rlp}")
        set(_rlp "${SOURCE_DIR}/${_run_log}")
    endif()
    if(NOT EXISTS "${_rlp}")
        message(FATAL_ERROR "RENDER: run log does not exist: ${_run_log}")
    endif()
    file(READ "${_rlp}" _rl_content)
    string(FIND "${_rl_content}" "shader built: ${_shader}" _rlpos)
    if(_rlpos LESS 0)
        message(FATAL_ERROR "RENDER: run log ${_run_log} never reports 'shader built: ${_shader}'")
    endif()
    if(_measured STREQUAL "")
        message(FATAL_ERROR "RENDER: 'measured:' must name the property AND two numbers (before/after) — a picture with no numbers is not an assertion.")
    endif()
    string(REGEX MATCHALL "[0-9]+\\.?[0-9]*" _nums "${_measured}")
    list(LENGTH _nums _nnums)
    if(_nnums LESS 2)
        message(FATAL_ERROR "RENDER: 'measured:' must contain at least TWO numbers (before/after); found ${_nnums}")
    endif()
    message(STATUS "  [ok] render: shader '${_shader}' reported, measured '${_measured}'")
endif()

if(_class STREQUAL "DOC")
    # item-107 hole 2 (Verity, measured): a single verbatim stale string is a
    # loop — the same claim reworded walks straight through the grep (it had
    # already happened in ROADMAP.md:191 vs :37, different phrasing). So the
    # stale side is a PHRASE SET plus optional REGEXES: every corrected phrase
    # must be present, every stale phrase absent, no regex may match. The
    # author enumerates the phrasings they know; the regex is for when the
    # claim survives inflection. Residue: a paraphrase nobody listed and no
    # regex covers still passes — enumeration cannot be proven complete.
    list(LENGTH _doc_must_contain _n_contain)
    if(_n_contain EQUAL 0)
        message(FATAL_ERROR "DOC class needs at least one 'doc_must_contain: <sentence>' (the corrected text).")
    endif()
    # sweep the WHOLE tree (not just src — per D33/TEAM §10 the stale claim
    # lives wherever it was written), excluding generated dirs and vendor.
    file(GLOB_RECURSE _tree_files "${SOURCE_DIR}/src/*" "${SOURCE_DIR}/tests/*"
         "${SOURCE_DIR}/docs/*" "${SOURCE_DIR}/*.md" "${SOURCE_DIR}/cmake/*")
    # EXCLUDED: *.evidence declarations - a declaration QUOTES its own phrases
    # (doc_must_not_contain literally appears in the declaring file), so
    # sweeping declarations would make every DOC case fail itself. First
    # DOC-class declaration in the tree caught this (item 108): declarations
    # assert ABOUT text, they are not themselves the claim. A stale claim in a
    # real doc still goes red - that is the whole point of the whole-tree sweep.
    # start from 'unfound' and REMOVE as soon as any file contains the phrase:
    # a phrase must be present SOMEWHERE, so only never-found ones remain.
    set(_unfound "${_doc_must_contain}")
    set(_stale_hits "")
    foreach(_f IN LISTS _tree_files)
        if(IS_DIRECTORY "${_f}")
            continue()
        endif()
        if(_f MATCHES "\\.evidence$")
            continue()
        endif()
        if("${_f}" STREQUAL "${EVIDENCE_FILE}")
            continue()
        endif()
        get_filename_component(_ext "${_f}" EXT)
        if(_ext STREQUAL ".png" OR _ext STREQUAL ".exe" OR _ext STREQUAL ".lib")
            continue()
        endif()
        file(READ "${_f}" _c LIMIT 262144)
        foreach(_phrase IN LISTS _doc_must_contain)
            string(FIND "${_c}" "${_phrase}" _p1)
            if(NOT _p1 LESS 0)
                list(REMOVE_ITEM _unfound "${_phrase}")
            endif()
        endforeach()
        foreach(_phrase IN LISTS _doc_must_not_contain)
            string(FIND "${_c}" "${_phrase}" _p2)
            if(NOT _p2 LESS 0)
                list(APPEND _stale_hits "${_f} verbatim: \"${_phrase}\"")
            endif()
        endforeach()
        foreach(_rx IN LISTS _doc_must_not_contain_regex)
            if(_c MATCHES "${_rx}")
                list(APPEND _stale_hits "${_f} matching regex /${_rx}/")
            endif()
        endforeach()
    endforeach()
    # phrases still in _unfound after the whole sweep are missing everywhere
    if(_unfound)
        list(REMOVE_DUPLICATES _unfound)
        list(JOIN _unfound "\n    " _missing_txt)
        message(FATAL_ERROR
            "DOC: corrected text NOT FOUND anywhere in the tree (every doc_must_contain "
            "phrase must be present in at least one swept file):\n    ${_missing_txt}")
    endif()
    if(_stale_hits)
        list(REMOVE_DUPLICATES _stale_hits)
        list(JOIN _stale_hits "\n    " _stale_txt)
        message(FATAL_ERROR
            "DOC: STALE CLAIM still present (verbatim phrases and regexes are each "
            "checked across the whole tree — reworded variants need their own entry "
            "or a regex, item-107 hole 2):\n    ${_stale_txt}\n"
            "Regex dialect note: CMake uses Spencer regexes — {n,m} INTERVALS ARE NOT "
            "SUPPORTED (they silently never match); use [^.]* or .* instead.")
    endif()
    list(LENGTH _doc_must_not_contain _n_stale)
    list(LENGTH _doc_must_not_contain_regex _n_rx)
    message(STATUS "  [ok] doc: ${_n_contain} corrected phrase(s) present; "
                   "${_n_stale} stale phrase(s) + ${_n_rx} regex(es) absent")
endif()

if(_class STREQUAL "DECISION")
    if(_decision_id STREQUAL "")
        message(FATAL_ERROR "DECISION class needs 'decision_id: <finding id>'.")
    endif()
    message(STATUS "  [ok] decision id declared: ${_decision_id}")
    message(STATUS "  RESIDUE (verifier): the frozen-ness of this decision is verified by the hub "
                   "(GET /contracts -> decisionId), NOT by this script — CMake cannot send "
                   "authenticated HTTP. This check confirms the claim NAMES a decision; it "
                   "cannot confirm that decision is FROZEN. Ask the Lead or the hub for that.")
endif()

# ---- D36: attribution of a DECLARED Blender port ---------------------------
if(NOT _port_file STREQUAL "")
    set(_pfp "${_port_file}")
    if(NOT IS_ABSOLUTE "${_pfp}")
        set(_pfp "${SOURCE_DIR}/${_port_file}")
    endif()
    if(NOT EXISTS "${_pfp}")
        message(FATAL_ERROR "D36: declared port file does not exist: ${_port_file}")
    endif()
    file(STRINGS "${_pfp}" _phead LIMIT_COUNT 40)
    string(REPLACE ";" "\n" _phead_text "${_phead}")
    foreach(_needle "SPDX-FileCopyrightText" "SPDX-License-Identifier" "Origin: blender/blender" "Modifications:")
        string(FIND "${_phead_text}" "${_needle}" _ppos)
        if(_ppos LESS 0)
            message(FATAL_ERROR
                "D36 ATTRIBUTION FAILED — ${_port_file} is a DECLARED port but lacks '${_needle}' "
                "in its first 40 lines.\n"
                "Required verbatim (from the original Blender header):\n"
                "  /* SPDX-FileCopyrightText: <year> Blender Authors\n"
                "   *\n"
                "   * SPDX-License-Identifier: GPL-2.0-or-later */\n"
                "  Origin: blender/blender <path in blender> @ <sha|tag>\n"
                "  Modifications: <what you changed>\n"
                "A missing header is a build failure, not a style nit (WAVE-5 §1.5).")
        endif()
    endforeach()
    foreach(_o IN LISTS _origins)
        string(FIND "${_phead_text}" "${_o}" _opo)
        if(_opo LESS 0)
            message(FATAL_ERROR "D36: declared origin '${_o}' not found in the first 40 lines of ${_port_file}")
        endif()
    endforeach()
    message(STATUS "  [ok] D36 attribution complete in ${_port_file}")
    message(STATUS "  RESIDUE (verifier, D36): an UNDECLARED port CANNOT be detected by grep — "
                   "this check verifies DECLARED ports only. An undeclared port needs a human "
                   "or a source diff against Blender. Do not read a green run as 'no plagiarised "
                   "code'; read it as 'every port someone declared is attributed'.")
endif()

# ---- verdict ----------------------------------------------------------------
message(STATUS "==== PASS: item '${_item}' — all declared checks hold ====")
message(STATUS "residue (what this does NOT prove): ${_residue}")
