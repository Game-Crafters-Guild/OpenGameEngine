#pragma once

#include "ECS/ChangeFilter.h"
#include "Rendering/Core/WindVolumeGPU.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine
{

/// Per-tick GPU snapshot of the world's wind volumes for the terrain grass upload.
///
/// The snapshot is rebuilt only when a WindVolume or WorldTransform column was
/// written since the last run, or when the world's archetype layout changed
/// (volumes added, removed, disabled, or re-enabled). Otherwise the previous
/// frame's records are returned and no query runs. The result vector keeps its
/// capacity across ticks.
class GrassWindVolumeExtractor
{
public:
    const std::vector<Rendering::WindVolumeGPU>& Extract(ECS::World& world);

private:
    bool HasChangedSince(ECS::World& world) const;

    std::vector<Rendering::WindVolumeGPU> m_Volumes;
    ECS::ChangeGate m_Gate{};
    std::size_t m_StructuralVersion = 0;
    bool m_Primed = false;
};

} // namespace GameEngine
