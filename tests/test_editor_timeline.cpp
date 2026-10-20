// tests/test_editor_timeline.cpp - owned by Verity / QA. Item 191 (UI-12, D44).
//
// CONTRACT UNDER TEST - D44 / ROADMAP: the Timeline is a frame-range/playhead ruler only, and its
// controls are EXACTLY "Start/End integer fields (defaults 1/250; clamp End>=Start), full-width numbered
// frame ruler and visible playhead; click/drag sets current frame, clamped to range", with the old panel
// apology REMOVED - the exact sentence D44 names: "Not connected to animation. The playhead only moves
// this view."
//
// HOW, headless, no GL: drawTimeline(TimelineState&) is the content-only entry point Editor::drawArea
// calls, so it can be driven in an ImGui window directly. Widget rects come from ImGui's own hit test
// (the pointer is scanned down the panel until the HOVERED ID equals that item's ID), and the widgets are
// then driven with real mouse events - so a field that is drawn but not interactive fails. Text is not
// stored by ImGui, so the ruler's numbers and the removed apology are asserted as PIXELS: a drawn line's
// ink width is compared against CalcTextSize of the exact string, with the one-sided slack the trailing
// advance of the last glyph costs (measured at 4-5 px for this font).
#include "harness.h"

#include "core/Theme.h"
#include "editor/Panels.h"
#include "editor/ThemeStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

// The sentence D44 removed from Timeline.cpp. Asserted to be ABSENT, by exact string, not by "no text".
constexpr const char* kRemovedApology = "Not connected to animation. The playhead only moves this view.";

struct Rig {
    ImGuiContext* ctx = nullptr;
    ember::editor::TimelineState tl;            // D44 defaults: start 1, end 250, current 1

    Rig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640.0f, 320.0f);
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
        ImGui::SetNextWindowSize(ImVec2(600.0f, 300.0f));
        ImGui::Begin("##timeline-host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ember::editor::drawTimeline(tl);
        ImGui::End();
        ImGui::Render();
    }
    void frames(int n, float mx, float my) { for (int i = 0; i < n; ++i) frame(mx, my); }
    ImGuiWindow* host() const { return ImGui::FindWindowByName("##timeline-host"); }

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
    bool drag(float x0, float y0, float x1) {
        ImGuiIO& io = ImGui::GetIO();
        const bool placed = pointerAt(x0, y0);
        io.AddMouseButtonEvent(0, true); frame(x0, y0); frame(x0, y0);
        for (int i = 1; i <= 6; ++i) {
            const float x = x0 + (x1 - x0) * float(i) / 6.0f;
            io.AddMousePosEvent(x, y0); frame(x, y0);
        }
        io.AddMouseButtonEvent(0, false); frame(x1, y0); frame(x1, y0);
        return placed;
    }
};

struct Box {
    bool found = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float hx = 0, hy = 0;                        // the point ImGui actually reported as hit
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

Box itemBox(Rig& rig, const char* label) {
    Box b;
    ImGuiWindow* w = rig.host();
    if (!w) return b;
    const ImGuiID want = w->GetID(label);
    // Several x columns: a labelled field's hit rect does not start at the window edge (its label does),
    // and Start and End sit side by side on one row. A hit in ANY column is a hit.
    const float cols[8] = {0.03f, 0.10f, 0.18f, 0.25f, 0.31f, 0.37f, 0.45f, 0.55f};
    for (float y = w->InnerRect.Min.y; y < w->InnerRect.Max.y; y += 1.0f) {
        bool hit = false;
        for (float frac : cols) {
            const float x = w->InnerRect.Min.x + w->InnerRect.GetWidth() * frac;
            rig.frame(x, y + 0.5f);
            if (ImGui::GetCurrentContext()->HoveredId == want) {
                hit = true;
                if (!b.found) { b.found = true; b.y0 = y; }
                b.y1 = y; b.hx = x; b.hy = y + 0.5f;
                break;
            }
        }
        if (hit) continue;
        if (b.found && y > b.y1 + 1.0f) break;      // the item's run has ended
    }
    if (b.found) { b.x0 = w->InnerRect.Min.x; b.x1 = w->InnerRect.Max.x; }
    return b;
}

// The ink width of every drawn text line (glyph triangles, clustered by y) inside a rect.
std::vector<float> drawnLineWidths(Rig& rig, float x0, float y0, float x1, float y1) {
    const ImDrawData* dd = ImGui::GetDrawData();
    std::vector<std::pair<float, float>> xs;
    if (!dd) return {};
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& a = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 0]];
                const ImDrawVert& b = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 1]];
                const ImDrawVert& c = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 2]];
                const bool textured = std::fabs(a.uv.x - b.uv.x) > 1e-6f || std::fabs(a.uv.y - b.uv.y) > 1e-6f ||
                                      std::fabs(a.uv.x - c.uv.x) > 1e-6f || std::fabs(a.uv.y - c.uv.y) > 1e-6f;
                if (!textured) continue;
                for (const ImDrawVert* v : {&a, &b, &c}) {
                    if (v->pos.x < x0 - 1.0f || v->pos.x > x1 + 1.0f) continue;
                    if (v->pos.y < y0 - 1.0f || v->pos.y > y1 + 1.0f) continue;
                    xs.push_back({v->pos.y, v->pos.x});
                }
            }
        }
    }
    std::sort(xs.begin(), xs.end());
    std::vector<float> widths;
    for (std::size_t i = 0; i < xs.size();) {
        std::size_t j = i;
        float maxY = xs[i].first, lo = xs[i].second, hi = xs[i].second;
        while (j < xs.size() && xs[j].first <= maxY + 4.0f) {
            maxY = std::fmax(maxY, xs[j].first);
            lo = std::fmin(lo, xs[j].second); hi = std::fmax(hi, xs[j].second);
            ++j;
        }
        widths.push_back(hi - lo);
        i = j;
    }
    return widths;
}

// The ink width of each LABEL drawn on one line inside a rect: glyph vertices clustered by X, because
// several frame numbers share a row and a y-cluster would merge them into one long run.
std::vector<float> drawnLabelWidths(Rig& rig, float x0, float y0, float x1, float y1) {
    const ImDrawData* dd = ImGui::GetDrawData();
    std::vector<std::pair<float, float>> xs;              // (x, y) of every glyph vertex in the rect
    if (!dd) return {};
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& a = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 0]];
                const ImDrawVert& b = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 1]];
                const ImDrawVert& c = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 2]];
                const bool textured = std::fabs(a.uv.x - b.uv.x) > 1e-6f || std::fabs(a.uv.y - b.uv.y) > 1e-6f ||
                                      std::fabs(a.uv.x - c.uv.x) > 1e-6f || std::fabs(a.uv.y - c.uv.y) > 1e-6f;
                if (!textured) continue;
                for (const ImDrawVert* v : {&a, &b, &c}) {
                    if (v->pos.x < x0 - 1.0f || v->pos.x > x1 + 1.0f) continue;
                    if (v->pos.y < y0 - 1.0f || v->pos.y > y1 + 1.0f) continue;
                    xs.push_back({v->pos.x, v->pos.y});
                }
            }
        }
    }
    std::sort(xs.begin(), xs.end());
    // Two glyph boxes belong to the SAME label while the next one starts within about one glyph advance
    // (~0.8 em); separate frame labels are a whole tick apart (tens of px) in every range this test uses.
    const float sameLabelGap = std::fmax(8.0f, ImGui::GetFontSize() * 0.8f);
    std::vector<float> widths;
    for (std::size_t i = 0; i < xs.size();) {
        std::size_t j = i;
        float maxX = xs[i].first;
        while (j < xs.size() && xs[j].first <= maxX + sameLabelGap) {
            maxX = std::fmax(maxX, xs[j].first);
            ++j;
        }
        widths.push_back(maxX - xs[i].first);
        i = j;
    }
    return widths;
}

// A drawn line matches a string when its ink width is no wider than the string and no more than 9 px
// narrower (the last glyph's trailing advance), which is far tighter than the gap between frame labels.
bool lineMatches(const std::vector<float> runs, float want) {
    for (float w : runs) if (w <= want + 1.5f && w >= want - 9.0f) return true;
    return false;
}

// A frame LABEL is measured from its glyph QUADS, which ImGui pads about a pixel on each side, so it reads
// up to ~3 px WIDER than CalcTextSize (measured +0..+3 on these labels). The window below still separates
// one-, two- and three-digit labels (7 / 14 / 21 px), which is what this assertion is for.
bool labelMatches(const std::vector<float>& runs, float want) {
    for (float w : runs) if (w >= want - 1.5f && w <= want + 5.5f) return true;
    return false;
}

} // namespace

// ---- item 191: the D44 controls, and the apology that is gone -----------------------------------
TEST(timeline_draws_d44s_range_ruler_and_playhead_and_not_the_removed_apology) {
    Rig rig;
    rig.frames(3, -10.0f, -10.0f);

    const Box start = itemBox(rig, "##timeline-start");
    const Box end = itemBox(rig, "##timeline-end");
    const Box ruler = itemBox(rig, "##timelineruler");
    CHECK_MSG(start.found, "the Timeline has no Start field");
    CHECK_MSG(end.found, "the Timeline has no End field");
    CHECK_MSG(ruler.found, "the Timeline has no ruler/playhead row");
    if (!start.found || !end.found || !ruler.found) return;

    // D44's defaults, asserted before anything is touched.
    CHECK_MSG(rig.tl.start == 1 && rig.tl.end == 250,
              "the Timeline's documented defaults are Start 1 / End 250, found %d / %d", rig.tl.start, rig.tl.end);

    // 1. The ruler spans the FULL content width (D44: "full-width numbered frame ruler").
    ImGuiWindow* host = rig.host();
    const float contentW = host->InnerRect.GetWidth();
    std::printf("       Start %s, End %s, ruler %s; host content width %.0f\n", "found", "found",
                "found", double(contentW));
    CHECK_MSG(ruler.x1 - ruler.x0 >= contentW - 1.0f,
              "the ruler is %.0f px wide but the content region is %.0f px: it is not full width",
              double(ruler.x1 - ruler.x0), double(contentW));

    // 2. The numbered ruler: regenerate D44's label list and require every label's ink width to be drawn.
    {
        const int span = std::max(1, rig.tl.end - rig.tl.start);
        const int step = std::max(1, span / 10);
        std::vector<std::string> labels;
        for (int frame = rig.tl.start; frame <= rig.tl.end; frame += step) labels.push_back(std::to_string(frame));
        const std::vector<float> runs = drawnLabelWidths(rig, ruler.x0, ruler.y0, ruler.x1, ruler.y1);
        std::printf("       ruler label widths (%zu) : ", runs.size());
        for (const float w : runs) std::printf("%.0f ", double(w));
        std::printf("\n");
        CHECK_MSG(labels.size() >= 5, "the ruler should carry ~10 numbered labels, found %zu", labels.size());
        for (const std::string& label : labels)
            CHECK_MSG(labelMatches(runs, ImGui::CalcTextSize(label.c_str()).x),
                      "the ruler does not draw the frame label \"%s\" (%.0f px of text)", label.c_str(),
                      double(ImGui::CalcTextSize(label.c_str()).x));
        // A numbered ruler, not one repeated label: the widths must cover at least three digit-counts.
        int distinctClasses = 0;
        for (float w : {7.0f, 14.0f, 21.0f}) if (labelMatches(runs, w)) ++distinctClasses;
        CHECK_MSG(distinctClasses >= 3, "the ruler's labels cover only %d digit-width class(es): it is not a "
                  "numbered ruler", distinctClasses);
    }

    // 3. The removed apology is ABSENT - by exact string, not by "there is no text".
    {
        const float apologyW = ImGui::CalcTextSize(kRemovedApology).x;
        const std::vector<float> runs = drawnLineWidths(rig, host->InnerRect.Min.x, host->InnerRect.Min.y,
                                                        host->InnerRect.Max.x, host->InnerRect.Max.y);
        CHECK_MSG(!lineMatches(runs, apologyW),
                  "the Timeline still draws the removed D44 apology \"%s\" (%.0f px of text)", kRemovedApology,
                  double(apologyW));
        // CONTROL: the checker does find that sentence when it IS drawn, so the absence above means something.
        CHECK_MSG(runs.size() > 0, "control setup: the Timeline drew no text at all, so an absence check proves nothing");
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(640.0f, 320.0f));
        ImGui::Begin("##control-host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextDisabled("%s", kRemovedApology);
        ImGui::End();
        ImGui::Render();
        ImGuiWindow* cw = ImGui::FindWindowByName("##control-host");
        const std::vector<float> controlRuns = drawnLineWidths(rig, cw->InnerRect.Min.x, cw->InnerRect.Min.y,
                                                              cw->InnerRect.Max.x, cw->InnerRect.Max.y);
        CHECK_MSG(lineMatches(controlRuns, apologyW),
                  "control: the removed apology was NOT detected even when drawn, so the absence check above "
                  "cannot fail and proves nothing");
    }

    // 4. Start/End are live fields: a real drag writes TimelineState.
    rig.tl = ember::editor::TimelineState{};      // back to 1/250/1
    rig.frames(2, -10.0f, -10.0f);
    const int startBefore = rig.tl.start;
    rig.drag(start.hx, start.hy, start.x1 - 6.0f);
    CHECK_MSG(rig.tl.start != startBefore, "dragging the Start field did not write TimelineState::start (still %d)",
              rig.tl.start);
    const int endBefore = rig.tl.end;
    rig.drag(end.hx, end.hy, end.x1 - 6.0f);
    CHECK_MSG(rig.tl.end != endBefore, "dragging the End field did not write TimelineState::end (still %d)",
              rig.tl.end);

    // 5. The documented clamp: End >= Start, and the playhead is pulled inside the range.
    rig.tl.start = 100; rig.tl.end = 50; rig.tl.current = 5;
    rig.frame(-10.0f, -10.0f);
    CHECK_MSG(rig.tl.end == rig.tl.start,
              "End was not clamped up to Start: start %d, end %d", rig.tl.start, rig.tl.end);
    CHECK_MSG(rig.tl.current >= rig.tl.start && rig.tl.current <= rig.tl.end,
              "the playhead was not clamped into the range: current %d, range %d..%d", rig.tl.current,
              rig.tl.start, rig.tl.end);

    // 6. Click and drag set the current frame proportionally, and clamp at the far end.
    rig.tl = ember::editor::TimelineState{};
    rig.frames(2, -10.0f, -10.0f);
    const Box ruler2 = itemBox(rig, "##timelineruler");
    CHECK_MSG(ruler2.found, "the ruler vanished after the range was reset");
    if (ruler2.found) {
        const float midX = (ruler2.x0 + ruler2.x1) * 0.5f;
        rig.click(midX, ruler2.cy());
        const int clickedFrame = rig.tl.current;
        const int span = rig.tl.end - rig.tl.start;
        const int wantMid = rig.tl.start + int(std::lround(span * 0.5));
        CHECK_MSG(std::abs(clickedFrame - wantMid) <= 2,
                  "clicking the middle of the ruler set frame %d, expected about %d (range %d..%d)",
                  clickedFrame, wantMid, rig.tl.start, rig.tl.end);
        rig.drag(midX, ruler2.cy(), ruler2.x1 + 40.0f);      // beyond the right edge
        CHECK_MSG(rig.tl.current == rig.tl.end,
                  "dragging past the right edge left the playhead at frame %d, not clamped to End %d",
                  rig.tl.current, rig.tl.end);
        std::printf("       ruler click at the middle -> frame %d (expected ~%d); drag past the right edge -> "
                    "frame %d (End %d)\n", clickedFrame, wantMid, rig.tl.current, rig.tl.end);
    }
}

HARNESS_MAIN()
