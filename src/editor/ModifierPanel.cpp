#include "editor/Panels.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "editor/Commands.h"
#include "editor/ThemeStyle.h"
#include "core/Theme.h"

namespace ember::editor {

namespace {

float readParam(const ember::Modifier& modifier, const char* key) {
    return std::visit([&](const auto& p) -> float {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, ember::SubdivisionParams>) {
            if (std::string(key) == "levels") return static_cast<float>(p.levels);
            if (std::string(key) == "smooth_normals") return p.smoothNormals ? 1.0f : 0.0f;
        } else if constexpr (std::is_same_v<T, ember::MirrorParams>) {
            if (std::string(key) == "axis_x") return p.axisX ? 1.0f : 0.0f;
            if (std::string(key) == "axis_y") return p.axisY ? 1.0f : 0.0f;
            if (std::string(key) == "axis_z") return p.axisZ ? 1.0f : 0.0f;
            if (std::string(key) == "merge_threshold") return p.mergeThreshold;
        } else if constexpr (std::is_same_v<T, ember::ArrayParams>) {
            if (std::string(key) == "count") return static_cast<float>(p.count);
            if (std::string(key) == "offset_x") return p.offsetX;
            if (std::string(key) == "offset_y") return p.offsetY;
            if (std::string(key) == "offset_z") return p.offsetZ;
            if (std::string(key) == "relative") return p.relative ? 1.0f : 0.0f;
        }
        return 0.0f;
    }, modifier.params);
}

void writeParam(ember::Modifier& modifier, const char* key, float value) {
    std::visit([&](auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, ember::SubdivisionParams>) {
            if (std::string(key) == "levels") p.levels = static_cast<int>(value);
            else if (std::string(key) == "smooth_normals") p.smoothNormals = value >= 0.5f;
        } else if constexpr (std::is_same_v<T, ember::MirrorParams>) {
            if (std::string(key) == "axis_x") p.axisX = value >= 0.5f;
            else if (std::string(key) == "axis_y") p.axisY = value >= 0.5f;
            else if (std::string(key) == "axis_z") p.axisZ = value >= 0.5f;
            else if (std::string(key) == "merge_threshold") p.mergeThreshold = value;
        } else if constexpr (std::is_same_v<T, ember::ArrayParams>) {
            if (std::string(key) == "count") p.count = static_cast<int>(value);
            else if (std::string(key) == "offset_x") p.offsetX = value;
            else if (std::string(key) == "offset_y") p.offsetY = value;
            else if (std::string(key) == "offset_z") p.offsetZ = value;
            else if (std::string(key) == "relative") p.relative = value >= 0.5f;
        }
    }, modifier.params);
}

struct ParameterDrag {
    bool active = false;
    bool applied = false;
    ObjectId object;
    u32 index = 0;
    std::string key;
    std::vector<ember::Modifier> before;
};

ParameterDrag gDrag;

void pushStack(scene::World& world, UndoStack& undo, ObjectId object,
               const std::vector<ember::Modifier>& before,
               const std::vector<ember::Modifier>& after, bool continuation = false) {
    (void)before;
    undo.push(std::make_unique<ModifierStackCommand>(world, object, before, after, continuation));
}

bool drawParameter(scene::World& world, UndoStack& undo, ObjectId object, u32 index,
                   const ember::ParamDesc& desc, const ember::Modifier& shown) {
    float value = readParam(shown, desc.key);
    bool changed = false;
    switch (desc.type) {
        case ember::ParamType::Bool: {
            bool checked = value >= 0.5f;
            changed = ImGui::Checkbox(desc.label, &checked);
            value = checked ? 1.0f : 0.0f;
            break;
        }
        case ember::ParamType::Int: {
            int iv = static_cast<int>(value);
            changed = ImGui::DragInt(desc.label, &iv, std::max(desc.step, 1.0f),
                                     static_cast<int>(desc.minimum), static_cast<int>(desc.maximum), "%d");
            value = static_cast<float>(iv);
            break;
        }
        case ember::ParamType::Float:
            changed = ImGui::DragFloat(desc.label, &value, desc.step, desc.minimum, desc.maximum, "%.4g");
            break;
    }

    const bool activated = ImGui::IsItemActivated();
    const bool active = ImGui::IsItemActive();
    const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
    if (activated) {
        gDrag.active = true;
        gDrag.applied = false;
        gDrag.object = object;
        gDrag.index = index;
        gDrag.key = desc.key;
        gDrag.before = world.modifiers(object);
    }

    if (changed) {
        const std::vector<ember::Modifier> current = world.modifiers(object);
        if (index < current.size()) {
            std::vector<ember::Modifier> after = current;
            writeParam(after[index], desc.key, value);
            const bool sameGesture = gDrag.active && gDrag.object == object &&
                                     gDrag.index == index && gDrag.key == desc.key;
            pushStack(world, undo, object, sameGesture ? gDrag.before : current,
                      after, sameGesture && gDrag.applied);
            if (sameGesture) gDrag.applied = true;
        }
    }
    if (deactivated || (!active && !gDrag.applied && gDrag.active &&
                        gDrag.object == object && gDrag.index == index && gDrag.key == desc.key)) {
        gDrag = ParameterDrag{};
    }
    return changed;
}

} // namespace

void drawModifiers(scene::World& world, UndoStack& undo, ObjectId object) {
    if (!world.get(object)) return;

    if (ImGui::Button((std::string("Add ") + ember::UiVocabulary::Modifier).c_str())) ImGui::OpenPopup("##addmodifier");
    if (ImGui::BeginPopup("##addmodifier")) {
        const ember::ModifierKind kinds[] = {ember::ModifierKind::Subdivision,
                                             ember::ModifierKind::Mirror,
                                             ember::ModifierKind::Array};
        for (const ember::ModifierKind kind : kinds) {
            if (ImGui::MenuItem(ember::modifierDesc(kind).name)) {
                const std::vector<ember::Modifier> before = world.modifiers(object);
                std::vector<ember::Modifier> after = before;
                ember::Modifier added;
                switch (kind) {
                    case ember::ModifierKind::Subdivision: added.params = ember::SubdivisionParams{}; break;
                    case ember::ModifierKind::Mirror:      added.params = ember::MirrorParams{}; break;
                    case ember::ModifierKind::Array:       added.params = ember::ArrayParams{}; break;
                }
                after.push_back(std::move(added));
                pushStack(world, undo, object, before, after);
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
        }
        ImGui::EndPopup();
    }

    const std::vector<ember::Modifier> stack = world.modifiers(object);
    if (stack.empty()) {
        ImGui::TextDisabled("No %ss", ember::UiVocabulary::Modifier);
        return;
    }

    for (u32 i = 0; i < stack.size(); ++i) {
        const ember::Modifier& modifier = stack[i];
        const ember::ModifierDesc desc = ember::modifierDesc(modifier.kind());
        ImGui::PushID(static_cast<int>(i));

        bool enabled = modifier.enabled;
        if (ImGui::Checkbox("##enabled", &enabled) && enabled != modifier.enabled) {
            std::vector<ember::Modifier> before = stack;
            std::vector<ember::Modifier> after = stack;
            after[i].enabled = enabled;
            pushStack(world, undo, object, before, after);
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        const u32 failed = world.evaluatedFailureIndex(object);
        const bool evalFailed = failed == i;
        if (evalFailed) ImGui::PushStyleColor(ImGuiCol_Text, theme::color(theme::modifierError));
        const bool expanded = ImGui::CollapsingHeader(desc.name, ImGuiTreeNodeFlags_DefaultOpen);
        if (evalFailed) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Evaluation failed; this modifier is a no-op.");
        }

        ImGui::SameLine(ImGui::GetContentRegionMax().x - 58.0f);
        if (ImGui::SmallButton("Up") && i > 0) {
            const std::vector<ember::Modifier> before = stack;
            std::vector<ember::Modifier> after = stack;
            std::swap(after[i], after[i - 1]);
            pushStack(world, undo, object, before, after);
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dn") && i + 1 < stack.size()) {
            const std::vector<ember::Modifier> before = stack;
            std::vector<ember::Modifier> after = stack;
            std::swap(after[i], after[i + 1]);
            pushStack(world, undo, object, before, after);
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            std::vector<ember::Modifier> before = stack;
            std::vector<ember::Modifier> after = stack;
            after.erase(after.begin() + i);
            pushStack(world, undo, object, before, after);
            ImGui::PopID();
            return;
        }

        if (expanded) {
            for (u32 p = 0; p < desc.paramCount; ++p) {
                drawParameter(world, undo, object, i, desc.params[p], modifier);
            }
            if (evalFailed) ImGui::TextColored(theme::color(theme::modifierError),
                                               "Evaluation failed (no-op)");
        }
        ImGui::Separator();
        ImGui::PopID();
    }
}

} // namespace ember::editor
