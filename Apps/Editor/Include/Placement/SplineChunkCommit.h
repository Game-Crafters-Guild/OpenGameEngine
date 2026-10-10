#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Types/Types.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
struct Mesh;
}
namespace GameEngine::Components
{
struct MeshRenderer;
}
namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// How one spline recipe's generated meshes reach the screen and leave it.
//
// A mesh is registered under a stable key and re-registered IN PLACE on a
// rebuild, which is the mesh registry's own update path: the handle, the
// GPUScene row and every entity drawing it survive, so a steady-state rebuild
// churns no entities. A key the new build no longer reaches is released.
// There is no refcount on a registered submesh, so the recipe that registered
// it owns the release — through here, and nowhere else.
//
// Two shapes of output use it. A swept recipe draws each mesh with a chunk
// entity of its own, addressed by slot (CommitSlots). The fence draws its cut
// span variants on the span entities it already spawns, so it registers keyed
// meshes only (Register, Keep, RetireUnreached).
class SplineChunkCommit
{
public:
    // Registers `mesh` under `key` — an in-place update when the key is
    // registered already — and keeps the key through the build in progress.
    // The CPU mesh is retained: without it the entry is tombstoned on the next
    // device rebuild, and editor picking has nothing to intersect.
    Rendering::MeshGPUHandle Register(Rendering::MeshGPURegistry& registry,
                                      const Rendering::MeshGPUKey& key, const Mesh& mesh);

    // Keeps a key an earlier build registered, without uploading it again: for
    // a mesh that is a pure function of its key. False when the key is not
    // registered.
    bool Keep(const Rendering::MeshGPUKey& key, Rendering::MeshGPUHandle& outHandle);

    // Releases every registered key the build in progress neither registered
    // nor kept, and starts the next build.
    void RetireUnreached(Rendering::MeshGPURegistry& registry);

    // One mesh per chunk slot, each drawn by a child entity of `owner` with an
    // identity transform, `renderer` and bounds from the mesh; slot c's key is
    // (keySpace, c). Slots are addressed BY INDEX (SplineChunkSlots.h): a mesh
    // with no triangles leaves its slot a hole rather than shifting every later
    // chunk onto its neighbour's key. Returns true when an entity was spawned
    // or destroyed.
    bool CommitSlots(ECS::World& world, Rendering::MeshGPURegistry& registry, ECS::EntityHandle owner,
                     const GUID& keySpace, std::span<const Mesh> meshes,
                     const Components::MeshRenderer& renderer, bool castShadows);

    // Whether every chunk entity the last commit spawned is still alive. A
    // hole is not a dead chunk: reading it as one would re-arm the rebuild
    // every frame for as long as the hole exists.
    bool ChunksAlive(const ECS::World& world) const;

    // Destroys every chunk entity and releases every key. Returns true when an
    // entity was destroyed.
    bool Retire(ECS::World& world, Rendering::MeshGPURegistry* registry);

    // After a world reset: the chunk handles name entities of a world that no
    // longer exists, and a version restart makes them alias live entities of
    // whatever replaced it, so they are forgotten, never destroyed. The keys
    // are not handle-based and are still released.
    void DropAfterWorldReset(Rendering::MeshGPURegistry* registry);

private:
    void ReleaseKeys(Rendering::MeshGPURegistry* registry);

    struct RegisteredKey
    {
        Rendering::MeshGPUHandle Handle;
        bool Reached = false;
    };

    std::vector<ECS::EntityHandle> m_Chunks;
    std::unordered_map<Rendering::MeshGPUKey, RegisteredKey> m_Keys;
};

} // namespace GameEngine::Editor
