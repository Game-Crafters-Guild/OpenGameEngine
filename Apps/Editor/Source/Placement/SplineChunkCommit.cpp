#include "Placement/SplineChunkCommit.h"

#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/Vector3.h"
#include "Placement/PieceEntity.h"
#include "Placement/SplineChunkSlots.h"

#include <algorithm>

namespace GameEngine::Editor
{

Rendering::MeshGPUHandle SplineChunkCommit::Register(Rendering::MeshGPURegistry& registry,
                                                     const Rendering::MeshGPUKey& key,
                                                     const Mesh& mesh)
{
    // Registration always yields a live handle (a failed upload registers
    // sentinel content under it), so there is no invalid-handle path to guard.
    const Rendering::MeshGPUHandle handle =
        registry.RegisterSubmesh(key, mesh, /*retainCpuMesh=*/true);
    m_Keys[key] = {handle, true};
    return handle;
}

bool SplineChunkCommit::Keep(const Rendering::MeshGPUKey& key, Rendering::MeshGPUHandle& outHandle)
{
    const auto found = m_Keys.find(key);
    if (found == m_Keys.end())
        return false;
    found->second.Reached = true;
    outHandle = found->second.Handle;
    return true;
}

void SplineChunkCommit::RetireUnreached(Rendering::MeshGPURegistry& registry)
{
    for (auto it = m_Keys.begin(); it != m_Keys.end();)
    {
        if (it->second.Reached)
        {
            it->second.Reached = false;
            ++it;
            continue;
        }
        registry.UnregisterSubmesh(it->first);
        it = m_Keys.erase(it);
    }
}

bool SplineChunkCommit::CommitSlots(ECS::World& world, Rendering::MeshGPURegistry& registry,
                                    ECS::EntityHandle owner, const GUID& keySpace,
                                    std::span<const Mesh> meshes,
                                    const Components::MeshRenderer& renderer, bool castShadows)
{
    std::vector<uint8> chunkHasGeometry(meshes.size(), 0u);
    for (size_t c = 0; c < meshes.size(); ++c)
        chunkHasGeometry[c] = !meshes[c].Vertices.empty() && !meshes[c].Indices.empty() ? 1u : 0u;

    // Which slot is created, updated in place, or retired is pure index
    // bookkeeping (SplineChunkSlots.h); this pass owns the side effects.
    std::vector<uint8> slotHasLiveEntity(m_Chunks.size(), 0u);
    for (size_t c = 0; c < m_Chunks.size(); ++c)
        slotHasLiveEntity[c] = world.IsValid(m_Chunks[c]) ? 1u : 0u;
    const ChunkSlotPlan plan = ReconcileChunkSlots(slotHasLiveEntity, chunkHasGeometry);

    bool spawnedOrRetired = false;
    for (size_t c = 0; c < plan.Slots.size() && c < m_Chunks.size(); ++c)
    {
        if (plan.Slots[c] != ChunkSlotAction::Retire)
            continue;
        // Either this stretch collapsed or the build no longer reaches this
        // slot: clear it rather than leave a stale mesh drawn over ground the
        // recipe no longer covers. Its key goes with RetireUnreached below.
        if (world.IsValid(m_Chunks[c]))
        {
            world.DestroyEntity(m_Chunks[c]);
            spawnedOrRetired = true;
        }
        m_Chunks[c] = ECS::EntityHandle{};
    }
    m_Chunks.resize(plan.ChunkCount, ECS::EntityHandle{});

    for (uint32 c = 0; c < plan.ChunkCount; ++c)
    {
        if (plan.Slots[c] == ChunkSlotAction::Retire)
            continue;

        const Mesh& mesh = meshes[c];
        Components::MeshRenderer chunkRenderer = renderer;
        chunkRenderer.meshGpuHandleId =
            static_cast<uint64>(Register(registry, Rendering::MeshGPUKey{keySpace, c}, mesh));

        Components::LocalBounds bounds{};
        bounds.Box = Mathematics::BoundingBox::FromMinMax(
            Mathematics::Vector3(mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]),
            Mathematics::Vector3(mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]));
        bounds.DynamicObject = false;
        bounds.CastShadows = castShadows;

        if (plan.Slots[c] == ChunkSlotAction::Update)
        {
            // The steady-state rebuild churns no entities: the mesh was
            // re-registered in place above, under the same handle.
            if (auto* existing = world.GetComponentForWrite<Components::MeshRenderer>(m_Chunks[c]))
                *existing = chunkRenderer;
            if (auto* existingBounds = world.GetComponentForWrite<Components::LocalBounds>(m_Chunks[c]))
                *existingBounds = bounds;
            continue;
        }

        ECS::Entity chunk = world.Create();
        // Identity local transform: the geometry is already in the owner's
        // local space, and the hierarchy system composes World = Parent * Local.
        chunk.Set(Components::Transform{});
        chunk.Set(chunkRenderer);
        chunk.Set(bounds);
        chunk.Set(Components::MeshGPUData{});
        chunk.Set(Components::RuntimeOnlyEntity{});
        chunk.Set(Components::Parent{owner});
        chunk.Set(MakePieceLabel("Section", c));
        m_Chunks[c] = chunk.GetHandle();
        spawnedOrRetired = true;
    }

    RetireUnreached(registry);
    return spawnedOrRetired;
}

bool SplineChunkCommit::ChunksAlive(const ECS::World& world) const
{
    return std::all_of(m_Chunks.begin(), m_Chunks.end(), [&world](ECS::EntityHandle chunk)
                       { return !chunk.IsValid() || world.IsValid(chunk); });
}

bool SplineChunkCommit::Retire(ECS::World& world, Rendering::MeshGPURegistry* registry)
{
    bool destroyedAny = false;
    for (ECS::EntityHandle chunk : m_Chunks)
    {
        if (world.IsValid(chunk))
        {
            world.DestroyEntity(chunk);
            destroyedAny = true;
        }
    }
    m_Chunks.clear();
    ReleaseKeys(registry);
    return destroyedAny;
}

void SplineChunkCommit::DropAfterWorldReset(Rendering::MeshGPURegistry* registry)
{
    m_Chunks.clear();
    ReleaseKeys(registry);
}

void SplineChunkCommit::ReleaseKeys(Rendering::MeshGPURegistry* registry)
{
    if (registry)
    {
        for (const auto& [key, registered] : m_Keys)
            registry->UnregisterSubmesh(key);
    }
    m_Keys.clear();
}

} // namespace GameEngine::Editor
