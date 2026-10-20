// tests/theme_probe.cpp - owned by Verity / QA. Item 156 (theme tokens).
//
// NOT a test by itself: a measuring tool. It is compiled TWICE by tests/CMakeLists.txt, from the
// real editor sources, into two executables:
//   theme_probe_default  - against the real src/core/Theme.h
//   theme_probe_swapped  - against a GENERATED Theme.h whose token VALUES are different
// and tests/test_editor_theme_swap.cpp runs both and compares what they DREW.
//
// WHY THIS SHAPE: kDefaultTheme is an `inline constexpr` global and applyTheme()/drawArea() are
// file-local to Editor.cpp, so a token cannot be swapped at run time without editing src/. This
// file #includes Editor.cpp, which gives it the REAL applyTheme() and the REAL drawArea(); the
// only difference between the two builds is the token values compiled in.
//
// WHAT IT MEASURES: it draws real widgets and a real area header into a headless ImGui context,
// then software-rasterises the actual ImDrawData (vertex colours, clip rects, alpha blending) at
// chosen pixels over a known backdrop. That is "what is drawn", not "what the style struct says".
// Output: one line per measurement, `key v0 v1 ...`.
#include "editor/Editor.cpp"   // the real applyTheme() and drawArea(); deliberately included

#include <cstdio>
#include <string>
#include <vector>

using namespace ember;
using namespace ember::editor;

namespace {

constexpr int kW = 1280, kH = 720;
constexpr float kBackdrop[3] = {200.0f, 100.0f, 50.0f};   // what shows through any transparent pixel

struct Px { float r, g, b; };

// Sample points are offset by this so they never sit exactly on a triangle's diagonal/edge: the centre of a
// rectangle lies ON the diagonal its two triangles share, and a translucent fill would be counted twice.
constexpr ImVec2 kOff{0.37f, 0.21f};

// A FILLED shape samples one texel: all three vertices share a uv. A glyph quad's uvs differ across its
// corners. (In this ImGui the rounded fills sample a flat white block of the atlas, not the single
// "white pixel", so comparing against GetFontTexUvWhitePixel() would silently skip every rounded
// widget - the first version of this probe did exactly that and reported the region colour for the
// buttons. `flat` is the right test.)
bool flat(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c) {
    return std::fabs(a.uv.x - b.uv.x) < 1e-6f && std::fabs(a.uv.y - b.uv.y) < 1e-6f &&
           std::fabs(a.uv.x - c.uv.x) < 1e-6f && std::fabs(a.uv.y - c.uv.y) < 1e-6f;
}

// Software rasteriser over ImDrawData: flat-uv triangles only, i.e. every filled rect / rounded rect /
// line - not glyphs. Standard source-over in byte space, like the GL backend.
Px sample(const ImDrawData* dd, float px, float py) {
    Px out{kBackdrop[0], kBackdrop[1], kBackdrop[2]};
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            if (px < cmd.ClipRect.x || py < cmd.ClipRect.y || px >= cmd.ClipRect.z || py >= cmd.ClipRect.w) continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& a = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 0]];
                const ImDrawVert& b = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 1]];
                const ImDrawVert& c = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 2]];
                if (!flat(a, b, c)) continue;
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
                out.r = out.r * (1 - al) + ch(0) * al;
                out.g = out.g * (1 - al) + ch(8) * al;
                out.b = out.b * (1 - al) + ch(16) * al;
            }
        }
    }
    return out;
}


// Glyph vertex colours (textured triangles) in draw order, de-duplicated: text colours as drawn.
std::vector<unsigned> glyphColours(const ImDrawData* dd) {
    std::vector<unsigned> out;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert& a = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 0]];
                const ImDrawVert& b = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 1]];
                const ImDrawVert& c = dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + i + 2]];
                if (flat(a, b, c)) continue;           // a glyph triangle: uvs vary
                bool seen = false;
                for (unsigned x : out) seen = seen || x == a.col;
                if (!seen) out.push_back(a.col);
            }
        }
    }
    return out;
}

struct Ctx {
    ImGuiContext* ctx = nullptr;
    Ctx() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(float(kW), float(kH));
        io.DisplayFramebufferScale = ImVec2(1, 1);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        // The REAL theme application, with the REAL default accent (derived from the selection token).
        const Preferences prefs;
        applyTheme(prefs.accent);
    }
    ~Ctx() { ImGui::DestroyContext(ctx); }
};

void report(const char* key, Px p) { std::printf("%s %.2f %.2f %.2f\n", key, double(p.r), double(p.g), double(p.b)); }

// One real area (drawArea) at a fixed place. type = Outliner for the header/region probes, or the
// 3D Viewport host for the transparent-body probe.
void drawRealArea(AreaType type, bool viewportHost, scene::World& world, UndoStack& undo) {
    ImGui::SetNextWindowPos(ImVec2(50, 50));
    ImGui::SetNextWindowSize(ImVec2(600, 300));
    RenderStats stats{};
    TimelineState timeline;
    ViewportSettings viewport{};
    TransformSpace space = TransformSpace::Global;
    SelectMode selectMode = SelectMode::Vertex;
    PropertiesTab tab = PropertiesTab::Object;
    bool snap = false;
    SnapTarget snapTarget = SnapTarget::GridIncrement;
    AreaType areaType = type;
    (void)drawArea("probe-area", areaType, viewportHost, world, undo, stats, timeline, Vec3{0, 0, 0}, viewport, space,
                   InteractionMode::Object, selectMode, tab, snap, snapTarget);
}

} // namespace

int main() {
    scene::World world;
    UndoStack undo;

    // ---- area: header strip + region background --------------------------------------------
    {
        Ctx c;
        for (int f = 0; f < 4; ++f) {
            ImGui::NewFrame();
            drawRealArea(AreaType::Outliner, false, world, undo);
            ImGui::Render();
        }
        const ImDrawData* dd = ImGui::GetDrawData();
        const Px region = sample(dd, 350.5f, 330.5f);        // far below the header: the area's own background
        report("region", region);
        // Header color pixel and extent of the real header fill. Measuring the
        // vertical run at one pixel center was fragile: an AA text/Separator
        // vertex could terminate it before the rectangle's actual lower edge.
        const ImVec4 headerColor = ember::editor::theme::color(
            ember::editor::theme::tokens().headerBackground);
        const ImU32 headerU = ImGui::ColorConvertFloat4ToU32(headerColor);
        float headerTop = 1e9f, headerBottom = -1e9f;
        for (int n = 0; n < dd->CmdListsCount; ++n) {
            const ImDrawList* dl = dd->CmdLists[n];
            for (const ImDrawVert& v : dl->VtxBuffer) {
                if (v.col != headerU || v.pos.x < 40.0f || v.pos.x > 660.0f ||
                    v.pos.y < 50.0f || v.pos.y > 130.0f) continue;
                headerTop = std::fmin(headerTop, v.pos.y);
                headerBottom = std::fmax(headerBottom, v.pos.y);
            }
        }
        report("header", Px{headerColor.x * 255.0f, headerColor.y * 255.0f,
                             headerColor.z * 255.0f});
        std::printf("header_height %.2f\n",
                    headerBottom >= headerTop ? double(headerBottom - headerTop) : 0.0);
    }

    // ---- 3D Viewport host: the body must be the (swappable) viewport-body token --------------
    {
        Ctx c;
        for (int f = 0; f < 4; ++f) {
            ImGui::NewFrame();
            drawRealArea(AreaType::Viewport, true, world, undo);
            ImGui::Render();
        }
        report("viewport_body", sample(ImGui::GetDrawData(), 350.5f, 330.5f));
    }

    // ---- widgets: selected row, button (rest / hover / active), field + its height ------------
    for (int mode = 0; mode < 3; ++mode) {   // 0 rest, 1 mouse over the button, 2 mouse held on it
        Ctx c;
        ImVec2 btnMin{}, btnMax{}, rowMin{}, rowMax{}, fldMin{}, fldMax{};
        float value = 1.5f;
        ImGuiIO& io = ImGui::GetIO();
        for (int f = 0; f < 6; ++f) {
            if (mode > 0 && f >= 2) {
                io.AddMousePosEvent((btnMin.x + btnMax.x) * 0.5f, (btnMin.y + btnMax.y) * 0.5f);
                if (mode == 2 && f >= 3) io.AddMouseButtonEvent(0, true);
            } else {
                io.AddMousePosEvent(900.0f, 650.0f);
            }
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(700, 50));
            ImGui::SetNextWindowSize(ImVec2(420, 400));
            ImGui::Begin("probe-widgets", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
            // ITEM 204 CORRECTION: the global ImGuiCol_Header is no longer the selection fill - the editor
            // opts in per call site (theme::pushSelectedObjectColors for a selected object row,
            // pushActiveItemColors for an active control). A SELECTED ROW is selection-coloured, so this
            // probe pushes the selection TOKEN locally. That is deliberate: the pixel must follow the token
            // through a swap, instead of depending on a global mapping that no longer carries that meaning.
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ember::editor::theme::color(ember::editor::theme::tokens().selection,
                                                              ember::editor::theme::tokens().selectionFillAlpha));
            ImGui::Selectable("##sel", true, 0, ImVec2(300, 30));
            ImGui::PopStyleColor();
            rowMin = ImGui::GetItemRectMin(); rowMax = ImGui::GetItemRectMax();
            ImGui::Button("##btn", ImVec2(300, 40));
            btnMin = ImGui::GetItemRectMin(); btnMax = ImGui::GetItemRectMax();
            ImGui::SetNextItemWidth(300.0f);
            ImGui::DragFloat("##fld", &value);
            fldMin = ImGui::GetItemRectMin(); fldMax = ImGui::GetItemRectMax();
            ImGui::End();
            ImGui::Render();
        }
        const ImDrawData* dd = ImGui::GetDrawData();
        if (mode == 0) {
            report("selected_row", sample(dd, rowMax.x - 10.0f + kOff.x, (rowMin.y + rowMax.y) * 0.5f + kOff.y));
            report("button", sample(dd, btnMin.x + 150.0f + kOff.x, (btnMin.y + btnMax.y) * 0.5f + kOff.y));
            report("field", sample(dd, fldMin.x + 4.0f + kOff.x, (fldMin.y + fldMax.y) * 0.5f + kOff.y));
            std::printf("field_height %.2f\n", double(fldMax.y - fldMin.y));
        } else if (mode == 1) {
            report("button_hover", sample(dd, btnMin.x + 150.0f + kOff.x, (btnMin.y + btnMax.y) * 0.5f + kOff.y));
        } else {
            report("button_active", sample(dd, btnMin.x + 150.0f + kOff.x, (btnMin.y + btnMax.y) * 0.5f + kOff.y));
        }
    }

    // ---- Regression control: reapplying the theme after the first frame must be stable ------------
    // Editor::init() applies the theme before NewFrame; the preferences path can reapply it later.
    // Both must derive FramePadding from the same base font size and produce equal field heights.
    {
        Ctx c;                                  // applyTheme() already ran before any frame (as init does)
        ImGui::NewFrame();
        ImGui::EndFrame();
        const Preferences prefs;
        applyTheme(prefs.accent);               // ...and again after a frame has run
        ImVec2 fmin{}, fmax{};
        float value = 1.5f;
        for (int f = 0; f < 3; ++f) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(700, 50));
            ImGui::SetNextWindowSize(ImVec2(420, 400));
            ImGui::Begin("probe-widgets2", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetNextItemWidth(300.0f);
            ImGui::DragFloat("##fld", &value);
            fmin = ImGui::GetItemRectMin(); fmax = ImGui::GetItemRectMax();
            ImGui::End();
            ImGui::Render();
        }
        std::printf("field_height_after_first_frame %.2f\n", double(fmax.y - fmin.y));
    }

    // ---- text colours as actually drawn (glyph vertex colours) -------------------------------
    {
        Ctx c;
        for (int f = 0; f < 3; ++f) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(50, 400));
            ImGui::SetNextWindowSize(ImVec2(300, 100));
            ImGui::Begin("probe-text", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("MMMMMMMM");
            ImGui::TextDisabled("MMMMMMMM");
            ImGui::End();
            ImGui::Render();
        }
        const std::vector<unsigned> cols = glyphColours(ImGui::GetDrawData());
        const char* names[2] = {"text_primary", "text_disabled"};
        for (int i = 0; i < 2; ++i) {
            if (i < int(cols.size())) {
                const unsigned v = cols[std::size_t(i)];
                std::printf("%s %u %u %u %u\n", names[i], v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24);
            } else {
                std::printf("%s 0 0 0 0\n", names[i]);
            }
        }
    }
    return 0;
}
