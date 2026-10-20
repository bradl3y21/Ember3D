#include "editor/Panels.h"
#include "editor/EditorLabels.h"

#include <imgui.h>

#include <string>

#include "editor/ThemeStyle.h"

namespace ember::editor {

void drawHistory(UndoStack& undo, bool& open) {
    if (!open) return;

    if (!beginDockedPanel(labels::UndoHistory, &open)) {
        endDockedPanel();
        return;
    }

    ImGui::TextWrapped("Clicking a step runs repeated Undo or Redo until it is reached. "
                       "It is not a direct restore of that state.");
    ImGui::Separator();

    const std::size_t done = undo.doneCount();
    const std::size_t total = done + undo.redoCount();
    std::size_t target = total + 1;   // "no click this frame"

    // Row 0 is the original state. Rows up to `done` are applied; rows after it are undone.
    for (std::size_t p = 0; p <= total; ++p) {
        std::string label;
        if (p == 0) {
            label = "Original";
        } else if (p <= done) {
            label = undo.doneLabelAt(p - 1);
        } else {
            label = undo.redoLabelAt(p - done - 1);
        }

        const bool current = p == done;
        const bool undone = p > done;
        std::string row = (current ? "> " : "   ") + label;
        if (current) row += "   (current)";

        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        theme::pushActiveItemColors();
        if (ImGui::Selectable(row.c_str(), current)) {
            target = p;
        }
        theme::popHeaderColors();
        if (undone) ImGui::PopStyleColor();
    }

    if (target <= total && target != done) undo.jumpTo(target);

    endDockedPanel();
}

} // namespace ember::editor
