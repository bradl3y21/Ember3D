#include "editor/Preferences.h"

#include <imgui.h>

#include "core/Theme.h"
#include "editor/Panels.h"
#include "editor/ThemeStyle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace ember::editor {

namespace {

std::filesystem::path prefsFile() {
    const char* appData = std::getenv("APPDATA");
    const std::filesystem::path dir =
        appData != nullptr ? std::filesystem::path(appData) / "Ember3D" : std::filesystem::path(".");
    return dir / "preferences.ini";
}

float clampGizmoSize(float v) { return std::clamp(v, 40.0f, 200.0f); }

} // namespace

std::string preferencesPath() { return prefsFile().string(); }

Preferences loadPreferences() {
    Preferences prefs;
    std::ifstream in(prefsFile());
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        const float number = std::strtof(value.c_str(), nullptr);
        if (key == "accentR") prefs.accent[0] = std::clamp(number, 0.0f, 1.0f);
        else if (key == "accentG") prefs.accent[1] = std::clamp(number, 0.0f, 1.0f);
        else if (key == "accentB") prefs.accent[2] = std::clamp(number, 0.0f, 1.0f);
        else if (key == "invertOrbit") prefs.invertOrbit = value == "1";
        else if (key == "emulateNumpad") prefs.emulateNumpad = value == "1";
        else if (key == "gizmoSize") prefs.gizmoSize = clampGizmoSize(number);
    }
    return prefs;
}

bool savePreferences(const Preferences& prefs) {
    const std::filesystem::path file = prefsFile();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "accentR=%.4f\naccentG=%.4f\naccentB=%.4f\n",
                  static_cast<double>(prefs.accent[0]), static_cast<double>(prefs.accent[1]),
                  static_cast<double>(prefs.accent[2]));
    out << buffer;
    out << "invertOrbit=" << (prefs.invertOrbit ? 1 : 0) << "\n";
    out << "emulateNumpad=" << (prefs.emulateNumpad ? 1 : 0) << "\n";
    std::snprintf(buffer, sizeof buffer, "gizmoSize=%.1f\n", static_cast<double>(prefs.gizmoSize));
    out << buffer;
    return static_cast<bool>(out);
}

void drawPreferences(Preferences& prefs, bool& open, bool& changed) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(theme::metric(380.0f), 0.0f), ImGuiCond_Appearing);
    if (!beginDialog("Preferences", &open)) {
        endDialog();
        return;
    }

    ImGui::TextWrapped("Stored on this computer only, in %s. "
                       "They are not synced to other machines or user accounts.",
                       preferencesPath().c_str());
    ImGui::Separator();

    if (ImGui::ColorEdit3("Theme accent", prefs.accent)) changed = true;

    if (ImGui::Checkbox("Invert orbit direction", &prefs.invertOrbit)) changed = true;
    if (ImGui::Checkbox("Emulate Numpad", &prefs.emulateNumpad)) changed = true;


    if (ImGui::SliderFloat("Gizmo size (px)", &prefs.gizmoSize, 40.0f, 200.0f, "%.0f")) {
        prefs.gizmoSize = clampGizmoSize(prefs.gizmoSize);
        changed = true;
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Keymap", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BulletText("Q / W / E / R   Select / Move / Rotate / Scale tool");
        ImGui::BulletText("Tab   toggle Object / Edit Mode");
        ImGui::BulletText("Ctrl+Tab   mode-switch pie (Object / Edit)");
        ImGui::BulletText("1 / 2 / 3   Vertex / Edge / Face in Edit Mode (Emulate Numpad off)");
        ImGui::BulletText("E / X / M   Extrude Face / Delete Selection / Merge at Center in Edit Mode");
        ImGui::BulletText("Numpad 1 / 3 / 7   Front / Right / Top views");
        ImGui::BulletText("Ctrl + Numpad 1 / 3 / 7   Back / Left / Bottom views");
        ImGui::BulletText("Numpad 9   opposite axis view");
        ImGui::BulletText("Numpad 5   toggle perspective / orthographic");
        ImGui::BulletText("Numpad 0   look through active Camera");
        ImGui::BulletText("Numpad .   frame selection; Home   frame all");
        ImGui::BulletText("Emulate Numpad uses top-row 0 / 1 / 3 / 5 / 7 / 9 for views");
        ImGui::BulletText("T               toggle the %s", ember::UiVocabulary::Toolbar);
        ImGui::BulletText("N               toggle the %s", ember::UiVocabulary::Sidebar);
        ImGui::BulletText("A / Alt+A       select all / deselect all");
        ImGui::BulletText("X or Delete     delete the selection");
        ImGui::BulletText("Shift+D         duplicate the selection");
        ImGui::BulletText("Ctrl+Z          undo");
        ImGui::BulletText("Ctrl+Y          redo");
        ImGui::BulletText("F3              search palette");
        ImGui::BulletText("Ctrl+,          this window");
        ImGui::BulletText("Left drag       box select (Select tool)");
        ImGui::BulletText("Shift / Ctrl + click   add to selection");
        ImGui::BulletText("Ctrl + drag gizmo      snap");
        ImGui::BulletText("Esc             cancel a gizmo drag");
        ImGui::BulletText("Right-click object   context menu (viewport / %s)",
                          ember::UiVocabulary::Outliner);
    }

    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
        prefs = Preferences{};
        changed = true;
    }

    endDialog();
}

} // namespace ember::editor
