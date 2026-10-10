#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/ECS.h"
#include "ECS/Entity.h" // defines ECS::World; ECS/World.h only forward-declares it

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor::MultiEntityUndo
{

// Snapshot target covering the same component on several entities, so one
// interactive edit over a multi-entity selection undoes as one step.
//
// Wire format: uint32 entity count, then per entity uint32 byte count followed
// by that entity's component bytes. Apply refuses a snapshot whose entity count
// or total length disagrees with the target list, so a selection change between
// capture and undo fails the restore instead of writing one entity's bytes onto
// another.
inline UndoRedoService::SnapshotTarget MakeComponentSnapshotTarget(
    ECS::World* world,
    std::vector<ECS::EntityHandle> entities,
    ECS::ComponentTypeId typeId,
    EditorChangeNotifications* notifications,
    std::string debugLabel)
{
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = std::move(debugLabel);

    auto sharedEntities =
        std::make_shared<std::vector<ECS::EntityHandle>>(std::move(entities));

    target.Capture = [world, entities = sharedEntities, typeId](UndoRedoService::SnapshotTarget::Snapshot& outSnapshot)
    {
        if (!world || entities->empty())
            return false;

        outSnapshot.clear();
        const uint32_t count = static_cast<uint32_t>(entities->size());
        outSnapshot.resize(4u);
        std::memcpy(outSnapshot.data(), &count, 4u);

        for (const auto& entity : *entities)
        {
            UndoRedoService::SnapshotTarget::Snapshot one;
            if (!world->CaptureComponentBytes(entity, typeId, one))
                return false;

            const uint32_t bytes = static_cast<uint32_t>(one.size());
            const size_t oldSize = outSnapshot.size();
            outSnapshot.resize(oldSize + 4u + one.size());
            std::memcpy(outSnapshot.data() + oldSize, &bytes, 4u);
            if (!one.empty())
                std::memcpy(outSnapshot.data() + oldSize + 4u, one.data(), one.size());
        }
        return true;
    };

    target.Apply = [world, entities = sharedEntities, typeId](const UndoRedoService::SnapshotTarget::Snapshot& snapshot)
    {
        if (!world || snapshot.size() < 4u)
            return false;

        uint32_t count = 0;
        std::memcpy(&count, snapshot.data(), 4u);
        if (count != entities->size())
            return false;

        size_t offset = 4u;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (offset + 4u > snapshot.size())
                return false;
            uint32_t bytes = 0;
            std::memcpy(&bytes, snapshot.data() + offset, 4u);
            offset += 4u;
            if (offset + bytes > snapshot.size())
                return false;

            UndoRedoService::SnapshotTarget::Snapshot one(bytes);
            if (bytes > 0u)
                std::memcpy(one.data(), snapshot.data() + offset, bytes);
            offset += bytes;

            if (!world->ApplyComponentBytesImmediate((*entities)[i], typeId, one))
                return false;
        }
        return offset == snapshot.size();
    };

    target.Notify = [notifications, world, entities = sharedEntities, typeId](EditorChangeNotifications::ChangeKind kind)
    {
        if (!notifications)
            return;

        for (const auto& entity : *entities)
        {
            EditorChangeNotifications::ComponentChangedEvent e;
            e.world = world;
            e.entity = entity;
            e.componentType = typeId;
            e.kind = kind;
            notifications->NotifyComponentChanged(e);
        }
    };

    return target;
}

} // namespace GameEngine::Editor::MultiEntityUndo
