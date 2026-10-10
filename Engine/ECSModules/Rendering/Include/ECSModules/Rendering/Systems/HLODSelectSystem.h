#pragma once

// HLOD runtime switch system (design v0.2 §5, runtime integration layer). Runs in
// the Extraction phase BEFORE RenderExtractionSystem: once per frame, for each
// reconciled cluster in the bound world's HlodRuntime, it computes the main-view
// screen coverage of the cluster sphere (the ge_SelectLOD metric, so the CPU
// cluster switch and the GPU per-mesh LOD agree at the boundary) and toggles
// residency between the cluster's members and its proxy through
// MeshGPUData.hlodEvicted + GPUScene::RemoveInstances. On any flip it asks
// RenderExtractionSystem (via RenderServices) to run the full lane so the winning
// set is re-added. Members resident XOR proxy resident, in every view slice, by
// single-boolean construction (§5.4).
//
// A member move or a material/mesh reassignment falls the cluster back to
// members-only with a persistent stale flag (§3.4 / C3); a rebake + reload clears
// it. GE_HLOD=0 or a world with no reconciled clusters is a zero-overhead early
// out (the bench neutrality gate).

#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <cstdint>
#include <vector>

namespace GameEngine {

namespace Hlod { struct RuntimeCluster; }
namespace Rendering { class GPUScene; }

namespace Engine::Renderer {

class RenderServices;

class HLODSelectSystem : public ECS::ISystem
{
public:
    explicit HLODSelectSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "HLODSelect"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Proxy-enter: free the members' GPUScene slots (batched) + evict them, and
    // clear the proxies' eviction so the full lane re-adds them.
    void EnterProxy(ECS::World& world, Rendering::GPUScene& scene, Hlod::RuntimeCluster& cluster);
    // Proxy-exit: reverse of EnterProxy.
    void ExitProxy(ECS::World& world, Rendering::GPUScene& scene, Hlod::RuntimeCluster& cluster);
    // Detect a live edit (member moved / material or mesh reassigned) and, if any,
    // mark the cluster stale + force it back to members-only. Returns true when the
    // cluster is (now or already) stale.
    bool RefreshStaleness(ECS::World& world, Rendering::GPUScene& scene, Hlod::RuntimeCluster& cluster);

    RenderServices* m_RenderServices = nullptr;

    // Scratch reused across clusters to avoid per-flip allocation.
    std::vector<uint32_t> m_SlotScratch;

    // Gate the per-member staleness scan behind world-wide Transform/MeshRenderer
    // change gates: a fully idle world pays only the coverage math, but any such
    // edit anywhere re-runs the bounded O(total members) scan that frame.
    ECS::ChangeGate m_TransformEditGate;
    ECS::ChangeGate m_MeshRendererEditGate;
};

} // namespace Engine::Renderer
} // namespace GameEngine
