// Commands.h — every world mutation the editor makes, as one undoable command.
// Owner: Chisel · Editor.
//
// Rules the panels and menus follow:
//   * A panel or menu never calls a mutating World method directly. It builds
//     a command and pushes it on the UndoStack, which runs it.
//   * Property edits made by dragging are one command per frame, with
//     continuation = true after the first frame, so the whole drag is one undo
//     step. The UndoStack folds the continuation into the command below it.
//   * Delete and create capture an ObjectSnapshot (scene owns the mechanics) so
//     undo restores the same ids and hierarchy.
//
// Implementations live in Commands.cpp.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Types.h"
#include "core/Theme.h"
#include "editor/Undo.h"
#include "scene/Scene.h"

namespace ember::editor {

namespace command_labels {

inline const char* modifierStack() {
    static const std::string value = std::string(ember::UiVocabulary::Modifier) + " Stack";
    return value.c_str();
}

inline const char* moveToCollection() {
    static const std::string value = std::string("Move to ") + ember::UiVocabulary::Collection;
    return value.c_str();
}

inline const char* collectionVisibility() {
    static const std::string value = std::string(ember::UiVocabulary::Collection) + " Visibility";
    return value.c_str();
}

inline const char* newCollection() {
    static const std::string value = std::string("New ") + ember::UiVocabulary::Collection;
    return value.c_str();
}

inline const char* deleteCollection() {
    static const std::string value = std::string("Delete ") + ember::UiVocabulary::Collection;
    return value.c_str();
}

} // namespace command_labels

class TransformCommand final : public Command {
public:
    TransformCommand(scene::World& world, ObjectId id, const Transform& before,
                     const Transform& after, bool continuation = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Transform"; }
    bool mergeWith(const Command& next) override;

private:
    scene::World& world_;
    ObjectId id_;
    Transform before_;
    Transform after_;
    bool continuation_;
};

// Absolute edit-mesh vertex positions for one modal gesture. before/after are
// mesh-local coordinates captured from World; the World API updates the render
// MeshData. Continuations merge, so G/R/S on a vertex selection is one undo step.
class EditVertexPositionsCommand final : public Command {
public:
    EditVertexPositionsCommand(scene::World& world, ObjectId object,
                               std::vector<u32> vertices,
                               std::vector<Vec3> before,
                               std::vector<Vec3> after,
                               bool continuation = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Edit Vertices"; }
    bool mergeWith(const Command& next) override;

private:
    scene::World& world_;
    ObjectId object_;
    std::vector<u32> vertices_;
    std::vector<Vec3> before_;
    std::vector<Vec3> after_;
    bool continuation_;
};

// Reversible, discrete topology edit. Scene owns the topology mutations; the
// editor captures before/after object snapshots so the whole operation is one
// undo step without adding inverse topology APIs to World.
class EditTopologyCommand final : public Command {
public:
    EditTopologyCommand(scene::World& world, ObjectId object,
                        scene::ObjectSnapshot before, scene::ObjectSnapshot after,
                        std::vector<ObjectId> selectionBefore,
                        std::vector<ObjectId> selectionAfter, SelectMode selectMode,
                        std::vector<u32> editSelectionBefore,
                        std::vector<u32> editSelectionAfter, std::string label);
    void execute() override;
    void undo() override;
    const char* label() const override { return label_.c_str(); }

private:
    void restore(const scene::ObjectSnapshot& snapshot,
                 const std::vector<ObjectId>& selection,
                 const std::vector<u32>& editSelection);

    scene::World& world_;
    ObjectId object_;
    scene::ObjectSnapshot before_;
    scene::ObjectSnapshot after_;
    std::vector<ObjectId> selectionBefore_;
    std::vector<ObjectId> selectionAfter_;
    SelectMode selectMode_;
    std::vector<u32> editSelectionBefore_;
    std::vector<u32> editSelectionAfter_;
    std::string label_;
    bool initialExecute_ = true;
};

class MaterialCommand final : public Command {
public:
    MaterialCommand(scene::World& world, ObjectId id, const MaterialDesc& before,
                    const MaterialDesc& after, bool continuation = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Material"; }
    bool mergeWith(const Command& next) override;

private:
    scene::World& world_;
    ObjectId id_;
    MaterialDesc before_;
    MaterialDesc after_;
    bool continuation_;
};

// Snapshot command for modifier-stack edits. `continuation` coalesces frames of
// a single numeric-parameter drag into one undo step; discrete add/remove/move/
// enable actions use continuation=false.
class ModifierStackCommand final : public Command {
public:
    ModifierStackCommand(scene::World& world, ObjectId object,
                         std::vector<ember::Modifier> before,
                         std::vector<ember::Modifier> after,
                         bool continuation = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return command_labels::modifierStack(); }
    bool mergeWith(const Command& next) override;

private:
    scene::World& world_;
    ObjectId object_;
    std::vector<ember::Modifier> before_;
    std::vector<ember::Modifier> after_;
    bool continuation_;
};

class WorldBackgroundCommand final : public Command {
public:
    WorldBackgroundCommand(scene::World& world, const ember::WorldBackground& before,
                          const ember::WorldBackground& after, bool continuation = false);
    void execute() override;
    void undo() override;
    const char* label() const override { return "World Background"; }
    bool mergeWith(const Command& next) override;
private:
    scene::World& world_;
    ember::WorldBackground before_;
    ember::WorldBackground after_;
    bool continuation_;
};

class UnitScaleCommand final : public Command {
public:
    UnitScaleCommand(scene::World& world, float before, float after, bool continuation = false);
    void execute() override;
    void undo() override;
    const char* label() const override { return "Unit Scale"; }
    bool mergeWith(const Command& next) override;
private:
    scene::World& world_;
    float before_;
    float after_;
    bool continuation_;
};

class LightCommand final : public Command {
public:
    LightCommand(scene::World& world, ObjectId id, const ember::Light& before,
                 const ember::Light& after, bool continuation = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Light"; }
    bool mergeWith(const Command& next) override;

private:
    scene::World& world_;
    ObjectId id_;
    ember::Light before_;
    ember::Light after_;
    bool continuation_;
};

class CameraSettingsCommand final : public Command {
public:
    CameraSettingsCommand(scene::World& world, ObjectId id,
                          const ember::CameraSettings& before,
                          const ember::CameraSettings& after,
                          bool continuation = false);
    void execute() override;
    void undo() override;
    const char* label() const override { return "Camera Settings"; }
    bool mergeWith(const Command& next) override;
private:
    scene::World& world_;
    ObjectId id_;
    ember::CameraSettings before_;
    ember::CameraSettings after_;
    bool continuation_;
};

class ActiveCameraCommand final : public Command {
public:
    ActiveCameraCommand(scene::World& world, ObjectId before, ObjectId after);
    void execute() override;
    void undo() override;
    const char* label() const override { return "Set Active Camera"; }
private:
    scene::World& world_;
    ObjectId before_;
    ObjectId after_;
};

class RenameCommand final : public Command {
public:
    RenameCommand(scene::World& world, ObjectId id, std::string before, std::string after);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Rename"; }

private:
    scene::World& world_;
    ObjectId id_;
    std::string before_;
    std::string after_;
};

class VisibilityCommand final : public Command {
public:
    VisibilityCommand(scene::World& world, ObjectId id, bool before, bool after);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Visibility"; }

private:
    scene::World& world_;
    ObjectId id_;
    bool before_;
    bool after_;
};

class ParentCommand final : public Command {
public:
    ParentCommand(scene::World& world, ObjectId child, ObjectId before, ObjectId after);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Parent"; }

private:
    scene::World& world_;
    ObjectId child_;
    ObjectId before_;
    ObjectId after_;
};

// Adds one object. The first execute runs `make` (createPrimitive, duplicate,
// ...). Undo captures the object and destroys it; redo reinserts that snapshot,
// so the object keeps its id. The new object is selected on execute and the
// previous selection is restored on undo. `additive` adds it to the current
// selection instead of replacing it (used for the 2nd and later copies).
class AddCommand final : public Command {
public:
    AddCommand(scene::World& world, std::string label,
               std::function<ObjectId(scene::World&)> make, bool additive = false);

    void execute() override;
    void undo() override;
    const char* label() const override { return label_.c_str(); }

private:
    scene::World& world_;
    std::string label_;
    std::function<ObjectId(scene::World&)> make_;
    bool additive_;
    ObjectId id_;
    std::optional<scene::ObjectSnapshot> snapshot_;
    std::vector<ObjectId> selectionBefore_;
};

// Deletes one object and its subtree (children are re-parented by destroy).
// Each execute captures the subtree first, so undo reinserts the same ids.
class DeleteCommand final : public Command {
public:
    DeleteCommand(scene::World& world, ObjectId id);

    void execute() override;
    void undo() override;
    const char* label() const override { return "Delete"; }

private:
    scene::World& world_;
    ObjectId id_;
    std::optional<scene::ObjectSnapshot> snapshot_;
    std::vector<ObjectId> selectionBefore_;
};

// Moves one object into another collection (invalid = the implicit root).
class MembershipCommand final : public Command {
public:
    MembershipCommand(scene::World& world, ObjectId id, CollectionId before, CollectionId after);

    void execute() override;
    void undo() override;
    const char* label() const override { return command_labels::moveToCollection(); }

private:
    scene::World& world_;
    ObjectId id_;
    CollectionId before_;
    CollectionId after_;
};

class CollectionVisibilityCommand final : public Command {
public:
    CollectionVisibilityCommand(scene::World& world, CollectionId id, bool before, bool after);

    void execute() override;
    void undo() override;
    const char* label() const override { return command_labels::collectionVisibility(); }

private:
    scene::World& world_;
    CollectionId id_;
    bool before_;
    bool after_;
};

// Creates a collection. Undo captures it and destroys it; redo reinserts the
// snapshot so the collection keeps its id and members.
class CreateCollectionCommand final : public Command {
public:
    CreateCollectionCommand(scene::World& world, std::string name, CollectionId parent);

    void execute() override;
    void undo() override;
    const char* label() const override { return command_labels::newCollection(); }

private:
    scene::World& world_;
    std::string name_;
    CollectionId parent_;
    CollectionId id_;
    std::optional<scene::World::CollectionSnapshot> snapshot_;
};

// Destroys a collection and its subtree; members move to the parent. Undo
// reinserts the captured subtree with its members.
class DestroyCollectionCommand final : public Command {
public:
    DestroyCollectionCommand(scene::World& world, CollectionId id);

    void execute() override;
    void undo() override;
    const char* label() const override { return command_labels::deleteCollection(); }

private:
    scene::World& world_;
    CollectionId id_;
    std::optional<scene::World::CollectionSnapshot> snapshot_;
};

// Several commands as one undo step. Execute runs them in order; undo runs
// them in reverse.
class MacroCommand final : public Command {
public:
    explicit MacroCommand(std::string label);

    void add(std::unique_ptr<Command> cmd);
    bool empty() const { return commands_.empty(); }

    void execute() override;
    void undo() override;
    const char* label() const override { return label_.c_str(); }

private:
    std::string label_;
    std::vector<std::unique_ptr<Command>> commands_;
};

} // namespace ember::editor
