// tests/test_editor_icons.cpp - owned by Verity / QA. Item 185 (D43 icon vector glyphs).
//
// CONTRACT UNDER TEST - D43, ARCHITECTURE.md "Icon drawing strategy for Properties and Toolbar":
//   "add a Verity-owned tests/** pixel test proving a non-empty bounded glyph at scale 1 and a
//    second DPI scale plus correct active/hover tint."
//
// HOW, and why this is a PIXEL test: drawIcon() is called with explicit button rects and a uiScale
// into a headless ImGui frame, and the REAL ImDrawData is then software-rasterised (the
// theme_probe.cpp pattern). Every assertion is therefore about pixels the glyph actually painted,
// not about what the source says it should paint. Nothing here needs OpenGL, so the default CTest
// run stays context-free.
//
// WHAT EACH ASSERTION MEANS (and its limit):
//   non-empty : >= 15 painted pixel centres at uiScale 1.
//   bounded   : the EXACT vertex extent lies inside the caller's button rect (+1px, the AA feather),
//               so no vertex anywhere in the frame escapes the rect; AND no painted pixel lies
//               outside the icon square the implementation derives, min(rect, 16*uiScale) centred on
//               the rect - i.e. the glyph is never stretched to fill a larger button.
//   centred   : the painted bounding box's centre is within ONE grid unit of the button centre. A
//               16x16 authoring grid makes this the tightest true statement: 'Output' is deliberately
//               0.5 units right of centre, so "exactly centred" would be a false claim.
//   scaled    : at uiScale 2 the exact VERTEX extent doubles, the painted area roughly quadruples, and
//               the two vertex sets coincide in normalized 16x16 grid units (worst nearest-vertex miss
//               <= 0.8 units, where the fixed 1px antialiasing feather alone accounts for 0.5). That is
//               the D43 rule "centered and uniformly scaled, never given a per-icon magic pixel size":
//               the SAME drawing, scaled. (Vertex counts are NOT assumed equal in general - ImGui's
//               automatic arc segmentation depends on the radius - but they are asserted equal here,
//               which additionally makes the grid-space match a near-bijection. The extent uses
//               vertices, not painted pixels, because the AA feather is a fixed 1px at both scales and
//               would bias a pixel-count extent.)
//   tinted    : every vertex carries the caller's colour, RGB exact - the glyph never tints itself.
//               Alpha may only be ATTENUATED, never raised: ImGui puts its antialiasing gradient in the
//               vertex alpha for strokes, so an AA fringe vertex is legitimately fainter. At least one
//               vertex carries the caller's full alpha (the glyph ids are outline/hatching, so
//               every non-fringe vertex is the exact ImU32).
//   controls  : an empty frame is reported as empty, a second tint is distinguishable from the first,
//               and a vertex placed outside the rect IS reported - so each predicate can fail.
//
// The second half drives the REAL Properties rail (drawProperties) and asserts, in pixels, that the
// selected category is drawn in ThemeTokens::active, the others in ThemeTokens::textPrimary, that
// the active marker follows the selection, and that a hovered row's background becomes
// ThemeTokens::hover.
#include "harness.h"

#include "core/Theme.h"
#include "editor/Icons.h"
#include "editor/Panels.h"
#include "editor/ThemeStyle.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using ember::editor::IconId;
using ember::editor::PropertiesTab;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr float kGrid = 16.0f;          // the authoring grid D43 fixes
constexpr int kW = 640, kH = 480;

// ---- software rasteriser over the real draw data (theme_probe.cpp pattern) ---------------------

// One composited pixel: RGB over `backdrop`, plus `cov` = the LARGEST triangle alpha covering the
// pixel centre. Coverage, not colour, is what a geometry assertion needs: a stroke is two triangles
// and must not be counted twice, and an antialiased edge must not read as solid ink.
struct Px {
    float r = 0, g = 0, b = 0, cov = 0;
};

// Filled shapes, AA strokes and line segments are ALL included here, deliberately. The earlier
// version of this file filtered on "all three vertices share a uv" (the theme_probe.cpp pattern,
// which only ever samples FILLED rects) - and that silently dropped every stroked shape, because
// this ImGui builds with ImDrawListFlags_AntiAliasedLinesUseTex: an antialiased line's uv samples
// the atlas's AA band, so its uvs DIFFER across the triangle. Every frame sampled here contains
// only glyph geometry (a NoBackground window; in the rail, the strip's Selectable label is a hidden
// "##id" and its own fill is not a token colour), so no filter is needed and none is used.
const float kBackdrop[3] = {24.0f, 26.0f, 30.0f};   // what shows through a transparent pixel

Px sample(const ImDrawData* dd, float px, float py, const float bg[3]) {
    Px out{bg[0], bg[1], bg[2], 0.0f};
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            if (px < cmd.ClipRect.x || py < cmd.ClipRect.y || px >= cmd.ClipRect.z || py >= cmd.ClipRect.w) continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& a = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 0]];
                const ImDrawVert& b = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 1]];
                const ImDrawVert& c = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 2]];
                const float d = (b.pos.y - c.pos.y) * (a.pos.x - c.pos.x) + (c.pos.x - b.pos.x) * (a.pos.y - c.pos.y);
                if (std::fabs(d) < 1e-6f) continue;
                const float l1 = ((b.pos.y - c.pos.y) * (px - c.pos.x) + (c.pos.x - b.pos.x) * (py - c.pos.y)) / d;
                const float l2 = ((c.pos.y - a.pos.y) * (px - c.pos.x) + (a.pos.x - c.pos.x) * (py - c.pos.y)) / d;
                const float l3 = 1.0f - l1 - l2;
                if (l1 < 0 || l2 < 0 || l3 < 0) continue;
                auto ch = [&](int shift) {
                    return l1 * float((a.col >> shift) & 0xFF) + l2 * float((b.col >> shift) & 0xFF) + l3 * float((c.col >> shift) & 0xFF);
                };
                const float al = ch(24) / 255.0f;
                if (al > out.cov) out.cov = al;
                out.r = out.r * (1.0f - al) + ch(0) * al;
                out.g = out.g * (1.0f - al) + ch(8) * al;
                out.b = out.b * (1.0f - al) + ch(16) * al;
            }
        }
    }
    return out;
}

// A painted pixel: more than a trace of alpha at its centre.
bool painted(const Px& p) { return p.cov > 0.05f; }

// ImGui packs colours as ABGR in an ImU32.
int redOf(unsigned c)   { return int(c & 0xFFu); }
int greenOf(unsigned c) { return int((c >> 8) & 0xFFu); }
int blueOf(unsigned c)  { return int((c >> 16) & 0xFFu); }
int alphaOf(unsigned c) { return int((c >> 24) & 0xFFu); }

// ---- one measured frame ------------------------------------------------------------------------

struct Measure {
    int vtx = 0;
    float vMinX = 1e9f, vMinY = 1e9f, vMaxX = -1e9f, vMaxY = -1e9f;   // EXACT vertex extent
    std::vector<ImVec2> verts;                                        // every vertex position, in order
    long paintedCount = 0;
    float pMinX = 1e9f, pMinY = 1e9f, pMaxX = -1e9f, pMaxY = -1e9f;   // painted pixel centres
    std::vector<unsigned> colours;                                    // distinct vertex colours, in order
    int gx = 0, gy = 0, gw = 0, gh = 0;                               // sampled grid (window corner + pixel)
    std::vector<unsigned char> mask;                                  // 1 = painted pixel centre

    bool anyPaintedOutside(float x0, float y0, float x1, float y1) const {
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                if (mask[std::size_t(y) * gw + x]) {
                    const float px = float(gx + x) + 0.5f, py = float(gy + y) + 0.5f;
                    if (px < x0 || py < y0 || px > x1 || py > y1) return true;
                }
        return false;
    }
};

// Draw one frame whose window contributes NOTHING of its own (NoBackground, no title bar), so every
// vertex in the draw data belongs to whatever `draw` put there. Then read back the vertices and
// rasterise the pixel centres of [sampleMin, sampleMax).
template <typename DrawFn>
Measure measureFrame(DrawFn draw, const ImVec2& sampleMin, const ImVec2& sampleMax) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##icon-frame", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    draw();
    ImGui::End();
    ImGui::Render();

    const ImDrawData* dd = ImGui::GetDrawData();
    Measure m;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (int v = 0; v < dl->VtxBuffer.Size; ++v) {
            const ImDrawVert& vert = dl->VtxBuffer[v];
            m.verts.push_back(vert.pos);
            m.vMinX = std::fmin(m.vMinX, vert.pos.x); m.vMaxX = std::fmax(m.vMaxX, vert.pos.x);
            m.vMinY = std::fmin(m.vMinY, vert.pos.y); m.vMaxY = std::fmax(m.vMaxY, vert.pos.y);
            if (std::find(m.colours.begin(), m.colours.end(), vert.col) == m.colours.end())
                m.colours.push_back(vert.col);
            ++m.vtx;
        }
    }

    m.gx = int(std::floor(sampleMin.x)); m.gy = int(std::floor(sampleMin.y));
    m.gw = std::max(0, int(std::ceil(sampleMax.x)) - m.gx);
    m.gh = std::max(0, int(std::ceil(sampleMax.y)) - m.gy);
    m.mask.assign(std::size_t(m.gw) * m.gh, 0);
    for (int y = 0; y < m.gh; ++y) {
        for (int x = 0; x < m.gw; ++x) {
            const float px = float(m.gx + x) + 0.5f, py = float(m.gy + y) + 0.5f;
            if (painted(sample(dd, px, py, kBackdrop))) {
                m.mask[std::size_t(y) * m.gw + x] = 1;
                ++m.paintedCount;
                m.pMinX = std::fmin(m.pMinX, px); m.pMaxX = std::fmax(m.pMaxX, px);
                m.pMinY = std::fmin(m.pMinY, py); m.pMaxY = std::fmax(m.pMaxY, py);
            }
        }
    }
    return m;
}

// The icon square the implementation derives for a button rect and scale (Icons.cpp).
float iconSize(const ImVec2& bmin, const ImVec2& bmax, float uiScale) {
    return std::min({bmax.x - bmin.x, bmax.y - bmin.y, kGrid * uiScale});
}

bool insideBy(float v0, float v1, float lo, float hi, float tol) { return v0 >= lo - tol && v1 <= hi + tol; }

// Same-glyph-at-both-scales check, in GRID space. drawIcon maps one normalized point set through
// origin + p*scale, so the two vertex sets must coincide once each is expressed in 16x16 grid units
// (a vertex on the AA feather differs by up to ~0.5 units, because the feather is a fixed 1 pixel:
// 1 grid unit at uiScale 1, 0.5 at uiScale 2). A glyph that were re-authored per size, or that
// stretched non-uniformly, would miss by whole units - so 0.8 units is a tight bound, not a fudge.
std::vector<ImVec2> gridVerts(const std::vector<ImVec2>& verts, const ImVec2& origin, float size) {
    const float scale = size / kGrid;
    std::vector<ImVec2> out;
    out.reserve(verts.size());
    for (const ImVec2& v : verts) out.push_back(ImVec2((v.x - origin.x) / scale, (v.y - origin.y) / scale));
    return out;
}

double maxNearestDistance(const std::vector<ImVec2>& a, const std::vector<ImVec2>& b) {
    double worst = 0.0;
    for (const ImVec2& p : a) {
        double best = 1e9;
        for (const ImVec2& q : b) {
            const double dx = double(p.x - q.x), dy = double(p.y - q.y);
            best = std::fmin(best, std::sqrt(dx * dx + dy * dy));
        }
        worst = std::fmax(worst, best);
    }
    return worst;
}

// ---- the shared headless context ---------------------------------------------------------------

struct IconRig {
    ImGuiContext* ctx = nullptr;
    explicit IconRig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(float(kW), float(kH));
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    }
    ~IconRig() { ImGui::DestroyContext(ctx); }
    IconRig(const IconRig&) = delete;
    IconRig& operator=(const IconRig&) = delete;
};

// Render one icon into a button rect and measure it. `button` is placed inside the frame window so
// the glyph is never clipped by it.
Measure drawOne(IconId icon, const ImVec2& bmin, const ImVec2& bmax, ImU32 colour, float uiScale) {
    const ImVec2 lo(bmin.x - 6.0f, bmin.y - 6.0f), hi(bmax.x + 6.0f, bmax.y + 6.0f);
    return measureFrame([&] {
        ember::editor::drawIcon(*ImGui::GetWindowDrawList(), icon, bmin, bmax, colour, uiScale);
    }, lo, hi);
}

} // namespace

// ---- item 185: the Properties glyphs, at scale 1 and at a second DPI scale ---------------------
//
// uiScale is the test's DPI knob: the implementation derives the glyph square from it
// (min(button, 16 * uiScale)), which is exactly the "second DPI scale" D43 asks the test to cover,
// and it is the same parameter the rail passes from ThemeTokens::uiScale.
TEST(icons_category_glyphs_are_non_empty_bounded_and_centred_at_1x_and_2x) {
    IconRig rig;
    const ember::ThemeTokens& t = ember::editor::theme::tokens();

    // Setup: the mapping is one-to-one, so the loop below cannot silently iterate a duplicate tab or
    // icon (iconForTab is the function the rail itself uses).
    {
        std::vector<PropertiesTab> tabs(ember::editor::kPropertiesTabOrder.begin(),
                                       ember::editor::kPropertiesTabOrder.end());
        std::vector<IconId> ids;
        for (PropertiesTab tab : tabs) ids.push_back(ember::editor::iconForTab(tab));
        std::sort(ids.begin(), ids.end());
        CHECK_MSG(tabs.size() == 7, "expected seven Properties tabs, got %zu", tabs.size());
        CHECK_MSG(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
                  "iconForTab maps two categories to the same IconId: glyphs are not one-to-one");
    }

    const unsigned kTint = IM_COL32(204, 204, 204, 255);

    for (IconId icon : ember::editor::kPropertiesTabIcons) {
        const float S = 40.0f;                                  // a button larger than either glyph
        const ImVec2 bmin(100.0f, 100.0f), bmax(100.0f + S, 100.0f + S);
        const Measure one = drawOne(icon, bmin, bmax, kTint, 1.0f);
        const Measure two = drawOne(icon, bmin, bmax, kTint, 2.0f);
        const int id = static_cast<int>(icon);
        const float size1 = iconSize(bmin, bmax, 1.0f), size2 = iconSize(bmin, bmax, 2.0f);
        const ImVec2 c((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f);

        std::printf("       icon %d  1x: vtx=%d painted=%ld bbox=(%.1f,%.1f)-(%.1f,%.1f)  "
                    "2x: vtx=%d painted=%ld bbox=(%.1f,%.1f)-(%.1f,%.1f)\n",
                    id, one.vtx, one.paintedCount, double(one.pMinX), double(one.pMinY), double(one.pMaxX), double(one.pMaxY),
                    two.vtx, two.paintedCount, double(two.pMinX), double(two.pMinY), double(two.pMaxX), double(two.pMaxY));

        // 1. non-empty at both scales.
        CHECK_MSG(one.paintedCount >= 15, "icon %d paints only %ld pixel(s) at uiScale 1: it is empty or invisible",
                  id, one.paintedCount);
        CHECK_MSG(two.paintedCount >= 15, "icon %d paints only %ld pixel(s) at uiScale 2: it is empty or invisible",
                  id, two.paintedCount);

        // 2. bounded: no VERTEX (AA feather included) escapes the button rect...
        for (const Measure* m : {&one, &two})
            CHECK_MSG(insideBy(m->vMinX, m->vMaxX, bmin.x, bmax.x, 1.0f) &&
                          insideBy(m->vMinY, m->vMaxY, bmin.y, bmax.y, 1.0f),
                      "icon %d draws a vertex outside the button rect (%.1f,%.1f)-(%.1f,%.1f): "
                      "vertices (%.2f,%.2f)-(%.2f,%.2f)", id, double(bmin.x), double(bmin.y), double(bmax.x),
                      double(bmax.y), double(m->vMinX), double(m->vMinY), double(m->vMaxX), double(m->vMaxY));
        // ...and no PAINTED pixel escapes the icon square the implementation derived (so a wider
        // button does not stretch the glyph).
        CHECK_MSG(!one.anyPaintedOutside(c.x - size1 * 0.5f, c.y - size1 * 0.5f, c.x + size1 * 0.5f, c.y + size1 * 0.5f),
                  "icon %d paints outside its %.0fpx square at uiScale 1: it is stretched, not scaled", id, double(size1));
        CHECK_MSG(!two.anyPaintedOutside(c.x - size2 * 0.5f, c.y - size2 * 0.5f, c.x + size2 * 0.5f, c.y + size2 * 0.5f),
                  "icon %d paints outside its %.0fpx square at uiScale 2: it is stretched, not scaled", id, double(size2));

        // 3. centred: within ONE grid unit of the button centre (a 16x16 authoring grid; 'Output' is
        //    deliberately half a unit right of centre).
        const float unit1 = size1 / kGrid, unit2 = size2 / kGrid;
        CHECK_MSG(std::fabs((one.pMinX + one.pMaxX) * 0.5f - c.x) <= 1.0f * unit1 &&
                      std::fabs((one.pMinY + one.pMaxY) * 0.5f - c.y) <= 1.0f * unit1,
                  "icon %d is not centred at uiScale 1: painted centre (%.2f,%.2f) vs button centre (%.1f,%.1f)",
                  id, double((one.pMinX + one.pMaxX) * 0.5f), double((one.pMinY + one.pMaxY) * 0.5f), double(c.x), double(c.y));
        CHECK_MSG(std::fabs((two.pMinX + two.pMaxX) * 0.5f - c.x) <= 1.0f * unit2 &&
                      std::fabs((two.pMinY + two.pMaxY) * 0.5f - c.y) <= 1.0f * unit2,
                  "icon %d is not centred at uiScale 2: painted centre (%.2f,%.2f) vs button centre (%.1f,%.1f)",
                  id, double((two.pMinX + two.pMaxX) * 0.5f), double((two.pMinY + two.pMaxY) * 0.5f), double(c.x), double(c.y));

        // 4. scaled, not re-authored: the EXACT vertex extent doubles (a pixel-count extent would be
        //    biased by the fixed 1px antialiasing feather), the painted area roughly quadruples, and
        //    the vertex SET is the same drawing in normalized grid units.
        const float ext1 = std::fmax(one.vMaxX - one.vMinX, one.vMaxY - one.vMinY);
        const float ext2 = std::fmax(two.vMaxX - two.vMinX, two.vMaxY - two.vMinY);
        CHECK_MSG(ext1 > 0.0f && (ext2 / ext1) > 1.8f && (ext2 / ext1) < 2.2f,
                  "icon %d vertex extent %.1fpx -> %.1fpx at 2x: ratio %.2f, expected ~2", id, double(ext1),
                  double(ext2), double(ext1 > 0 ? ext2 / ext1 : 0.0));
        const double areaRatio = one.paintedCount > 0 ? double(two.paintedCount) / double(one.paintedCount) : 0.0;
        CHECK_MSG(areaRatio > 3.0 && areaRatio < 5.2,
                  "icon %d painted area %ld -> %ld px: ratio %.2f, expected ~4 (it is scaled)", id,
                  one.paintedCount, two.paintedCount, areaRatio);
        // The same vertex set, in grid units: the 2x glyph is the 1x glyph scaled, not a re-authored
        // drawing or a stretch. Equal counts make this a near-bijection; a whole-unit miss (the
        // symptom of per-size magic numbers) fails it.
        const ImVec2 origin1(c.x - size1 * 0.5f, c.y - size1 * 0.5f);
        const ImVec2 origin2(c.x - size2 * 0.5f, c.y - size2 * 0.5f);
        const std::vector<ImVec2> g1 = gridVerts(one.verts, origin1, size1);
        const std::vector<ImVec2> g2 = gridVerts(two.verts, origin2, size2);
        CHECK_MSG(one.vtx == two.vtx && one.vtx > 0,
                  "icon %d emits %d vertices at uiScale 1 and %d at uiScale 2: the two scales are not the "
                  "same drawing", id, one.vtx, two.vtx);
        const double miss12 = maxNearestDistance(g1, g2), miss21 = maxNearestDistance(g2, g1);
        std::printf("        icon %d grid-space vertex match: worst %.3f / %.3f grid units (vtx %d = %d)\n",
                    id, miss12, miss21, one.vtx, two.vtx);
        CHECK_MSG(miss12 <= 0.8 && miss21 <= 0.8,
                  "icon %d: the 2x glyph's vertices are up to %.3f (1x->2x) / %.3f (2x->1x) grid units from "
                  "the 1x glyph's: it is not the same drawing scaled", id, miss12, miss21);
        // 5. the caller's tint reaches every vertex, RGB-exact. Alpha may only be ATTENUATED, never
        //    raised: ImGui puts its antialiasing gradient in the vertex alpha for strokes, so a fringe
        //    vertex is legitimately fainter. That is ImGui's alpha, not a different COLOUR.
        CHECK_MSG(!one.colours.empty(), "icon %d emitted no vertex colour at uiScale 1", id);
        for (const Measure* m : {&one, &two}) {
            bool anyFullAlpha = false;
            for (unsigned c : m->colours) {
                CHECK_MSG(redOf(c) == redOf(kTint) && greenOf(c) == greenOf(kTint) && blueOf(c) == blueOf(kTint),
                          "icon %d drew RGB(%d,%d,%d); the caller passed RGB(%d,%d,%d) - the glyph tints itself",
                          id, redOf(c), greenOf(c), blueOf(c), redOf(kTint), greenOf(kTint), blueOf(kTint));
                CHECK_MSG(alphaOf(c) <= alphaOf(kTint), "icon %d raised the caller's alpha %d -> %d", id,
                          alphaOf(kTint), alphaOf(c));
                anyFullAlpha = anyFullAlpha || alphaOf(c) == alphaOf(kTint);
            }
            CHECK_MSG(anyFullAlpha, "icon %d never draws the caller's colour at full alpha: the glyph is "
                      "uniformly faded, not tinted", id);
        }
    }
}

// The rect is the caller's: a button NARROWER than 16*uiScale must clamp the glyph, not clip or
// overflow it, and a degenerate rect must draw nothing at all.
TEST(icons_clamp_to_a_small_button_rect_and_draw_nothing_for_an_empty_one) {
    IconRig rig;
    const ImU32 tint = IM_COL32(204, 204, 204, 255);

    const ImVec2 smallMin(300.0f, 300.0f), smallMax(312.0f, 312.0f);   // 12x12, but uiScale 2 asks 32
    const Measure m = drawOne(IconId::Object, smallMin, smallMax, tint, 2.0f);
    CHECK_MSG(m.paintedCount >= 15, "a 12x12 button at uiScale 2 painted only %ld pixel(s)", m.paintedCount);
    CHECK_MSG(insideBy(m.vMinX, m.vMaxX, smallMin.x, smallMax.x, 1.0f) &&
                  insideBy(m.vMinY, m.vMaxY, smallMin.y, smallMax.y, 1.0f),
              "the glyph escaped a 12x12 button rect: vertices (%.2f,%.2f)-(%.2f,%.2f)", double(m.vMinX),
              double(m.vMinY), double(m.vMaxX), double(m.vMaxY));
    CHECK_MSG(!m.anyPaintedOutside(smallMin.x, smallMin.y, smallMax.x, smallMax.y),
              "the glyph painted outside a 12x12 button rect");

    // A wide-but-flat button: the glyph fits the SHORTER side and stays centred.
    const ImVec2 flatMin(400.0f, 400.0f), flatMax(460.0f, 410.0f);
    const Measure flat = drawOne(IconId::World, flatMin, flatMax, tint, 2.0f);
    CHECK_MSG(flat.paintedCount >= 15, "a 60x10 button painted only %ld pixel(s)", flat.paintedCount);
    CHECK_MSG(insideBy(flat.vMinY, flat.vMaxY, flatMin.y, flatMax.y, 1.0f),
              "the glyph escaped a 60x10 button rect vertically: (%.2f,%.2f)", double(flat.vMinY), double(flat.vMaxY));

    const Measure empty = drawOne(IconId::Scene, smallMin, smallMin, tint, 1.0f);
    CHECK_MSG(empty.vtx == 0 && empty.paintedCount == 0,
              "a zero-size button rect still emitted %d vertices / %ld painted pixels", empty.vtx, empty.paintedCount);
}

// ---- CONTROLS: every predicate above must be able to fail ---------------------------------------
TEST(icons_pixel_predicates_can_fail) {
    IconRig rig;
    const ImVec2 bmin(200.0f, 200.0f), bmax(216.0f, 216.0f);
    const ImU32 tint = IM_COL32(204, 204, 204, 255);

    // (a) an empty frame is empty: the non-empty predicate is not satisfied by nothing.
    const Measure nothing = measureFrame([] {}, ImVec2(bmin.x - 6, bmin.y - 6), ImVec2(bmax.x + 6, bmax.y + 6));
    CHECK_MSG(nothing.paintedCount == 0, "control: an empty frame reported %ld painted pixel(s)", nothing.paintedCount);
    CHECK_MSG(!(nothing.paintedCount >= 15), "control: the non-empty predicate accepted an empty frame");

    // (b) tints are distinguishable, so "the caller's colour reached the pixels" is a real check.
    const Measure idle = drawOne(IconId::Object, bmin, bmax, tint, 1.0f);
    const Measure selected = drawOne(IconId::Object, bmin, bmax, IM_COL32(128, 191, 255, 255), 1.0f);
    CHECK_MSG(idle.vtx > 0 && selected.vtx > 0, "control setup: a glyph drew nothing");
    CHECK_MSG(idle.colours != selected.colours, "control: two different tints produced identical vertex colours");

    // (c) a vertex outside the rect IS reported by the bound predicate.
    const Measure stray = measureFrame([&] {
        ImGui::GetWindowDrawList()->AddLine(ImVec2(bmin.x, bmin.y), ImVec2(bmax.x + 8.0f, bmax.y), tint, 1.0f);
    }, ImVec2(bmin.x - 6, bmin.y - 6), ImVec2(bmax.x + 12, bmax.y + 6));
    CHECK_MSG(!insideBy(stray.vMinX, stray.vMaxX, bmin.x, bmax.x, 1.0f),
              "control: the bound predicate accepted a vertex %.1fpx outside the rect", double(stray.vMaxX - bmax.x));
    CHECK_MSG(stray.anyPaintedOutside(bmin.x, bmin.y, bmax.x, bmax.y),
              "control: the painted-pixel bound did not report ink drawn outside the rect");
}

// ---- item 185: the REAL Properties rail hands the right theme colour to the right category -------

// The rail, drawn by the real drawProperties() in a headless window: no source is edited and no
// internals are poked, so this asserts the pixels the user would see in the strip.
struct RailRig {
    ImGuiContext* ctx = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;
    PropertiesTab tab = PropertiesTab::Object;

    RailRig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(900.0f, 700.0f);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    }
    ~RailRig() { ImGui::DestroyContext(ctx); }
    RailRig(const RailRig&) = delete;
    RailRig& operator=(const RailRig&) = delete;

    void frame(float mx, float my) {
        ImGui::GetIO().AddMousePosEvent(mx, my);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(420, 400));
        ember::editor::drawProperties(world, undo, tab);
        ImGui::Render();          // Render = EndFrame + the draw data this test samples
    }
    void frames(int n, float mx, float my) { for (int i = 0; i < n; ++i) frame(mx, my); }

    ImGuiWindow* strip() const {
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (std::string(w->Name).find("##properties-icons") != std::string::npos) return w;
        return nullptr;
    }
};

// The icon rows of the strip, top to bottom. Ink is a pixel whose composited colour IS one of the two
// glyph tokens: the strip's own background is opaque, so coverage cannot tell ink from background, and
// the active row's Selectable fill is the active token at partial alpha - which blends to something
// else - so an exact colour match sees the opaque glyph and nothing else.
struct Band { int y0, y1; int activePixels, idlePixels; };

bool nearColour(const Px& p, unsigned u, float tol) {
    return std::fabs(p.r - float(redOf(u))) <= tol && std::fabs(p.g - float(greenOf(u))) <= tol &&
           std::fabs(p.b - float(blueOf(u))) <= tol;
}

std::vector<Band> scanIconBands(RailRig& rig, unsigned activeU, unsigned idleU) {
    ImGuiWindow* strip = rig.strip();
    std::vector<Band> bands;
    if (!strip) return bands;
    const int x0 = int(strip->InnerRect.Min.x), x1 = int(strip->InnerRect.Max.x);
    const int y0 = int(strip->Pos.y), y1 = int(strip->Pos.y + strip->Size.y);
    for (int y = y0; y < y1; ++y) {
        int na = 0, ni = 0;
        for (int x = x0; x < x1; ++x) {
            const Px p = sample(ImGui::GetDrawData(), float(x) + 0.5f, float(y) + 0.5f, kBackdrop);
            if (nearColour(p, activeU, 2.0f)) ++na;
            else if (nearColour(p, idleU, 2.0f)) ++ni;
        }
        if (na + ni == 0) continue;
        if (!bands.empty() && y == bands.back().y1 + 1) {
            bands.back().y1 = y; bands.back().activePixels += na; bands.back().idlePixels += ni; continue;
        }
        bands.push_back({y, y, na, ni});
    }
    return bands;
}

TEST(properties_rail_icons_use_the_active_token_for_the_selected_category_and_textprimary_for_the_rest) {
    const ember::ThemeTokens& t = ember::editor::theme::tokens();
    const unsigned activeU = ImGui::ColorConvertFloat4ToU32(ember::editor::theme::color(t.active));
    const unsigned idleU = ImGui::ColorConvertFloat4ToU32(ember::editor::theme::color(t.textPrimary));
    CHECK_MSG(activeU != idleU, "setup: the active and textPrimary tokens are the same colour, so the "
              "tint assertion below could not tell a selected category from an idle one");

    for (int selected = 0; selected < 7; ++selected) {
        RailRig rig;
        rig.tab = ember::editor::kPropertiesTabOrder[std::size_t(selected)];
        rig.frames(4, -10.0f, -10.0f);
        const std::vector<Band> bands = scanIconBands(rig, activeU, idleU);
        CHECK_MSG(bands.size() == 7, "tab %d: the strip drew %zu icon band(s), expected seven categories",
                  selected, bands.size());
        if (bands.size() != 7) continue;

        int activeRows = 0;
        for (std::size_t i = 0; i < bands.size(); ++i) {
            const bool isActive = bands[i].activePixels > 0;
            const bool isIdle = bands[i].idlePixels > 0;
            if (isActive) ++activeRows;
            // Each row is drawn in ONE of the two tokens; a row in both would mean the marker and the
            // idle tint overlap on the same category.
            CHECK_MSG(isActive != isIdle, "tab %d row %zu: %d active-token pixel(s) and %d textPrimary "
                      "pixel(s) - a category icon must be exactly one of the two", selected, i,
                      bands[i].activePixels, bands[i].idlePixels);
        }
        CHECK_MSG(activeRows == 1, "tab %d: %d row(s) carry the active tint, expected exactly 1", selected, activeRows);
        CHECK_MSG(bands[std::size_t(selected)].activePixels > 0,
                  "tab %d: the active tint is not on the SELECTED row (%d) - the marker does not follow the tab",
                  selected, selected);
        std::printf("       tab %d: 7 icon bands; selected row %d drawn RGB(%d,%d,%d); other rows textPrimary RGB(%d,%d,%d)\n",
                    selected, selected, redOf(activeU), greenOf(activeU), blueOf(activeU), redOf(idleU),
                    greenOf(idleU), blueOf(idleU));
    }
}

TEST(properties_rail_hover_paints_the_hover_theme_token_behind_the_hovered_row_only) {
    const ember::ThemeTokens& t = ember::editor::theme::tokens();
    const ImVec4 hoverV = ember::editor::theme::color(t.hover);
    const float a = hoverV.w;                    // the hover token is translucent (e.g. 20/255)
    CHECK_MSG(a > 0.02f && a < 0.6f, "setup: the hover token's alpha is %.3f; this test assumes a "
              "translucent hover fill over the row background", double(a));

    RailRig rig;                                  // tab = Object, so row 4 is the active one
    rig.frames(4, -10.0f, -10.0f);
    ImGuiWindow* strip = rig.strip();
    CHECK_MSG(strip != nullptr, "the Properties icon strip was not found");
    if (!strip) return;

    const unsigned activeU = ImGui::ColorConvertFloat4ToU32(ember::editor::theme::color(t.active));
    const unsigned idleU = ImGui::ColorConvertFloat4ToU32(ember::editor::theme::color(t.textPrimary));

    // Rows: the second category's band, and one that must stay unchanged.
    const std::vector<Band> bands = scanIconBands(rig, activeU, idleU);
    CHECK_MSG(bands.size() == 7, "the strip drew %zu icon band(s), expected 7", bands.size());
    if (bands.size() != 7) return;
    const Band hovered = bands[1], other = bands[3];

    // A background sample per row: inside the strip's inner rect (where the Selectable fill is) at its
    // right edge, away from the centred glyph. Kept in 0..255 like every other colour here.
    const float bgX = strip->InnerRect.Max.x - 3.0f;
    auto rowBg = [&](int y) {
        const Px p = sample(ImGui::GetDrawData(), bgX, float(y) + 0.5f, kBackdrop);
        return ImVec4(p.r, p.g, p.b, 255.0f);
    };
    const ImVec4 hoveredBefore = rowBg((hovered.y0 + hovered.y1) / 2);
    const ImVec4 otherBefore = rowBg((other.y0 + other.y1) / 2);

    // Hold the pointer on the second row (three frames: far inside ImGui's tooltip delay, so no
    // tooltip window can overlap the strip while it is sampled).
    const float hoverY = float((hovered.y0 + hovered.y1) / 2) + 0.5f;
    rig.frames(3, bgX, hoverY);

    const ImVec4 hoveredAfter = rowBg((hovered.y0 + hovered.y1) / 2);
    const ImVec4 otherAfter = rowBg((other.y0 + other.y1) / 2);

    // Source-over prediction, in the 0..255 space the sampler works in: hover colour over the row's
    // own background.
    const ImVec4 hover255(hoverV.x * 255.0f, hoverV.y * 255.0f, hoverV.z * 255.0f, 255.0f);
    auto composite = [&](const ImVec4& bg) {
        return ImVec4(bg.x * (1 - a) + hover255.x * a, bg.y * (1 - a) + hover255.y * a, bg.z * (1 - a) + hover255.z * a, 255.0f);
    };
    auto near = [](const ImVec4& p, const ImVec4& q, float tol) {
        return std::fabs(p.x - q.x) <= tol && std::fabs(p.y - q.y) <= tol && std::fabs(p.z - q.z) <= tol;
    };
    std::printf("       hovered row bg %.0f,%.0f,%.0f -> %.0f,%.0f,%.0f; predicted with hover token "
                "%.0f,%.0f,%.0f (alpha %.2f)\n", double(hoveredBefore.x), double(hoveredBefore.y),
                double(hoveredBefore.z), double(hoveredAfter.x), double(hoveredAfter.y),
                double(hoveredAfter.z), double(composite(hoveredBefore).x), double(composite(hoveredBefore).y),
                double(composite(hoveredBefore).z), double(a));

    CHECK_MSG(near(hoveredAfter, composite(hoveredBefore), 3.0f),
              "hovering a category did not paint ThemeTokens::hover over the row: got %.0f,%.0f,%.0f, "
              "predicted %.0f,%.0f,%.0f", double(hoveredAfter.x), double(hoveredAfter.y),
              double(hoveredAfter.z), double(composite(hoveredBefore).x),
              double(composite(hoveredBefore).y), double(composite(hoveredBefore).z));
    CHECK_MSG(near(otherAfter, otherBefore, 1.0f),
              "a row that is NOT hovered changed colour too (%.0f,%.0f,%.0f -> %.0f,%.0f,%.0f): the hover "
              "fill is not confined to the hovered row", double(otherBefore.x), double(otherBefore.y),
              double(otherBefore.z), double(otherAfter.x), double(otherAfter.y), double(otherAfter.z));
}

HARNESS_MAIN()
