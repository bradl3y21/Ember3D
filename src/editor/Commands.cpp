#include "editor/Commands.h"

namespace ember::editor {

namespace {

void restoreSelection(scene::World& world, const std::vector<ObjectId>& ids) {
    world.clearSelection();
    for (ObjectId id : ids) world.select(id, true);
}

} // namespace

// ---- TransformCommand ------------------------------------------------------

TransformCommand::TransformCommand(scene::World& world, ObjectId id, const Transform& before,
                                   const Transform& after, bool continuation)
    : world_(world), id_(id), before_(before), after_(after), continuation_(continuation) {}

void TransformCommand::execute() { world_.setTransform(id_, after_); }
void TransformCommand::undo() { world_.setTransform(id_, before_); }

bool TransformCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const TransformCommand*>(&next);
    if (!n || !n->continuation_ || n->id_ != id_) return false;
    after_ = n->after_;
    return true;
}

// ---- EditVertexPositionsCommand --------------------------------------------

EditVertexPositionsCommand::EditVertexPositionsCommand(
    scene::World& world, ObjectId object, std::vector<u32> vertices,
    std::vector<Vec3> before, std::vector<Vec3> after, bool continuation)
    : world_(world), object_(object), vertices_(std::move(vertices)),
      before_(std::move(before)), after_(std::move(after)),
      continuation_(continuation) {}

void EditVertexPositionsCommand::execute() {
    world_.editApplyVertexPositions(object_, vertices_, after_);
}

void EditVertexPositionsCommand::undo() {
    world_.editApplyVertexPositions(object_, vertices_, before_);
}

bool EditVertexPositionsCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const EditVertexPositionsCommand*>(&next);
    if (!n || !n->continuation_ || n->object_ != object_ || n->vertices_ != vertices_) return false;
    after_ = n->after_;
    return true;
}

// ---- EditTopologyCommand ---------------------------------------------------

EditTopologyCommand::EditTopologyCommand(
    scene::World& world, ObjectId object, scene::ObjectSnapshot before,
    scene::ObjectSnapshot after, std::vector<ObjectId> selectionBefore,
    std::vector<ObjectId> selectionAfter, SelectMode selectMode,
    std::vector<u32> editSelectionBefore,
    std::vector<u32> editSelectionAfter, std::string label)
    : world_(world), object_(object), before_(std::move(before)),
      after_(std::move(after)), selectionBefore_(std::move(selectionBefore)),
      selectionAfter_(std::move(selectionAfter)), selectMode_(selectMode),
      editSelectionBefore_(std::move(editSelectionBefore)),
      editSelectionAfter_(std::move(editSelectionAfter)), label_(std::move(label)) {}

void EditTopologyCommand::restore(const scene::ObjectSnapshot& snapshot,
                                  const std::vector<ObjectId>& selection,
                                  const std::vector<u32>& editSelection) {
    if (world_.get(object_)) world_.destroy(object_);
    world_.reinsert(snapshot);
    restoreSelection(world_, selection);
    world_.editSetSelection(object_, selectMode_, editSelection);
}

void EditTopologyCommand::execute() {
    // The first application already happened in the UI operation so it can
    // report a refused edit without pushing an empty undo entry.
    if (initialExecute_) {
        initialExecute_ = false;
        return;
    }
    restore(after_, selectionAfter_, editSelectionAfter_);
}

void EditTopologyCommand::undo() {
    restore(before_, selectionBefore_, editSelectionBefore_);
}

// ---- MaterialCommand -------------------------------------------------------

MaterialCommand::MaterialCommand(scene::World& world, ObjectId id, const MaterialDesc& before,
                                 const MaterialDesc& after, bool continuation)
    : world_(world), id_(id), before_(before), after_(after), continuation_(continuation) {}

void MaterialCommand::execute() { world_.setMaterial(id_, after_); }
void MaterialCommand::undo() { world_.setMaterial(id_, before_); }

bool MaterialCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const MaterialCommand*>(&next);
    if (!n || !n->continuation_ || n->id_ != id_) return false;
    after_ = n->after_;
    return true;
}

// ---- ModifierStackCommand ---------------------------------------------------

ModifierStackCommand::ModifierStackCommand(scene::World& world, ObjectId object,
                                           std::vector<ember::Modifier> before,
                                           std::vector<ember::Modifier> after,
                                           bool continuation)
    : world_(world), object_(object), before_(std::move(before)),
      after_(std::move(after)), continuation_(continuation) {}

void ModifierStackCommand::execute() { world_.setModifierStack(object_, after_); }
void ModifierStackCommand::undo() { world_.setModifierStack(object_, before_); }

bool ModifierStackCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const ModifierStackCommand*>(&next);
    if (!n || !n->continuation_ || n->object_ != object_) return false;
    after_ = n->after_;
    return true;
}

// ---- World settings ---------------------------------------------------------

WorldBackgroundCommand::WorldBackgroundCommand(scene::World& world,
                                               const ember::WorldBackground& before,
                                               const ember::WorldBackground& after,
                                               bool continuation)
    : world_(world), before_(before), after_(after), continuation_(continuation) {}

void WorldBackgroundCommand::execute() { world_.setBackground(after_); }
void WorldBackgroundCommand::undo() { world_.setBackground(before_); }

bool WorldBackgroundCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const WorldBackgroundCommand*>(&next);
    if (!n || !n->continuation_) return false;
    after_ = n->after_;
    return true;
}

UnitScaleCommand::UnitScaleCommand(scene::World& world, float before, float after,
                                   bool continuation)
    : world_(world), before_(before), after_(after), continuation_(continuation) {}

void UnitScaleCommand::execute() { world_.setUnitScale(after_); }
void UnitScaleCommand::undo() { world_.setUnitScale(before_); }

bool UnitScaleCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const UnitScaleCommand*>(&next);
    if (!n || !n->continuation_) return false;
    after_ = n->after_;
    return true;
}

// ---- LightCommand ----------------------------------------------------------

LightCommand::LightCommand(scene::World& world, ObjectId id, const ember::Light& before,
                           const ember::Light& after, bool continuation)
    : world_(world), id_(id), before_(before), after_(after), continuation_(continuation) {}

void LightCommand::execute() { world_.setLight(id_, after_); }
void LightCommand::undo() { world_.setLight(id_, before_); }

bool LightCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const LightCommand*>(&next);
    if (!n || !n->continuation_ || n->id_ != id_) return false;
    after_ = n->after_;
  return true;
}

// ---- Camera settings --------------------------------------------------------

CameraSettingsCommand::CameraSettingsCommand(scene::World& world, ObjectId id,
                                             const ember::CameraSettings& before,
                                             const ember::CameraSettings& after,
                                             bool continuation)
    : world_(world), id_(id), before_(before), after_(after),
      continuation_(continuation) {}

void CameraSettingsCommand::execute() { world_.setCameraSettings(id_, after_); }
void CameraSettingsCommand::undo() { world_.setCameraSettings(id_, before_); }

bool CameraSettingsCommand::mergeWith(const Command& next) {
    const auto* n = dynamic_cast<const CameraSettingsCommand*>(&next);
    if (!n || !n->continuation_ || n->id_ != id_) return false;
    after_ = n->after_;
    return true;
}

ActiveCameraCommand::ActiveCameraCommand(scene::World& world, ObjectId before, ObjectId after)
    : world_(world), before_(before), after_(after) {}

void ActiveCameraCommand::execute() { world_.setActiveCamera(after_); }
void ActiveCameraCommand::undo() { world_.setActiveCamera(before_); }

// ---- RenameCommand ---------------------------------------------------------

RenameCommand::RenameCommand(scene::World& world, ObjectId id, std::string before, std::string after)
    : world_(world), id_(id), before_(std::move(before)), after_(std::move(after)) {}

void RenameCommand::execute() {
    world_.setName(id_, after_);
    // setName uniquifies on clash, so keep the name the world actually stored.
    if (const scene::Object* o = world_.get(id_)) after_ = o->name;
}

void RenameCommand::undo() { world_.setName(id_, before_); }

// ---- VisibilityCommand -----------------------------------------------------

VisibilityCommand::VisibilityCommand(scene::World& world, ObjectId id, bool before, bool after)
    : world_(world), id_(id), before_(before), after_(after) {}

void VisibilityCommand::execute() { world_.setVisible(id_, after_); }
void VisibilityCommand::undo() { world_.setVisible(id_, before_); }

// ---- ParentCommand ---------------------------------------------------------

ParentCommand::ParentCommand(scene::World& world, ObjectId child, ObjectId before, ObjectId after)
    : world_(world), child_(child), before_(before), after_(after) {}

void ParentCommand::execute() { world_.setParent(child_, after_); }
void ParentCommand::undo() { world_.setParent(child_, before_); }

// ---- AddCommand ------------------------------------------------------------

AddCommand::AddCommand(scene::World& world, std::string label,
                       std::function<ObjectId(scene::World&)> make, bool additive)
    : world_(world), label_(std::move(label)), make_(std::move(make)), additive_(additive) {}

void AddCommand::execute() {
    selectionBefore_ = world_.selection();
    if (snapshot_) {
        id_ = world_.reinsert(*snapshot_);
    } else {
        id_ = make_(world_);
    }
    if (id_.valid()) world_.select(id_, additive_);
}

void AddCommand::undo() {
    if (!id_.valid()) return;
    snapshot_ = world_.captureSnapshot(id_);
    world_.destroy(id_);
    restoreSelection(world_, selectionBefore_);
}

// ---- DeleteCommand ---------------------------------------------------------

DeleteCommand::DeleteCommand(scene::World& world, ObjectId id) : world_(world), id_(id) {}

void DeleteCommand::execute() {
    selectionBefore_ = world_.selection();
    snapshot_ = world_.captureSnapshot(id_);
    if (!snapshot_) return;
    world_.destroy(id_);
}

void DeleteCommand::undo() {
    if (snapshot_) world_.reinsert(*snapshot_);
    restoreSelection(world_, selectionBefore_);
}

// ---- Collections -----------------------------------------------------------

MembershipCommand::MembershipCommand(scene::World& world, ObjectId id, scene::CollectionId before,
                                     scene::CollectionId after)
    : world_(world), id_(id), before_(before), after_(after) {}

void MembershipCommand::execute() { world_.assignToCollection(id_, after_); }
void MembershipCommand::undo() { world_.assignToCollection(id_, before_); }

CollectionVisibilityCommand::CollectionVisibilityCommand(scene::World& world, scene::CollectionId id,
                                                         bool before, bool after)
    : world_(world), id_(id), before_(before), after_(after) {}

void CollectionVisibilityCommand::execute() { world_.setCollectionVisible(id_, after_); }
void CollectionVisibilityCommand::undo() { world_.setCollectionVisible(id_, before_); }

CreateCollectionCommand::CreateCollectionCommand(scene::World& world, std::string name,
                                                 scene::CollectionId parent)
    : world_(world), name_(std::move(name)), parent_(parent) {}

void CreateCollectionCommand::execute() {
    if (snapshot_) {
        id_ = world_.reinsertCollection(*snapshot_);
    } else {
        id_ = world_.createCollection(name_, parent_);
    }
}

void CreateCollectionCommand::undo() {
    if (!id_.valid()) return;
    snapshot_ = world_.captureCollection(id_);
    world_.destroyCollection(id_);
}

DestroyCollectionCommand::DestroyCollectionCommand(scene::World& world, scene::CollectionId id)
    : world_(world), id_(id) {}

void DestroyCollectionCommand::execute() {
    snapshot_ = world_.captureCollection(id_);
    if (!snapshot_) return;
    world_.destroyCollection(id_);
}

void DestroyCollectionCommand::undo() {
    if (snapshot_) world_.reinsertCollection(*snapshot_);
}

// ---- MacroCommand ----------------------------------------------------------

MacroCommand::MacroCommand(std::string label) : label_(std::move(label)) {}

void MacroCommand::add(std::unique_ptr<Command> cmd) {
    if (cmd) commands_.push_back(std::move(cmd));
}

void MacroCommand::execute() {
    for (auto& cmd : commands_) cmd->execute();
}

void MacroCommand::undo() {
    for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) (*it)->undo();
}

} // namespace ember::editor
