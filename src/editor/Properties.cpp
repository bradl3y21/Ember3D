#include "editor/Panels.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "editor/Commands.h"
#include "editor/EditorLabels.h"
#include "editor/Icons.h"
#include "editor/ThemeStyle.h"
#include "core/Theme.h"

namespace ember::editor {

namespace {

constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kDegToRad = 0.017453292519943295f;

int resizeStringInput(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
    auto* value = static_cast<std::string*>(data->UserData);
    value->resize(static_cast<std::size_t>(data->BufTextLen));
    data->Buf = value->data();
    return 0;
}

const char* primitiveName(scene::Primitive p) {
    switch (p) {
        case scene::Primitive::Cube:     return "Cube";
        case scene::Primitive::Sphere:   return "Sphere";
        case scene::Primitive::Plane:    return "Plane";
        case scene::Primitive::Cylinder: return "Cylinder";
        case scene::Primitive::Torus:    return "Torus";
    }
    return "Unknown";
}

// Name commits when the field is left, not per keystroke. setName may uniquify
// the name ("Cube" -> "Cube.001"), so the buffer is re-read from the world.
void drawName(scene::World& world, UndoStack& undo, ObjectId id, const scene::Object& o) {
    static ObjectId bufferFor;
    static char buffer[128];
    if (bufferFor != id) {
        std::snprintf(buffer, sizeof buffer, "%s", o.name.c_str());
        bufferFor = id;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##name", buffer, sizeof buffer);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string before = o.name;
        const std::string after = buffer;
        if (!after.empty() && after != before) {
            undo.push(std::make_unique<RenameCommand>(world, id, before, after));
        }
        bufferFor = ObjectId{};
    }
}

// A drag becomes one command per frame. The first frame is a new command, and
// the rest carry continuation = true, so the UndoStack folds them into one step.
void drawTransform(scene::World& world, UndoStack& undo, ObjectId id, const scene::Object& o) {
    static Transform dragStart;
    static bool dragging = false;
    const Transform current = o.transform;

    float pos[3] = {current.position.x, current.position.y, current.position.z};
    float rot[3] = {current.rotation.x * kRadToDeg, current.rotation.y * kRadToDeg,
                    current.rotation.z * kRadToDeg};
    float scl[3] = {current.scale.x, current.scale.y, current.scale.z};

    bool changed = false;
    bool began = false;
    bool ended = false;

    changed |= ImGui::DragFloat3("Location", pos, 0.05f, 0.0f, 0.0f, "%.3f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::DragFloat3("Rotation", rot, 0.5f, 0.0f, 0.0f, "%.2f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::DragFloat3("Scale", scl, 0.01f, 0.0f, 0.0f, "%.3f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    if (began) {
        dragStart = current;
        dragging = false;
    }
    if (changed) {
        Transform next;
        next.position = Vec3{pos[0], pos[1], pos[2]};
        next.rotation = Vec3{rot[0] * kDegToRad, rot[1] * kDegToRad, rot[2] * kDegToRad};
        next.scale = Vec3{scl[0], scl[1], scl[2]};
        undo.push(std::make_unique<TransformCommand>(world, id, dragStart, next, dragging));
        dragging = true;
    }
    if (ended) dragging = false;
}

// Kind is a discrete choice: one command when it changes. Colour, strength and
// range are drags: one command per gesture, like the material widgets.
void drawLight(scene::World& world, UndoStack& undo, ObjectId id, const scene::Object& o) {
    static ember::Light dragStart;
    static bool dragging = false;
    const ember::Light current = o.light;

    int kind = current.type == ember::LightType::Point ? 1 : 0;
    const char* kinds[] = {"Sun", "Point"};
    if (ImGui::Combo("Kind", &kind, kinds, 2)) {
        ember::Light next = current;
        next.type = kind == 1 ? ember::LightType::Point : ember::LightType::Directional;
        undo.push(std::make_unique<LightCommand>(world, id, current, next));
        return;
    }

    float color[3] = {current.color.r, current.color.g, current.color.b};
    float strength = current.intensity;
    float range = current.radius;

    bool changed = false;
    bool began = false;
    bool ended = false;

    changed |= ImGui::ColorEdit3("Colour", color);
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::DragFloat("Strength", &strength, 0.05f, 0.0f, 1000.0f, "%.2f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    const bool hasRange = current.type == ember::LightType::Point;
    if (hasRange) {
        changed |= ImGui::DragFloat("Range", &range, 0.05f, 0.0f, 1000.0f, "%.2f");
        began |= ImGui::IsItemActivated();
        ended |= ImGui::IsItemDeactivated();
    }

    if (began) {
        dragStart = current;
        dragging = false;
    }
    if (changed) {
        ember::Light next = current;
        next.color = ember::Color{color[0], color[1], color[2], current.color.a};
        next.intensity = strength;
        next.radius = range;
        undo.push(std::make_unique<LightCommand>(world, id, dragStart, next, dragging));
        dragging = true;
    }
    if (ended) dragging = false;
}

class MaterialSlotCommand final : public Command {
public:
    MaterialSlotCommand(scene::World& world, ObjectId object, u32 slot,
                        const MaterialDesc& before, const MaterialDesc& after,
                        bool continuation)
        : world_(world), object_(object), slot_(slot), before_(before), after_(after),
          continuation_(continuation) {}

    void execute() override { world_.setMaterialSlot(object_, slot_, after_); }
    void undo() override { world_.setMaterialSlot(object_, slot_, before_); }
    const char* label() const override { return "Material"; }
    bool mergeWith(const Command& next) override {
        const auto* other = dynamic_cast<const MaterialSlotCommand*>(&next);
        if (!other || !other->continuation_ || other->object_ != object_ ||
            other->slot_ != slot_) return false;
        after_ = other->after_;
        return true;
    }

private:
    scene::World& world_;
    ObjectId object_;
    u32 slot_;
    MaterialDesc before_;
    MaterialDesc after_;
    bool continuation_;
};

class AddMaterialSlotCommand final : public Command {
public:
    AddMaterialSlotCommand(scene::World& world, ObjectId object, std::string name)
        : world_(world), object_(object), name_(std::move(name)) {}
    void execute() override {
        slot_ = world_.addMaterialSlot(object_, MaterialDesc{}, name_);
    }
    void undo() override { world_.removeMaterialSlot(object_, slot_); }
    const char* label() const override { return "Add Material Slot"; }
private:
    scene::World& world_;
    ObjectId object_;
    std::string name_;
    u32 slot_ = 0;
};

class MaterialSlotsStateCommand final : public Command {
public:
    MaterialSlotsStateCommand(scene::World& world, ObjectId object,
                              std::vector<ember::MaterialSlot> before,
                              std::vector<ember::MaterialSlot> after,
                              std::vector<u32> beforeFaces,
                              std::vector<u32> afterFaces)
        : world_(world), object_(object), before_(std::move(before)), after_(std::move(after)),
          beforeFaces_(std::move(beforeFaces)), afterFaces_(std::move(afterFaces)) {}
    void execute() override { apply(after_, afterFaces_); }
    void undo() override { apply(before_, beforeFaces_); }
    const char* label() const override { return "Remove Material Slot"; }
private:
    void apply(const std::vector<ember::MaterialSlot>& state,
               const std::vector<u32>& faceMaterials) {
        const u32 currentCount = world_.materialSlotCount(object_);
        for (u32 i = currentCount; i > 1; --i)
            world_.removeMaterialSlot(object_, i - 1);
        if (!state.empty()) {
            world_.setMaterialSlot(object_, 0, state[0].material);
            for (std::size_t i = 1; i < state.size(); ++i)
                world_.addMaterialSlot(object_, state[i].material, state[i].name);
        }
        if (world_.readableEdit(object_)) {
            scene::EditableMesh* edit = world_.editable(object_);
            const std::size_t count = std::min(edit->faces.size(), faceMaterials.size());
            for (std::size_t i = 0; i < count; ++i)
                edit->faces[i].materialIndex = faceMaterials[i];
        }
    }
    scene::World& world_;
    ObjectId object_;
    std::vector<ember::MaterialSlot> before_;
    std::vector<ember::MaterialSlot> after_;
    std::vector<u32> beforeFaces_;
    std::vector<u32> afterFaces_;
};

class FaceMaterialCommand final : public Command {
public:
    FaceMaterialCommand(scene::World& world, ObjectId object, std::vector<u32> faces,
                        std::vector<u32> before, u32 after)
        : world_(world), object_(object), faces_(std::move(faces)),
          before_(std::move(before)), after_(after) {}
    void execute() override { apply(after_); }
    void undo() override {
        if (!world_.readableEdit(object_)) return;
        scene::EditableMesh* edit = world_.editable(object_);
        if (!edit) return;
        for (std::size_t i = 0; i < faces_.size() && i < before_.size(); ++i)
            if (faces_[i] < edit->faces.size()) edit->faces[faces_[i]].materialIndex = before_[i];
    }
    const char* label() const override { return "Assign Material"; }

private:
    void apply(u32 material) {
        if (!world_.readableEdit(object_)) return;
        scene::EditableMesh* edit = world_.editable(object_);
        if (!edit) return;
        for (const u32 face : faces_)
            if (face < edit->faces.size()) edit->faces[face].materialIndex = material;
    }
    scene::World& world_;
    ObjectId object_;
    std::vector<u32> faces_;
    std::vector<u32> before_;
    u32 after_;
};

void drawMaterialSlot(scene::World& world, UndoStack& undo, ObjectId id, u32 slot,
                      const MaterialDesc& current) {
    static MaterialDesc dragStart;
    static bool dragging = false;
    static ObjectId dragObject;
    static u32 dragSlot = 0;
    if (dragObject != id || dragSlot != slot) {
        dragObject = id;
        dragSlot = slot;
        dragging = false;
    }

    float color[4] = {current.baseColor.r, current.baseColor.g, current.baseColor.b,
                      current.baseColor.a};
    float metallic = current.metallic;
    float roughness = current.roughness;
    float emissionColor[3] = {current.emissionColor.r, current.emissionColor.g,
                              current.emissionColor.b};
    float emissionStrength = current.emissionStrength;
    float transmission = current.transmission;
    float ior = current.ior;

    bool changed = false;
    bool began = false;
    bool ended = false;

    changed |= ImGui::ColorEdit4("Base color", color);
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f, "%.2f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.2f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::ColorEdit3("Emission color", emissionColor, ImGuiColorEditFlags_HDR);
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    changed |= ImGui::DragFloat("Emission strength", &emissionStrength, 0.05f,
                                0.0f, 1000.0f, "%.2f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("Transmission", &transmission, 0.0f, 1.0f, "%.2f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();

    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::DragFloat("IOR", &ior, 0.01f, 1.0f, 3.0f, "%.3f");
    began |= ImGui::IsItemActivated();
    ended |= ImGui::IsItemDeactivated();
    ImGui::TextDisabled("Cycles evaluates transmission and IOR; Raster currently ignores them.");

    // Keep the authored path verbatim, including relative paths. Resolution is
    // owned by the scene/render asset-base contract, not by the UI.
    static ObjectId pathObject;
    static u32 pathSlot = 0;
    static std::string imagePath;
    static bool pathEditing = false;
    if (pathObject != id || pathSlot != slot) {
        pathObject = id;
        pathSlot = slot;
        imagePath = current.baseColorTexture.path;
        pathEditing = false;
    } else if (!pathEditing) {
        // Reflect undo/redo or external edits when the field is not being typed.
        imagePath = current.baseColorTexture.path;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("Base Color Image", imagePath.data(), imagePath.capacity() + 1,
                     ImGuiInputTextFlags_CallbackResize, resizeStringInput, &imagePath);
    const bool pathActive = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (imagePath != current.baseColorTexture.path) {
            MaterialDesc next = current;
            next.baseColorTexture.path = imagePath;
            undo.push(std::make_unique<MaterialSlotCommand>(world, id, slot, current, next, false));
        }
        pathEditing = false;
    } else {
        pathEditing = pathActive;
    }

    static constexpr const char* colorSpaceNames[] = {"sRGB", "Linear"};
    int colorSpace = current.baseColorTexture.colorSpace == ImageColorSpace::Linear ? 1 : 0;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("Image Color Space", &colorSpace, colorSpaceNames, 2)) {
        MaterialDesc next = current;
        next.baseColorTexture.colorSpace = colorSpace == 1 ? ImageColorSpace::Linear
                                                           : ImageColorSpace::SRgb;
        undo.push(std::make_unique<MaterialSlotCommand>(world, id, slot, current, next, false));
    }
    ImGui::TextDisabled("Base-color images feed Raster and Cycles CPU; lighting and supported material features differ.");

    if (began) {
        dragStart = current;
        dragging = false;
    }
    if (changed) {
        MaterialDesc next = current;
        next.baseColor = Color{color[0], color[1], color[2], color[3]};
        next.metallic = metallic;
        next.roughness = roughness;
        next.emissionColor = Color{emissionColor[0], emissionColor[1], emissionColor[2],
                                   current.emissionColor.a};
        next.emissionStrength = emissionStrength;
        next.transmission = transmission;
        next.ior = ior;
        undo.push(std::make_unique<MaterialSlotCommand>(world, id, slot, dragStart, next, dragging));
        dragging = true;
    }
    if (ended) dragging = false;
}

} // namespace

namespace {

void drawWorldTab(scene::World& world, UndoStack& undo) {
    if (ImGui::CollapsingHeader(labels::Surface, ImGuiTreeNodeFlags_DefaultOpen)) {
        static ember::WorldBackground dragStart;
        static bool draggingColor = false;
        static bool draggingStrength = false;
        const ember::WorldBackground current = world.background();

        float color[4] = {current.color.r, current.color.g, current.color.b, current.color.a};
        const bool colorChanged = ImGui::ColorEdit4("Background", color);
        if (ImGui::IsItemActivated()) { dragStart = current; draggingColor = false; }
        if (colorChanged) {
            ember::WorldBackground next = current;
            next.color = ember::Color{color[0], color[1], color[2], color[3]};
            undo.push(std::make_unique<WorldBackgroundCommand>(world, dragStart, next, draggingColor));
            draggingColor = true;
        }
        if (ImGui::IsItemDeactivated()) draggingColor = false;

        float strength = current.strength;
        const bool strengthChanged = ImGui::DragFloat("Strength", &strength, 0.01f, 0.0f, 100.0f, "%.2f");
        if (ImGui::IsItemActivated()) { dragStart = current; draggingStrength = false; }
        if (strengthChanged) {
            ember::WorldBackground next = current;
            next.strength = strength;
            undo.push(std::make_unique<WorldBackgroundCommand>(world, dragStart, next, draggingStrength));
            draggingStrength = true;
        }
        if (ImGui::IsItemDeactivated()) draggingStrength = false;

        ember::WorldBackground next = current;
        if (ImGui::Checkbox("Use as ambient contribution", &next.lighting)) {
            undo.push(std::make_unique<WorldBackgroundCommand>(world, current, next));
        }
        ImGui::TextDisabled("Gradient backgrounds are not available yet.");

        static const scene::World* pathWorld = nullptr;
        static std::string environmentPath;
        static bool environmentPathEditing = false;
        if (pathWorld != &world) {
            pathWorld = &world;
            environmentPath = current.environmentTexture.path;
            environmentPathEditing = false;
        } else if (!environmentPathEditing) {
            environmentPath = current.environmentTexture.path;
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("Environment Image", environmentPath.data(), environmentPath.capacity() + 1,
                         ImGuiInputTextFlags_CallbackResize, resizeStringInput, &environmentPath);
        const bool pathActive = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (environmentPath != current.environmentTexture.path) {
                ember::WorldBackground imageNext = current;
                imageNext.environmentTexture.path = environmentPath;
                undo.push(std::make_unique<WorldBackgroundCommand>(world, current, imageNext));
            }
            environmentPathEditing = false;
        } else {
            environmentPathEditing = pathActive;
        }

        static constexpr const char* environmentColorSpaceNames[] = {"sRGB", "Linear"};
        int environmentColorSpace =
            current.environmentTexture.colorSpace == ImageColorSpace::Linear ? 1 : 0;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("Environment Color Space", &environmentColorSpace,
                         environmentColorSpaceNames, 2)) {
            ember::WorldBackground imageNext = current;
            imageNext.environmentTexture.colorSpace = environmentColorSpace == 1
                ? ImageColorSpace::Linear : ImageColorSpace::SRgb;
            undo.push(std::make_unique<WorldBackgroundCommand>(world, current, imageNext));
        }
        ImGui::TextDisabled("Raster displays the image with approximate ambient/specular lighting; Cycles CPU samples it as an environment.");
    }
}

void drawSceneTab(scene::World& world, UndoStack& undo) {
    if (ImGui::CollapsingHeader(labels::Units, ImGuiTreeNodeFlags_DefaultOpen)) {
        static float start = 1.0f;
        static bool dragging = false;
        float scale = world.unitScale();
        const bool changed = ImGui::DragFloat("Unit scale (m)", &scale, 0.01f, 0.001f, 10000.0f, "%.3f");
        if (ImGui::IsItemActivated()) { start = scale; dragging = false; }
        if (changed) {
            undo.push(std::make_unique<UnitScaleCommand>(world, start, scale, dragging));
            dragging = true;
        }
        if (ImGui::IsItemDeactivated()) dragging = false;
        ImGui::TextDisabled("Unit display system is not stored yet.");
    }
    if (ImGui::CollapsingHeader(labels::Scene, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Objects  %zu", world.objects().size());
        ImGui::Text("%ss  %zu", ember::UiVocabulary::Collection, world.collections().size());
    }
}

void drawObjectTab(scene::World& world, UndoStack& undo) {
    const std::vector<ObjectId>& selection = world.selection();
    if (selection.empty()) {
        ImGui::TextDisabled("Nothing selected");
        return;
    }
    const ObjectId id = selection.back();
    const scene::Object* o = world.get(id);
    if (!o) { ImGui::TextDisabled("Selection is stale"); return; }

    if (ImGui::CollapsingHeader(labels::Transform, ImGuiTreeNodeFlags_DefaultOpen))
        drawTransform(world, undo, id, *o);
    if (ImGui::CollapsingHeader(labels::Visibility, ImGuiTreeNodeFlags_DefaultOpen)) {
        bool visible = o->visible;
        if (ImGui::Checkbox("Visible", &visible) && visible != o->visible)
            undo.push(std::make_unique<VisibilityCommand>(world, id, o->visible, visible));
    }
    if (o->isLight && ImGui::CollapsingHeader(labels::Light, ImGuiTreeNodeFlags_DefaultOpen))
        drawLight(world, undo, id, *o);
    if (!o->isLight && o->hasMesh &&
        ImGui::CollapsingHeader((std::string(ember::UiVocabulary::Modifier) + "s").c_str()))
        drawModifiers(world, undo, id);
}

void drawCameraTab(scene::World& world, UndoStack& undo) {
    const std::vector<ObjectId>& selection = world.selection();
    if (selection.empty()) {
        ImGui::TextDisabled("Select a Camera object");
        return;
    }
    const ObjectId id = selection.back();
    const scene::Object* object = world.get(id);
    if (!object) { ImGui::TextDisabled("Selection is stale"); return; }
    if (!object->camera.isCamera) {
        ImGui::TextDisabled("Selected object is not a Camera");
        return;
    }

    static ObjectId dragObject;
    static CameraSettings dragStart[4];
    static bool dragging[4] = {};
    if (dragObject != id) {
        dragObject = id;
        for (bool& active : dragging) active = false;
    }

    const CameraSettings current = object->camera;
    float values[4] = {ember::degrees(current.fovY), current.clipNear,
                       current.clipFar, current.orthoScale};
    bool changed[4] = {};
    bool began[4] = {};
    bool ended[4] = {};

    ImGui::SetNextItemWidth(-1.0f);
    changed[0] = ImGui::DragFloat("Field of View", &values[0], 0.25f, 1.0f, 179.0f, "%.2f deg");
    began[0] = ImGui::IsItemActivated(); ended[0] = ImGui::IsItemDeactivated();
    ImGui::SetNextItemWidth(-1.0f);
    changed[1] = ImGui::DragFloat("Clip Start", &values[1], 0.01f, 0.001f,
                                  std::max(0.002f, current.clipFar - 0.001f), "%.3f");
    began[1] = ImGui::IsItemActivated(); ended[1] = ImGui::IsItemDeactivated();
    ImGui::SetNextItemWidth(-1.0f);
    changed[2] = ImGui::DragFloat("Clip End", &values[2], 1.0f,
                                  current.clipNear + 0.001f, 1000000.0f, "%.3f");
    began[2] = ImGui::IsItemActivated(); ended[2] = ImGui::IsItemDeactivated();
    ImGui::SetNextItemWidth(-1.0f);
    changed[3] = ImGui::DragFloat("Orthographic Scale", &values[3], 0.05f,
                                  0.001f, 1000000.0f, "%.3f");
    began[3] = ImGui::IsItemActivated(); ended[3] = ImGui::IsItemDeactivated();

    for (int i = 0; i < 4; ++i) {
        if (began[i]) { dragStart[i] = current; dragging[i] = false; }
        if (changed[i]) {
            CameraSettings next = current;
            switch (i) {
                case 0: next.fovY = ember::radians(values[i]); break;
                case 1: next.clipNear = std::min(values[i], current.clipFar - 0.001f); break;
                case 2: next.clipFar = std::max(values[i], current.clipNear + 0.001f); break;
                case 3: next.orthoScale = values[i]; break;
            }
            undo.push(std::make_unique<CameraSettingsCommand>(world, id, dragStart[i], next,
                                                               dragging[i]));
            dragging[i] = true;
        }
        if (ended[i]) dragging[i] = false;
    }

    CameraSettings next = current;
    if (ImGui::Checkbox("Orthographic", &next.orthographic))
        undo.push(std::make_unique<CameraSettingsCommand>(world, id, current, next));

    if (world.activeCamera() == id) {
        ImGui::TextUnformatted("Active Camera");
    } else if (ImGui::Button("Set Active Camera")) {
        undo.push(std::make_unique<ActiveCameraCommand>(world, world.activeCamera(), id));
    }
}

void materialSlots(scene::World& world, UndoStack& undo, ObjectId id,
                   InteractionMode interaction) {
    u32 count = 0;
    const ember::MaterialSlot* slots = world.materialSlots(id, count);
    static ObjectId activeObject;
    static const scene::World* activeWorld = nullptr;
    static u32 activeSlot = 0;
    if (activeWorld != &world || activeObject != id) {
        activeWorld = &world;
        activeObject = id;
        activeSlot = 0;
    }
    if (count == 0 || !slots) {
        ImGui::TextDisabled("This mesh has no material slots.");
        return;
    }
    if (activeSlot >= count) activeSlot = count - 1;

    if (ImGui::CollapsingHeader("Material Slots", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("##material-slot-list", ImVec2(0.0f, theme::metric(96.0f)), true);
        for (u32 i = 0; i < count; ++i) {
            ImGui::PushID(static_cast<int>(i));
            const char* rowName = slots[i].name.empty() ? "Material" : slots[i].name.c_str();
            theme::pushSelectedObjectColors();
            if (ImGui::Selectable(rowName, activeSlot == i)) activeSlot = i;
            theme::popHeaderColors();
            ImGui::PopID();
        }
        ImGui::EndChild();

        const float buttonWidth = (ImGui::GetContentRegionAvail().x - theme::metric(4.0f)) * 0.5f;
        if (ImGui::Button("+", ImVec2(buttonWidth, 0.0f))) {
            const std::string name = "Material " + std::to_string(count + 1);
            undo.push(std::make_unique<AddMaterialSlotCommand>(world, id, name));
            activeSlot = world.materialSlotCount(id) - 1;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(activeSlot == 0);
        if (ImGui::Button("−", ImVec2(buttonWidth, 0.0f))) {
            std::vector<ember::MaterialSlot> before(slots, slots + count);
            std::vector<ember::MaterialSlot> after = before;
            after.erase(after.begin() + activeSlot);
            std::vector<u32> beforeFaces;
            std::vector<u32> afterFaces;
            if (const scene::EditableMesh* edit = world.readableEdit(id)) {
                beforeFaces.reserve(edit->faces.size());
                afterFaces.reserve(edit->faces.size());
                for (const scene::Face& face : edit->faces) {
                    beforeFaces.push_back(face.materialIndex);
                    u32 remapped = face.materialIndex;
                    if (remapped == activeSlot) remapped = 0;
                    else if (remapped > activeSlot) --remapped;
                    afterFaces.push_back(remapped);
                }
            }
            undo.push(std::make_unique<MaterialSlotsStateCommand>(world, id,
                                                                  std::move(before),
                                                                  std::move(after),
                                                                  std::move(beforeFaces),
                                                                  std::move(afterFaces)));
            const u32 remaining = world.materialSlotCount(id);
            activeSlot = remaining == 0 ? 0 : std::min(activeSlot, remaining - 1);
        }
        ImGui::EndDisabled();
    }

    // Re-query after add/remove because either operation can reallocate the
    // object-local slot vector.
    slots = world.materialSlots(id, count);
    if (count == 0 || !slots) return;
    if (activeSlot >= count) activeSlot = count - 1;
    ImGui::Text("Active: %s", slots[activeSlot].name.empty() ? "Material" : slots[activeSlot].name.c_str());
    drawMaterialSlot(world, undo, id, activeSlot, slots[activeSlot].material);

    const scene::EditableMesh* edit = world.readableEdit(id);
    const scene::MeshSelection* faceSelection = world.editReadSelection(id, SelectMode::Face);
    std::vector<u32> faces;
    if (edit && faceSelection) {
        for (const u32 face : faceSelection->indices)
            if (face < edit->faces.size()) faces.push_back(face);
    }
    const bool canAssign = interaction == InteractionMode::Edit && !faces.empty();
    ImGui::BeginDisabled(!canAssign);
    if (ImGui::Button("Assign", ImVec2(-1.0f, 0.0f))) {
        std::vector<u32> before;
        before.reserve(faces.size());
        for (const u32 face : faces) before.push_back(edit->faces[face].materialIndex);
        undo.push(std::make_unique<FaceMaterialCommand>(world, id, std::move(faces),
                                                         std::move(before), activeSlot));
    }
    ImGui::EndDisabled();
    if (!canAssign) ImGui::TextDisabled("Enter Edit Mode and select faces to assign this material.");

}

void drawMaterialTab(scene::World& world, UndoStack& undo, InteractionMode interaction) {
    const std::vector<ObjectId>& selection = world.selection();
    if (selection.empty()) { ImGui::TextDisabled("Nothing selected"); return; }
    const ObjectId id = selection.back();
    const scene::Object* o = world.get(id);
    if (!o) { ImGui::TextDisabled("Selection is stale"); return; }
    if (o->isLight) { ImGui::TextDisabled("Light colour is in the Object tab."); return; }
    if (!o->hasMesh) { ImGui::TextDisabled("Material slots require a mesh object."); return; }
    materialSlots(world, undo, id, interaction);
}

void drawRenderEngineSettings(scene::World& world) {
    ember::SceneRenderSettings settings = world.renderSettings();
    int engine = settings.engine == ember::RenderEngine::Cycles ? 1 : 0;
    const char* engines[] = {"Raster", "Cycles"};
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("Render Engine", &engine, engines, 2)) {
        settings.engine = engine == 1 ? ember::RenderEngine::Cycles : ember::RenderEngine::Raster;
        world.setRenderSettings(settings);
        settings = world.renderSettings();
    }

    if (settings.engine != ember::RenderEngine::Cycles) return;

    ImGui::TextDisabled("Cycles uses the CPU and progressively refines the Rendered viewport.");
    const ember::u32 minSamples = 1;
    const ember::u32 maxSamples = 1u << 24;
    const ember::u32 minBounces = 1;
    const ember::u32 maxBounces = 64;
    bool changed = false;
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::DragScalar("Samples per Pixel", ImGuiDataType_U32,
                                 &settings.samplesPerPixel, 1.0f,
                                 &minSamples, &maxSamples, "%u");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::DragScalar("Max Bounces", ImGuiDataType_U32, &settings.maxBounces,
                                 1.0f, &minBounces, &maxBounces, "%u");
    ImGui::SetNextItemWidth(-1.0f);
    const ember::u32 minSeed = 0;
    const ember::u32 maxSeed = 0xffffffffu;
    changed |= ImGui::DragScalar("Seed", ImGuiDataType_U32, &settings.seed,
                                 1.0f, &minSeed, &maxSeed, "%u");
    if (changed) world.setRenderSettings(settings);
}

void drawRenderTab(scene::World& world, ember::ViewportSettings& viewport) {
    if (!std::isfinite(viewport.compareSplit)) viewport.compareSplit = 0.5f;
    viewport.compareSplit = std::clamp(viewport.compareSplit, 0.05f, 0.95f);
    if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen))
        drawRenderEngineSettings(world);

    if (ImGui::CollapsingHeader("Viewport Compare", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Raster / Cycles Compare", &viewport.compareRasterCycles);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Compare Split", &viewport.compareSplit, 0.05f, 0.95f, "%.2f");
        ImGui::TextDisabled("Rendered shading only; does not change the Scene Render Engine.");
        ImGui::TextDisabled("Cycles CPU output is a viewport preview, not a production render.");
    }

    if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
        ember::ShadowSettings& shadows = viewport.shadows;
        ImGui::Checkbox("Enable Shadows", &shadows.enabled);

        ImGui::BeginDisabled(!shadows.enabled);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("Shadow Map Resolution", &shadows.resolution, 256, 4096, "%d px");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Depth Bias", &shadows.bias, 0.0f, 0.05f, "%.4f");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Normal Bias", &shadows.normalBias, 0.0f, 1.0f, "%.3f");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("Maximum Distance", &shadows.maxDistance, 1.0f, 10000.0f, "%.1f units");
        ImGui::EndDisabled();

        ImGui::TextDisabled("Used in Rendered shading with a directional light and a shadow caster.");
    }
}

void drawOutputTab(FileDialog& file, DocumentState& doc, TimelineState& timeline) {
    if (ImGui::CollapsingHeader("Scene File", ImGuiTreeNodeFlags_DefaultOpen)) {
        static std::string bufferFor;
        static char path[260] = {};
        if (bufferFor != doc.path) {
            std::snprintf(path, sizeof(path), "%s", doc.path.c_str());
            bufferFor = doc.path;
        }

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("Scene File Path", path, sizeof(path))) {
            doc.path = path;
            bufferFor = doc.path;
        }
        if (ImGui::Button("Save As...")) openFileDialog(file, doc, false);
    }

    if (ImGui::CollapsingHeader("Frame Range", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragInt("Start Frame", &timeline.start, 1.0f, -100000, 100000);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragInt("End Frame", &timeline.end, 1.0f, -100000, 100000);
        if (timeline.end < timeline.start) timeline.end = timeline.start;
        timeline.current = std::clamp(timeline.current, timeline.start, timeline.end);
    }
}

} // namespace

void drawProperties(scene::World& world, UndoStack& undo, PropertiesTab& tab,
                    ember::ViewportSettings& viewport, FileDialog& file, DocumentState& doc,
                    TimelineState& timeline, InteractionMode interaction) {
    beginDockedPanel(ember::UiVocabulary::Properties, nullptr, ImGuiWindowFlags_NoTitleBar);

    static constexpr const char* names[] = {labels::Render, labels::Output, labels::Scene, labels::World,
                                              labels::Object, labels::Material, labels::Camera};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(theme::metric(4.0f), theme::metric(3.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(theme::metric(4.0f), theme::metric(3.0f)));
    ImGui::BeginChild("##properties-icons", ImVec2(theme::metric(theme::propertiesTabStripWidth), 0.0f),
                      false, ImGuiWindowFlags_None);
    // Seven category glyphs fit the default Properties Area rail without
    // scrolling; shorter windows still use the child rail's scrollbar.
    const float rowHeight = theme::metric(24.0f);
    for (std::size_t i = 0; i < kPropertiesTabOrder.size(); ++i) {
        const PropertiesTab candidate = kPropertiesTabOrder[i];
        ImGui::PushID(static_cast<int>(i));
        const bool wasActive = tab == candidate;
        theme::pushActiveItemColors();
        if (ImGui::Selectable("##category", wasActive, 0, ImVec2(0.0f, rowHeight))) tab = candidate;
        const bool active = tab == candidate;
        const ImVec2 buttonMin = ImGui::GetItemRectMin();
        const ImVec2 buttonMax = ImGui::GetItemRectMax();
        ImGui::SetItemTooltip("%s", names[i]);
        const ImU32 glyphColor = ImGui::GetColorU32(active ? theme::color(theme::tokens().active)
                                                           : theme::color(theme::tokens().textPrimary));
        drawIcon(*ImGui::GetWindowDrawList(), iconForTab(candidate), buttonMin, buttonMax, glyphColor,
                 theme::tokens().uiScale);
        theme::popHeaderColors();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, theme::metric(4.0f));
    ImGui::BeginChild("##properties-scroll", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextUnformatted(names[static_cast<int>(tab)]);
    ImGui::Separator();
    switch (tab) {
        case PropertiesTab::Render:
            drawRenderTab(world, viewport);
            break;
        case PropertiesTab::Output:
            drawOutputTab(file, doc, timeline);
            break;
        case PropertiesTab::Scene:
            drawSceneTab(world, undo);
            break;
        case PropertiesTab::World:
            drawWorldTab(world, undo);
            break;
        case PropertiesTab::Object:
            drawObjectTab(world, undo);
            break;
        case PropertiesTab::Material:
            drawMaterialTab(world, undo, interaction);
            break;
        case PropertiesTab::Camera:
            drawCameraTab(world, undo);
            break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    endDockedPanel();
}

void drawProperties(scene::World& world, UndoStack& undo, PropertiesTab& tab,
                    ember::ViewportSettings& viewport) {
    static FileDialog file;
    static DocumentState doc;
    static TimelineState timeline;
    drawProperties(world, undo, tab, viewport, file, doc, timeline);
}

void drawProperties(scene::World& world, UndoStack& undo, PropertiesTab& tab) {
    static ember::ViewportSettings standaloneViewport;
    drawProperties(world, undo, tab, standaloneViewport);
}

void drawProperties(scene::World& world, UndoStack& undo) {
    static PropertiesTab tab = PropertiesTab::Object;
    drawProperties(world, undo, tab);
}

} // namespace ember::editor
