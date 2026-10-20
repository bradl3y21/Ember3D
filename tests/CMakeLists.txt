# tests/CMakeLists.txt — owned by Verity · QA.
# Included by the root CMakeLists.txt via add_subdirectory (ARCHITECTURE.md §5).
#
# Policy (tests/README.md):
#   * CTest must pass with NO OpenGL context. Default tests link only
#     core/scene (pure data). Anything needing a GPU is registered only
#     under the explicit EMBER_ENABLE_GPU_TESTS option and passes --gpu.
#
# Modules land over time (M0 gate). A test whose module target does not
# exist yet is DEFERRED with a loud configure-time message — not silently
# dropped — so `ctest` stays green while coverage visibly grows with the
# code it covers.

set(EMBER_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- harness self-test (no module dependencies; always built) ----------------
add_executable(test_harness_selftest test_harness_selftest.cpp)
target_include_directories(test_harness_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(test_harness_selftest PRIVATE cxx_std_20)
add_test(NAME harness.selftest COMMAND test_harness_selftest)

# ---- helper: register a pure (no-GPU) test if its module target exists -------
function(ember_add_pure_test name source link_target)
    if(TARGET ${link_target})
        add_executable(${name} ${source})
        target_include_directories(${name} PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(${name} PRIVATE ${link_target})
        # ARCHITECTURE.md §3 pins C++20; demand it on our targets directly
        # rather than trusting whatever the root sets globally.
        target_compile_features(${name} PRIVATE cxx_std_20)
        add_test(NAME ${name} COMMAND ${name})
    else()
        message(STATUS
            "Verity/QA: test '${name}' deferred — target '${link_target}' "
            "does not exist yet (owner: see TEAM.md). It registers automatically "
            "once the module lands.")
    endif()
endfunction()

# ---- helper: header-only test (no link at all) -------------------------------
# core's vocabulary (Math.h, Types.h) is header-only by design, so the math
# tests must NOT link ember_core: linking would couple them to App.cpp's
# compilation, and a breakage there would mask pure-math results. Verified
# working on 2026-10-16 while ember_core did not compile (missing
# platform::timeSeconds declaration): the math suite still built and ran.
function(ember_add_header_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(${name} PRIVATE cxx_std_20)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---- the suite ---------------------------------------------------------------
# core math: pure, header-only — no module target needed (see above).
ember_add_header_test(test_core_math        test_core_math.cpp)
ember_add_header_test(test_core_math_edges  test_core_math_edges.cpp)
# scene + .ember round-trip: need the compiled scene module (links core).
ember_add_pure_test(test_scene       test_scene.cpp       ember_scene)
ember_add_pure_test(test_scene_io    test_scene_io.cpp    ember_scene)
# Item 157 (D22): the 3D cursor is scene data. Round-trip bit-exact, ONE record in
# the documented place with 9 significant digits, absent record -> origin, and the
# FORMAT.md section 7 failure rules (nan/inf, duplicate, wrong field count,
# out-of-order) leave the target world completely unchanged.
ember_add_pure_test(test_scene_cursor test_scene_cursor.cpp ember_scene)
# Item 213/209: camera objects, typed projection settings and the selected
# scene camera (the World::activeCamera() pointer the v3 `active <id>` record carries) survive a
# save/load cycle with their fields intact, plus controls that prove the datum
# is carried by its record (remove it -> no active camera), a dangling active id
# is rejected atomically, and all typed v3 camera projection fields round-trip.
ember_add_pure_test(test_scene_camera test_scene_camera.cpp ember_scene)
# M1 orbit camera: pure matrix math (no GL calls, no context needed). Links
# ember_render because Camera.cpp lives there, but OrbitCamera never touches
# GL, so the suite still passes on a context-free machine.
ember_add_pure_test(test_render_camera test_render_camera.cpp ember_render)
# CPU-only path-tracer regressions: packet snapshot ownership, deterministic
# progressive accumulation, and bounded diffuse/environment convergence.
ember_add_pure_test(test_render_path_tracer test_render_path_tracer.cpp ember_render)
# Dependency-free image I/O and texture decode/sampling regressions; both are
# CPU-only and remain available in the context-free default test run.
ember_add_pure_test(test_render_path_image_io test_render_path_image_io.cpp ember_render)
ember_add_pure_test(test_render_texture_image test_render_texture_image.cpp ember_render)
# CPU/headless viewport lifecycle coverage: engine/mode transitions, stable
# packet keys, cancellation/restart, and observable progressive job state.
ember_add_pure_test(test_editor_cycles_viewport test_editor_cycles_viewport.cpp ember_editor)
# CPU-only numerical and lifecycle suites for offline path-rendering utilities.
ember_add_pure_test(test_render_path_bsdf test_render_path_bsdf.cpp ember_render)
ember_add_pure_test(test_render_path_sampling test_render_path_sampling.cpp ember_render)
ember_add_pure_test(test_render_path_denoiser test_render_path_denoiser.cpp ember_render)
ember_add_pure_test(test_render_offline_job test_render_offline_job.cpp ember_render)
# M5 prep: delete-undo (captureSnapshot/reinsert), duplicate(), and the
# destroy() survivor-consistency contract. Pure scene-graph data, no GPU.
ember_add_pure_test(test_scene_undo    test_scene_undo.cpp    ember_scene)
# WHAT-REMAINS defect 1 (fullscreen/resize orbit), PURE half: the hit-test
# contract, the D17 same-space rule, viewportRect() as staleness signal.
# Input.cpp includes only Input.h, so this runs with no window and no context.
ember_add_pure_test(test_platform_input test_platform_input.cpp ember_platform)
# M5 gizmo drag math (item 19). Links ember_editor: the math lives behind
# ImGui::GetIO() in Gizmo.cpp's anonymous namespace, so the test drives the REAL
# GizmoController through a HEADLESS ImGui context (CPU-only, no GL, no window).
# Proven context-free: no glfw/glad entry point is called at runtime.
ember_add_pure_test(test_editor_gizmo test_editor_gizmo.cpp ember_editor)
# Modal G/R/S state machine (item 103, WHAT-REMAINS A2 / D29 4.5). Drives the
# REAL ModalTransform through a headless ImGui context, CPU-only. This closes the
# gap Chisel's EMBER_SELFTEST_TRANSFORM states plainly: "rotate/scale share the
# same machine but have no scripted assertions yet". Covers typed translate AND
# rotate AND scale, cancel restoring the EXACT original, and ONE gesture == ONE
# undo entry across a many-frame drag (the continuation-merge path).
ember_add_pure_test(test_editor_modal test_editor_modal.cpp ember_editor)
# Item 135: the fixed layout TILES the window — every pixel in exactly one of
# {header, status, viewport, Area A/B/C} or in a uniform-width split border
# (Blender has area borders too: docs.blender.org .../window_system/areas.html),
# no gap, no overlap — and the default layout is identical across two launches.
# Drives the real drawWorkspace/beginDockedPanel/drawHeaderBar/drawStatusBar in a
# headless ImGui context with docking enabled; no GL.
ember_add_pure_test(test_editor_layout test_editor_layout.cpp ember_editor)
# Item 168: the file also carries TEST_GPU real-Editor invariants (hidden GLFW window), so it needs the platform
# window and glad; they are skipped unless --gpu, and run as editor.layout_invariants_real_editor below.
if(TARGET test_editor_layout AND TARGET ember_platform AND TARGET glad)
    target_link_libraries(test_editor_layout PRIVATE ember_platform ember_render ember_scene glad)
    target_compile_definitions(test_editor_layout PRIVATE EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
# Independent headless chrome regression: actual tiled Workspace, Area Header
# selector, splitter drag, semantic theme colors and pinned Status Bar geometry.
ember_add_pure_test(test_editor_ui_chrome test_editor_ui_chrome.cpp ember_editor)
if(TARGET test_editor_ui_chrome AND TARGET ember_platform AND TARGET glad)
    target_link_libraries(test_editor_ui_chrome PRIVATE ember_platform ember_render ember_scene glad)
endif()
# Item 185 (D43): the Properties category rail's six original vector glyphs (src/editor/Icons.*).
# A PIXEL test: drawIcon() is called with explicit button rects and a uiScale, and the real ImDrawData
# is software-rasterised. Proves non-empty, bounded, centred, uniformly scaled glyphs at uiScale 1 and
# 2 (the same glyph, IoU-checked after downsampling), that every vertex carries the caller's tint, that
# the rail draws the SELECTED category in ThemeTokens::active and the rest in textPrimary, and that a
# hovered row's background becomes ThemeTokens::hover. Headless - no GL context, so the default CTest
# run stays context-free.
ember_add_pure_test(test_editor_icons test_editor_icons.cpp ember_editor)
# Items 186/187 and 209: the Properties Render, Output and Camera categories must OFFER WORKING
# CONTROLS, not just visible ones. Headless (no GL) against the LIVE overload
# drawProperties(world, undo, tab, viewport, file, doc, timeline) that Editor::drawArea calls: real
# input events drive the item rects ImGui reports, the shadow settings / document path / timeline are
# read back after the write, the unsupported-setting notices are asserted as their DRAWN line widths
# (text is not stored by ImGui), and each predicate has a control that can fail.
ember_add_pure_test(test_editor_properties test_editor_properties.cpp ember_editor)
# The Outliner collection tree, eye toggle and child rows are a real UI path,
# not just World backend coverage. Headless ImGui hit-tests the Collection rows
# and confirms visibility changes are undoable through drawOutliner().
ember_add_pure_test(test_editor_outliner test_editor_outliner.cpp ember_editor)
# Item 191 (UI-12, D44): the Timeline is a frame-range/playhead ruler - Start/End fields (defaults 1/250,
# End>=Start clamp), a full-width NUMBERED ruler, and click/drag moving the playhead clamped to the range;
# and the removed apology "Not connected to animation. The playhead only moves this view." must not be
# drawn. Headless through drawTimeline(TimelineState&), the content-only entry point drawArea calls; text
# is asserted as drawn ink widths (ImGui stores no text), with a control that the checker finds that
# sentence when it IS drawn.
ember_add_pure_test(test_editor_timeline test_editor_timeline.cpp ember_editor)
# Item 149: PivotMode has an OBSERVABLE effect - resolver, gizmo origin, and modal rotate/scale about the
# 3D cursor / median / bbox / individual origins (one-active-object scope stated in the file).
ember_add_pure_test(test_editor_pivot test_editor_pivot.cpp ember_editor)
# Item 154: the Z shading pie (Blender slots: Left Wireframe, Right Solid, Bottom Material, Top Rendered),
# hover/hold-release/tap/keyboard/cancel and input ownership. CPU-only ImGui events, no GL.
ember_add_pure_test(test_editor_pie test_editor_pie.cpp ember_editor)
# Blender's Ctrl+Tab mode-switch pie. Only the supported Object/Edit modes are
# offered; headless ImGui events exercise hold-release, tap/click, keyboard and cancel.
ember_add_pure_test(test_editor_mode_pie test_editor_mode_pie.cpp ember_editor)
# Item 117's first reachable mesh operations: Edit Mode E/X/M shortcuts share
# the same operation/undo path as the Tool tab, tested with the real ImGui key queue.
ember_add_pure_test(test_editor_edit_tools test_editor_edit_tools.cpp ember_editor)
# Object Mode empty-space drag box selection is exercised through the headless
# editor/ImGui path; it does not require a window or OpenGL context.
ember_add_pure_test(test_editor_box_select test_editor_box_select.cpp ember_editor)
if(TARGET test_editor_edit_tools AND TARGET ember_platform AND TARGET glad)
    # TEST_GPU coverage in the same binary creates a hidden GLFW context only
    # when explicitly run with --gpu; CPU-only tool tests remain headless.
    target_link_libraries(test_editor_edit_tools PRIVATE ember_platform glad)
endif()
# Item 215 (ROADMAP "Next, in order" #1): the four shading entry points — Wireframe, Solid, Material
# Preview, Rendered — are each REACHABLE. Drives the REAL viewport header combo (the real drawArea()
# Editor::drawArea calls) with real ImGui clicks, and the REAL Z pie (handleShadingPie) with real
# events, for all four modes. CPU-only: no window, no GL, so it runs in the default CTest run.
# drawArea lives in Editor.cpp's anonymous namespace, so this TU includes Editor.cpp (as item 175's
# test does) and therefore needs the same extra link set as test_editor_layout.
ember_add_pure_test(test_editor_shading_entry_points test_editor_shading_entry_points.cpp ember_editor)
if(TARGET test_editor_shading_entry_points AND TARGET ember_platform AND TARGET glad)
    target_link_libraries(test_editor_shading_entry_points PRIVATE ember_platform ember_render ember_scene glad)
    target_compile_definitions(test_editor_shading_entry_points PRIVATE EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
# D22 collections (item 60, the one wave-2 area with a declared API today).
# Pure scene-graph grouping: membership, the NEVER-transforms rule, the
# destroy-reparents-up rule, and the rule-3 visibility exclusion.
ember_add_pure_test(test_scene_collections test_scene_collections.cpp ember_scene)
# Item 112: topology tests for the D34 EditableMesh. Adjacency after every
# mutation, manifold invariants, MeshData->EditableMesh->MeshData round-trip
# identity, plus the degenerate cases (empty, single triangle, one vertex
# shared by many faces, n-gon faces). Runs validate() AND an independent
# CSR recomputation, because a bug shared by the impl and its own validator
# is invisible to the validator alone.
ember_add_pure_test(test_scene_topology test_scene_topology.cpp ember_scene)
# Item 123: modifiers are NON-DESTRUCTIVE (D40, items 120/121). Asserts the
# base mesh survives evaluation, that the version-stamped cache is invalidated
# on every mutator (a missed bump serves stale geometry — Scene.h names this as
# THIS item's obligation), that stack order changes the result where the ops do
# not commute, that removing/disabling restores the original EXACTLY, and that
# a modifier under a NON-UNIFORM parent scale still evaluates in local space.
# Reads the evaluated mesh out of the RenderPacket's DrawItem — what the
# renderer actually draws — rather than trusting the implementation's own
# bookkeeping.
ember_add_pure_test(test_scene_modifiers test_scene_modifiers.cpp ember_scene)

# ---- item 156: vocabulary + theme tokens ------------------------------------------------------
# (a) test_editor_vocabulary: UI strings come from the ONE vocabulary source and contain no banned
#     synonyms; theme tokens and metrics equal the frozen D42 table; no hard-coded colour literals.
# (b) the TOKEN SWAP: the real editor sources are compiled TWICE into two probes - one against the
#     real core/Theme.h, one against a GENERATED copy whose token values are different - each draws
#     real widgets and an area header into a headless ImGui, rasterises the actual draw data at
#     chosen pixels, and test_editor_theme_swap compares what was drawn. Proves a token change
#     changes pixels, and that nothing is drawn from a literal that bypasses the tokens.
if(TARGET ember_editor)
    set(EMBER_SOURCE_DIR_FOR_TESTS "${CMAKE_SOURCE_DIR}")
    add_executable(test_editor_vocabulary test_editor_vocabulary.cpp)
    target_include_directories(test_editor_vocabulary PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_editor_vocabulary PRIVATE ember_editor)
    target_compile_features(test_editor_vocabulary PRIVATE cxx_std_20)
    target_compile_definitions(test_editor_vocabulary PRIVATE EMBER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME editor.vocabulary_and_theme_contract COMMAND test_editor_vocabulary)

    include("${CMAKE_CURRENT_SOURCE_DIR}/theme_swap_values.cmake")
    set(_theme_real "${CMAKE_SOURCE_DIR}/src/core/Theme.h")
    set(_swap_dir "${CMAKE_CURRENT_BINARY_DIR}/theme_swap")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_theme_real}" "${CMAKE_CURRENT_SOURCE_DIR}/theme_swap_values.cmake")
    file(READ "${_theme_real}" _theme_text)
    set(_values_h "// GENERATED by tests/CMakeLists.txt from tests/theme_swap_values.cmake - do not edit.\n#pragma once\nnamespace swap {\n")
    foreach(_item IN LISTS EMBER_THEME_SWAP)
        string(REPLACE "|" ";" _parts "${_item}")
        list(GET _parts 0 _kind)
        list(GET _parts 1 _name)
        list(GET _parts 2 _value)
        if(_kind STREQUAL "color")
            set(_re "ThemeColor ${_name}\\{[^}]*\\}")
            set(_new "ThemeColor ${_name}{${_value}}")
        else()
            set(_re "float ${_name} = [-0-9.]+f;")
            set(_new "float ${_name} = ${_value}f;")
        endif()
        string(REGEX MATCH "${_re}" _hit "${_theme_text}")
        if(NOT _hit)
            message(FATAL_ERROR "Verity/QA: theme token '${_name}' (${_kind}) not found in src/core/Theme.h - "
                "it was renamed or removed. Update tests/theme_swap_values.cmake AND the token-swap test.")
        endif()
        string(REGEX REPLACE "${_re}" "${_new}" _theme_text "${_theme_text}")
        if(_kind STREQUAL "color")
            string(APPEND _values_h "inline constexpr int ${_name}[4] = {${_value}};\n")
        else()
            string(APPEND _values_h "inline constexpr float ${_name} = ${_value}f;\n")
        endif()
    endforeach()
    string(APPEND _values_h "} // namespace swap\n")
    file(WRITE "${_swap_dir}/core/Theme.h.tmp" "${_theme_text}")
    file(WRITE "${_swap_dir}/values/swap_values.h.tmp" "${_values_h}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_swap_dir}/core/Theme.h.tmp" "${_swap_dir}/core/Theme.h")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_swap_dir}/values/swap_values.h.tmp" "${_swap_dir}/values/swap_values.h")

    get_target_property(_ed_srcs ember_editor SOURCES)
    get_target_property(_ed_dir ember_editor SOURCE_DIR)
    set(_probe_srcs "")
    foreach(_s IN LISTS _ed_srcs)
        if(NOT _s STREQUAL "Editor.cpp")   # theme_probe.cpp #includes Editor.cpp itself
            list(APPEND _probe_srcs "${_ed_dir}/${_s}")
        endif()
    endforeach()
    foreach(_t theme_probe_default theme_probe_swapped)
        add_executable(${_t} theme_probe.cpp ${_probe_srcs})
        target_include_directories(${_t} PRIVATE "${CMAKE_SOURCE_DIR}/src")
        target_link_libraries(${_t} PRIVATE ember_core ember_render ember_scene glfw glad imgui)
        target_compile_features(${_t} PRIVATE cxx_std_20)
    endforeach()
    # The swapped probe resolves "core/Theme.h" to the GENERATED copy: its dir comes first.
    target_include_directories(theme_probe_swapped BEFORE PRIVATE "${_swap_dir}")

    add_executable(test_editor_theme_swap test_editor_theme_swap.cpp)
    # NOTE: only the VALUES dir is added here, never ${_swap_dir}: this test must see the REAL Theme.h.
    target_include_directories(test_editor_theme_swap PRIVATE
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${_swap_dir}/values")
    target_compile_features(test_editor_theme_swap PRIVATE cxx_std_20)
    add_dependencies(test_editor_theme_swap theme_probe_default theme_probe_swapped)
    add_test(NAME editor.theme_token_swap_changes_pixels COMMAND test_editor_theme_swap)
    set_tests_properties(editor.theme_token_swap_changes_pixels PROPERTIES
        TIMEOUT 120 LABELS "editor;theme;pixels"
        ENVIRONMENT "EMBER_PROBE_DEFAULT=$<TARGET_FILE:theme_probe_default>;EMBER_PROBE_SWAPPED=$<TARGET_FILE:theme_probe_swapped>")
endif()
# ---- runtime smoke + CLI-contract tests (the exe itself) ---------------------
# Ash's headless smoke flag (decision D14, src/core/main.cpp):
#     Ember3D --frames N --exit
# takes the SAME code path as the interactive loop, so a broken GL context,
# shader compile/link, VAO setup, or ImGui init fails the run. Its exit code
# is a documented CONTRACT:
#     0  ran and the last frame drew something (drawCalls > 0)
#     1  startup failed: window or renderer could not initialise
#     2  ran but the frame produced nothing, or the budget was not met
#
# The generator expression $<TARGET_FILE:Ember3D> is required because the root
# CMakeLists defines the Ember3D executable AFTER add_subdirectory(tests); a
# plain `if(TARGET Ember3D)` guard would be false here and silently skip
# registration. $<...> resolves at generate time, when the target exists.
#
# LIMIT OF WHAT THIS PROVES (stated plainly, per Ash's handoff): drawCalls=1
# means the grid+axes overlay drew over an EMPTY scene. It does NOT prove an
# ImGui panel exists — src/editor has no Editor.h/.cpp yet. When Chisel lands
# the host, extend this to assert the panel path too.

# --- negative CLI-contract cases: ALWAYS registered, no GPU needed ---
# Measured 2026-10-16: these exit during argument parsing and never open a
# window (verified: "no-window", 7-10 ms), so they are safe on a context-free
# machine and still exercise the real binary's contract.
#   --frames 0  -> 1   --exit alone -> 1   --frames abc -> 1   --help -> 0
function(ember_add_cli_negative name)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:Ember3D> ${ARGN})
    # The contract REQUIRES a non-zero exit; CTest's default is the opposite.
    set_tests_properties(${name} PROPERTIES
        WILL_FAIL TRUE
        TIMEOUT 30
        LABELS "runtime;cli")
endfunction()

if(TARGET Ember3D OR TRUE)  # target may be defined later; $<TARGET_FILE> resolves at generate time
    add_test(NAME runtime.cli_help COMMAND $<TARGET_FILE:Ember3D> --help)
    set_tests_properties(runtime.cli_help PROPERTIES
        TIMEOUT 30 LABELS "runtime;cli")   # --help must exit 0

    ember_add_cli_negative(runtime.cli_frames_zero_requires_positive --frames 0)
    ember_add_cli_negative(runtime.cli_exit_alone_requires_frames     --exit)
    ember_add_cli_negative(runtime.cli_unknown_option_is_rejected     --bogus)
    ember_add_cli_negative(runtime.cli_frames_nonnumeric_is_rejected  --frames abc)
endif()

# --- the runtime smoke test: DUPLICATE AVOIDED ---
# The canonical smoke test `app.smoke_frames` is registered by the ROOT
# CMakeLists.txt (Kiln's file, line ~86), guarded by EMBER_ENABLE_GPU_TESTS.
# QA deliberately does NOT register a second copy of the same command here —
# two CTest cases firing `Ember3D --frames 60 --exit` would double the window
# opens and muddy ownership of the result. What QA adds uniquely is the
# negative CLI-contract block above, which needs no GL context at all.
if(NOT EMBER_ENABLE_GPU_TESTS)
    message(STATUS
        "Verity/QA: runtime smoke is app.smoke_frames (root CMakeLists, Kiln). "
        "It is absent because EMBER_ENABLE_GPU_TESTS is OFF — enable with "
        "-DEMBER_ENABLE_GPU_TESTS=ON to run 'Ember3D --frames 60 --exit'.")
endif()

# ---- GPU-gated tests (need a live window / GL context) -----------------------
# Registered only under EMBER_ENABLE_GPU_TESTS so the default `ctest` run stays
# context-free (tests/README.md hard rule). Run with:
#   cmake -B build-gpu -DEMBER_ENABLE_GPU_TESTS=ON && ctest --test-dir build-gpu -C Release
if(EMBER_ENABLE_GPU_TESTS)
    if(TARGET test_editor_edit_tools AND TARGET ember_platform AND TARGET glad)
        add_test(NAME editor.edit_mode_mesh_tools_overlay
            COMMAND test_editor_edit_tools --gpu --only edit_mode_render_overlay_changes_viewport_pixels)
        set_tests_properties(editor.edit_mode_mesh_tools_overlay PROPERTIES
            TIMEOUT 180 LABELS "editor;gpu;edit-mode;pixels")
    endif()

    # WHAT-REMAINS defect 1, the half that actually reproduced the bug:
    # Input::setViewportRect was always correct WHEN CALLED — the defect was
    # Window capturing the rect once and never resyncing on resize. Catching
    # that requires a real window through a real resize, hence --gpu.
    if(TARGET ember_platform)
        add_executable(test_platform_window test_platform_window.cpp)
        target_include_directories(test_platform_window PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_platform_window PRIVATE ember_platform)
        target_compile_features(test_platform_window PRIVATE cxx_std_20)
        add_test(NAME platform.window_resize_tracks_rect
                 COMMAND test_platform_window --gpu)
        set_tests_properties(platform.window_resize_tracks_rect PROPERTIES
            TIMEOUT 120 LABELS "platform;gpu")
    endif()

    # ---- shadow map (item 103): PIXEL EVIDENCE of geometric placement --------
    # An earlier audit found the shadow map NOT LANDED: core/Types.h declared
    # ShadowSettings + shadowLightIndex + castShadow while src/render had ZERO
    # references. A shader that compiles is not a shadow. This test therefore
    # asserts the stronger claim: the darkened pixels must land where pure
    # geometry predicts (8 cube corners projected along the light direction onto
    # the floor), must NOT darken outside that footprint, must disappear when
    # the caster is removed, and must MOVE when the caster moves.
    if(TARGET ember_render)
        add_executable(test_render_shadow test_render_shadow.cpp)
        target_include_directories(test_render_shadow PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        # ember_scene too: buildPacket/createPrimitive/setTransform live there,
        # and ember_render does not pull it in transitively. ember_platform for
        # the hidden GL window a renderer needs before init().
        target_link_libraries(test_render_shadow PRIVATE
            ember_render ember_scene ember_platform)
        target_compile_features(test_render_shadow PRIVATE cxx_std_20)
        add_test(NAME render.shadow_geometric_placement
                 COMMAND test_render_shadow --gpu)
        set_tests_properties(render.shadow_geometric_placement PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;shadow")
    endif()

    # GPU pixel proof for presenting a progressive path-trace image in the
    # viewport, including display transform and viewport-only scissoring.
    if(TARGET ember_render AND TARGET ember_scene AND TARGET ember_platform)
        add_executable(test_render_path_trace_viewport test_render_path_trace_viewport.cpp)
        target_include_directories(test_render_path_trace_viewport PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_path_trace_viewport PRIVATE
            ember_render ember_scene ember_platform)
        target_compile_features(test_render_path_trace_viewport PRIVATE cxx_std_20)
        add_test(NAME render.path_trace_viewport_presentation
                 COMMAND test_render_path_trace_viewport --gpu)
        set_tests_properties(render.path_trace_viewport_presentation PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;pixels;cycles")
    endif()

    # Raster/Cycles A/B wipe and raster temporal-AA history both exercise real
    # viewport pixels and GL resource lifetime, so keep them out of the
    # context-free default suite. Their CPU-only supporting logic remains covered
    # by the always-registered render/path tests above.
    if(TARGET ember_render AND TARGET ember_scene AND TARGET ember_platform)
        add_executable(test_render_compare_pixels test_render_compare_pixels.cpp)
        target_include_directories(test_render_compare_pixels PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_compare_pixels PRIVATE
            ember_render ember_scene ember_platform)
        target_compile_features(test_render_compare_pixels PRIVATE cxx_std_20)
        add_test(NAME render.raster_cycles_compare_pixels
            COMMAND test_render_compare_pixels --gpu)
        set_tests_properties(render.raster_cycles_compare_pixels PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;pixels;compare")

        add_executable(test_render_taa test_render_taa.cpp)
        target_include_directories(test_render_taa PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_taa PRIVATE
            ember_render ember_scene ember_platform)
        target_compile_features(test_render_taa PRIVATE cxx_std_20)
        add_test(NAME render.raster_taa_history
            COMMAND test_render_taa --gpu)
        set_tests_properties(render.raster_taa_history PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;pixels;taa")
    endif()

    # ---- item 168: the layout invariants against the REAL Editor, at 1280x720 and 1920x1080 ----------
    if(TARGET test_editor_layout)
        add_test(NAME editor.layout_invariants_real_editor COMMAND test_editor_layout --gpu --only real_editor_)
        add_test(NAME editor.layout_content_fits_real_editor COMMAND test_editor_layout --gpu --only realcontent_)
        add_test(NAME editor.header_controls_fit_their_labels COMMAND test_editor_layout --gpu --only header_controls)
        # Item 195 (UI-07): the rail's six categories must FIT inside the rail at 1280x720 and be
        # clickable there without a wheel. Registered as its own case on purpose: it is RED BY DESIGN
        # until the owner of src/editor/** closes the 212-vs-183 px deficit, and a pinned defect has to
        # be runnable and reproducibly failing, not silently skipped. The default (non-GPU) CPU run of
        # test_editor_layout is unaffected, and the case mirrors editor.header_controls_fit_their_labels.
        add_test(NAME editor.every_properties_category_is_reachable COMMAND test_editor_layout --gpu --only every_properties_category_is_reachable)
        # Item 196 (UI-03): the Toolbar and Sidebar must be REGIONS of the viewport Area - not top-level
        # windows - and must abut the renderer body with no gap (the property that made the Sidebar look
        # like it intersected the Outliner). Same rig and registration shape as the two cases above.
        add_test(NAME editor.toolbar_and_sidebar_are_viewport_regions COMMAND test_editor_layout --gpu --only toolbar_and_sidebar_are_viewport_regions)
        # Item 194 (UI-04/05/08/09): no Sidebar tab and no Properties category may be a PLACEHOLDER. Counts
        # the interactive items each panel draws with ImGui's own hit test (CPU only, no GL). RED BY DESIGN
        # until src/editor/** gives every tab and category a real control; registered as its own case for
        # the same reason as the two above - a pinned defect has to be runnable, not silently skipped.
        add_test(NAME editor.no_tab_or_category_is_a_placeholder COMMAND test_editor_layout --gpu --only no_tab_or_category_is_a_placeholder)
        # Item 190 (UI-13, D41): the STARTUP Timeline is exactly 20% of the SETTLED DockSpace. RED BY DESIGN
        # until Workspace.cpp builds the dock after ImGui has applied the Topbar's work-area offset (the tree
        # is currently built on frame 1 from display.h - statusBarHeight, so the realised share is ~21%).
        # Registered as its own case so the defect is runnable; the default CPU run is unaffected.
        add_test(NAME editor.timeline_startup_split_is_d41s_frozen_proportion COMMAND test_editor_layout --gpu --only timeline_startup_split_is_d41s)
        set_tests_properties(editor.layout_invariants_real_editor editor.layout_content_fits_real_editor
            editor.header_controls_fit_their_labels editor.every_properties_category_is_reachable
            editor.toolbar_and_sidebar_are_viewport_regions editor.no_tab_or_category_is_a_placeholder
            editor.timeline_startup_split_is_d41s_frozen_proportion
            PROPERTIES TIMEOUT 300 LABELS "editor;gpu;layout")
    endif()

    # ---- item 166: the camera gate follows the POINTER (REAL Editor, hidden GLFW window) ----------
    # Pins the wave-long bug where Editor::wantsViewportFocus() was !WantCaptureMouse (always false over
    # the docked viewport) so no camera input ever worked. Also evaluates the contract against the OLD
    # predicate and asserts that it is caught.
    if(TARGET ember_editor AND TARGET ember_render AND TARGET ember_platform AND TARGET glad)
        add_executable(test_editor_camera_gate test_editor_camera_gate.cpp)
        target_include_directories(test_editor_camera_gate PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_editor_camera_gate PRIVATE ember_editor ember_render ember_scene ember_platform glad)
        target_compile_features(test_editor_camera_gate PRIVATE cxx_std_20)
        # One CTest per concern (the harness's --only filters by test-name substring).
        add_test(NAME editor.camera_gate_follows_the_pointer COMMAND test_editor_camera_gate --gpu --only camera_gate_)
        add_test(NAME editor.camera_gate_old_predicate_is_caught COMMAND test_editor_camera_gate --gpu --only the_OLD_predicate)
        add_test(NAME editor.viewport_regions_do_not_pass_the_camera_gate COMMAND test_editor_camera_gate --gpu --only regions_)
        add_test(NAME editor.viewport_click_selects_under_the_pointer COMMAND test_editor_camera_gate --gpu --only pick_)
        set_tests_properties(editor.camera_gate_follows_the_pointer editor.camera_gate_old_predicate_is_caught
            editor.viewport_regions_do_not_pass_the_camera_gate editor.viewport_click_selects_under_the_pointer
            PROPERTIES TIMEOUT 180 LABELS "editor;gpu;camera")
    endif()

    # ---- item 160: the PERMANENT render-claims audit (ported from Slate's temp harness) ----
    # FINDING_1..5 each fail while their claim is not true of the pixels and pass when it is;
    # guard_* tests keep the BACKED claims backed. Public-API only + stderr warnings.
    if(TARGET ember_render AND TARGET glad)
        add_executable(test_render_audit test_render_audit.cpp)
        target_include_directories(test_render_audit PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_audit PRIVATE ember_editor ember_render ember_scene ember_platform glad)
        target_compile_features(test_render_audit PRIVATE cxx_std_20)
        add_test(NAME render.audit_claims COMMAND test_render_audit --gpu)
        # Item 176: only the viewport area may show the 3D render (composited-frame differential, real Editor + Renderer).
        add_test(NAME render.only_viewport_area_shows_the_render COMMAND test_render_audit --gpu --only only_the_viewport_area_shows_the_render)
        add_test(NAME render.viewport_only_render_controls COMMAND test_render_audit --gpu --only guard_viewport_only)
        add_test(NAME render.no_pixel_outside_render_rect_is_unpainted COMMAND test_render_audit --gpu --only no_pixel_outside_the_render_rect)
        # Item 205: only a SELECTED item may be drawn in the selection colour. Composited-frame pixel test,
        # two states (nothing selected / one selected), selection-orange counted per chrome region.
        add_test(NAME render.only_a_selected_item_is_selection_coloured COMMAND test_render_audit --gpu --only only_a_selected_item_is_selection_coloured)
        set_tests_properties(render.only_viewport_area_shows_the_render render.viewport_only_render_controls render.no_pixel_outside_render_rect_is_unpainted PROPERTIES TIMEOUT 300 LABELS "render;editor;gpu")
        set_tests_properties(render.audit_claims PROPERTIES
            TIMEOUT 300 LABELS "render;gpu;pixels;audit")
    endif()

    # ---- item 158: transparency and material slots, measured in pixels ---------
    # Source-over predictions built from other renders of the same scene (no renderer internals),
    # plus slot authority and the end-to-end two-faces/two-slots claim (reports scene/packet/pixel
    # stages separately; blocked on item 144's per-triangle mapping, so it fails today ON PURPOSE).
    if(TARGET ember_render)
        add_executable(test_render_material_pixels test_render_material_pixels.cpp)
        target_include_directories(test_render_material_pixels PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_material_pixels PRIVATE ember_render ember_scene ember_platform)
        target_compile_features(test_render_material_pixels PRIVATE cxx_std_20)
        add_test(NAME render.alpha_and_material_slots COMMAND test_render_material_pixels --gpu)
        set_tests_properties(render.alpha_and_material_slots PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;pixels;material")
    endif()

    # Raster image-texture/environment pixel coverage needs a real GL context;
    # keep it out of the default context-free CTest set.
    if(TARGET ember_render AND TARGET ember_scene AND TARGET ember_platform)
        add_executable(test_render_texture_pixels test_render_texture_pixels.cpp)
        target_include_directories(test_render_texture_pixels PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_texture_pixels PRIVATE
            ember_render ember_scene ember_platform)
        target_compile_features(test_render_texture_pixels PRIVATE cxx_std_20)
        add_test(NAME render.texture_pixels
            COMMAND test_render_texture_pixels --gpu)
        set_tests_properties(render.texture_pixels PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;pixels;texture")
    endif()

    # ---- item 214: the DEDICATED alpha pixel proof (item 158, ROADMAP gap #13) --
    # Item 158 mixed transparency with material slots, so its result cited unrelated
    # evidence and its S2 slot case fails by design. This file is alpha ONLY: one
    # controlled fixture (facing planes, alpha the only variable) and source-over
    # predictions built from OTHER renders of the same scene (the backdrop alone, the
    # layer alone opaque), so lighting and tone cancel. It proves the before/after
    # numbers directly: alpha 1.0 hides the backdrop, 0.5 is exactly 0.5*F+0.5*S, the
    # sweep holds at 0/0.25/0.5/0.75/1, and two transparent layers composite in the
    # far-to-near order the renderer sorts them into. Needs a GL context -> --gpu.
    if(TARGET ember_render)
        add_executable(test_render_alpha test_render_alpha.cpp)
        target_include_directories(test_render_alpha PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_alpha PRIVATE ember_render ember_scene ember_platform)
        target_compile_features(test_render_alpha PRIVATE cxx_std_20)
        add_test(NAME render.alpha_blends_source_over COMMAND test_render_alpha --gpu)
        set_tests_properties(render.alpha_blends_source_over PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;pixels;alpha")
    endif()

    # ---- item 159: gizmo axis colours as they reach the screen -----------------
    # drawGizmo is rendered into a hidden window and read back; handles are found by walking
    # the camera-projected segment, not by trusting a coordinate. Needs GL -> --gpu only.
    if(TARGET ember_render)
        add_executable(test_render_gizmo_colors test_render_gizmo_colors.cpp)
        target_include_directories(test_render_gizmo_colors PRIVATE
            "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(test_render_gizmo_colors PRIVATE ember_render ember_scene ember_platform)
        target_compile_features(test_render_gizmo_colors PRIVATE cxx_std_20)
        add_test(NAME render.gizmo_axis_colours COMMAND test_render_gizmo_colors --gpu)
        set_tests_properties(render.gizmo_axis_colours PROPERTIES
            TIMEOUT 180 LABELS "render;gpu;gizmo")
    endif()

    # ---- item 141: the ACTIVE dock splitter is neutral chrome, not accent ------
    # Drives the real exe with the app's EMBER_SELFTEST_SPLITTER hook and reads the
    # full-frame BMP; finds the splitter by scanning pixels (no trusted coordinate).
    add_executable(test_editor_splitter_color test_editor_splitter_color.cpp)
    target_include_directories(test_editor_splitter_color PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(test_editor_splitter_color PRIVATE cxx_std_20)
    add_test(NAME editor.splitter_active_is_neutral_chrome
             COMMAND test_editor_splitter_color --gpu)
    set_tests_properties(editor.splitter_active_is_neutral_chrome PROPERTIES
        TIMEOUT 120 LABELS "editor;gpu;pixels"
        ENVIRONMENT "EMBER_EXE=$<TARGET_FILE:Ember3D>")

    # ---- geometry smoke: the smoke test must prove an OBJECT renders --------
    # Ash's D18 gate: if >=1 VISIBLE MESHED object exists and the last frame
    # draws 0 triangles, the app exits 2. Before D18 the smoke passed with
    # triangles=0, which is only correct for an EMPTY scene — so the draw path
    # for real geometry was untested. These two cases cover the two distinct
    # paths into that draw code:
    #   --demo-object : World::createPrimitive  (the Add-menu path, M2)
    #   --scene       : scene::load             (File>Open, M6)
    set(EMBER_SMOKE_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/render_cube.ember")
    add_test(NAME app.smoke_draws_added_object
             COMMAND $<TARGET_FILE:Ember3D> --frames 60 --exit --demo-object)
    set_tests_properties(app.smoke_draws_added_object PROPERTIES
        TIMEOUT 120 LABELS "runtime;gpu;smoke")
    add_test(NAME app.smoke_draws_loaded_scene
             COMMAND $<TARGET_FILE:Ember3D> --frames 60 --exit
                     --scene "${EMBER_SMOKE_FIXTURE}")
    set_tests_properties(app.smoke_draws_loaded_scene PROPERTIES
        TIMEOUT 120 LABELS "runtime;gpu;smoke")
    # A loaded scene that FAILS to load must exit non-zero (fails closed), so a
    # broken .ember or a silent empty world cannot masquerade as a pass.
    add_test(NAME app.smoke_missing_scene_fails_closed
             COMMAND $<TARGET_FILE:Ember3D> --frames 60 --exit
                     --scene "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/does_not_exist.ember")
    set_tests_properties(app.smoke_missing_scene_fails_closed PROPERTIES
        WILL_FAIL TRUE TIMEOUT 120 LABELS "runtime;gpu;smoke")
    # A scene with an UNMESHED object (hasMesh=false) legitimately draws 0
    # triangles and must NOT fail — pins the "visible AND meshed" predicate
    # against over-strictness (Ash's own first cut counted all objects).
    add_test(NAME app.smoke_unmeshed_scene_is_not_a_failure
             COMMAND $<TARGET_FILE:Ember3D> --frames 60 --exit
                     --scene "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/unmeshed_only.ember")
    set_tests_properties(app.smoke_unmeshed_scene_is_not_a_failure PROPERTIES
        TIMEOUT 120 LABELS "runtime;gpu;smoke")
    # Item 209: real App frame loop, no stubbed camera maths. The self-test
    # seeds an active scene camera and fails unless its pose/projection become
    # the RenderPacket matrices used by Renderer::renderFrame.
    add_test(NAME app.active_camera_drives_render_packet
             COMMAND $<TARGET_FILE:Ember3D> --frames 3 --exit)
    set_tests_properties(app.active_camera_drives_render_packet PROPERTIES
        TIMEOUT 120 LABELS "runtime;gpu;camera"
        ENVIRONMENT "EMBER_CAMERA_SELFTEST=1")
    # The actual GLFW/App navigation dispatch must use Blender's numpad codes,
    # not the main number row. The App self-test calls the same key dispatcher
    # and asserts the resulting Front/Back/Right/Left/Top/Bottom orientations.
    add_test(NAME app.numpad_navigation_views COMMAND $<TARGET_FILE:Ember3D> --frames 3 --exit)
    set_tests_properties(app.numpad_navigation_views PROPERTIES
        TIMEOUT 120 LABELS "runtime;gpu;navigation"
        ENVIRONMENT "EMBER_NAV_SELFTEST=1")
else()
    message(STATUS
        "Verity/QA: GPU tests OFF — skipping platform.window_resize_tracks_rect "
        "and the geometry smoke cases (app.smoke_draws_added_object / "
        "app.smoke_draws_loaded_scene / app.smoke_missing_scene_fails_closed / "
        "app.smoke_unmeshed_scene_is_not_a_failure). "
        "Enable with -DEMBER_ENABLE_GPU_TESTS=ON.")
endif()
