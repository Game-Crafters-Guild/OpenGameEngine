#pragma once

// Did the terrains the world believes in actually reach the renderer?
//
// A terrain's ECS component carries live handles from the moment extraction provisions it, which
// is BEFORE extraction decides whether it can render the thing. Every consumer that asks "is a
// terrain active" through the component therefore answers yes for a terrain that draws nothing.
// The renderer's own published set is the authority, and the gap between the two is the only
// honest render-skip signal — so it is derived here, once, rather than inferred at each surface.
//
// Lives beside the terrain systems rather than inside the editor's IPC handler so the rule has
// one definition and can be unit-tested without a live editor (the handler TU compiles into no
// test binary).

#include "Components/Terrain/Terrain.h"
#include "ECS/Components.h" // ECS::Disabled
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

enum class TerrainPublicationState : uint32
{
    NoTerrain,        // nothing enabled in the world — an empty scene, not a fault
    Published,        // every live terrain reached the renderer
    PartiallySkipped, // some reached it; the rest draw nothing
    AllSkipped,       // live terrains exist and NONE reached the renderer
};

// Terrains the world considers live, counted with the SAME filter the extraction system applies:
// the query engine's own exclusion of disabled entities and switched-off terrains. A terrain
// extraction never visits must not be counted, or one hierarchy disable fabricates a render
// fault — which is why this is one function rather than a filter re-typed at the call site.
inline uint32 CountLiveTerrains(ECS::World& world)
{
    uint32 live = 0;
    world.Query<ECS::Read<Components::Terrain>>()
        .Each([&](const Components::Terrain& terrain) {
            const bool hasHandle =
                terrain.TerrainDataHandle != 0 || terrain.TerrainDataGeneration != 0 ||
                terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0;
            if (hasHandle)
                ++live;
        });
    return live;
}

// liveCount counts terrain components carrying a terrain handle; publishedCount counts
// the render infos extraction actually handed the render feature this frame.
inline TerrainPublicationState DiagnoseTerrainPublication(uint32 liveCount, uint32 publishedCount)
{
    if (liveCount == 0u)
        return TerrainPublicationState::NoTerrain;
    if (publishedCount == 0u)
        return TerrainPublicationState::AllSkipped;
    if (publishedCount < liveCount)
        return TerrainPublicationState::PartiallySkipped;
    return TerrainPublicationState::Published;
}

inline const char* TerrainPublicationDescription(TerrainPublicationState state)
{
    switch (state)
    {
    case TerrainPublicationState::NoTerrain:
        return "NO TERRAIN: no enabled terrain carries a live handle in this scene. Tessellation "
               "figures below describe an idle renderer, not a fault.";
    case TerrainPublicationState::AllSkipped:
        return "NOT RENDERING: the scene has live terrain that never reached the renderer, so "
               "nothing is drawn and every tessellation figure below describes an empty tree. "
               "The extraction system skipped it — check the terrain log for the reason.";
    case TerrainPublicationState::PartiallySkipped:
        return "PARTIALLY RENDERING: some live terrain reached the renderer and some did not. "
               "The tessellation figures below cover only what was published.";
    case TerrainPublicationState::Published:
        break;
    }
    return "RENDERING: every live terrain reached the renderer.";
}

} // namespace GameEngine::TerrainECS
