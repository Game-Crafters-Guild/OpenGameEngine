#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

#include <cstddef>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// Per-rendering-owner change gates for BindChangedSkinnedMeshAnimation. A
// World change resets every gate; no World or component pointer is retained.
// Reset this value when model assets reload.
struct SkeletonResolveState
{
    struct ModelState
    {
        GUID ModelGuid;
        uint32 SkeletonId = 0; // Zero also represents an unavailable source.
    };
    uint64 WorldId = 0;
    uint64 WorldGeneration = 0;
    std::size_t StructuralVersion = 0;
    ECS::ChangeGate Skeletons, SkinnedMeshes, ModelMeshes;
    // Cache-only residency probes cover helper-only async loads and unloads;
    // unchanged frames never load assets or scan model entities for readiness.
    std::vector<ModelState> Models;
    // SkeletonRefs that name no model source and no skeleton, reported once
    // each; an entity leaves the list when it resolves or is destroyed.
    std::vector<ECS::EntityHandle> ReportedSourceless;
    // The resolve service's hand-back generation this state last saw
    // (SceneResolveService::HandBackGeneration).
    uint64 ResolveHandBacks = 0;
};

} // namespace GameEngine::Engine::Renderer
