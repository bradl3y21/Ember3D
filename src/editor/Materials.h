// Materials.h — material slots: the undoable slot-list command, the built-in
// material library, and the Material section of Properties.
// Owner: Chisel · Editor. Scene API: World::materialSlots / setMaterialSlot /
// addMaterialSlot / removeMaterialSlot (D22(c)).
#pragma once

#include <vector>

#include "core/Types.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

namespace ember::editor {

// Replaces an object's whole slot list. Used for adding, removing, assigning and
// for slot colour edits on slots other than 0. Undo restores the old list, so
// each operation is one step, and a continuation merges one drag into one step.
class SlotsCommand final : public Command {
public:
    SlotsCommand(scene::World& world, ObjectId id, std::vector<MaterialDesc> before,
                 std::vector<MaterialDesc> after, bool continuation = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Material Slots"; }
    bool mergeWith(const Command& next) override;

private:
    scene::World& world_;
    ObjectId id_;
    std::vector<MaterialDesc> before_;
    std::vector<MaterialDesc> after_;
    bool continuation_;
};

struct LibraryMaterial {
    const char* name;
    MaterialDesc material;
};

// Built-in materials to assign from. MaterialDesc has no emission term (D12
// pins baseColor/metallic/roughness), so an emissive material cannot be
// represented yet and is not offered.
const std::vector<LibraryMaterial>& materialLibrary();

} // namespace ember::editor
