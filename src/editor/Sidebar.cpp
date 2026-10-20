#include "editor/Panels.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "editor/Commands.h"
#include "editor/EditModeTools.h"
#include "editor/EditorLabels.h"
#include "editor/ThemeStyle.h"
#include "core/Theme.h"

namespace ember::editor {

namespace {

constexpr float kRadToDegF = 57.29577951308232f;
constexpr float kDegToRadF = 0.017453292519943295f;

// Numeric fields commit once per edit: on Enter, or when the field is left.
// Keystrokes only change the field's own buffer. Each commit is one
// TransformCommand, and a commit that changes nothing is skipped.
void drawNumericTransform(scene::World& world, UndoStack& undo, ObjectId id, const scene::Object& o) {
    static ObjectId bufferFor;
    static float loc[3];
    static float rot[3];
    static float scl[3];
    static bool locEditing = false;
    static bool rotEditing = false;
    static bool sclEditing = false;

    const Transform current = o.transform;
    if (bufferFor != id) {
        bufferFor = id;
        locEditing = rotEditing = sclEditing = false;
    }
    if (!locEditing) {
        loc[0] = current.position.x;
        loc[1] = current.position.y;
        loc[2] = current.position.z;
    }
    if (!rotEditing) {
        rot[0] = current.rotation.x * kRadToDegF;
        rot[1] = current.rotation.y * kRadToDegF;
        rot[2] = current.rotation.z * kRadToDegF;
    }
    if (!sclEditing) {
        scl[0] = current.scale.x;
        scl[1] = current.scale.y;
        scl[2] = current.scale.z;
    }

    // Applies `change` to the object's transform as it is now, and pushes one
    // command if the result differs.
    auto commit = [&](auto change) {
        const scene::Object* now = world.get(id);
        if (!now) return;
        Transform next = now->transform;
        change(next);
        if (next == now->transform) return;
        undo.push(std::make_unique<TransformCommand>(world, id, now->transform, next));
    };

    const ImGuiInputTextFlags commitOnEnter = ImGuiInputTextFlags_EnterReturnsTrue;

    const bool locEnter = ImGui::InputFloat3("Location", loc, "%.3f", commitOnEnter);
    locEditing = ImGui::IsItemActive();
    if (locEnter || ImGui::IsItemDeactivatedAfterEdit()) {
        commit([&](Transform& t) { t.position = Vec3{loc[0], loc[1], loc[2]}; });
    }

    const bool rotEnter = ImGui::InputFloat3("Rotation", rot, "%.2f", commitOnEnter);
    rotEditing = ImGui::IsItemActive();
    if (rotEnter || ImGui::IsItemDeactivatedAfterEdit()) {
        commit([&](Transform& t) {
            t.rotation = Vec3{rot[0] * kDegToRadF, rot[1] * kDegToRadF, rot[2] * kDegToRadF};
        });
    }

    const bool sclEnter = ImGui::InputFloat3("Scale", scl, "%.3f", commitOnEnter);
    sclEditing = ImGui::IsItemActive();
    if (sclEnter || ImGui::IsItemDeactivatedAfterEdit()) {
        commit([&](Transform& t) { t.scale = Vec3{scl[0], scl[1], scl[2]}; });
    }
}

// Blender's Dimensions: the mesh's local extents times the object's scale.
void drawDimensions(const scene::Object& o) {
    if (!o.hasMesh || o.mesh.positions.size() < 3) {
        ImGui::TextDisabled("No geometry");
        return;
    }
    constexpr float inf = std::numeric_limits<float>::infinity();
    Vec3 lo{inf, inf, inf};
    Vec3 hi{-inf, -inf, -inf};
    for (std::size_t i = 0; i + 2 < o.mesh.positions.size(); i += 3) {
        const float x = o.mesh.positions[i];
        const float y = o.mesh.positions[i + 1];
        const float z = o.mesh.positions[i + 2];
        lo = Vec3{std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
        hi = Vec3{std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
    }
    const Vec3 s = o.transform.scale;
    ImGui::Text("X  %.3f", static_cast<double>((hi.x - lo.x) * s.x));
    ImGui::Text("Y  %.3f", static_cast<double>((hi.y - lo.y) * s.y));
    ImGui::Text("Z  %.3f", static_cast<double>((hi.z - lo.z) * s.z));
}

} // namespace

namespace {

// Item: the active object's numbers.
void drawItemTab(scene::World& world, UndoStack& undo) {
    const std::vector<ObjectId>& selection = world.selection();
    const ObjectId id = selection.empty() ? ObjectId{} : selection.back();
    const scene::Object* o = world.get(id);
    if (!o) {
        ImGui::TextDisabled("No active object");
        return;
    }

    ImGui::TextUnformatted(o->name.c_str());
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawNumericTransform(world, undo, id, *o);
    }
    if (ImGui::CollapsingHeader("Dimensions", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawDimensions(*o);
    }
}

// Tool: how the transform tools act.
void drawToolTab(scene::World& world, UndoStack& undo, ToolState& tools,
                 TransformSpace& space, ember::ViewportSettings& viewport,
                 bool& snapEnabled, InteractionMode interaction, SelectMode& selectMode) {
    static constexpr Tool toolValues[] = {Tool::Select, Tool::Move, Tool::Rotate, Tool::Scale};
    static constexpr const char* toolNames[] = {"Select  Q", "Move  W", "Rotate  R", "Scale  S"};
    static constexpr const char* orientationNames[] = {"Global", "Local"};
    static constexpr const char* pivotNames[] = {"Median Point", "3D Cursor", "Individual Origins",
                                                  "Bounding Box Center", "Active Element"};
    static constexpr ember::PivotMode pivotValues[] = {ember::PivotMode::MedianPoint,
        ember::PivotMode::Cursor3D, ember::PivotMode::IndividualOrigins,
        ember::PivotMode::BoundingBoxCenter, ember::PivotMode::ActiveElement};
    if (ImGui::CollapsingHeader("Active Tool", ImGuiTreeNodeFlags_DefaultOpen)) {
        theme::pushActiveItemColors();
        for (std::size_t i = 0; i < std::size(toolValues); ++i) {
            if (ImGui::Selectable(toolNames[i], tools.active == toolValues[i]))
                tools.active = toolValues[i];
        }
        theme::popHeaderColors();
    }

    if (ImGui::CollapsingHeader("Tool Options", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (tools.active == Tool::Select) {
            if (interaction == InteractionMode::Edit) {
                static constexpr const char* selectNames[] = {"Vertex", "Edge", "Face"};
                int selection = static_cast<int>(selectMode);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("Selection Mode", &selection, selectNames, 3))
                    selectMode = static_cast<SelectMode>(selection);
            } else {
                ImGui::TextWrapped("Click to select; drag on empty space to box-select.");
            }
        } else {
            ImGui::Checkbox("Show Gizmo", &viewport.showGizmo);
            switch (tools.active) {
                case Tool::Move:
                    ImGui::TextWrapped("Drag a handle to move. G starts a free move.");
                    break;
                case Tool::Rotate:
                    ImGui::TextWrapped("Drag a handle to rotate. R starts a free rotation.");
                    break;
                case Tool::Scale:
                    ImGui::TextWrapped("Drag a handle to scale. S starts a free scale.");
                    break;
                case Tool::Select:
                    break;
            }
        }
    }

    if (interaction == InteractionMode::Edit) {
        const ObjectId active = world.selection().size() == 1 ? world.selection().back() : ObjectId{};
        drawEditMeshTools(world, undo, active, selectMode);
    }

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        int orientation = space == TransformSpace::Global ? 0 : 1;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("Orientation", &orientation, orientationNames, 2))
            space = orientation == 0 ? TransformSpace::Global : TransformSpace::Local;

        int pivot = 0;
        for (int i = 0; i < static_cast<int>(std::size(pivotValues)); ++i)
            if (viewport.pivot == pivotValues[i]) pivot = i;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("Pivot Point", &pivot, pivotNames, static_cast<int>(std::size(pivotNames))))
            viewport.pivot = pivotValues[pivot];

        ImGui::Checkbox("Snap", &snapEnabled);
        ImGui::TextDisabled("Target: Increment");
        ImGui::TextWrapped("Increment snapping is available. Ctrl toggles it during a transform.");
    }
}

// View: what the viewport shows.
void drawViewTab(scene::World& world, UndoStack& undo, render::OrbitCamera& camera,
                 ember::ViewportSettings& viewport, InteractionMode interaction) {
    if (ImGui::CollapsingHeader("View Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonWidth = std::max(1.0f, (ImGui::GetContentRegionAvail().x - 2.0f * spacing) / 3.0f);
        if (ImGui::Button("Front", ImVec2(buttonWidth, 0.0f))) camera.setView(render::StandardView::Front);
        ImGui::SameLine();
        if (ImGui::Button("Right", ImVec2(buttonWidth, 0.0f))) camera.setView(render::StandardView::Right);
        ImGui::SameLine();
        if (ImGui::Button("Top", ImVec2(buttonWidth, 0.0f))) camera.setView(render::StandardView::Top);

        bool orthographic = camera.isOrthographic();
        if (ImGui::Checkbox("Orthographic", &orthographic)) camera.setOrthographic(orthographic);
        float orthoHeight = camera.orthoHeight();
        if (orthographic && ImGui::DragFloat("View Scale", &orthoHeight, 0.05f, 0.01f, 100000.0f,
                                              "%.3f", ImGuiSliderFlags_AlwaysClamp))
            camera.setOrthoHeight(orthoHeight);

        const std::optional<scene::WorldBounds> selectionBounds = world.selectionBounds();
        ImGui::BeginDisabled(!selectionBounds.has_value());
        if (ImGui::Button("Frame Selected", ImVec2(-1.0f, 0.0f)))
            camera.frameSphere(selectionBounds->center, selectionBounds->radius);
        ImGui::EndDisabled();
        const std::optional<scene::WorldBounds> allBounds = world.bounds();
        ImGui::BeginDisabled(!allBounds.has_value());
        if (ImGui::Button("Frame All", ImVec2(-1.0f, 0.0f)))
            camera.frameSphere(allBounds->center, allBounds->radius);
        ImGui::EndDisabled();

        float fovDegrees = camera.fovY() * kRadToDegF;
        if (ImGui::SliderFloat("Field of View", &fovDegrees, 1.0f, 170.0f, "%.1f deg"))
            camera.setFOV(fovDegrees * kDegToRadF);

        float clipStart = camera.nearPlane();
        const float maxClipStart = std::max(0.001f, camera.farPlane() - 0.001f);
        if (ImGui::DragFloat("Clip Start", &clipStart, 0.001f, 0.001f, maxClipStart, "%.3f",
                             ImGuiSliderFlags_AlwaysClamp))
            camera.setNearFar(clipStart, camera.farPlane());

        float clipEnd = camera.farPlane();
        if (ImGui::DragFloat("Clip End", &clipEnd, std::max(0.01f, clipEnd * 0.001f),
                             camera.nearPlane() + 0.001f, 1000000.0f, "%.3f",
                             ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
            camera.setNearFar(camera.nearPlane(), clipEnd);
    }

    if (ImGui::CollapsingHeader(ember::UiVocabulary::Collection, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (world.collections().empty()) {
            ImGui::TextDisabled("No collections in this scene.");
        } else {
            for (const scene::Collection& collection : world.collections()) {
                ImGui::PushID(collection.name.c_str());
                bool visible = collection.visible;
                if (ImGui::Checkbox("##visible", &visible) && visible != collection.visible) {
                    undo.push(std::make_unique<CollectionVisibilityCommand>(
                        world, collection.id, collection.visible, visible));
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(collection.name.c_str());
                ImGui::PopID();
            }
        }
    }

    if (ImGui::CollapsingHeader("Gizmos", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show Gizmos", &viewport.showGizmo);
        ImGui::Checkbox("Show Origins", &viewport.showOrigins);
    }

    if (ImGui::CollapsingHeader("Viewport Overlays", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Grid", &viewport.showGrid);
        ImGui::Checkbox("World Axes", &viewport.showAxes);
        ImGui::Checkbox("Selection Outline", &viewport.showSelectionOutline);
        if (interaction == InteractionMode::Edit) {
            ImGui::Separator();
            ImGui::Checkbox("Edit Vertices", &viewport.showEditVertices);
            ImGui::Checkbox("Edit Edges", &viewport.showEditEdges);
            ImGui::Checkbox("Edit Faces", &viewport.showEditFaces);
            ImGui::Checkbox("X-Ray", &viewport.xray);
        } else {
            ImGui::TextDisabled("Edit overlays: available in Edit Mode");
        }
    }
}

} // namespace

float sidebarRegionWidth() {
    return theme::metric(272.0f);
}

void drawSidebar(scene::World& world, UndoStack& undo, bool open, ImVec4 region,
                 SidebarTab& tab, ember::ViewportSettings& viewport, TransformSpace& space,
                 ToolState& tools, bool& snapEnabled, SnapTarget& snapTarget,
                 InteractionMode interaction, SelectMode& selectMode,
                 render::OrbitCamera& camera) {
    if (!open) return;

    ImGui::SetCursorScreenPos(ImVec2(region.x, region.y));
    const float tabWidth = theme::metric(48.0f);
    const float gap = theme::metric(theme::tokens().itemSpacing.x);
    const ImGuiWindowFlags regionFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::BeginChild("##sidebar", ImVec2(region.z, region.w), false, regionFlags)) {
        ImGui::EndChild();
        return;
    }

    const float contentWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x - tabWidth - gap);
    const float contentPadding = theme::metric(theme::tokens().itemSpacing.x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(contentPadding, contentPadding));
    const bool contentVisible = ImGui::BeginChild("##sidebarcontent", ImVec2(contentWidth, 0.0f), false,
                                                   ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    (void)snapTarget; // Snap To is fixed to Increment until the other targets have working semantics.
    if (contentVisible) {
        switch (tab) {
            case SidebarTab::Item: drawItemTab(world, undo); break;
            case SidebarTab::Tool:
                drawToolTab(world, undo, tools, space, viewport, snapEnabled, interaction, selectMode);
                break;
            case SidebarTab::View: drawViewTab(world, undo, camera, viewport, interaction); break;
        }
    }
    ImGui::EndChild();

    // The tabs run down the right edge of the region.
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginGroup();
    const float tabHeight = theme::metric(theme::tokens().widgetHeight * 1.5f);
    theme::pushActiveItemColors();
    if (ImGui::Selectable(labels::Item, tab == SidebarTab::Item, 0, ImVec2(tabWidth, tabHeight))) tab = SidebarTab::Item;
    if (ImGui::Selectable(labels::Tool, tab == SidebarTab::Tool, 0, ImVec2(tabWidth, tabHeight))) tab = SidebarTab::Tool;
    if (ImGui::Selectable(labels::View, tab == SidebarTab::View, 0, ImVec2(tabWidth, tabHeight))) tab = SidebarTab::View;
    theme::popHeaderColors();
    ImGui::EndGroup();

    ImGui::EndChild();
}

void drawSidebar(scene::World& world, UndoStack& undo, bool open, ImVec4 region,
                 SidebarTab& tab, ember::ViewportSettings& viewport, TransformSpace& space) {
    ToolState tools;
    bool snapEnabled = false;
    SnapTarget snapTarget = SnapTarget::GridIncrement;
    SelectMode selectMode = SelectMode::Vertex;
    static render::OrbitCamera camera;
    drawSidebar(world, undo, open, region, tab, viewport, space, tools, snapEnabled,
                snapTarget, InteractionMode::Object, selectMode, camera);
}

} // namespace ember::editor
