#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/ECS.h"
#include "MarkupECS/MarkupService.h"
#include "UndoRedo/IEditorCommand.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// One undo step for an edit of a mark-up's notes (description, tags, a comment, a status
// change): the notes and the Markup component before and after, and the tags the edit
// added to the project's vocabulary. Undo and redo restore both and announce the change,
// so the panel and the inspector rebuild; undo removes the added tags (unless something
// else holds them by then) and redo adds them again.
class MarkupNotesEditCommand final : public IEditorCommand
{
public:
    MarkupNotesEditCommand(std::string label, ECS::World& world, ECS::EntityHandle entity,
                           EditorChangeNotifications* notifications, MarkupECS::MarkupNotes notesBefore,
                           Components::Markup markupBefore, MarkupECS::MarkupNotes notesAfter,
                           Components::Markup markupAfter, std::vector<uint32> addedTags,
                           std::function<void()> vocabularyChanged);

    const char* GetName() const override { return m_Label.c_str(); }
    const char* GetTypeName() const override { return "MarkupNotesEditCommand"; }
    void Do() override;
    void Undo() override;

private:
    void Apply(const MarkupECS::MarkupNotes& notes, const Components::Markup& markup);
    void AddTagsAgain();

    std::string m_Label;
    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    EditorChangeNotifications* m_Notifications;
    MarkupECS::MarkupNotes m_NotesBefore;
    Components::Markup m_MarkupBefore;
    MarkupECS::MarkupNotes m_NotesAfter;
    Components::Markup m_MarkupAfter;
    // The vocabulary ids the edit added, oldest first, and their tags for a redo.
    std::vector<uint32> m_AddedTagIds;
    std::vector<MarkupECS::MarkupTag> m_AddedTags;
    std::function<void()> m_VocabularyChanged;
};

// Runs `edit` (MarkupService writers on `entity`, and any AddTag the edit needs) as one
// undo step named `label`, or directly when `undo` is null. Returns what `edit` returned; a
// refused edit records no step and removes the tags it added. `vocabularyChanged` (may be
// empty) runs whenever the step adds or removes tags, to save the vocabulary.
bool CommitMarkupNotesEdit(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                           EditorChangeNotifications* notifications, const std::string& label,
                           const std::function<bool()>& edit, const std::function<void()>& vocabularyChanged);

} // namespace GameEngine::Editor
