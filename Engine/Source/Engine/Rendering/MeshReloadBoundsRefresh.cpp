#include "Engine/Rendering/MeshReloadBoundsRefresh.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{

MeshReloadBoundsRefreshReport RefreshLocalBoundsAfterMeshReload(
    ECS::World& world, RenderServices& renderServices,
    const std::vector<GUID>& reloadedModelGuids)
{
    MeshReloadBoundsRefreshReport report{};
    if (reloadedModelGuids.empty())
        return report;

    const auto& registry = renderServices.GetMeshGPURegistry();

    // LocalBounds is bound Read and written through the value gate below: a
    // Write binding would stamp every visited chunk whether or not the geometry
    // moved, and extraction probes this column.
    // An asset reload has to reach every entity that references the model; a
    // disabled one still shows its bounds in the editor and renders when it is
    // switched back on.
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::LocalBounds>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle entity,
                  const Components::MeshRenderer& meshRenderer,
                  const Components::LocalBounds& localBounds)
        {
            // Linear scan: the list holds the models reloaded in one drain,
            // which is one entry in every observed case and is driven by a
            // human saving a file.
            const GUID modelGuid = meshRenderer.modelAssetGuid.ToGuid();
            if (std::find(reloadedModelGuids.begin(), reloadedModelGuids.end(), modelGuid)
                == reloadedModelGuids.end())
                return;

            ++report.EntitiesVisited;

            // A cleared model reference has no geometry to derive from.
            if (meshRenderer.meshGpuHandleId == 0u)
                return;

            const ::GameEngine::Rendering::MeshGPUEntry* entry = registry.Find(
                ::GameEngine::Rendering::MeshGPUHandle(meshRenderer.meshGpuHandleId));
            if (!entry)
            {
                ++report.HandlesUnresolved;
                return;
            }

            if (std::memcmp(&localBounds.Box, &entry->bounds, sizeof(localBounds.Box)) == 0)
                return;

            if (auto* writable = world.GetComponentForWrite<Components::LocalBounds>(entity))
            {
                writable->Box = entry->bounds;
                ++report.BoundsRefreshed;
            }
        });

    return report;
}

} // namespace Engine::Renderer
} // namespace GameEngine
