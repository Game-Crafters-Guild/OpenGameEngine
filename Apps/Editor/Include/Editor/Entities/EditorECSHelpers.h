#pragma once

#include "ECS/ECSTemplates.h"
#include "EditorChangeNotifications.h"

namespace GameEngine::Editor
{

/// Write an updated component to the world and fire a commit notification.
/// Template parameter T is deduced from `updated` — callers never spell it out.
template<typename T>
void CommitComponentUpdate(ECS::World* world,
                                   ECS::EntityHandle entity,
                                   EditorChangeNotifications* notifications,
                                   const T& updated)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;
    world->AddComponentImmediate(entity, updated);
    if (notifications)
        notifications->NotifyComponentCommit<T>(world, entity);
}

/// Same world write as CommitComponentUpdate, but notifies subscribers with
/// ChangeKind::Preview (live drag / in-progress edit) instead of Commit.
template<typename T>
void PreviewComponentUpdate(ECS::World* world,
                            ECS::EntityHandle entity,
                            EditorChangeNotifications* notifications,
                            const T& updated)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;
    world->AddComponentImmediate(entity, updated);
    if (notifications)
        notifications->NotifyComponentChange<T>(world, entity, EditorChangeNotifications::ChangeKind::Preview);
}

} // namespace GameEngine::Editor
