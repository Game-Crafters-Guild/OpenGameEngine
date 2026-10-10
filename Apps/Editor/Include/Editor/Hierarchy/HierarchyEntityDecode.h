#pragma once

#include "ECS/ECS.h"
#include "Editor/DragDropPayloads.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Interaction/Payload.h"

#include <cstdint>

namespace GameEngine::Editor
{

// Decode the first entity of a HierarchyEntityDragPayload. Mirrors
// HierarchyDataProvider::Encode: raw = index | (version << kEntityIndexBits).
// The single decode shared by every drop target that accepts hierarchy drags.
inline ECS::EntityHandle DecodeFirstHierarchyEntity(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<HierarchyEntityDragPayload>();
    if (!payload || payload->treeIds.empty())
        return ECS::EntityHandle();
    const std::uint32_t raw = static_cast<std::uint32_t>(payload->treeIds[0]);
    const ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
    const ECS::EntityVersion ver =
        static_cast<ECS::EntityVersion>((raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
    return ECS::EntityHandle(idx, ver);
}

} // namespace GameEngine::Editor
