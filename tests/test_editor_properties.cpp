// tests/test_editor_properties.cpp - live Properties behavior and category/material controls.
//
// CONTRACT UNDER TEST: the LIVE Properties path - drawProperties(world, undo, tab, viewport, file, doc,
// timeline), the same overload Editor::drawArea calls - OFFERS WORKING CONTROLS, not merely visible ones:
//
//   Render: the Raster/Cycles selector and Cycles sample/bounce/seed controls write
//     World::renderSettings(); new Worlds retain Raster defaults; "Enable Shadows" writes
//     ViewportSettings::shadows.enabled; the four shadow sliders write resolution / bias /
//     normalBias / maxDistance, stay in range, and are disabled while shadows are off.
//   Output: "Scene File Path" writes DocumentState::path; "Save As..." opens the real file
//     dialog as a SAVE; "Start Frame" / "End Frame" write TimelineState and the documented clamp holds
//     (end >= start, current inside [start, end]).
//
// HOW: headless ImGui (no GL, so this stays a default-CPU CTest). Item rects are found with ImGui's own
// hit test - the pointer is scanned through the relevant child and the y-range where the HOVERED ID
// equals the item's ID is its rect. Widgets are then driven with real mouse events, so a control that is
// drawn but not interactive fails.
#include "harness.h"

#include "core/Theme.h"
#include "core/Types.h"
#include "editor/Preferences.h"
#include "editor/Panels.h"
#include "editor/ThemeStyle.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using ember::editor::DocumentState;
using ember::editor::FileDialog;
using ember::editor::PropertiesTab;
using ember::editor::TimelineState;
using ember::ObjectId;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

// ---- a headless Properties panel on the live overload ------------------------------------------

struct Rig {
    ImGuiContext* ctx = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    ember::ViewportSettings viewport{};
    FileDialog file;
    DocumentState doc;
    TimelineState timeline;
    PropertiesTab tab = PropertiesTab::Render;
    ember::InteractionMode interaction = ember::InteractionMode::Object;

    Rig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // A typical Properties Area width; tests may shrink DisplaySize to exercise rail scrolling.
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    }
    ~Rig() { ImGui::DestroyContext(ctx); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void frame(float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ember::editor::drawProperties(world, undo, tab, viewport, file, doc, timeline,
                                      interaction);
        ImGui::Render();
    }
    void frames(int n, float mx, float my) { for (int i = 0; i < n; ++i) frame(mx, my); }

    ImGuiWindow* child(const char* fragment) const {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).find(fragment) != std::string::npos) return w;
        return nullptr;
    }
    ImGuiWindow* content() const { return child("##properties-scroll"); }
    ImGuiWindow* rail() const { return child("##properties-icons"); }

    bool pointerAt(float x, float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        frame(x, y); frame(x, y);
        const ImVec2 p = ImGui::GetIO().MousePos;
        return std::fabs(p.x - x) < 1.0f && std::fabs(p.y - y) < 1.0f;
    }
    bool click(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        const bool placed = pointerAt(x, y);
        io.AddMouseButtonEvent(0, true); frame(x, y);
        io.AddMouseButtonEvent(0, false); frame(x, y); frame(x, y);
        return placed;
    }
    // A real drag: press, move in steps, release - what a slider/drag needs to change its value.
    bool drag(float x0, float y0, float x1) {
        ImGuiIO& io = ImGui::GetIO();
        const bool placed = pointerAt(x0, y0);
        io.AddMouseButtonEvent(0, true); frame(x0, y0); frame(x0, y0);
        const int steps = 6;
        for (int i = 1; i <= steps; ++i) {
            const float x = x0 + (x1 - x0) * float(i) / float(steps);
            io.AddMousePosEvent(x, y0); frame(x, y0);
        }
        io.AddMouseButtonEvent(0, false); frame(x1, y0); frame(x1, y0);
        return placed;
    }

    void press(ImGuiKey key) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        frame(io.MousePos.x, io.MousePos.y);
        io.AddKeyEvent(key, false);
        frame(io.MousePos.x, io.MousePos.y);
    }
};

struct Box {
    bool found = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float hx = 0, hy = 0;                 // the point ImGui actually reported as hit - the only safe
                                          // place to click: a checkbox or a button is NOT clickable
                                          // across the whole row, only across its own item rect.
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

// The item's rect, from ImGui's own hit test: scan the pointer down the content column and keep the
// y-run where the hovered ID is the ID ImGui gives that label in that window.
Box itemBoxById(Rig& rig, ImGuiID want) {
    Box b;
    ImGuiWindow* w = rig.content();
    if (!w) return b;
    const float x = w->InnerRect.Min.x + 12.0f;
    for (float y = w->InnerRect.Min.y; y < w->InnerRect.Max.y; y += 1.0f) {
        rig.frame(x, y + 0.5f);
        const bool hit = ImGui::GetCurrentContext()->HoveredId == want;
        if (hit) {
            if (!b.found) { b.found = true; b.y0 = y; }
            b.y1 = y;
            b.hx = x;
            b.hy = y + 0.5f;
        } else if (b.found && y > b.y1 + 1.0f) {
            break;                        // the item's run has ended
        }
    }
    if (b.found) { b.x0 = w->InnerRect.Min.x; b.x1 = w->InnerRect.Max.x; }
    return b;
}

Box itemBox(Rig& rig, const char* label) {
    ImGuiWindow* w = rig.content();
    return w ? itemBoxById(rig, w->GetID(label)) : Box{};
}

Box itemBoxWithIndex(Rig& rig, ImGuiWindow* w, const char* label, int index) {
    Box b;
    if (!w) return b;
    const ImGuiID seed = ImHashData(&index, sizeof(index), w->ID);
    const ImGuiID want = ImHashStr(label, 0, seed);
    const float x = w->InnerRect.Min.x + 12.0f;
    for (float y = w->InnerRect.Min.y; y < w->InnerRect.Max.y; y += 1.0f) {
        rig.frame(x, y + 0.5f);
        if (ImGui::GetCurrentContext()->HoveredId == want) {
            if (!b.found) { b.found = true; b.y0 = y; }
            b.y1 = y;
            b.hx = x;
            b.hy = y + 0.5f;
        } else if (b.found && y > b.y1 + 1.0f) {
            break;
        }
    }
    if (b.found) { b.x0 = w->InnerRect.Min.x; b.x1 = w->InnerRect.Max.x; }
    return b;
}

Box categoryBox(Rig& rig, int index) {
    return itemBoxWithIndex(rig, rig.rail(), "##category", index);
}

} // namespace

// ---- Render: controls write the actual viewport shadow settings --------------------------------
TEST(properties_render_engine_and_cycles_settings_are_world_backed) {
    Rig rig;
    rig.tab = PropertiesTab::Render;
    rig.frames(4, -10.0f, -10.0f);

    const ember::SceneRenderSettings defaults = rig.world.renderSettings();
    CHECK_MSG(defaults.engine == ember::RenderEngine::Raster,
              "a new World must default to the Raster render engine");
    CHECK_MSG(defaults.samplesPerPixel == 64 && defaults.maxBounces == 4 && defaults.seed == 1,
              "Raster defaults should retain 64 samples, 4 bounces, and seed 1");

    Box engine = itemBox(rig, "Render Engine");
    CHECK_MSG(engine.found, "the Render Properties category has no Render Engine selector");
    if (!engine.found) return;
    rig.click(engine.hx, engine.hy);
    rig.press(ImGuiKey_DownArrow); // Raster -> Cycles in the open combo.
    rig.press(ImGuiKey_Enter);

    ember::SceneRenderSettings settings = rig.world.renderSettings();
    CHECK_MSG(settings.engine == ember::RenderEngine::Cycles,
              "choosing Cycles did not update World::renderSettings().engine");
    Box samples = itemBox(rig, "Samples per Pixel");
    Box bounces = itemBox(rig, "Max Bounces");
    Box seed = itemBox(rig, "Seed");
    CHECK_MSG(samples.found && bounces.found && seed.found,
              "Cycles must expose sample, bounce, and seed controls");
    if (!samples.found || !bounces.found || !seed.found) return;

    const ember::u32 originalSamples = settings.samplesPerPixel;
    const ember::u32 originalBounces = settings.maxBounces;
    const ember::u32 originalSeed = settings.seed;
    rig.drag(samples.x1 - 100.0f, samples.cy(), samples.x1 - 6.0f);
    settings = rig.world.renderSettings();
    CHECK_MSG(settings.samplesPerPixel != originalSamples,
              "dragging Samples per Pixel did not edit the world's scene render settings");
    CHECK_MSG(settings.samplesPerPixel >= 1 && settings.samplesPerPixel <= (1u << 24),
              "Samples per Pixel escaped its validated range: %u", settings.samplesPerPixel);

    bounces = itemBox(rig, "Max Bounces");
    rig.drag(bounces.x1 - 100.0f, bounces.cy(), bounces.x1 - 6.0f);
    settings = rig.world.renderSettings();
    CHECK_MSG(settings.maxBounces != originalBounces,
              "dragging Max Bounces did not edit the world's scene render settings");
    CHECK_MSG(settings.maxBounces >= 1 && settings.maxBounces <= 64,
              "Max Bounces escaped its validated range: %u", settings.maxBounces);

    seed = itemBox(rig, "Seed");
    rig.drag(seed.x1 - 100.0f, seed.cy(), seed.x1 - 6.0f);
    settings = rig.world.renderSettings();
    CHECK_MSG(settings.seed != originalSeed,
              "dragging Seed did not edit the world's scene render settings");

    // Switch away and back: the controls must read the world-owned values, not an
    // editor-local copy of the render settings.
    engine = itemBox(rig, "Render Engine");
    rig.click(engine.hx, engine.hy);
    rig.press(ImGuiKey_UpArrow); // Cycles -> Raster.
    rig.press(ImGuiKey_Enter);
    settings = rig.world.renderSettings();
    CHECK_MSG(settings.engine == ember::RenderEngine::Raster,
              "switching back to Raster did not update the world render engine");
    const ember::u32 samplesAfterEdits = settings.samplesPerPixel;
    const ember::u32 bouncesAfterEdits = settings.maxBounces;
    const ember::u32 seedAfterEdits = settings.seed;
    CHECK_MSG(settings.samplesPerPixel != defaults.samplesPerPixel ||
                  settings.maxBounces != defaults.maxBounces || settings.seed != defaults.seed,
              "switching engines discarded the Cycles settings stored by the world");
    CHECK_MSG(!itemBox(rig, "Samples per Pixel").found,
              "Cycles-only settings should be hidden while Raster is selected");

    engine = itemBox(rig, "Render Engine");
    if (!engine.found) {
        CHECK_MSG(false, "Render Engine selector disappeared after switching to Raster");
        return;
    }
    rig.click(engine.hx, engine.hy);
    rig.press(ImGuiKey_DownArrow); // Raster -> Cycles again.
    rig.press(ImGuiKey_Enter);
    settings = rig.world.renderSettings();
    CHECK_MSG(settings.engine == ember::RenderEngine::Cycles &&
                  settings.samplesPerPixel == samplesAfterEdits &&
                  settings.maxBounces == bouncesAfterEdits && settings.seed == seedAfterEdits,
              "reselecting Cycles did not restore the settings held by the World");

    ember::scene::World otherWorld;
    const ember::SceneRenderSettings otherDefaults = otherWorld.renderSettings();
    CHECK_MSG(otherDefaults.engine == ember::RenderEngine::Raster &&
                  otherDefaults.samplesPerPixel == 64 && otherDefaults.maxBounces == 4 &&
                  otherDefaults.seed == 1,
              "a separate World inherited render settings from the editor UI");
}

TEST(properties_raster_cycles_compare_is_viewport_state_and_keeps_engine_selector_independent) {
    Rig rig;
    rig.tab = PropertiesTab::Render;
    rig.frames(4, -10.0f, -10.0f);

    Box compare = itemBox(rig, "Raster / Cycles Compare");
    Box split = itemBox(rig, "Compare Split");
    CHECK_MSG(compare.found && split.found,
              "Render Properties must expose compare enable and split controls with Raster selected");
    if (!compare.found || !split.found) return;

    CHECK_MSG(!rig.viewport.compareRasterCycles,
              "viewport compare must default off without changing the scene engine default");
    CHECK_MSG(rig.world.renderSettings().engine == ember::RenderEngine::Raster,
              "a new scene should remain on the Raster engine");
    rig.click(compare.hx, compare.hy);
    CHECK_MSG(rig.viewport.compareRasterCycles,
              "compare checkbox did not write ViewportSettings::compareRasterCycles");
    CHECK_MSG(rig.world.renderSettings().engine == ember::RenderEngine::Raster,
              "enabling viewport compare must not switch the scene Render Engine selector");

    const float beforeSplit = rig.viewport.compareSplit;
    rig.drag(split.x1 - 100.0f, split.cy(), split.x1 - 6.0f);
    CHECK_MSG(rig.viewport.compareSplit > beforeSplit,
              "dragging Compare Split did not move the viewport divider position");
    CHECK_MSG(rig.viewport.compareSplit >= 0.05f && rig.viewport.compareSplit <= 0.95f,
              "Compare Split escaped its supported [0.05, 0.95] range");

    Box engine = itemBox(rig, "Render Engine");
    CHECK_MSG(engine.found, "compare controls hid the independent Render Engine selector");
    if (!engine.found) return;
    rig.click(engine.hx, engine.hy);
    rig.press(ImGuiKey_DownArrow);
    rig.press(ImGuiKey_Enter);
    CHECK_MSG(rig.world.renderSettings().engine == ember::RenderEngine::Cycles,
              "Render Engine selector no longer changes the scene engine");
    CHECK_MSG(rig.viewport.compareRasterCycles,
              "changing the scene engine unexpectedly disabled the viewport compare toggle");
}

TEST(properties_render_controls_write_viewport_shadow_settings) {
    Rig rig;
    rig.tab = PropertiesTab::Render;
    rig.frames(4, -10.0f, -10.0f);

    const Box enable = itemBox(rig, "Enable Shadows");
    const Box res = itemBox(rig, "Shadow Map Resolution");
    const Box dBias = itemBox(rig, "Depth Bias");
    const Box nBias = itemBox(rig, "Normal Bias");
    const Box maxD = itemBox(rig, "Maximum Distance");
    CHECK_MSG(enable.found, "the Render tab has no 'Enable Shadows' control");
    CHECK_MSG(res.found, "the Render tab has no 'Shadow Map Resolution' control");
    CHECK_MSG(dBias.found, "the Render tab has no 'Depth Bias' control");
    CHECK_MSG(nBias.found, "the Render tab has no 'Normal Bias' control");
    CHECK_MSG(maxD.found, "the Render tab has no 'Maximum Distance' control");
    if (!enable.found || !res.found || !dBias.found || !nBias.found || !maxD.found) return;

    // 1. The checkbox writes ViewportSettings::shadows.enabled, both ways.
    rig.viewport.shadows.enabled = false;
    rig.click(enable.hx, enable.hy);
    CHECK_MSG(rig.viewport.shadows.enabled, "clicking 'Enable Shadows' did not set shadows.enabled");
    rig.click(enable.hx, enable.hy);
    CHECK_MSG(!rig.viewport.shadows.enabled, "clicking 'Enable Shadows' again did not clear shadows.enabled");

    // 2. While shadows are OFF the sliders are disabled: a full drag must not move them.
    const int resBefore = rig.viewport.shadows.resolution;
    const float biasBefore = rig.viewport.shadows.bias;
    rig.drag(res.hx, res.hy, res.x1 - 6.0f);
    rig.drag(dBias.hx, dBias.hy, dBias.x1 - 6.0f);
    CHECK_MSG(rig.viewport.shadows.resolution == resBefore,
              "'Shadow Map Resolution' moved (%d -> %d) while shadows were disabled", resBefore,
              rig.viewport.shadows.resolution);
    CHECK_MSG(rig.viewport.shadows.bias == biasBefore,
              "'Depth Bias' moved while shadows were disabled");
    CHECK_MSG(!rig.viewport.shadows.enabled, "the disabled-state drag turned shadows back on");

    // 3. With shadows ON each control writes its field, inside its declared range.
    rig.click(enable.hx, enable.hy);
    CHECK_MSG(rig.viewport.shadows.enabled, "could not re-enable shadows for the write checks");

    rig.drag(res.hx, res.hy, res.x1 - 6.0f);
    CHECK_MSG(rig.viewport.shadows.resolution != resBefore,
              "dragging 'Shadow Map Resolution' did not write shadows.resolution (still %d)",
              rig.viewport.shadows.resolution);
    CHECK_MSG(rig.viewport.shadows.resolution >= 256 && rig.viewport.shadows.resolution <= 4096,
              "shadows.resolution left its declared range: %d", rig.viewport.shadows.resolution);

    rig.drag(dBias.hx, dBias.hy, dBias.x1 - 6.0f);
    CHECK_MSG(rig.viewport.shadows.bias != biasBefore, "dragging 'Depth Bias' did not write shadows.bias");
    CHECK_MSG(rig.viewport.shadows.bias >= 0.0f && rig.viewport.shadows.bias <= 0.05f,
              "shadows.bias left its declared range: %f", double(rig.viewport.shadows.bias));

    const float nBefore = rig.viewport.shadows.normalBias;
    rig.drag(nBias.hx, nBias.hy, nBias.x1 - 6.0f);
    CHECK_MSG(rig.viewport.shadows.normalBias != nBefore, "dragging 'Normal Bias' did not write shadows.normalBias");
    CHECK_MSG(rig.viewport.shadows.normalBias >= 0.0f && rig.viewport.shadows.normalBias <= 1.0f,
              "shadows.normalBias left its declared range: %f", double(rig.viewport.shadows.normalBias));

    const float maxBefore = rig.viewport.shadows.maxDistance;
    rig.drag(maxD.hx, maxD.hy, maxD.x1 - 6.0f);
    CHECK_MSG(rig.viewport.shadows.maxDistance != maxBefore,
              "dragging 'Maximum Distance' did not write shadows.maxDistance");
    CHECK_MSG(rig.viewport.shadows.maxDistance >= 1.0f && rig.viewport.shadows.maxDistance <= 10000.0f,
              "shadows.maxDistance left its declared range: %f", double(rig.viewport.shadows.maxDistance));

    std::printf("       shadows.enabled toggles; disabled sliders did not move (%d, %.4f); enabled: "
                "resolution %d, depth %.4f, normal %.3f, max %.1f\n", resBefore, double(biasBefore),
                rig.viewport.shadows.resolution, double(rig.viewport.shadows.bias),
                double(rig.viewport.shadows.normalBias), double(rig.viewport.shadows.maxDistance));
}

// ---- Output: working scene-file and timeline-range controls ------------------------------------
TEST(properties_output_controls_write_document_and_timeline) {
    Rig rig;
    rig.tab = PropertiesTab::Output;
    rig.doc.path = "scene.ember";
    rig.frames(4, -10.0f, -10.0f);

    const Box pathField = itemBox(rig, "Scene File Path");
    const Box saveAs = itemBox(rig, "Save As...");
    const Box startFrame = itemBox(rig, "Start Frame");
    const Box endFrame = itemBox(rig, "End Frame");
    CHECK_MSG(pathField.found, "the Output tab has no 'Scene File Path' field");
    CHECK_MSG(saveAs.found, "the Output tab has no 'Save As...' button");
    CHECK_MSG(startFrame.found, "the Output tab has no 'Start Frame' control");
    CHECK_MSG(endFrame.found, "the Output tab has no 'End Frame' control");
    if (!pathField.found || !saveAs.found || !startFrame.found || !endFrame.found) return;

    // 1. Typing into the path field writes DocumentState::path. The click lands near the field's right
    //    edge (the frame is full width) so the caret goes to the end of the text.
    const std::string before = rig.doc.path;
    const float typeX = pathField.x1 - 6.0f;
    CHECK_MSG(rig.pointerAt(typeX, pathField.cy()), "could not place the pointer on the 'Scene File Path' field");
    {
        ImGuiWindow* w = rig.content();
        CHECK_MSG(w && ImGui::GetCurrentContext()->HoveredId == w->GetID("Scene File Path"),
                  "setup: the typing point is not on the 'Scene File Path' field, so the write check "
                  "below could not tell a keystroke from a miss");
    }
    rig.click(typeX, pathField.cy());
    ImGui::GetIO().AddInputCharactersUTF8("X");
    rig.frames(2, typeX, pathField.cy());
    CHECK_MSG(rig.doc.path.size() == before.size() + 1 && rig.doc.path.find('X') != std::string::npos,
              "typing into 'Scene File Path' did not write DocumentState::path (\"%s\" -> \"%s\")",
              before.c_str(), rig.doc.path.c_str());
    std::printf("       scene file path \"%s\" -> \"%s\" after one keystroke\n", before.c_str(),
                rig.doc.path.c_str());

    // 2. "Save As..." opens the real dialog as a SAVE.
    CHECK_MSG(!rig.file.open, "setup: the file dialog was already open");
    rig.click(saveAs.hx, saveAs.hy);
    CHECK_MSG(rig.file.open, "clicking 'Save As...' did not open the file dialog");
    CHECK_MSG(!rig.file.wantLoad, "'Save As...' opened the dialog in LOAD mode");
    rig.file.open = false;      // leave the dialog closed: a modal popup would cover the controls below

    // 3. A click with no movement must NOT change a drag control (so the write below is the drag).
    const int endBefore = rig.timeline.end;
    rig.click(endFrame.hx, endFrame.hy);
    CHECK_MSG(rig.timeline.end == endBefore,
              "'End Frame' changed (%d -> %d) on a click with no drag, so the write check below could "
              "not tell a drag from a click", endBefore, rig.timeline.end);

    // 4. A real drag writes TimelineState::start.
    const int startBefore = rig.timeline.start;
    rig.drag(startFrame.hx, startFrame.hy, startFrame.x1 - 6.0f);
    CHECK_MSG(rig.timeline.start != startBefore,
              "dragging 'Start Frame' did not write TimelineState::start (still %d)", rig.timeline.start);
    CHECK_MSG(rig.timeline.end >= rig.timeline.start,
              "after the Start Frame drag the range is inverted: start %d > end %d", rig.timeline.start,
              rig.timeline.end);

    // 5. The documented clamp, stated as the tab's own behaviour: end < start is corrected, and the
    //    playhead is pulled back inside the range - the values are read back AFTER a frame.
    rig.timeline.start = 100;
    rig.timeline.end = 50;
    rig.timeline.current = 5;
    rig.frame(-10.0f, -10.0f);
    CHECK_MSG(rig.timeline.end == rig.timeline.start,
              "the Output tab did not enforce end >= start: start %d, end %d", rig.timeline.start,
              rig.timeline.end);
    CHECK_MSG(rig.timeline.current >= rig.timeline.start && rig.timeline.current <= rig.timeline.end,
              "the Output tab did not keep the playhead inside the range: current %d, range %d..%d",
              rig.timeline.current, rig.timeline.start, rig.timeline.end);
}

// Item 209: exercise the live Properties draw path for the Camera category.
// Each control is found through ImGui's hit-test and then driven by mouse input.
TEST(properties_camera_controls_edit_selected_camera_and_set_active) {
    Rig rig;
    const ObjectId cameraId = rig.world.createCamera("Camera");
    rig.world.select(cameraId);
    rig.tab = PropertiesTab::Camera;
    rig.frames(4, -10.0f, -10.0f);

    const Box fov = itemBox(rig, "Field of View");
    const Box clipStart = itemBox(rig, "Clip Start");
    const Box clipEnd = itemBox(rig, "Clip End");
    const Box orthographic = itemBox(rig, "Orthographic");
    const Box orthoScale = itemBox(rig, "Orthographic Scale");
    const Box setActive = itemBox(rig, "Set Active Camera");
    CHECK_MSG(fov.found, "Camera Properties has no 'Field of View' control");
    CHECK_MSG(clipStart.found, "Camera Properties has no 'Clip Start' control");
    CHECK_MSG(clipEnd.found, "Camera Properties has no 'Clip End' control");
    CHECK_MSG(orthographic.found, "Camera Properties has no 'Orthographic' control");
    CHECK_MSG(orthoScale.found, "Camera Properties has no 'Orthographic Scale' control");
    CHECK_MSG(setActive.found, "Camera Properties has no 'Set Active Camera' control");
    if (!fov.found || !clipStart.found || !clipEnd.found || !orthographic.found ||
        !orthoScale.found || !setActive.found) return;

    const float beforeFov = rig.world.get(cameraId)->camera.fovY;
    rig.drag(fov.hx, fov.hy, fov.hx + 80.0f);
    CHECK_MSG(rig.world.get(cameraId)->camera.fovY != beforeFov,
              "dragging 'Field of View' did not update CameraSettings::fovY");
    CHECK_MSG(rig.world.get(cameraId)->camera.fovY > 0.0f &&
                  rig.world.get(cameraId)->camera.fovY < ember::kPi,
              "Field of View escaped its valid projection range");

    const float beforeNear = rig.world.get(cameraId)->camera.clipNear;
    rig.drag(clipStart.hx, clipStart.hy, clipStart.hx + 60.0f);
    CHECK_MSG(rig.world.get(cameraId)->camera.clipNear != beforeNear,
              "dragging 'Clip Start' did not update CameraSettings::clipNear");
    CHECK_MSG(rig.world.get(cameraId)->camera.clipNear < rig.world.get(cameraId)->camera.clipFar,
              "Clip Start moved to or beyond Clip End");

    const float beforeFar = rig.world.get(cameraId)->camera.clipFar;
    rig.drag(clipEnd.hx, clipEnd.hy, clipEnd.hx + 60.0f);
    CHECK_MSG(rig.world.get(cameraId)->camera.clipFar != beforeFar,
              "dragging 'Clip End' did not update CameraSettings::clipFar");
    CHECK_MSG(rig.world.get(cameraId)->camera.clipFar > rig.world.get(cameraId)->camera.clipNear,
              "Clip End moved to or below Clip Start");

    rig.click(orthographic.hx, orthographic.hy);
    CHECK_MSG(rig.world.get(cameraId)->camera.orthographic,
              "clicking 'Orthographic' did not update CameraSettings::orthographic");

    const float beforeScale = rig.world.get(cameraId)->camera.orthoScale;
    rig.drag(orthoScale.hx, orthoScale.hy, orthoScale.hx + 60.0f);
    CHECK_MSG(rig.world.get(cameraId)->camera.orthoScale != beforeScale,
              "dragging 'Orthographic Scale' did not update CameraSettings::orthoScale");

    rig.click(setActive.hx, setActive.hy);
    CHECK_MSG(rig.world.activeCamera() == cameraId,
              "clicking 'Set Active Camera' did not select the scene camera");
}

TEST(properties_category_rail_scrolls_and_every_category_is_reachable) {
    Rig rig;
    ImGui::GetIO().DisplaySize = ImVec2(600.0f, 170.0f);
    rig.tab = PropertiesTab::Render;
    rig.frames(4, -10.0f, -10.0f);

    ImGuiWindow* rail = rig.rail();
    CHECK_MSG(rail != nullptr, "Properties has no category rail child window");
    if (!rail) return;
    CHECK_MSG(rail->ScrollMax.y > 0.0f,
              "a short Properties Area did not make the category rail scrollable");

    // Click every category visible at the top of the rail, including the middle entries.
    for (int index : {0, 1, 2, 3}) {
        const Box category = categoryBox(rig, index);
        CHECK_MSG(category.found, "Properties category %d is not reachable at the top of the rail", index);
        if (category.found) rig.click(category.hx, category.hy);
        CHECK_MSG(static_cast<int>(rig.tab) == index,
                  "clicking Properties category %d did not activate it", index);
    }

    const float railX = (rail->InnerRect.Min.x + rail->InnerRect.Max.x) * 0.5f;
    const float railY = (rail->InnerRect.Min.y + rail->InnerRect.Max.y) * 0.5f;
    ImGui::GetIO().AddMousePosEvent(railX, railY);
    rig.frame(railX, railY);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, -12.0f);
    rig.frames(3, railX, railY);
    rail = rig.rail();
    CHECK_MSG(rail && rail->Scroll.y > 0.0f,
              "scrolling over the category rail did not advance its scroll position");

    for (int index : {4, 5, 6}) {
        const Box category = categoryBox(rig, index);
        CHECK_MSG(category.found, "scrolled Properties category %d is not reachable", index);
        if (category.found) rig.click(category.hx, category.hy);
        CHECK_MSG(static_cast<int>(rig.tab) == index,
                  "clicking scrolled Properties category %d did not activate it", index);
    }
}

TEST(properties_material_slots_edit_active_material_and_remove_with_undo) {
    Rig rig;
    const ObjectId object = rig.world.createPrimitive(ember::scene::Primitive::Cube);
    rig.world.select(object);
    rig.tab = PropertiesTab::Material;
    rig.frames(4, -10.0f, -10.0f);

    ImGuiWindow* slotList = rig.child("##material-slot-list");
    const Box firstRow = itemBoxWithIndex(rig, slotList, "Material", 0);
    const Box add = itemBox(rig, "+");
    ImGuiWindow* content = rig.content();
    const float removeX = content ? content->InnerRect.Max.x - 12.0f : 0.0f;
    CHECK_MSG(firstRow.found, "Material Properties has no selectable base material slot");
    CHECK_MSG(add.found && content, "Material Properties is missing its slot controls or content region");
    if (!firstRow.found || !add.found || !content) return;

    const std::size_t initialHistory = rig.undo.doneCount();
    rig.click(removeX, add.hy); // right-hand half of the add/remove button pair
    CHECK_MSG(rig.world.materialSlotCount(object) == 1,
              "the base material slot was removed despite the slot-0 disabled gate");
    CHECK_MSG(rig.undo.doneCount() == initialHistory,
              "clicking disabled slot removal added an undo entry");

    rig.click(add.hx, add.hy);
    CHECK_MSG(rig.world.materialSlotCount(object) == 2, "the + control did not append a material slot");
    ember::u32 count = 0;
    const ember::MaterialSlot* slots = rig.world.materialSlots(object, count);
    CHECK_MSG(count == 2 && slots && slots[1].name == "Material 2",
              "the added slot does not follow the Material N naming contract");

    slotList = rig.child("##material-slot-list");
    const Box secondRow = itemBoxWithIndex(rig, slotList, "Material 2", 1);
    CHECK_MSG(secondRow.found, "the newly-added material slot is not selectable in the slot list");
    if (!secondRow.found) return;
    rig.click(secondRow.hx, secondRow.hy);

    ImGuiWindow* materialContent = rig.content();
    const ImGuiID colorEditId = materialContent ? materialContent->GetID("Base color") : 0;
    // ColorEdit4 scopes its RGB/alpha drag widgets below PushID(label); probe
    // the live red component instead of mistaking the label text for a slider.
    const ImGuiID redComponentId = ImHashStr("##X", 0, colorEditId);
    const Box baseColor = itemBoxById(rig, redComponentId);
    const Box metallic = itemBox(rig, "Metallic");
    const Box roughness = itemBox(rig, "Roughness");
    CHECK_MSG(baseColor.found && metallic.found && roughness.found,
              "the active material does not expose Base color, Metallic, and Roughness controls");
    if (!baseColor.found || !metallic.found || !roughness.found) return;
    const float baseRedBefore = slots[1].material.baseColor.r;
    rig.drag(baseColor.hx, baseColor.hy, baseColor.hx + 48.0f);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.baseColor.r != baseRedBefore,
              "dragging the active Base Color red component did not edit the material slot");
    const float metallicBefore = slots[1].material.metallic;
    rig.drag(metallic.cx(), metallic.cy(), metallic.x1 - 6.0f);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.metallic != metallicBefore,
              "dragging Metallic did not edit the selected material slot");
    const float roughnessBefore = slots ? slots[1].material.roughness : 0.0f;
    rig.drag(roughness.cx(), roughness.cy(), roughness.x1 - 6.0f);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.roughness != roughnessBefore,
              "dragging Roughness did not edit the selected material slot");

    // The row selection is independent of scene selection; removing/undoing the slot must
    // preserve which object is selected and restore the slot value.
    rig.click(removeX, add.hy);
    CHECK_MSG(rig.world.materialSlotCount(object) == 1,
              "removing the selected non-base slot did not update the slot list");
    CHECK_MSG(rig.world.selection().size() == 1 && rig.world.selection().back() == object,
              "material slot removal changed the scene object selection");
    CHECK_MSG(rig.undo.undo(), "material slot removal did not create an undoable command");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(count == 2 && slots && slots[1].material.metallic != metallicBefore,
              "undoing slot removal did not restore the slot and its edited material");
}

TEST(properties_emission_edits_are_per_slot_undoable_and_redoable) {
    Rig rig;
    const ObjectId object = rig.world.createPrimitive(ember::scene::Primitive::Cube);
    rig.world.select(object);
    rig.tab = PropertiesTab::Material;
    rig.frames(4, -10.0f, -10.0f);

    const Box add = itemBox(rig, "+");
    CHECK_MSG(add.found, "Material Properties has no slot add control for emission editing");
    if (!add.found) return;
    rig.click(add.hx, add.hy);
    CHECK_MSG(rig.world.materialSlotCount(object) == 2,
              "could not create a second slot for the emission test");

    ImGuiWindow* slotList = rig.child("##material-slot-list");
    const Box firstRow = itemBoxWithIndex(rig, slotList, "Material", 0);
    const Box secondRow = itemBoxWithIndex(rig, slotList, "Material 2", 1);
    CHECK_MSG(firstRow.found && secondRow.found,
              "emission test could not find both material slot rows");
    if (!firstRow.found || !secondRow.found) return;
    rig.click(secondRow.hx, secondRow.hy);

    ImGuiWindow* content = rig.content();
    const ImGuiID emissionColorId = content ? content->GetID("Emission color") : 0;
    const ImGuiID redComponentId = ImHashStr("##X", 0, emissionColorId);
    const Box emissionRed = itemBoxById(rig, redComponentId);
    const Box emissionStrength = itemBox(rig, "Emission strength");
    CHECK_MSG(emissionRed.found, "active material has no editable Emission color widget");
    CHECK_MSG(emissionStrength.found, "active material has no editable Emission strength widget");
    if (!emissionRed.found || !emissionStrength.found) return;

    ember::u32 count = 0;
    const ember::MaterialSlot* slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && count == 2 && slots[1].material.emissionColor.r == 0.0f &&
                  slots[1].material.emissionStrength == 0.0f,
              "new material slots must start with zero emission");
    if (!slots || count != 2) return;

    const std::size_t beforeColor = rig.undo.doneCount();
    rig.drag(emissionRed.hx, emissionRed.hy, emissionRed.hx + 48.0f);
    slots = rig.world.materialSlots(object, count);
    const float editedRed = slots ? slots[1].material.emissionColor.r : 0.0f;
    CHECK_MSG(slots && editedRed > 0.0f,
              "dragging Emission color did not write MaterialDesc::emissionColor");
    CHECK_MSG(rig.undo.doneCount() == beforeColor + 1,
              "one Emission color drag should coalesce to one undo entry");
    CHECK_MSG(rig.undo.undo(), "Emission color edit was not undoable");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.emissionColor.r == 0.0f,
              "undo did not restore the prior emission color");
    CHECK_MSG(rig.undo.redo(), "Emission color edit could not be redone");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.emissionColor.r == editedRed,
              "redo did not restore the edited emission color");

    const std::size_t beforeStrength = rig.undo.doneCount();
    rig.drag(emissionStrength.cx(), emissionStrength.cy(), emissionStrength.x1 - 6.0f);
    slots = rig.world.materialSlots(object, count);
    const float editedStrength = slots ? slots[1].material.emissionStrength : 0.0f;
    CHECK_MSG(slots && editedStrength > 0.0f,
              "dragging Emission strength did not write MaterialDesc::emissionStrength");
    CHECK_MSG(rig.undo.doneCount() == beforeStrength + 1,
              "one Emission strength drag should coalesce to one undo entry");
    CHECK_MSG(rig.undo.undo(), "Emission strength edit was not undoable");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.emissionStrength == 0.0f,
              "undo did not restore the prior emission strength");
    CHECK_MSG(rig.undo.redo(), "Emission strength edit could not be redone");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.emissionStrength == editedStrength,
              "redo did not restore the edited emission strength");

    // Each slot owns its authored emission; changing the active row must only
    // change which MaterialDesc the widgets display/edit.
    rig.click(firstRow.hx, firstRow.hy);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[0].material.emissionColor.r == 0.0f &&
                  slots[0].material.emissionStrength == 0.0f,
              "switching to slot 0 leaked slot 1 emission values");
    const Box firstStrength = itemBox(rig, "Emission strength");
    CHECK_MSG(firstStrength.found, "slot 0 lost its Emission strength widget");
    if (firstStrength.found)
        rig.drag(firstStrength.cx(), firstStrength.cy(), firstStrength.x1 - 6.0f);
    slots = rig.world.materialSlots(object, count);
    const float firstSlotStrength = slots ? slots[0].material.emissionStrength : 0.0f;
    CHECK_MSG(slots && firstSlotStrength > 0.0f,
              "Emission strength did not edit the newly selected slot 0");

    // The item rect is re-found after the slot click because the active material
    // row changes, and the second slot must still retain its independent values.
    slotList = rig.child("##material-slot-list");
    const Box activeSecondRow = itemBoxWithIndex(rig, slotList, "Material 2", 1);
    CHECK_MSG(activeSecondRow.found, "slot 1 row disappeared after switching active materials");
    if (activeSecondRow.found) rig.click(activeSecondRow.hx, activeSecondRow.hy);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.emissionColor.r == editedRed &&
                  slots[1].material.emissionStrength == editedStrength,
              "switching slots did not preserve slot 1's emission values");
    CHECK_MSG(slots && slots[0].material.emissionStrength == firstSlotStrength,
              "switching back to slot 1 changed slot 0's emission strength");
}

TEST(properties_cycles_transmission_and_ior_are_per_slot_undoable) {
    Rig rig;
    const ObjectId object = rig.world.createPrimitive(ember::scene::Primitive::Cube);
    rig.world.select(object);
    rig.tab = PropertiesTab::Material;
    rig.frames(4, -10.0f, -10.0f);

    const Box add = itemBox(rig, "+");
    CHECK_MSG(add.found, "Material Properties has no slot add control for dielectric editing");
    if (!add.found) return;
    rig.click(add.hx, add.hy);
    CHECK_MSG(rig.world.materialSlotCount(object) == 2,
              "could not create a second slot for dielectric editing");

    ImGuiWindow* slotList = rig.child("##material-slot-list");
    const Box secondRow = itemBoxWithIndex(rig, slotList, "Material 2", 1);
    CHECK_MSG(secondRow.found, "dielectric test could not find the second material slot");
    if (!secondRow.found) return;
    rig.click(secondRow.hx, secondRow.hy);

    const Box transmission = itemBox(rig, "Transmission");
    const Box ior = itemBox(rig, "IOR");
    CHECK_MSG(transmission.found && ior.found,
              "Material Properties must expose Cycles Transmission and IOR controls");
    if (!transmission.found || !ior.found) return;

    ember::u32 count = 0;
    const ember::MaterialSlot* slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && count == 2 && slots[1].material.transmission == 0.0f &&
                  std::fabs(slots[1].material.ior - ember::kDefaultMaterialIor) < 1.0e-6f,
              "new slots should start with dielectric defaults");
    if (!slots || count != 2) return;

    const std::size_t beforeTransmission = rig.undo.doneCount();
    rig.drag(transmission.x1 - 100.0f, transmission.cy(), transmission.x1 - 6.0f);
    slots = rig.world.materialSlots(object, count);
    const float editedTransmission = slots ? slots[1].material.transmission : 0.0f;
    CHECK_MSG(slots && editedTransmission > 0.0f && editedTransmission <= 1.0f,
              "Transmission widget did not write a bounded MaterialDesc value (got %.4f; drag %.1f..%.1f at y %.1f)",
              editedTransmission, transmission.x1 - 100.0f, transmission.x1 - 6.0f,
              transmission.cy());
    CHECK_MSG(rig.undo.doneCount() == beforeTransmission + 1,
              "one Transmission drag should coalesce to one undo entry");
    CHECK_MSG(rig.undo.undo(), "Transmission edit was not undoable");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.transmission == 0.0f,
              "undo did not restore the prior Transmission value");
    CHECK_MSG(rig.undo.redo(), "Transmission edit could not be redone");

    const std::size_t beforeIor = rig.undo.doneCount();
    const Box refreshedIor = itemBox(rig, "IOR");
    CHECK_MSG(refreshedIor.found, "IOR widget disappeared after the Transmission edit");
    if (!refreshedIor.found) return;
    rig.drag(refreshedIor.x1 - 100.0f, refreshedIor.cy(), refreshedIor.x1 - 6.0f);
    slots = rig.world.materialSlots(object, count);
    const float editedIor = slots ? slots[1].material.ior : 0.0f;
    CHECK_MSG(slots && editedIor >= 1.0f && editedIor <= 3.0f &&
                  std::fabs(editedIor - ember::kDefaultMaterialIor) > 1.0e-4f,
              "IOR widget did not write a bounded MaterialDesc value (got %.4f; drag %.1f..%.1f at y %.1f)",
              editedIor, refreshedIor.x1 - 100.0f, refreshedIor.x1 - 6.0f,
              refreshedIor.cy());
    CHECK_MSG(rig.undo.doneCount() == beforeIor + 1,
              "one IOR drag should coalesce to one undo entry");
    CHECK_MSG(rig.undo.undo(), "IOR edit was not undoable");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && std::fabs(slots[1].material.ior - ember::kDefaultMaterialIor) < 1.0e-6f,
              "undo did not restore the default IOR");
    CHECK_MSG(rig.undo.redo(), "IOR edit could not be redone");

    const Box firstRow = itemBoxWithIndex(rig, rig.child("##material-slot-list"), "Material", 0);
    CHECK_MSG(firstRow.found, "first material slot disappeared");
    if (firstRow.found) rig.click(firstRow.hx, firstRow.hy);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[0].material.transmission == 0.0f &&
                  std::fabs(slots[0].material.ior - ember::kDefaultMaterialIor) < 1.0e-6f,
              "dielectric edits leaked from slot 1 into slot 0");
}

TEST(properties_material_image_path_and_color_space_are_slot_local_undoable_and_relative) {
    Rig rig;
    const ObjectId object = rig.world.createPrimitive(ember::scene::Primitive::Cube);
    rig.world.select(object);
    rig.tab = PropertiesTab::Material;
    rig.frames(4, -10.0f, -10.0f);

    Box add = itemBox(rig, "+");
    CHECK_MSG(add.found, "Material Properties has no slot add control for image authoring");
    if (!add.found) return;
    rig.click(add.hx, add.hy);
    CHECK_MSG(rig.world.materialSlotCount(object) == 2,
              "could not create a second material slot for image authoring");

    ImGuiWindow* slotList = rig.child("##material-slot-list");
    const Box secondRow = itemBoxWithIndex(rig, slotList, "Material 2", 1);
    CHECK_MSG(secondRow.found, "image authoring test could not find the second material slot");
    if (!secondRow.found) return;
    rig.click(secondRow.hx, secondRow.hy);

    Box path = itemBox(rig, "Base Color Image");
    Box colorSpace = itemBox(rig, "Image Color Space");
    CHECK_MSG(path.found && colorSpace.found,
              "active material slot must expose image path and color-space controls");
    if (!path.found || !colorSpace.found) return;

    // Replace the field through ImGui input and commit by leaving the widget.
    const std::string authoredRelativePath = "textures/albedo.ppm";
    rig.click(path.x1 - 10.0f, path.cy());
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    rig.frame(path.x1 - 10.0f, path.cy());
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    rig.frame(path.x1 - 10.0f, path.cy());
    io.AddInputCharactersUTF8(authoredRelativePath.c_str());
    rig.frame(path.x1 - 10.0f, path.cy());
    const std::size_t historyBeforePath = rig.undo.doneCount();
    rig.click(-10.0f, -10.0f);

    ember::u32 count = 0;
    const ember::MaterialSlot* slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && count == 2 &&
                  slots[1].material.baseColorTexture.path == authoredRelativePath,
              "material image path must preserve the authored relative path exactly");
    CHECK_MSG(rig.undo.doneCount() == historyBeforePath + 1,
              "committing one image path edit should create one undo entry");
    CHECK_MSG(rig.undo.undo(), "material image path edit was not undoable");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.baseColorTexture.path.empty(),
              "undo did not clear the second slot's image reference");
    CHECK_MSG(rig.undo.redo(), "material image path edit could not be redone");
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[1].material.baseColorTexture.path == authoredRelativePath,
              "redo did not restore the relative image path byte-for-byte");

    colorSpace = itemBox(rig, "Image Color Space");
    CHECK_MSG(colorSpace.found, "image color-space selector disappeared after path undo/redo");
    if (colorSpace.found) {
        const std::size_t historyBeforeColorSpace = rig.undo.doneCount();
        rig.click(colorSpace.hx, colorSpace.hy);
        rig.press(ImGuiKey_DownArrow); // sRGB -> Linear
        rig.press(ImGuiKey_Enter);
        slots = rig.world.materialSlots(object, count);
        CHECK_MSG(slots && slots[1].material.baseColorTexture.colorSpace ==
                              ember::ImageColorSpace::Linear,
                  "color-space selector did not store Linear on the active material slot");
        CHECK_MSG(rig.undo.doneCount() == historyBeforeColorSpace + 1,
                  "one color-space selection should create one undo entry");
        CHECK_MSG(rig.undo.undo(), "color-space edit was not undoable");
        slots = rig.world.materialSlots(object, count);
        CHECK_MSG(slots && slots[1].material.baseColorTexture.colorSpace ==
                              ember::ImageColorSpace::SRgb,
                  "undo did not restore the default sRGB color-space tag");
        CHECK_MSG(rig.undo.redo(), "color-space edit could not be redone");
    }

    const Box firstRow = itemBoxWithIndex(rig, rig.child("##material-slot-list"), "Material", 0);
    CHECK_MSG(firstRow.found, "first material slot disappeared during image isolation test");
    if (firstRow.found) rig.click(firstRow.hx, firstRow.hy);
    slots = rig.world.materialSlots(object, count);
    CHECK_MSG(slots && slots[0].material.baseColorTexture.path.empty() &&
                  slots[0].material.baseColorTexture.colorSpace == ember::ImageColorSpace::SRgb,
              "editing slot 1's path/color space leaked into slot 0");
}

TEST(properties_face_assign_is_disabled_without_faces_and_undo_preserves_selection) {
    Rig rig;
    const ObjectId object = rig.world.createPrimitive(ember::scene::Primitive::Cube);
    rig.world.select(object);
    ember::scene::EditableMesh* editable = rig.world.editable(object);
    CHECK_MSG(editable != nullptr && !editable->faces.empty(), "test setup could not create an editable mesh");
    if (!editable || editable->faces.empty()) return;
    rig.tab = PropertiesTab::Material;
    rig.interaction = ember::InteractionMode::Edit;
    rig.world.editSetSelection(object, ember::SelectMode::Face, {0});
    rig.frames(4, -10.0f, -10.0f);

    Box assign = itemBox(rig, "Assign");
    CHECK_MSG(assign.found, "Material Properties has no Face Assign control for a selected face");
    if (!assign.found) return;
    rig.world.editSetSelection(object, ember::SelectMode::Face, {});
    rig.frame(-10.0f, -10.0f);
    const std::size_t beforeDisabledClick = rig.undo.doneCount();
    rig.click(assign.hx, assign.hy);
    CHECK_MSG(rig.undo.doneCount() == beforeDisabledClick && editable->faces[0].materialIndex == 0,
              "Face Assign was not disabled when there was no selected face");

    // A stale sub-element selection must not make Assign active after leaving Edit Mode.
    rig.world.editSetSelection(object, ember::SelectMode::Face, {0});
    rig.interaction = ember::InteractionMode::Object;
    rig.frame(-10.0f, -10.0f);
    const std::size_t objectModeHistory = rig.undo.doneCount();
    rig.click(assign.hx, assign.hy);
    CHECK_MSG(rig.undo.doneCount() == objectModeHistory && editable->faces[0].materialIndex == 0,
              "Face Assign was enabled outside Edit Mode by a stale face selection");

    rig.interaction = ember::InteractionMode::Edit;
    rig.world.editSetSelection(object, ember::SelectMode::Face, {});
    rig.frame(-10.0f, -10.0f);
    assign = itemBox(rig, "Assign");
    CHECK_MSG(assign.found, "Face Assign disappeared in Edit Mode without a face selection");
    const std::size_t editModeNoFaceHistory = rig.undo.doneCount();
    if (assign.found) rig.click(assign.hx, assign.hy);
    CHECK_MSG(rig.undo.doneCount() == editModeNoFaceHistory,
              "Face Assign was enabled in Edit Mode with no selected faces");

    const Box add = itemBox(rig, "+");
    CHECK_MSG(add.found, "Material Properties has no slot add control");
    if (!add.found) return;
    rig.click(add.hx, add.hy);
    rig.world.editSetSelection(object, ember::SelectMode::Face, {0});
    rig.frame(-10.0f, -10.0f);
    assign = itemBox(rig, "Assign");
    CHECK_MSG(assign.found, "Face Assign disappeared for a selected Edit Mode face");
    if (!assign.found) return;

    rig.click(assign.hx, assign.hy);
    CHECK_MSG(editable->faces[0].materialIndex == 1,
              "Face Assign did not write the active slot to the selected face");
    CHECK_MSG(rig.world.selection().size() == 1 && rig.world.selection().back() == object,
              "Face Assign changed the scene object selection");
    const ember::scene::MeshSelection* selection =
        rig.world.editReadSelection(object, ember::SelectMode::Face);
    CHECK_MSG(selection && selection->indices.size() == 1 && selection->indices[0] == 0,
              "Face Assign changed the selected face set");
    CHECK_MSG(rig.undo.undo(), "Face Assign did not create an undoable command");
    CHECK_MSG(editable->faces[0].materialIndex == 0,
              "undoing Face Assign did not restore the prior face material index");
    CHECK_MSG(rig.undo.redo(), "Face Assign could not be redone");
    CHECK_MSG(editable->faces[0].materialIndex == 1,
              "redoing Face Assign did not reapply the material index");

    // Removing a material slot remaps its face indices to the default slot; undo restores both.
    const ImGuiWindow* content = rig.content();
    CHECK_MSG(content != nullptr, "Material Properties content region disappeared before slot removal");
    if (content) rig.click(content->InnerRect.Max.x - 12.0f, add.hy);
    CHECK_MSG(editable->faces[0].materialIndex == 0,
              "removing an assigned slot did not remap that face to slot 0");
    CHECK_MSG(rig.undo.undo(), "removing an assigned slot did not create an undoable command");
    CHECK_MSG(editable->faces[0].materialIndex == 1 && rig.world.materialSlotCount(object) == 2,
              "undoing slot removal did not restore the slot assignment");
}

TEST(preferences_persist_emulate_numpad) {
    const char* priorValue = std::getenv("APPDATA");
    const bool hadPriorValue = priorValue != nullptr;
    const std::string prior = priorValue ? priorValue : "";
    const std::filesystem::path isolated =
        std::filesystem::temp_directory_path() / "opencode" / "ember3d-emulate-numpad-test";
    std::filesystem::create_directories(isolated);
    const bool isolatedEnv = _putenv_s("APPDATA", isolated.string().c_str()) == 0;
    CHECK_MSG(isolatedEnv, "could not isolate APPDATA for the preferences round-trip test");
    if (!isolatedEnv) return;

    ember::editor::Preferences prefs;
    CHECK_MSG(!prefs.emulateNumpad, "Emulate Numpad must default off");
    prefs.emulateNumpad = true;
    CHECK_MSG(ember::editor::savePreferences(prefs), "savePreferences failed for Emulate Numpad");
    const ember::editor::Preferences loaded = ember::editor::loadPreferences();
    CHECK_MSG(loaded.emulateNumpad, "loadPreferences did not restore Emulate Numpad");

    CHECK_MSG(_putenv_s("APPDATA", hadPriorValue ? prior.c_str() : "") == 0,
              "could not restore APPDATA after the preferences test");
    std::error_code ec;
    std::filesystem::remove_all(isolated, ec);
}

HARNESS_MAIN()
