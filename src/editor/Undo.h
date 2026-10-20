#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "editor/EditorIds.h"

namespace ember::editor {

// A reversible edit. Commands capture only what they change (e.g. old and new
// Transform for one object), never a whole-world snapshot.
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual const char* label() const = 0;
    // Called when a new command arrives while this one is on top. Return true
    // to absorb it (e.g. consecutive drag frames become one undo step).
    virtual bool mergeWith(const Command&) { return false; }
};

class UndoStack {
public:
    explicit UndoStack(std::size_t maxDepth = 256) : maxDepth_(maxDepth) {}

    // Runs the command and records it. Clears the redo history.
    void push(std::unique_ptr<Command> cmd);

    bool undo();
    bool redo();
    // Reverts the newest command and drops it from history without adding it to
    // redo. For cancelling a gesture that was already applied (Escape mid-drag).
    bool discardLast();
    void clear();

    // History for the undo panel. Positions count applied commands: 0 is the
    // original state, and doneCount() is the current one.
    std::size_t doneCount() const { return done_.size(); }
    std::size_t redoCount() const { return undone_.size(); }
    const char* doneLabelAt(std::size_t i) const { return done_[i]->label(); }      // oldest first
    const char* redoLabelAt(std::size_t i) const { return undone_[undone_.size() - 1 - i]->label(); }  // i = 0 is the next redo
    // Reaches `position` by repeated undo or redo, one command at a time. This is
    // not a direct restore: each step is applied in turn.
    void jumpTo(std::size_t position);

    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    const char* undoLabel() const;
    const char* redoLabel() const;

private:
    std::size_t maxDepth_;
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> undone_;
};

} // namespace ember::editor
