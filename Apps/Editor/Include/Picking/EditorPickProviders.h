#pragma once

// EditorPickProviders — scene-view picking for plugin-owned entities whose
// rendered mesh is runtime-generated (no MeshRenderer). A provider describes
// WHAT to pick as data (a synthetic mesh identity); every raycast mechanism
// (BVH cache, TLAS sectors, layer masks, ignore lists) stays inside
// Editor.exe's MeshPickingService — nothing from the MeshPicking module
// crosses the DLL boundary.
//
// Lives in EditorSDK.dll (one registry instance across Editor.exe and
// editor-kind package modules). Replace-by-typeId registration, matching the
// module hot-reload contract of the other editor registries.

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "ECS/ModuleRegistration.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor::Picking
{

// A runtime mesh presented to the picking service as if it were a
// MeshRenderer: the GPU mesh to raycast and the model GUID for BVH caching.
struct SyntheticMeshPick
{
    std::uint64_t MeshGpuHandleId = 0; // raw MeshGPUHandle id (0 = mesh not built)
    GUID ModelAssetGuid{};
    std::uint32_t RenderLayerMask = 1u;
};

struct EditorPickProvider
{
    // Per-entity resolve: fills `outPick` when the entity is currently
    // pickable through this provider (component present, enabled, mesh
    // built). Used for restricted-entity tests.
    std::function<bool(ECS::World&, ECS::EntityHandle, SyntheticMeshPick&)> Resolve;
    // Scene-wide enumeration: invoke `emit` for every pickable entity. The
    // provider owns the typed ECS query; disabled entities must be skipped.
    std::function<void(ECS::World&,
                       const std::function<void(ECS::EntityHandle, const SyntheticMeshPick&)>& emit)>
        Enumerate;
};

class EditorPickProviderRegistry
{
public:
    static EditorPickProviderRegistry& Get();

    void Register(ECS::ComponentTypeId typeId, EditorPickProvider provider);

    // Stable snapshot for iteration during a pick.
    std::vector<EditorPickProvider> Snapshot() const;

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every entry attributed to `moduleId` (provider callables are module code
    // and pin its images mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorPickProviderRegistry() = default;

    mutable std::mutex m_Mutex;
    std::unordered_map<ECS::ComponentTypeId, EditorPickProvider> m_Providers;
    // Module stamp per registration (registry bookkeeping).
    std::unordered_map<ECS::ComponentTypeId, ECS::ModuleRegistrationStamp> m_ModuleOwners;
};

} // namespace GameEngine::Editor::Picking
