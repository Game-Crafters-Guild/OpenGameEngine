#pragma once

#include <cstdint>

namespace GameEngine::Editor
{
// Editor-side command interface used by UndoRedoService.
// Commands are expected to be deterministic and reversible.
class IEditorCommand
{
public:
    virtual ~IEditorCommand() = default;

    // Human-readable label for debugging/UI (ownership stays with command).
    virtual const char* GetName() const = 0;

    // Stable type identifier for tooling/inspection (e.g. undo-stack dumps).
    // Defaults to a generic label; commands whose identity matters for debugging
    // (DeleteEntitiesCommand vs WorldSnapshotCommand, etc.) override it.
    virtual const char* GetTypeName() const { return "Command"; }

    // Apply the command.
    virtual void Do() = 0;

    // Revert the command.
    virtual void Undo() = 0;

    // Optional explicit redo hook (default re-applies Do()).
    virtual void Redo() { Do(); }

    // Optional merge/coalescing support (not required for interactive edits that
    // already commit a single command).
    virtual bool CanMergeWith(const IEditorCommand& /*other*/) const { return false; }
    virtual bool MergeWith(const IEditorCommand& /*other*/) { return false; }
};

} // namespace GameEngine::Editor


