// Blender's Ctrl+Tab mode-switch pie, restricted to Ember3D's supported modes.
#include "harness.h"

#include "core/Types.h"
#include "editor/Panels.h"

#include <imgui.h>

using ember::InteractionMode;
using ember::editor::ModePieState;

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

constexpr float kCx = 640.0f;
constexpr float kCy = 360.0f;

struct Rig {
    ImGuiContext* ctx = nullptr;
    InteractionMode mode = InteractionMode::Object;
    ModePieState pie{};
    bool canEdit = true;
    int lastDrawVertices = 0;
    int lastDrawCommands = 0;

    Rig() {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Rig() { ImGui::DestroyContext(ctx); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    bool frame(float x, float y, bool ctrl = false, int tab = -1,
               ImGuiKey key = ImGuiKey_None, bool keyDown = false,
               int left = -1, int right = -1, bool textInput = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        if (tab >= 0) io.AddKeyEvent(ImGuiKey_Tab, tab != 0);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, keyDown);
        if (left >= 0) io.AddMouseButtonEvent(ImGuiMouseButton_Left, left != 0);
        if (right >= 0) io.AddMouseButtonEvent(ImGuiMouseButton_Right, right != 0);
        ImGui::NewFrame();
        io.WantTextInput = textInput;
        const bool owns = ember::editor::handleModePie(mode, canEdit, pie);
        ImGui::Render();
        const ImDrawData* drawData = ImGui::GetDrawData();
        lastDrawVertices = 0;
        lastDrawCommands = 0;
        if (drawData) {
            for (int i = 0; i < drawData->CmdListsCount; ++i) {
                lastDrawVertices += drawData->CmdLists[i]->VtxBuffer.Size;
                lastDrawCommands += drawData->CmdLists[i]->CmdBuffer.Size;
            }
        }
        return owns;
    }

    bool openByCtrlTab() {
        return frame(kCx, kCy, true, 1);
    }
    void tapCtrlTab() {
        openByCtrlTab();
        frame(kCx, kCy, true, 0);
    }
};

} // namespace

TEST(mode_pie_ctrl_tab_is_contextual_and_owns_input) {
    Rig r;
    r.canEdit = false;
    CHECK_MSG(!r.openByCtrlTab(), "Ctrl+Tab opened a mode pie when Edit Mode is unavailable");
    CHECK_MSG(!r.pie.open, "unavailable mode pie should not open");

    r.frame(kCx, kCy, false, 0);  // release the ignored chord before retrying
    r.canEdit = true;
    CHECK_MSG(r.openByCtrlTab(), "Ctrl+Tab did not open the mode pie for an editable mesh");
    CHECK_MSG(r.pie.open && r.pie.openedByShortcut, "the mode pie did not retain its held shortcut state");
    CHECK_MSG(r.pie.center.x == kCx && r.pie.center.y == kCy,
              "mode pie center was not captured at the cursor");
    CHECK_MSG(r.lastDrawVertices > 100 && r.lastDrawCommands > 0,
              "the open mode pie did not submit its visible sectors and labels to ImDrawData");
    CHECK_MSG(r.mode == InteractionMode::Object, "opening the mode pie changed mode before confirmation");
}

TEST(mode_pie_mouse_hold_release_and_tap_confirm_supported_modes) {
    {
        Rig r;
        CHECK_MSG(r.openByCtrlTab(), "could not open the mode pie");
        CHECK_MSG(r.frame(kCx + 55.0f, kCy, true, -1), "mode pie stopped owning input while held");
        CHECK_MSG(r.pie.hoveredSlice == 1 && r.mode == InteractionMode::Object,
                  "right slice must preview Edit Mode without applying it");
        r.frame(kCx + 55.0f, kCy, true, 0);
        CHECK_MSG(!r.pie.open && r.mode == InteractionMode::Edit,
                  "releasing Ctrl+Tab over Edit Mode did not confirm it");
    }
    {
        Rig r;
        r.tapCtrlTab();
        CHECK_MSG(r.pie.open && !r.pie.openedByShortcut,
                  "a tap on Ctrl+Tab should leave the pie open for selection");
        r.frame(kCx - 55.0f, kCy);
        CHECK_MSG(r.pie.hoveredSlice == 0, "left slice did not select Object Mode");
        r.frame(kCx - 55.0f, kCy, false, -1, ImGuiKey_None, false, 1);
        CHECK_MSG(!r.pie.open && r.mode == InteractionMode::Object,
                  "clicking Object Mode did not confirm the pie choice");
    }
}

TEST(mode_pie_keyboard_and_cancel_paths) {
    {
        Rig r;
        r.mode = InteractionMode::Object;
        r.tapCtrlTab();
        r.frame(kCx, kCy, false, -1, ImGuiKey_RightArrow, true);
        r.frame(kCx, kCy, false, -1, ImGuiKey_RightArrow, false);
        CHECK_MSG(r.pie.keyboardSlice == 1, "Right arrow did not select Edit Mode");
        r.frame(kCx, kCy, false, -1, ImGuiKey_Enter, true);
        CHECK_MSG(!r.pie.open && r.mode == InteractionMode::Edit,
                  "Enter did not confirm the keyboard-selected Edit Mode");
    }
    for (int cancel = 0; cancel < 2; ++cancel) {
        Rig r;
        r.mode = InteractionMode::Edit;
        r.tapCtrlTab();
        r.frame(kCx - 55.0f, kCy);
        if (cancel == 0)
            r.frame(kCx - 55.0f, kCy, false, -1, ImGuiKey_Escape, true);
        else
            r.frame(kCx - 55.0f, kCy, false, -1, ImGuiKey_None, false, -1, 1);
        CHECK_MSG(!r.pie.open && r.mode == InteractionMode::Edit,
                  "cancel path %d changed mode or left the pie open", cancel);
    }
}

TEST(mode_pie_does_not_open_from_text_input_or_without_ctrl) {
    Rig r;
    CHECK_MSG(!r.frame(kCx, kCy, false, 1), "plain Tab was intercepted as Ctrl+Tab");
    CHECK_MSG(!r.frame(kCx, kCy, true, 1, ImGuiKey_None, false, -1, -1, true),
              "Ctrl+Tab opened the mode pie while text input owned the keyboard");
    CHECK_MSG(!r.pie.open, "guarded shortcut unexpectedly left the pie open");
}

HARNESS_MAIN()
