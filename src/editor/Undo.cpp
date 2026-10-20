#include "editor/Undo.h"

namespace ember::editor {

void UndoStack::push(std::unique_ptr<Command> cmd) {
    if (!cmd) return;
    cmd->execute();
    undone_.clear();

    if (!done_.empty() && done_.back()->mergeWith(*cmd)) return;

    done_.push_back(std::move(cmd));
    if (done_.size() > maxDepth_) done_.erase(done_.begin());
}

bool UndoStack::undo() {
    if (done_.empty()) return false;
    done_.back()->undo();
    undone_.push_back(std::move(done_.back()));
    done_.pop_back();
    return true;
}

bool UndoStack::redo() {
    if (undone_.empty()) return false;
    undone_.back()->execute();
    done_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

void UndoStack::jumpTo(std::size_t position) {
    while (done_.size() > position && undo()) {
    }
    while (done_.size() < position && redo()) {
    }
}

bool UndoStack::discardLast() {
    if (done_.empty()) return false;
    done_.back()->undo();
    done_.pop_back();
    return true;
}

void UndoStack::clear() {
    done_.clear();
    undone_.clear();
}

const char* UndoStack::undoLabel() const {
    return done_.empty() ? nullptr : done_.back()->label();
}

const char* UndoStack::redoLabel() const {
    return undone_.empty() ? nullptr : undone_.back()->label();
}

} // namespace ember::editor
