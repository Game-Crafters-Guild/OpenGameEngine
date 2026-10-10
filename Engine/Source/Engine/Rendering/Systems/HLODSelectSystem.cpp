#include "ECSModules/Rendering/Systems/HLODSelectSystem.h"

#include "Assets/HlodSelect.h"
#include "Components/Rendering/HLODVolume.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Engine/Rendering/HlodRuntime.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/GPUScene.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <cstdlib>

namespace GameEngine {
namespace Engine::Renderer {

namespace {

// GE_HLOD=0 disables the runtime switch entirely (zero overhead); unset / any
// other value keeps it on. Read once.
bool IsHlodRuntimeEnabled() {
    static bool s_Enabled = []() {
        const char* env = std::getenv("GE_HLOD");
        if (!env || !*env)
            return true;
        return !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

constexpr uint32 kUnassignedSlot = 0xFFFFFFFFu;

void EvictOne(Components::MeshGPUData& gpu) {
    gpu.instanceIndex = kUnassignedSlot;
    gpu.meshIndex = kUnassignedSlot;
    gpu.materialIndex = kUnassignedSlot;
    gpu.hlodEvicted = true;
}

} // namespace

void HLODSelectSystem::EnterProxy(ECS::World& world, Rendering::GPUScene& scene,
                                  Hlod::RuntimeCluster& cluster) {
    m_SlotScratch.clear();
    for (ECS::EntityHandle member : cluster.Members) {
        auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(member);
        if (!gpu)
            continue;
        if (gpu->instanceIndex != kUnassignedSlot)
            m_SlotScratch.push_back(gpu->instanceIndex);
        EvictOne(*gpu);
    }
    if (!m_SlotScratch.empty())
        scene.RemoveInstances(m_SlotScratch);

    // Un-evict the proxies; the forced full lane re-adds them (Op::Add on
    // instanceIndex == 0xFFFFFFFF).
    for (ECS::EntityHandle proxy : cluster.Proxies) {
        if (auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(proxy))
            gpu->hlodEvicted = false;
    }
    cluster.ProxyActive = true;
}

void HLODSelectSystem::ExitProxy(ECS::World& world, Rendering::GPUScene& scene,
                                 Hlod::RuntimeCluster& cluster) {
    m_SlotScratch.clear();
    for (ECS::EntityHandle proxy : cluster.Proxies) {
        auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(proxy);
        if (!gpu)
            continue;
        if (gpu->instanceIndex != kUnassignedSlot)
            m_SlotScratch.push_back(gpu->instanceIndex);
        EvictOne(*gpu);
    }
    if (!m_SlotScratch.empty())
        scene.RemoveInstances(m_SlotScratch);

    for (ECS::EntityHandle member : cluster.Members) {
        if (auto* gpu = world.GetComponentForWrite<Components::MeshGPUData>(member))
            gpu->hlodEvicted = false;
    }
    cluster.ProxyActive = false;
}

bool HLODSelectSystem::RefreshStaleness(ECS::World& world, Rendering::GPUScene& scene,
                                        Hlod::RuntimeCluster& cluster) {
    if (cluster.Stale)
        return true;

    bool nowStale = false;
    for (size_t i = 0; i < cluster.Members.size(); ++i) {
        const ECS::EntityHandle member = cluster.Members[i];
        const auto* wt = world.GetComponent<Components::WorldTransform>(member);
        const auto* mr = world.GetComponent<Components::MeshRenderer>(member);
        // A member that vanished, moved, or had its mesh/material reassigned makes
        // the frozen proxy misrepresent the live scene (C3 / §3.4).
        if (!wt || !mr) {
            nowStale = true;
            break;
        }
        if (wt->Version != cluster.MemberTransformVersion[i] ||
            mr->meshGpuHandleId != cluster.MemberMeshHandleId[i] ||
            mr->materialAssetGuid.ToGuid() != cluster.MemberMaterialGuid[i]) {
            nowStale = true;
            break;
        }
    }
    if (!nowStale)
        return false;

    cluster.Stale = true;
    if (cluster.ProxyActive) {
        // Fall back to members-only immediately: the proxy geometry is now wrong.
        ExitProxy(world, scene, cluster);
        m_RenderServices->RequestHlodResidencyExtraction();
    }
    return true;
}

void HLODSelectSystem::Update(ECS::World& world, float32 /*deltaTime*/) {
    if (!IsHlodRuntimeEnabled() || !m_RenderServices)
        return;

    Hlod::HlodRuntime& runtime = m_RenderServices->GetHlodRuntime();
    if (!runtime.HasClusters() || runtime.GetBoundWorldId() != world.GetWorldId())
        return;

    Rendering::GPUScene* scene = m_RenderServices->GetGPUScene();
    if (!scene)
        return;

    // Main-view LOD inputs (mirrors MakeViewLODParams): the primary viewpoint
    // camera. projScaleY <= 0 (ortho / 2D editor / no camera) forces members.
    Hlod::ViewLod view;
    const Rendering::CameraId camId = m_RenderServices->Views().GetViewpointCamera();
    if (const Rendering::CameraData* cam = m_RenderServices->Views().FindCameraData(camId)) {
        view.CameraPos[0] = cam->cameraPos[0];
        view.CameraPos[1] = cam->cameraPos[1];
        view.CameraPos[2] = cam->cameraPos[2];
        view.ProjScaleY = (cam->cameraPos[3] == 1.0f) ? 0.0f : std::abs(cam->proj[5]);
    }
    view.LodBiasGlobal = m_RenderServices->GetLODGlobalBias();

    // Switch thresholds from the first enabled HLODVolume (v1: one applies
    // globally); default when none is present.
    Hlod::SwitchConfig switchConfig;
    {
        bool found = false;
        world.Query<ECS::Read<Components::HLODVolume>>().Each(
            [&](const Components::HLODVolume& volume) {
                if (found)
                    return;
                found = true;
                switchConfig = Hlod::MakeSwitchConfig(volume.SwitchCoverage, volume.SwitchHysteresis);
            });
    }

    // Gate the (bounded) per-member staleness scan behind an actual edit so idle
    // frames pay only the coverage math.
    bool anyEdit = false;
    {
        const uint64 probeVersion = world.GetGlobalSystemVersion();
        auto probe = [&](auto tag, ECS::ChangeGate& gate) {
            using C = decltype(tag);
            auto q = world.Query<ECS::Read<C>>();
            q.template Changed<C>(gate);
            bool fired = false;
            q.BatchEach([&](const C*, std::size_t) { fired = true; });
            gate.LastRunVersion = probeVersion;
            return fired;
        };
        anyEdit |= probe(Components::WorldTransform{}, m_TransformEditGate);
        anyEdit |= probe(Components::MeshRenderer{}, m_MeshRendererEditGate);
    }

    bool anyFlip = false;
    for (Hlod::RuntimeCluster& cluster : runtime.GetClusters()) {
        const bool stale = anyEdit ? RefreshStaleness(world, *scene, cluster) : cluster.Stale;
        if (stale)
            continue; // members-only forever until rebake + reload

        const float coverage =
            Hlod::ClusterCoverage(view, cluster.SphereCenter, cluster.SphereRadius);
        const bool wantProxy = Hlod::UpdateProxyActive(cluster.ProxyActive, coverage, switchConfig);
        if (wantProxy == cluster.ProxyActive)
            continue;

        if (wantProxy)
            EnterProxy(world, *scene, cluster);
        else
            ExitProxy(world, *scene, cluster);
        anyFlip = true;
    }

    // Any residency flip re-adds a set via the full lane (Op::Add).
    if (anyFlip)
        m_RenderServices->RequestHlodResidencyExtraction();
}

} // namespace Engine::Renderer
} // namespace GameEngine
