#include "editor/Panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "editor/Commands.h"
#include "editor/EditorLabels.h"
#include "editor/ThemeStyle.h"

namespace ember::editor {

namespace {

constexpr const char* kDragType = "EMBER_OBJECT";
constexpr const char* kDemoObjectName = "SmokeCube";

struct RenameState {
    ObjectId id;                 // the row being renamed, or invalid
    bool focus = false;          // grab keyboard focus on the next frame
    bool cancel = false;         // Escape was pressed: discard the edit
    char buffer[128] = {};
};

bool isSelected(const scene::World& world, ObjectId id) {
    for (ObjectId s : world.selection()) {
        if (s == id) return true;
    }
    return false;
}

ObjectId activeObject(const scene::World& world) {
    // Selection order is stable scene UI state; the most recently selected
    // object is the active object, just as it is in Blender's Outliner.
    return world.selection().empty() ? ObjectId{} : world.selection().back();
}

void drawObjectIcon(bool hasMesh, bool isCamera, bool isLight, bool visible,
                    ImVec2 min, ImVec2 max) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x = (min.x + max.x) * 0.5f;
    const float y = (min.y + max.y) * 0.5f;
    const float unit = theme::metric(5.0f);
    const ImU32 color = ImGui::GetColorU32(visible ? ImGuiCol_Text : ImGuiCol_TextDisabled);

    if (isLight) {
        draw->AddCircle(ImVec2(x, y), unit * 0.55f, color, 12, theme::metric(1.2f));
        for (int i = 0; i < 8; ++i) {
            const float angle = float(i) * 0.785398163f;
            const ImVec2 a(x + std::cos(angle) * unit * 0.78f,
                           y + std::sin(angle) * unit * 0.78f);
            const ImVec2 b(x + std::cos(angle) * unit * 1.05f,
                           y + std::sin(angle) * unit * 1.05f);
            draw->AddLine(a, b, color, theme::metric(1.0f));
        }
    } else if (isCamera) {
        const float halfW = unit * 0.85f;
        const float halfH = unit * 0.58f;
        draw->AddRect(ImVec2(x - halfW, y - halfH), ImVec2(x + halfW, y + halfH),
                      color, 0.0f, 0, theme::metric(1.2f));
        draw->AddTriangle(ImVec2(x + halfW, y - halfH * 0.5f),
                          ImVec2(x + halfW + unit * 0.45f, y - halfH),
                          ImVec2(x + halfW + unit * 0.45f, y + halfH), color);
    } else if (hasMesh) {
        const float half = unit * 0.68f;
        draw->AddRect(ImVec2(x - half, y - half), ImVec2(x + half, y + half),
                      color, 0.0f, 0, theme::metric(1.2f));
        draw->AddLine(ImVec2(x - half, y - half), ImVec2(x, y - unit * 1.05f), color);
        draw->AddLine(ImVec2(x + half, y - half), ImVec2(x, y - unit * 1.05f), color);
        draw->AddLine(ImVec2(x, y - unit * 1.05f), ImVec2(x, y + half), color);
    } else {
        draw->AddCircleFilled(ImVec2(x, y), theme::metric(2.0f), color, 8);
    }
}

void drawCollectionIcon(ImVec2 min, ImVec2 max, bool visible) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x = (min.x + max.x) * 0.5f;
    const float y = (min.y + max.y) * 0.5f;
    const float w = theme::metric(9.0f), h = theme::metric(6.0f);
    const ImU32 color = ImGui::GetColorU32(visible ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw->AddRect(ImVec2(x - w * 0.5f, y - h * 0.5f),
                  ImVec2(x + w * 0.5f, y + h * 0.5f), color,
                  theme::metric(1.0f), 0, theme::metric(1.0f));
    draw->AddLine(ImVec2(x - w * 0.32f, y - h * 0.5f),
                  ImVec2(x - w * 0.05f, y - h * 0.85f), color, theme::metric(1.0f));
}

void drawVisibilityEye(bool shown, ImVec2 min, ImVec2 max, bool hovered) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float w = theme::metric(7.0f), h = theme::metric(4.0f);
    const ImU32 color = ImGui::GetColorU32(
        hovered ? ImGuiCol_Text : (shown ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    const ImVec2 left(center.x - w, center.y), right(center.x + w, center.y);
    draw->AddBezierCubic(left, ImVec2(center.x - w * 0.4f, center.y - h),
                         ImVec2(center.x + w * 0.4f, center.y - h), right,
                         color, theme::metric(1.2f));
    draw->AddBezierCubic(right, ImVec2(center.x + w * 0.4f, center.y + h),
                         ImVec2(center.x - w * 0.4f, center.y + h), left,
                         color, theme::metric(1.2f));
    if (shown) draw->AddCircleFilled(center, theme::metric(1.7f), color, 8);
    else draw->AddLine(ImVec2(center.x - w, center.y + h),
                       ImVec2(center.x + w, center.y - h), color, theme::metric(1.0f));
}

template <typename SetVisibility>
void drawVisibilityControl(const char* id, bool& visible,
                           SetVisibility&& setVisibility, const char* tooltip) {
    const float size = theme::metric(18.0f);
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || ImGui::IsItemActive()) {
        ImGui::GetWindowDrawList()->AddRectFilled(min, max,
            ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBgActive),
            theme::metric(2.0f));
    }
    drawVisibilityEye(visible, min, max, hovered);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        visible = !visible;
        setVisibility(visible);
    }
    if (hovered) ImGui::SetTooltip("%s", tooltip);
}

void moveToCollection(scene::World& world, UndoStack& undo, ObjectId child,
                      scene::CollectionId target);

void drawMoveToCollectionMenu(scene::World& world, UndoStack& undo, ObjectId object) {
    if (!ImGui::BeginMenu(command_labels::moveToCollection())) return;
    const std::string rootCollectionName = std::string(labels::Scene) + " " +
                                           ember::UiVocabulary::Collection;
    if (ImGui::MenuItem(rootCollectionName.c_str(), nullptr, !world.collectionOf(object).valid()))
        moveToCollection(world, undo, object, scene::CollectionId{});
    for (const scene::Collection& collection : world.collections()) {
        const std::string label = collection.name + "##collection-" +
                                  std::to_string(collection.id.value);
        if (ImGui::MenuItem(label.c_str(), nullptr,
                            world.collectionOf(object) == collection.id))
            moveToCollection(world, undo, object, collection.id);
    }
    ImGui::EndMenu();
}

// Drag-and-drop reparent. A drop onto a descendant would create a cycle, so it
// is refused here, before a command exists. Local transforms are preserved by
// setParent, so the object stays where its local transform puts it.
void reparent(scene::World& world, UndoStack& undo, ObjectId child, ObjectId parent) {
    if (!child.valid() || child == parent) return;
    if (parent.valid() && world.isDescendant(parent, child)) return;
    const ObjectId before = world.parentOf(child);
    if (before == parent) return;
    undo.push(std::make_unique<ParentCommand>(world, child, before, parent));
}

// Drop onto a collection row: the object joins that collection. Parenting is not
// touched, so this never fights the reparent drop.
void moveToCollection(scene::World& world, UndoStack& undo, ObjectId child, scene::CollectionId target) {
    if (!child.valid() || !world.has(child)) return;
    const scene::CollectionId before = world.collectionOf(child);
    if (before == target) return;
    undo.push(std::make_unique<MembershipCommand>(world, child, before, target));
}

void beginRename(RenameState& rename, ObjectId id, const std::string& name) {
    rename.id = id;
    rename.focus = true;
    rename.cancel = false;
    std::snprintf(rename.buffer, sizeof rename.buffer, "%s", name.c_str());
}

// An object's children are listed under it only when they share its collection.
// A child in another collection appears in its own collection instead.
void drawNode(scene::World& world, UndoStack& undo, ObjectId id, RenameState& rename,
              scene::CollectionId coll) {
    const scene::Object* o = world.get(id);
    if (!o) return;

    // The --demo-object SmokeCube is created without selection. Keep the
    // Selected fill strictly tied to World::selection(); pointer hover is a
    // separate, neutral ImGui state and must not impersonate selection.
    bool selected = isSelected(world, id);
    const bool active = activeObject(world) == id;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    const std::vector<ObjectId> children = o->children;
    const bool hasVisibleChildren = std::any_of(children.begin(), children.end(),
        [&](ObjectId child) {
            return world.has(child) && world.collectionOf(child) == coll;
        });
    if (!hasVisibleChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    const bool visible = o->visible;
    const bool hasMesh = o->hasMesh;
    const bool isCamera = o->camera.isCamera;
    const bool isLight = o->isLight;
    const std::string name = o->name;
    const bool editing = rename.id == id;

    ImGui::PushID(static_cast<int>(id.value));
    theme::pushSelectedObjectColors();
    const bool open = ImGui::TreeNodeEx(editing ? "##rowlabel" : name.c_str(), flags);
    theme::popHeaderColors();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const bool rowHovered = ImGui::IsItemHovered();
    if (active) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(rowMin.x, rowMin.y + theme::metric(2.0f)),
            ImVec2(rowMin.x + theme::metric(2.0f), rowMax.y - theme::metric(2.0f)),
            ImGui::GetColorU32(theme::activeFill()));
    }

    // Right-click selects the row first (Blender), then opens its menu.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !isSelected(world, id)) {
        world.select(id, false);
        selected = true;
    }
    if (ImGui::BeginPopupContextItem("##rowmenu")) {
        if (ImGui::MenuItem("Rename")) beginRename(rename, id, name);
        if (ImGui::MenuItem("Duplicate")) duplicateCurrentSelection(world, undo);
        if (ImGui::MenuItem("Delete")) deleteCurrentSelection(world, undo);
        drawMoveToCollectionMenu(world, undo, id);
        if (isCamera) {
            if (ImGui::MenuItem("Set Active Camera")) {
                undo.push(std::make_unique<ActiveCameraCommand>(
                    world, world.activeCamera(), id));
            }
        }
        ImGui::EndPopup();
    }

    // Drag a row onto another row to reparent it; the drop target is this header.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload(kDragType, &id.value, sizeof(u32));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragType)) {
            u32 raw = 0;
            std::memcpy(&raw, payload->Data, sizeof raw);
            reparent(world, undo, ObjectId{raw}, id);
        }
        ImGui::EndDragDropTarget();
    }

    if (editing) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (rename.focus) {
            ImGui::SetKeyboardFocusHere();
            rename.focus = false;
        }
        ImGui::InputText("##renamefield", rename.buffer, sizeof rename.buffer);
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) rename.cancel = true;
        if (ImGui::IsItemDeactivated()) {
            const std::string typed = rename.buffer;
            if (!rename.cancel && !typed.empty() && typed != name) {
                undo.push(std::make_unique<RenameCommand>(world, id, name, typed));
            }
            rename.id = ObjectId{};
            rename.cancel = false;
        }
    } else {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const ImGuiIO& io = ImGui::GetIO();
            world.select(id, io.KeyCtrl || io.KeyShift);
            selected = isSelected(world, id);
        }
        // The default demo fixture is created unselected. Explain its neutral
        // hover wash so it cannot be mistaken for the Outliner selection fill.
        if (rowHovered && !selected && name == kDemoObjectName)
            ImGui::SetTooltip("SmokeCube is not selected; click the row to select it.");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            beginRename(rename, id, name);
        }
    }

    // Compact object-kind glyph and eye control share the row's trailing edge;
    // both remain real hit targets, and visibility still goes through undo.
    const float controlSize = theme::metric(18.0f);
    const float iconSize = theme::metric(16.0f);
    const float contentRight = ImGui::GetContentRegionMax().x;
    const float eyeX = std::max(ImGui::GetCursorPosX() + theme::metric(2.0f),
                                contentRight - controlSize);
    const float iconX = std::max(ImGui::GetCursorPosX() + theme::metric(2.0f),
                                 eyeX - iconSize - theme::metric(3.0f));
    ImGui::SameLine(iconX);
    ImGui::Dummy(ImVec2(iconSize, controlSize));
    drawObjectIcon(hasMesh, isCamera, isLight, visible,
                   ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine(eyeX);
    bool shown = visible;
    drawVisibilityControl("##visible", shown, [&](bool next) {
        undo.push(std::make_unique<VisibilityCommand>(world, id, visible, next));
    }, visible ? "Hide in Viewport" : "Show in Viewport");

    if (open) {
        for (ObjectId child : children) {
            if (world.collectionOf(child) == coll) drawNode(world, undo, child, rename, coll);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// The objects of one collection, as a tree by parent. The list is collected
// before drawing, because a command pushed while drawing can reallocate the
// scene's object vector.
void drawObjects(scene::World& world, UndoStack& undo, RenameState& rename, scene::CollectionId coll) {
    std::vector<ObjectId> roots;
    for (const scene::Object& o : world.objects()) {
        if (world.collectionOf(o.id) != coll) continue;
        const bool parentHere = o.parent.valid() && world.has(o.parent) &&
                                world.collectionOf(o.parent) == coll;
        if (!parentHere) roots.push_back(o.id);
    }
    for (ObjectId id : roots) drawNode(world, undo, id, rename, coll);
}

void drawCollection(scene::World& world, UndoStack& undo, RenameState& rename, scene::CollectionId id) {
    const scene::Collection* c = world.get(id);
    if (!c) return;
    const std::string name = c->name;
    const bool visible = c->visible;

    ImGui::PushID(static_cast<int>(id.value) + 0x40000000);
    const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                     ImGuiTreeNodeFlags_DefaultOpen;
    const bool open = ImGui::TreeNodeEx(name.c_str(), flags);

    if (ImGui::BeginPopupContextItem("##collmenu")) {
        if (ImGui::MenuItem((std::string("New ") + ember::UiVocabulary::Collection).c_str())) {
            undo.push(std::make_unique<CreateCollectionCommand>(world, ember::UiVocabulary::Collection, id));
        }
        if (ImGui::MenuItem((std::string("Delete ") + ember::UiVocabulary::Collection).c_str())) {
            undo.push(std::make_unique<DestroyCollectionCommand>(world, id));
        }
        ImGui::EndPopup();
    }

    // Objects dropped on a collection row change collection; the reparent drop
    // is on object rows and does not reach here.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragType)) {
            u32 raw = 0;
            std::memcpy(&raw, payload->Data, sizeof raw);
            moveToCollection(world, undo, ObjectId{raw}, id);
        }
        ImGui::EndDragDropTarget();
    }

    const float controlSize = theme::metric(18.0f);
    const float iconSize = theme::metric(16.0f);
    const float contentRight = ImGui::GetContentRegionMax().x;
    const float eyeX = std::max(ImGui::GetCursorPosX() + theme::metric(2.0f),
                                contentRight - controlSize);
    const float iconX = std::max(ImGui::GetCursorPosX() + theme::metric(2.0f),
                                 eyeX - iconSize - theme::metric(3.0f));
    ImGui::SameLine(iconX);
    ImGui::Dummy(ImVec2(iconSize, controlSize));
    drawCollectionIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), visible);
    ImGui::SameLine(eyeX);
    bool shown = visible;
    const std::string visibilityTooltip =
        std::string(visible ? "Hide " : "Show ") + ember::UiVocabulary::Collection;
    drawVisibilityControl("##collvisible", shown, [&](bool next) {
        undo.push(std::make_unique<CollectionVisibilityCommand>(world, id, visible, next));
    }, visibilityTooltip.c_str());

    if (open) {
        // Copy the child ids first: drawing a child may push a command that changes the list.
        std::vector<scene::CollectionId> children;
        for (const scene::Collection& other : world.collections()) {
            if (other.parent == id) children.push_back(other.id);
        }
        for (scene::CollectionId child : children) drawCollection(world, undo, rename, child);
        drawObjects(world, undo, rename, id);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace

void drawOutliner(scene::World& world, UndoStack& undo) {
    static RenameState rename;

    beginDockedPanel(ember::UiVocabulary::Outliner);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(theme::metric(4.0f), theme::metric(2.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(theme::metric(4.0f), theme::metric(1.0f)));

    if (ImGui::BeginPopupContextWindow("OutlinerContext", ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Clear selection")) world.clearSelection();
        if (ImGui::MenuItem((std::string("New ") + ember::UiVocabulary::Collection).c_str())) {
            undo.push(std::make_unique<CreateCollectionCommand>(world, ember::UiVocabulary::Collection, scene::CollectionId{}));
        }
        ImGui::EndPopup();
    }

    // The scene root is a synthetic Outliner row: scene data deliberately has no
    // record for invalid CollectionId membership (the implicit root collection).
    // It gives root objects and explicit collections the same visible parent.
    std::vector<scene::CollectionId> topLevel;
    for (const scene::Collection& c : world.collections()) {
        if (!c.parent.valid()) topLevel.push_back(c.id);
    }
    bool hasRootObjects = false;
    for (const scene::Object& o : world.objects()) {
        if (world.collectionOf(o.id) == scene::CollectionId{}) {
            hasRootObjects = true;
            break;
        }
    }
    const bool hasRootChildren = hasRootObjects || !topLevel.empty();
    const std::string rootName = std::string(labels::Scene) + " " +
                                 ember::UiVocabulary::Collection;
    ImGui::PushID("##scene-collection-root");
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (hasRootChildren) rootFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    else rootFlags |= ImGuiTreeNodeFlags_Leaf;
    const bool rootOpen = ImGui::TreeNodeEx(rootName.c_str(), rootFlags);

    if (ImGui::BeginPopupContextItem("##scene-root-menu")) {
        if (ImGui::MenuItem(command_labels::newCollection())) {
            undo.push(std::make_unique<CreateCollectionCommand>(
                world, ember::UiVocabulary::Collection, scene::CollectionId{}));
        }
        if (ImGui::MenuItem("Clear selection")) world.clearSelection();
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragType)) {
            u32 raw = 0;
            std::memcpy(&raw, payload->Data, sizeof raw);
            moveToCollection(world, undo, ObjectId{raw}, scene::CollectionId{});
        }
        ImGui::EndDragDropTarget();
    }

    if (rootOpen) {
        for (scene::CollectionId id : topLevel) drawCollection(world, undo, rename, id);
        drawObjects(world, undo, rename, scene::CollectionId{});
        ImGui::TreePop();
    }
    ImGui::PopID();

    // Dropping on the empty area below the rows moves the object to the top level.
    ImGui::InvisibleButton("##unparent",
                           ImVec2(ImGui::GetContentRegionAvail().x, theme::metric(24.0f)));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragType)) {
            u32 raw = 0;
            std::memcpy(&raw, payload->Data, sizeof raw);
            reparent(world, undo, ObjectId{raw}, ObjectId{});
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopStyleVar(2);
    endDockedPanel();
}

} // namespace ember::editor
