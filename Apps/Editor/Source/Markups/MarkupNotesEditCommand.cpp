#include "Markups/MarkupNotesEditCommand.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

// Removes the tags of `ids`, newest first; a tag something else holds by now stays.
void RemoveTags(MarkupECS::MarkupService& service, const std::vector<uint32>& ids)
{
    for (auto it = ids.rbegin(); it != ids.rend(); ++it)
        (void)service.RemoveTag(*it);
}

} // namespace

MarkupNotesEditCommand::MarkupNotesEditCommand(std::string label, ECS::World& world, ECS::EntityHandle entity,
                                               EditorChangeNotifications* notifications,
                                               MarkupECS::MarkupNotes notesBefore, Components::Markup markupBefore,
                                               MarkupECS::MarkupNotes notesAfter, Components::Markup markupAfter,
                                               std::vector<uint32> addedTags, std::function<void()> vocabularyChanged)
    : m_Label(std::move(label)),
      m_World(world),
      m_Entity(entity),
      m_Notifications(notifications),
      m_NotesBefore(std::move(notesBefore)),
      m_MarkupBefore(markupBefore),
      m_NotesAfter(std::move(notesAfter)),
      m_MarkupAfter(markupAfter),
      m_AddedTagIds(std::move(addedTags)),
      m_VocabularyChanged(std::move(vocabularyChanged))
{
    if (const auto* service = MarkupECS::MarkupService::TryGet())
    {
        for (const uint32 id : m_AddedTagIds)
            m_AddedTags.push_back(*service->GetTag(id));
    }
}

void MarkupNotesEditCommand::Do()
{
    AddTagsAgain();
    Apply(m_NotesAfter, m_MarkupAfter);
}

void MarkupNotesEditCommand::Undo()
{
    Apply(m_NotesBefore, m_MarkupBefore);
    auto* service = MarkupECS::MarkupService::TryGet();
    if (!service || m_AddedTagIds.empty())
        return;
    RemoveTags(*service, m_AddedTagIds);
    if (m_VocabularyChanged)
        m_VocabularyChanged();
}

// A tag added since the undo can have taken an added tag's id: the notes follow the id
// the name has now.
void MarkupNotesEditCommand::AddTagsAgain()
{
    auto* service = MarkupECS::MarkupService::TryGet();
    if (!service || m_AddedTags.empty())
        return;
    for (size_t i = 0; i < m_AddedTags.size(); ++i)
    {
        const MarkupECS::MarkupTag& tag = m_AddedTags[i];
        const uint32 id = service->AddTag(tag.Name, tag.Group, tag.Color);
        if (id == m_AddedTagIds[i])
            continue;
        std::replace(m_NotesAfter.Tags.begin(), m_NotesAfter.Tags.end(), m_AddedTagIds[i], id);
        m_AddedTagIds[i] = id;
    }
    if (m_VocabularyChanged)
        m_VocabularyChanged();
}

void MarkupNotesEditCommand::Apply(const MarkupECS::MarkupNotes& notes, const Components::Markup& markup)
{
    auto* service = MarkupECS::MarkupService::TryGet();
    if (!service || !m_World.IsValid(m_Entity) || !m_World.GetComponent<Components::Markup>(m_Entity))
        return;
    service->EnsureNotes(m_World, m_Entity) = notes;
    *m_World.GetComponentForWrite<Components::Markup>(m_Entity) = markup;
    if (m_Notifications)
        m_Notifications->NotifyComponentChange<Components::Markup>(&m_World, m_Entity,
                                                                   EditorChangeNotifications::ChangeKind::UndoRedo);
}

bool CommitMarkupNotesEdit(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                           EditorChangeNotifications* notifications, const std::string& label,
                           const std::function<bool()>& edit, const std::function<void()>& vocabularyChanged)
{
    auto* service = MarkupECS::MarkupService::TryGet();
    const auto* markup = world.GetComponent<Components::Markup>(entity);
    if (!service || !markup)
        return false;
    const MarkupECS::MarkupNotes* notes = service->FindNotes(world, entity);
    MarkupECS::MarkupNotes notesBefore = notes ? *notes : MarkupECS::MarkupNotes{};
    const Components::Markup markupBefore = *markup;
    const uint32 tagCountBefore = service->GetTagCount();
    const bool applied = edit();
    std::vector<uint32> addedTags;
    for (uint32 id = tagCountBefore; id < service->GetTagCount(); ++id)
        addedTags.push_back(id);
    if (!applied)
    {
        RemoveTags(*service, addedTags);
        return false;
    }
    if (!addedTags.empty() && vocabularyChanged)
        vocabularyChanged();
    if (undo)
    {
        undo->CommitAlreadyApplied(std::make_unique<MarkupNotesEditCommand>(
            label, world, entity, notifications, std::move(notesBefore), markupBefore,
            *service->FindNotes(world, entity), *world.GetComponent<Components::Markup>(entity),
            std::move(addedTags), vocabularyChanged));
    }
    if (notifications)
        notifications->NotifyComponentCommit<Components::Markup>(&world, entity);
    return true;
}

} // namespace GameEngine::Editor
