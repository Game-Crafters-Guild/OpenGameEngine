#include "TerrainECS/GrassWindVolumeExtractor.h"

#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/WindVolumeResolver.h"

namespace GameEngine
{

bool GrassWindVolumeExtractor::HasChangedSince(ECS::World& world) const
{
    if (!m_Primed || !ECS::ChangeFilter::Enabled())
        return true;
    // Add/remove of a volume, and the Disabled tag, are archetype moves: the
    // structural version covers them because column stamps cannot.
    if (world.GetStructuralChangeVersion() != m_StructuralVersion)
        return true;
    bool changed = false;
    auto query = world.Query<ECS::Read<Components::WindVolume>,
                             ECS::Read<Components::WorldTransform>>();
    query.Changed<Components::WindVolume, Components::WorldTransform>(m_Gate);
    query.Each([&](const Components::WindVolume&, const Components::WorldTransform&)
    {
        changed = true;
    });
    return changed;
}

const std::vector<Rendering::WindVolumeGPU>& GrassWindVolumeExtractor::Extract(ECS::World& world)
{
    // Entry-sample the gate before iterating (ChangeGate contract): a writer
    // that stamps during this tick is seen on the next run, never skipped.
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    if (!HasChangedSince(world))
        return m_Volumes;

    Engine::Renderer::WindVolumeResolver::ExtractGPU(world, m_Volumes);
    m_Gate.LastRunVersion = entryVersion;
    m_StructuralVersion = structuralVersion;
    m_Primed = true;
    return m_Volumes;
}

} // namespace GameEngine
