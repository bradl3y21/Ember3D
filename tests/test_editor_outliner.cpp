// Headless interaction coverage for the live Outliner collection tree.
// The source and target ids are discovered from ImGui's real item hit-tests;
// this drives the same drawOutliner() entry point Editor::drawArea calls.
#include "harness.h"

#include "core/Theme.h"
#include "editor/Panels.h"
#include "editor/ThemeStyle.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

#define CHECK_MSG(cond, ...)                                                      \
    do {                                                                         \
        ++harness::g_checks;                                                     \
        if (!(cond))                                                             \
            harness::reportFailure(__FILE__, __LINE__, harness::fmtf(__VA_ARGS__)); \
    } while (0)

using ember::ObjectId;

struct Rig {
    ImGuiContext* context = nullptr;
    ember::scene::World world;
    ember::editor::UndoStack undo;

    Rig() {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640.0f, 480.0f);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~Rig() { ImGui::DestroyContext(context); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void frame(float x = -10.0f, float y = -10.0f, int mouseButton = -1,
               bool buttonDown = false) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        if (mouseButton >= 0) ImGui::GetIO().AddMouseButtonEvent(mouseButton, buttonDown);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ember::editor::drawOutliner(world, undo);
        ImGui::Render();
    }

    ImGuiWindow* outliner() const {
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
            if (std::string(window->Name).find(ember::UiVocabulary::Outliner) != std::string::npos)
                return window;
        return nullptr;
    }

    bool findHovered(ImGuiID id, float x, float& outY) {
        ImGuiWindow* window = outliner();
        if (!window) return false;
        const float lo = window->InnerRect.Min.y;
        const float hi = window->InnerRect.Max.y;
        for (float y = lo; y < hi; y += 1.0f) {
            frame(x, y + 0.5f);
            ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.HoveredWindow == window && g.HoveredId == id) {
                outY = y + 0.5f;
                return true;
            }
        }
        return false;
    }

    struct HitBand { ImGuiID id; float top; float bottom; };
    std::vector<HitBand> hitBands(float x) {
        std::vector<HitBand> bands;
        ImGuiWindow* window = outliner();
        if (!window) return bands;
        ImGuiID current = 0;
        for (float y = window->InnerRect.Min.y; y < window->InnerRect.Max.y; y += 1.0f) {
            frame(x, y + 0.5f);
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID hovered = g.HoveredWindow == window ? g.HoveredId : 0;
            if (hovered == current) {
                if (hovered != 0) bands.back().bottom = y;
                continue;
            }
            current = hovered;
            if (hovered != 0) bands.push_back({hovered, y, y});
        }
        return bands;
    }

    void click(float x, float y) {
        frame(x, y);
        frame(x, y, ImGuiMouseButton_Left, true);
        frame(x, y, ImGuiMouseButton_Left, false);
        frame(x, y);
    }

    bool collectionItemIds(ember::scene::CollectionId collection, ImGuiID& rowId,
                           ImGuiID& visibleId) {
        ImGuiWindow* window = outliner();
        const ember::scene::Collection* row = world.get(collection);
        if (!window || !row) return false;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiWindow* previous = g.CurrentWindow;
        g.CurrentWindow = window;
        ImGui::PushID("##scene-collection-root");
        const ImGuiID root = ImGui::GetID("Scene Collection");
        ImGui::PushOverrideID(root);
        ImGui::PushID(static_cast<int>(collection.value) + 0x40000000);
        rowId = ImGui::GetID(row->name.c_str());
        ImGui::PushOverrideID(rowId);  // drawCollection's DefaultOpen tree node
        visibleId = ImGui::GetID("##collvisible");
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        g.CurrentWindow = previous;
        return true;
    }

    bool objectItemIds(ObjectId object, ImGuiID& rowId, ImGuiID& visibleId) {
        ImGuiWindow* window = outliner();
        const ember::scene::Object* row = world.get(object);
        if (!window || !row) return false;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiWindow* previous = g.CurrentWindow;
        g.CurrentWindow = window;
        // Object rows are descendants of the synthetic Scene Collection tree
        // node; reproduce its open TreePush scope before deriving row/eye ids.
        ImGui::PushID("##scene-collection-root");
        const ImGuiID root = ImGui::GetID("Scene Collection");
        ImGui::PushOverrideID(root);
        ImGui::PushID(static_cast<int>(object.value));
        rowId = ImGui::GetID(row->name.c_str());
        ImGui::PushOverrideID(rowId);  // TreeNodeEx scopes following row controls
        visibleId = ImGui::GetID("##visible");
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        g.CurrentWindow = previous;
        return true;
    }
};

} // namespace

TEST(outliner_collection_tree_rows_are_reachable_and_visibility_is_undoable) {
    Rig rig;
    const ember::scene::CollectionId collection = rig.world.createCollection("Assets");
    const ember::ObjectId object = rig.world.createPrimitive(ember::scene::Primitive::Cube, "Cube");
    rig.frame();

    ImGuiID collectionRow = 0, collectionVisible = 0;
    CHECK_MSG(rig.collectionItemIds(collection, collectionRow, collectionVisible),
              "could not derive the live Collection row ids");
    ImGuiWindow* window = rig.outliner();
    CHECK_MSG(window != nullptr, "drawOutliner did not create its Outliner area");
    if (!window) return;
    const float rowX = window->InnerRect.Min.x + 70.0f;
    const std::vector<Rig::HitBand> rows = rig.hitBands(rowX);
    CHECK_MSG(rows.size() >= 3, "expected root, Collection and object rows; found %zu", rows.size());
    if (rows.size() < 3) return;
    CHECK_MSG(rows[1].id == collectionRow, "the second tree row is not the 'Assets' Collection");
    const ImGuiID objectRow = rows[2].id;  // row 3 is the root Cube; the final band is the blank drop zone
    float rowY = 0.0f, visibilityY = 0.0f, objectY = 0.0f;
    const bool rowHit = rig.findHovered(collectionRow, rowX, rowY);
    const bool visibilityHit = rig.findHovered(collectionVisible, window->InnerRect.Max.x - 12.0f,
                                               visibilityY);
    const bool objectHit = rig.findHovered(objectRow, rowX, objectY);
    CHECK_MSG(rowHit, "Collection row 'Assets' was not hit-testable in the Outliner");
    CHECK_MSG(visibilityHit, "Collection visibility checkbox was not hit-testable");
    CHECK_MSG(objectHit, "the root object row was not hit-testable in the Outliner");
    if (!visibilityHit) return;

    const float checkboxX = window->InnerRect.Max.x - 12.0f;
    rig.click(checkboxX, visibilityY);
    CHECK_MSG(!rig.world.get(collection)->visible,
              "clicking the Collection eye did not hide the Collection");
    CHECK_MSG(rig.undo.canUndo() &&
                  std::string(rig.undo.undoLabel()).find("Collection") != std::string::npos,
              "Collection visibility did not create an undoable command");
    CHECK_MSG(rig.undo.undo(), "Collection visibility command would not undo");
    CHECK_MSG(rig.world.get(collection)->visible,
              "undo did not restore Collection visibility");

    // Drag a root object onto the Collection row. This uses ImGui's real
    // drag/drop payload path; collection membership is distinct from parenting.
    rig.frame(rowX, objectY);
    rig.frame(rowX, objectY, ImGuiMouseButton_Left, true);
    rig.frame(rowX + 20.0f, objectY, ImGuiMouseButton_Left, true);
    rig.frame(rowX, rowY, ImGuiMouseButton_Left, true);
    rig.frame(rowX, rowY, ImGuiMouseButton_Left, false);
    rig.frame();
    CHECK_MSG(rig.world.collectionOf(object) == collection,
              "dropping an object row on a Collection did not move its membership");
    CHECK_MSG(!rig.world.parentOf(object).valid(),
              "moving an object to a Collection unexpectedly changed parenting");
    CHECK_MSG(rig.undo.undo(), "Collection membership move would not undo");
    CHECK_MSG(!rig.world.collectionOf(object).valid(),
              "undo did not restore root collection membership");

    std::printf("       Collection row y=%.0f, eye y=%.0f, root object y=%.0f; visibility and drag/drop undo passed\n",
                double(rowY), double(visibilityY), double(objectY));
}

TEST(outliner_rows_are_compact_and_selection_tracks_active_object) {
    Rig rig;
    const ObjectId cube = rig.world.createPrimitive(ember::scene::Primitive::Cube, "Cube");
    const ObjectId sphere = rig.world.createPrimitive(ember::scene::Primitive::Sphere, "Sphere");
    rig.frame();

    ImGuiWindow* window = rig.outliner();
    CHECK_MSG(window != nullptr, "drawOutliner did not create its Outliner area");
    if (!window) return;

    ImGuiID cubeRow = 0, cubeEye = 0, sphereRow = 0, sphereEye = 0;
    CHECK_MSG(rig.objectItemIds(cube, cubeRow, cubeEye), "could not derive Cube's live item ids");
    CHECK_MSG(rig.objectItemIds(sphere, sphereRow, sphereEye), "could not derive Sphere's live item ids");
    const float rowX = window->InnerRect.Min.x + 72.0f;
    const std::vector<Rig::HitBand> rows = rig.hitBands(rowX);
    float cubeY = 0.0f, sphereY = 0.0f, eyeY = 0.0f;
    CHECK_MSG(rig.findHovered(cubeRow, rowX, cubeY), "Cube row was not hit-testable");
    CHECK_MSG(rig.findHovered(sphereRow, rowX, sphereY), "Sphere row was not hit-testable");
    CHECK_MSG(rig.findHovered(cubeEye, window->InnerRect.Max.x - 9.0f, eyeY),
              "compact object visibility control was not hit-testable");
    if (!rig.findHovered(cubeRow, rowX, cubeY) || !rig.findHovered(sphereRow, rowX, sphereY) ||
        !rig.findHovered(cubeEye, window->InnerRect.Max.x - 9.0f, eyeY))
        return;

    auto heightOf = [&](ImGuiID id) {
        for (const Rig::HitBand& band : rows)
            if (band.id == id) return band.bottom - band.top + 1.0f;
        return 0.0f;
    };
    const float cubeHeight = heightOf(cubeRow);
    CHECK_MSG(cubeHeight >= 12.0f && cubeHeight <= 28.0f,
              "object row hit target should be compact and usable (got %.1f px)", double(cubeHeight));

    rig.click(rowX, cubeY);
    CHECK_MSG(rig.world.selection().size() == 1 && rig.world.selection().back() == cube,
              "clicking an Outliner row did not select that stable object id");
    CHECK_MSG(!rig.undo.canUndo(), "selection should not create an undo command");

    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    rig.frame();
    rig.frame(rowX, sphereY);
    rig.frame(rowX, sphereY, ImGuiMouseButton_Left, true);
    rig.frame(rowX, sphereY, ImGuiMouseButton_Left, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    rig.frame();
    CHECK_MSG(rig.world.selection().size() == 2,
              "Ctrl-click should preserve multi-selection in the Outliner");
    const bool sphereIsActive = rig.world.selection().size() == 2 &&
                                rig.world.selection().back() == sphere;
    CHECK_MSG(sphereIsActive,
              "the most recently selected object should be the active object");
    if (!sphereIsActive) return;
    const ImU32 activeCue = ImGui::GetColorU32(ember::editor::theme::activeFill());
    bool activeCueDrawn = false;
    for (const ImDrawVert& vertex : window->DrawList->VtxBuffer)
        activeCueDrawn = activeCueDrawn || vertex.col == activeCue;
    CHECK_MSG(activeCueDrawn,
              "the active object should have a distinct active-state marker in the live row geometry");

    rig.frame(window->InnerRect.Max.x - 9.0f, eyeY);
    rig.frame(window->InnerRect.Max.x - 9.0f, eyeY, ImGuiMouseButton_Left, true);
    rig.frame(window->InnerRect.Max.x - 9.0f, eyeY, ImGuiMouseButton_Left, false);
    rig.frame();
    CHECK_MSG(!rig.world.get(cube)->visible,
              "clicking the compact eye control did not toggle object visibility");
    CHECK_MSG(rig.undo.canUndo() && std::string(rig.undo.undoLabel()) == "Visibility",
              "object visibility change should remain undoable");
    CHECK_MSG(rig.undo.undo() && rig.world.get(cube)->visible,
              "undo should restore object visibility without changing its id");
    CHECK_MSG(rig.world.has(cube) && rig.world.has(sphere),
              "Outliner selection and visibility must preserve stable object ids");
}

HARNESS_MAIN()
