#include "editor/Panels.h"

#include <imgui.h>

#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/Theme.h"
#include "editor/Commands.h"
#include "editor/EditModeTools.h"
#include "editor/ThemeStyle.h"
#include "editor/EditorLabels.h"

#include <algorithm>
#include <cstdio>

namespace ember::editor {

namespace {

struct PrimitiveChoice {
    scene::Primitive kind;
    const char* label;
};

constexpr PrimitiveChoice kPrimitives[] = {
    {scene::Primitive::Cube, "Cube"},
    {scene::Primitive::Sphere, "Sphere"},
    {scene::Primitive::Plane, "Plane"},
    {scene::Primitive::Cylinder, "Cylinder"},
    {scene::Primitive::Torus, "Torus"},
};

void addPrimitive(scene::World& world, UndoStack& undo, scene::Primitive kind, const char* label,
                  const Vec3& at) {
    Transform placed;
    placed.position = at;
    undo.push(std::make_unique<AddCommand>(
        world, std::string("Add ") + label, [kind, placed](scene::World& w) {
            const ObjectId id = w.createPrimitive(kind);
            w.setTransform(id, placed);
            return id;
        }));
}

// One undo step for the whole selection. Copies land in place (duplicate keeps
// the transform), and the copies become the new selection.
void addLight(scene::World& world, UndoStack& undo, ember::LightType type, const char* label,
              const Vec3& at) {
    Transform placed;
    placed.position = at;
    undo.push(std::make_unique<AddCommand>(
        world, std::string("Add ") + label, [type, placed](scene::World& w) {
            const ObjectId id = w.createLight(type);
            w.setTransform(id, placed);
            return id;
        }));
}

void addCamera(scene::World& world, UndoStack& undo, const Vec3& at) {
    Transform placed;
    placed.position = at;
    undo.push(std::make_unique<AddCommand>(world, "Add Camera", [placed](scene::World& w) {
        const ObjectId id = w.createCamera();
        w.setTransform(id, placed);
        return id;
    }));
}

void duplicateSelection(scene::World& world, UndoStack& undo) {
    const std::vector<ObjectId> sources = world.selection();
    auto batch = std::make_unique<MacroCommand>("Duplicate");
    for (ObjectId src : sources) {
        if (!world.get(src)) continue;
        const bool additive = !batch->empty();
        batch->add(std::make_unique<AddCommand>(world, "Duplicate", [src](scene::World& w) {
            return w.duplicate(src);
        }, additive));
    }
    if (!batch->empty()) undo.push(std::move(batch));
}

// Selection is view state, not a document edit, so these stay outside the undo
// stack (see the Viewport/Outliner rules).
void selectAll(scene::World& world) {
    world.clearSelection();
    for (const scene::Object& o : world.objects()) world.select(o.id, true);
}

void deleteSelection(scene::World& world, UndoStack& undo) {
    const std::vector<ObjectId> targets = world.selection();
    auto batch = std::make_unique<MacroCommand>("Delete");
    for (ObjectId id : targets) {
        if (world.get(id)) batch->add(std::make_unique<DeleteCommand>(world, id));
    }
    if (!batch->empty()) undo.push(std::move(batch));
}

std::string undoText(const UndoStack& undo) {
    return undo.canUndo() ? std::string("Undo ") + undo.undoLabel() : std::string("Undo");
}

std::string redoText(const UndoStack& undo) {
    return undo.canRedo() ? std::string("Redo ") + undo.redoLabel() : std::string("Redo");
}

} // namespace

namespace {

constexpr std::size_t kRecentLimit = 8;

void rememberPath(DocumentState& doc, const std::string& path) {
    auto& recent = doc.recent;
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > kRecentLimit) recent.resize(kRecentLimit);
}

void forgetPath(DocumentState& doc, const std::string& path) {
    auto& recent = doc.recent;
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
}

// Saves the world to `path`; on success that path becomes the document's path.
bool writeScene(scene::World& world, DocumentState& doc, FileDialog& file, const std::string& path) {
    const std::size_t objects = world.objects().size();
    const bool ok = scene::save(world, path);
    if (ok) {
        doc.path = path;
        rememberPath(doc, path);
        EMBER_LOG_INFO("file: saved %zu object(s) to '%s'", objects, path.c_str());
    } else {
        EMBER_LOG_ERROR("file: save failed for '%s'", path.c_str());
    }
    file.status = ok ? std::string("Saved ") + path : std::string("Save failed: ") + path;
    return ok;
}

// Replaces the world with the file's contents, or leaves it untouched. Old ids
// are gone after a successful load, so the undo history that names them is dropped.
bool readScene(scene::World& world, UndoStack& undo, DocumentState& doc, FileDialog& file,
               const std::string& path) {
    const bool ok = scene::load(world, path);
    if (ok) {
        undo.clear();
        world.clearSelection();
        doc.path = path;
        rememberPath(doc, path);
        EMBER_LOG_INFO("file: loaded %zu object(s) from '%s'", world.objects().size(), path.c_str());
    } else {
        forgetPath(doc, path);
        EMBER_LOG_ERROR("file: load failed for '%s'", path.c_str());
    }
    file.status = ok ? std::string("Loaded ") + path : std::string("Load failed: ") + path;
    return ok;
}

void saveCurrent(scene::World& world, DocumentState& doc, FileDialog& file) {
    if (doc.path.empty()) {
        openFileDialog(file, doc, false);
    } else {
        const std::string path = doc.path;
        writeScene(world, doc, file, path);
    }
}

// Removes every object as one undo step. Undo brings the old scene back.
void newScene(scene::World& world, UndoStack& undo, DocumentState& doc) {
    std::vector<ObjectId> ids;
    for (const scene::Object& o : world.objects()) ids.push_back(o.id);
    auto batch = std::make_unique<MacroCommand>("New Scene");
    for (ObjectId id : ids) batch->add(std::make_unique<DeleteCommand>(world, id));
    if (!batch->empty()) undo.push(std::move(batch));
    doc.path.clear();
    EMBER_LOG_INFO("file: new scene (%zu object(s) removed, undoable)", ids.size());
}

} // namespace

void openFileDialog(FileDialog& file, const DocumentState& doc, bool wantLoad) {
    file.open = true;
    file.wantLoad = wantLoad;
    file.focus = true;
    file.status.clear();
    std::string suggested;
    if (!doc.path.empty()) {
        suggested = doc.path;
    } else if (!doc.recent.empty()) {
        suggested = doc.recent.front();
    }
    if (!suggested.empty()) std::snprintf(file.path, sizeof file.path, "%s", suggested.c_str());
}

void duplicateCurrentSelection(scene::World& world, UndoStack& undo) {
    duplicateSelection(world, undo);
}

void deleteCurrentSelection(scene::World& world, UndoStack& undo) {
    deleteSelection(world, undo);
}

void drawViewportObjectMenus(scene::World& world, UndoStack& undo, const Vec3& addAt) {
    const bool hasSelection = !world.selection().empty();
    if (ImGui::Button("Add")) ImGui::OpenPopup("##viewport-add");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a mesh, light, or camera to the scene");
    if (ImGui::BeginPopup("##viewport-add")) {
        for (const PrimitiveChoice& p : kPrimitives)
            if (ImGui::MenuItem(p.label)) addPrimitive(world, undo, p.kind, p.label, addAt);
        ImGui::Separator();
        if (ImGui::MenuItem("Sun")) addLight(world, undo, ember::LightType::Directional, "Sun", addAt);
        if (ImGui::MenuItem("Point")) addLight(world, undo, ember::LightType::Point, "Point", addAt);
        if (ImGui::MenuItem("Camera")) addCamera(world, undo, addAt);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, theme::metric(4.0f));
    if (ImGui::Button(labels::Object)) ImGui::OpenPopup("##viewport-object");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate or delete the selection");
    if (ImGui::BeginPopup("##viewport-object")) {
        if (ImGui::MenuItem("Duplicate", "Shift+D", false, hasSelection)) duplicateSelection(world, undo);
        if (ImGui::MenuItem("Delete", "X", false, hasSelection)) deleteSelection(world, undo);
        ImGui::EndPopup();
    }
}

void drawMainMenu(scene::World& world, UndoStack& undo, FileDialog& file,
                  DocumentState& doc, WorkspacePreset& workspace, bool& sidebar,
                  bool& toolbar, bool& history, bool& preferences,
                  InteractionMode interaction, SelectMode selectMode) {
    if (!ImGui::BeginMainMenuBar()) return;

    // Workspace tabs are anchored in the Topbar before its menus. Each selects
    // a deterministic editor arrangement, not a separate floating window.
    const char* workspaceNames[] = {"Layout", "Modeling", "Animation"};
    const ember::ThemeTokens& themeTokens = theme::tokens();
    const ImVec4 hoverFill = theme::hoverFill();
    const ImVec4 activeFill = theme::activeFill();
    for (int i = 0; i < 3; ++i) {
        const bool selected = static_cast<int>(workspace) == i;
        const float tabWidth = ImGui::CalcTextSize(workspaceNames[i]).x +
                               2.0f * ImGui::GetStyle().FramePadding.x;
        const float tabHeight = ImGui::GetFrameHeight();
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##workspace-tab", ImVec2(tabWidth, tabHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImVec2 tabMin = ImGui::GetItemRectMin();
        const ImVec2 tabMax = ImGui::GetItemRectMax();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // The current workspace is an active editor, not an object selection:
        // use active blue for it and for a press, with a neutral hover wash.
        if (active || selected || hovered) {
            const ImVec4 fill = active || selected ? activeFill : hoverFill;
            drawList->AddRectFilled(tabMin, tabMax, ImGui::ColorConvertFloat4ToU32(fill));
        }
        if (selected) {
            const float indicatorHeight = theme::metric(2.0f);
            drawList->AddRectFilled(ImVec2(tabMin.x, tabMax.y - indicatorHeight), tabMax,
                                    ImGui::ColorConvertFloat4ToU32(theme::color(themeTokens.active)));
        }

        const ImVec2 textSize = ImGui::CalcTextSize(workspaceNames[i]);
        const ImVec2 textPos(tabMin.x + ImGui::GetStyle().FramePadding.x,
                             tabMin.y + (tabHeight - textSize.y) * 0.5f);
        drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), workspaceNames[i]);
        ImGui::PopID();

        if (clicked)
            workspace = static_cast<WorkspacePreset>(i);
        if (i != 2) ImGui::SameLine(0.0f, theme::metric(2.0f));
    }
    ImGui::SameLine(0.0f, theme::metric(12.0f));

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene")) newScene(world, undo, doc);
        if (ImGui::MenuItem("Open...")) openFileDialog(file, doc, true);
        if (ImGui::BeginMenu("Open Recent", !doc.recent.empty())) {
            // Copy first: opening a file reorders the list this loop walks.
            const std::vector<std::string> recent = doc.recent;
            for (const std::string& path : recent) {
                if (ImGui::MenuItem(path.c_str())) readScene(world, undo, doc, file, path);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save")) saveCurrent(world, doc, file);
        if (ImGui::MenuItem("Save As...")) openFileDialog(file, doc, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        const std::string undoLabel = undoText(undo);
        const std::string redoLabel = redoText(undo);
        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, undo.canUndo())) undo.undo();
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, undo.canRedo())) undo.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "A")) selectAll(world);
        if (ImGui::MenuItem("Deselect All", "Alt+A")) world.clearSelection();
        if (interaction == InteractionMode::Edit && ImGui::BeginMenu("Mesh")) {
            const ObjectId active = world.selection().size() == 1 ? world.selection().back() : ObjectId{};
            const bool canExtrude = canPerformEditMeshOperation(
                world, active, selectMode, EditMeshOperation::ExtrudeFace);
            if (ImGui::MenuItem("Extrude Face", "E", false, canExtrude))
                performEditMeshOperation(world, undo, active, selectMode,
                                         EditMeshOperation::ExtrudeFace,
                                         editExtrudeDistance());
            const bool canDelete = canPerformEditMeshOperation(
                world, active, selectMode, EditMeshOperation::DeleteSelection);
            if (ImGui::MenuItem("Delete Selected Geometry", "X", false, canDelete))
                performEditMeshOperation(world, undo, active, selectMode,
                                         EditMeshOperation::DeleteSelection);
            const bool canMerge = canPerformEditMeshOperation(
                world, active, selectMode, EditMeshOperation::MergeAtCenter);
            if (ImGui::MenuItem("Merge at Center", "M", false, canMerge))
                performEditMeshOperation(world, undo, active, selectMode,
                                         EditMeshOperation::MergeAtCenter);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(labels::UndoHistory)) history = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem(ember::UiVocabulary::Toolbar, "T", &toolbar);
        ImGui::MenuItem(ember::UiVocabulary::Sidebar, "N", &sidebar);
        ImGui::MenuItem(labels::UndoHistory, nullptr, &history);
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...", "Ctrl+,")) preferences = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keymap")) preferences = true;
        ImGui::TextDisabled("Ember3D - Blender-inspired editor");
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void drawMainMenu(scene::World& world, UndoStack& undo, const Vec3& addAt, FileDialog& file,
                  DocumentState& doc, ember::ViewportSettings& viewport, TransformSpace& space,
                  bool& sidebar, bool& history) {
    (void)addAt;
    (void)viewport;
    (void)space;
    WorkspacePreset workspace = WorkspacePreset::Layout;
    bool toolbar = true;
    bool preferences = false;
    drawMainMenu(world, undo, file, doc, workspace, sidebar, toolbar, history, preferences,
                 InteractionMode::Object, SelectMode::Vertex);
}

void handleShortcuts(scene::World& world, UndoStack& undo) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;  // typing into a field: keys are text, not commands

    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;

    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z)) undo.undo();
    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (shift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
        undo.redo();
    }

    if (!ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_A)) {
        if (io.KeyAlt) {
            world.clearSelection();
        } else {
            selectAll(world);
        }
    }

    if (!ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_D)) duplicateSelection(world, undo);
    if (!ctrl && !shift && (ImGui::IsKeyPressed(ImGuiKey_X) || ImGui::IsKeyPressed(ImGuiKey_Delete))) {
        deleteSelection(world, undo);
    }
}

void drawFileDialog(scene::World& world, UndoStack& undo, FileDialog& file, DocumentState& doc) {
    if (!file.open) return;

    ImGui::SetNextWindowSize(ImVec2(theme::metric(420.0f), 0.0f), ImGuiCond_Appearing);
    const char* title = file.wantLoad ? "Open scene" : "Save scene";
    if (!beginDialog(title, &file.open)) {
        endDialog();
        return;
    }

    if (file.focus) {
        ImGui::SetKeyboardFocusHere();
        file.focus = false;
    }
    // Enter in the path field and the button do the same thing.
    const bool enter = ImGui::InputText("Path", file.path, sizeof file.path,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool clicked = ImGui::Button(file.wantLoad ? "Open" : "Save");
    if (enter || clicked) {
        const std::string path = file.path;
        const bool ok = file.wantLoad ? readScene(world, undo, doc, file, path)
                                      : writeScene(world, doc, file, path);
        if (ok) file.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) file.open = false;

    if (!file.status.empty()) ImGui::TextUnformatted(file.status.c_str());
    endDialog();
}

namespace {

struct PaletteEntry {
    std::string label;
    const char* shortcut;
    bool enabled;
    std::function<void()> run;
};

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

// F3 opens the palette. Every entry calls the same function its menu item calls,
// so the palette and the menus cannot disagree.
void drawCommandPalette(scene::World& world, UndoStack& undo, const Vec3& addAt, FileDialog& file,
                        DocumentState& doc, ember::ViewportSettings& viewport, TransformSpace& space,
                        bool& sidebar, PaletteState& palette) {
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) {
        palette.open = !palette.open;
        if (palette.open) {
            palette.query[0] = '\0';
            palette.selected = 0;
            palette.focus = true;
        }
    }
    if (!palette.open) return;

    const bool hasSelection = !world.selection().empty();
    std::vector<PaletteEntry> entries;
    entries.push_back({"Undo", "Ctrl+Z", undo.canUndo(), [&] { undo.undo(); }});
    entries.push_back({"Redo", "Ctrl+Y", undo.canRedo(), [&] { undo.redo(); }});
    entries.push_back({"Select All", "A", true, [&] { selectAll(world); }});
    entries.push_back({"Deselect All", "Alt+A", true, [&] { world.clearSelection(); }});
    entries.push_back({"Duplicate", "Shift+D", hasSelection, [&] { duplicateSelection(world, undo); }});
    entries.push_back({"Delete", "X", hasSelection, [&] { deleteSelection(world, undo); }});
    for (const PrimitiveChoice& p : kPrimitives) {
        entries.push_back({std::string("Add ") + p.label, nullptr, true,
                           [&, p] { addPrimitive(world, undo, p.kind, p.label, addAt); }});
    }
    entries.push_back({"Add Sun", nullptr, true,
                       [&] { addLight(world, undo, ember::LightType::Directional, "Sun", addAt); }});
    entries.push_back({"Add Point", nullptr, true,
                       [&] { addLight(world, undo, ember::LightType::Point, "Point", addAt); }});
    entries.push_back({"Add Camera", nullptr, true, [&] { addCamera(world, undo, addAt); }});
    entries.push_back({"New Scene", nullptr, true, [&] { newScene(world, undo, doc); }});
    entries.push_back({"Open...", nullptr, true, [&] { openFileDialog(file, doc, true); }});
    entries.push_back({"Save", nullptr, true, [&] { saveCurrent(world, doc, file); }});
    entries.push_back({"Save As...", nullptr, true, [&] { openFileDialog(file, doc, false); }});
    for (const std::string& path : doc.recent) {
        entries.push_back({std::string("Open Recent: ") + path, nullptr, true,
                           [&, path] { readScene(world, undo, doc, file, path); }});
    }
    entries.push_back({std::string("Toggle ") + ember::UiVocabulary::Sidebar, "N", true,
                       [&] { sidebar = !sidebar; }});
    entries.push_back({"Toggle Grid", nullptr, true, [&] { viewport.showGrid = !viewport.showGrid; }});
    entries.push_back({"Toggle Axes", nullptr, true, [&] { viewport.showAxes = !viewport.showAxes; }});
    entries.push_back({"Toggle Gizmo", nullptr, true, [&] { viewport.showGizmo = !viewport.showGizmo; }});
    entries.push_back({"Toggle Origins", nullptr, true, [&] { viewport.showOrigins = !viewport.showOrigins; }});
    entries.push_back({"Shading: Solid", nullptr, true, [&] { viewport.shading = ember::ShadingMode::Solid; }});
    entries.push_back({"Shading: Wireframe", nullptr, true,
                       [&] { viewport.shading = ember::ShadingMode::Wireframe; }});
    entries.push_back({"Transform Space: Global", nullptr, true, [&] { space = TransformSpace::Global; }});
    entries.push_back({"Transform Space: Local", nullptr, true, [&] { space = TransformSpace::Local; }});

    const std::string query = lowered(palette.query);
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (query.empty() || lowered(entries[i].label).find(query) != std::string::npos) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        palette.selected = 0;
    } else if (palette.selected >= static_cast<int>(matches.size())) {
        palette.selected = static_cast<int>(matches.size()) - 1;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->WorkPos.y + theme::metric(120.0f)), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(theme::metric(420.0f), theme::metric(320.0f)), ImGuiCond_Appearing);
    bool open = true;
    if (!beginDialog("Search", &open, ImGuiWindowFlags_NoSavedSettings)) {
        endDialog();
        if (!open) palette.open = false;
        return;
    }

    if (palette.focus) {
        ImGui::SetKeyboardFocusHere();
        palette.focus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##palettequery", palette.query, sizeof palette.query);

    if (!matches.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            palette.selected = (palette.selected + 1) % static_cast<int>(matches.size());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            palette.selected = (palette.selected + static_cast<int>(matches.size()) - 1) %
                               static_cast<int>(matches.size());
        }
    }

    std::function<void()> toRun;
    ImGui::Separator();
    for (std::size_t k = 0; k < matches.size(); ++k) {
        const PaletteEntry& e = entries[matches[k]];
        const bool current = static_cast<int>(k) == palette.selected;
        ImGui::BeginDisabled(!e.enabled);
        theme::pushActiveItemColors();
        if (ImGui::Selectable(e.label.c_str(), current, ImGuiSelectableFlags_SpanAllColumns)) {
            palette.selected = static_cast<int>(k);
            toRun = e.run;
        }
        theme::popHeaderColors();
        ImGui::EndDisabled();
        if (e.shortcut) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - theme::metric(60.0f));
            ImGui::TextDisabled("%s", e.shortcut);
        }
        if (current) ImGui::SetScrollHereY(0.5f);
    }
    if (matches.empty()) ImGui::TextDisabled("No matching commands");

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (!matches.empty() && entries[matches[palette.selected]].enabled) {
            toRun = entries[matches[palette.selected]].run;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;

    endDialog();

    if (!open) palette.open = false;
    if (toRun) {
        palette.open = false;
        toRun();
    }
}

} // namespace ember::editor
