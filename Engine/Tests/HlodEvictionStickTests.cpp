// HLOD eviction stickiness (design v0.2 §5.1, runtime-half slice 3a). The
// switch is an eviction, not a disable: the HLODSelectSystem frees a member's
// GPUScene slot and sets MeshGPUData.hlodEvicted; the extraction gather must
// then skip that entity so it is never re-added to any view — and it must STICK
// across both the full lane (which would otherwise Op::Add on
// instanceIndex==0xFFFFFFFF) and the dirty-feed fast lane. On proxy-exit,
// clearing the flag + forcing the full lane re-adds the member the normal way.
// C6: eviction never touches mr.enabled / ECS::Disabled.
//
// Device-gated (GTEST_SKIP without Vulkan): the guard runs against a real
// GPUScene, mirroring the ExtractionFastPathTests harness.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"

#include "Components/Transform.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"

#include "Assets/ModelAsset.h" // Mesh/Vertex for the registry fixture
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::ECS;
using namespace GameEngine::Components;

#include "TestDeviceHelper.h"

namespace {

constexpr float32 kDt = 1.0f / 60.0f;

GUID FixedGuid(uint8 tag) {
    uint8 bytes[16] = {tag, 0x71, 0x0C, tag, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0xAA, tag};
    return GUID::FromBytes(bytes);
}

Transform MakeTransformAt(float x, float y, float z) {
    return Transform::FromTRS(Mathematics::Vector3{x, y, z}, Mathematics::Quaternion{},
                              Mathematics::Vector3{1.0f, 1.0f, 1.0f});
}

void SetIdentityMatrix(float* m) {
    for (int i = 0; i < 16; ++i)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

CameraData MakeIdentityCamera() {
    CameraData data{};
    SetIdentityMatrix(data.view);
    SetIdentityMatrix(data.proj);
    SetIdentityMatrix(data.viewProj);
    return data;
}

// One extraction environment with the dirty-feed fast path enabled — the lane
// eviction must survive.
struct EvictionHarness {
    RenderServices rs;
    std::unique_ptr<World> world;
    TransformHierarchySystem hierarchy;
    std::unique_ptr<RenderExtractionSystem> extraction;
    MeshGPUHandle meshHandle;
    GUID materialGuid;
    Material* material = nullptr;
    uint64 worldId = 0;

    bool Initialize(Rendering::IDevice* device) {
        if (!rs.Initialize(device))
            return false;
        world = std::make_unique<World>(nullptr);
        worldId = world->GetWorldId();
        world->EnableComponentDirtyFeed(GetComponentTypeId<WorldTransform>());

        Mesh mesh{};
        mesh.Name = "HlodEvictTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f; v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;  v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        mesh.Vertices = {v0, v1, v2};
        mesh.Indices = {0, 1, 2};
        meshHandle = rs.GetMeshGPURegistry().RegisterSubmesh({FixedGuid(0x11), 0}, mesh);

        materialGuid = FixedGuid(0x22);
        MaterialDocument doc{};
        doc.materialName = "HlodEvictMaterial";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "surfaces/standard_surface.glsl";
        material = rs.Materials().Registry().Register(materialGuid, doc);
        if (!material)
            return false;
        Material::TestFactory::SetGraphicsPipelineId(*material, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(*material, 0u);

        CameraId cameraId = rs.Views().AllocateCamera("HlodEvictCamera");
        rs.Views().SetCameraData(cameraId, MakeIdentityCamera());
        ViewId viewId = rs.Views().AllocateView("HlodEvictView", cameraId);
        rs.Views().SetViewWorldId(viewId, worldId);
        rs.Views().SetViewRenderLayerMask(viewId, 0x1u);

        extraction = std::make_unique<RenderExtractionSystem>(&rs, /*feedFastPathEnabled=*/true);
        return true;
    }

    ~EvictionHarness() {
        extraction.reset();
        world.reset();
        rs.Shutdown();
    }

    Entity SpawnRenderable(float x, float y, float z) {
        Entity e = world->Create();
        e.Set(MakeTransformAt(x, y, z));
        MeshRenderer mr{};
        mr.meshGpuHandleId = static_cast<uint64>(meshHandle);
        mr.renderLayerMask = 0x1u;
        mr.materialAssetGuid.Set(materialGuid);
        e.Set(mr);
        world->ProcessCommands();
        return e;
    }

    void StepFrame() {
        world->ProcessCommands();
        hierarchy.Update(*world, kDt);
        extraction->Update(*world, kDt);
        world->SwapComponentDirtyFeed();
    }

    const RenderServices::RenderExtractionStats& Stats() const {
        return rs.GetRenderExtractionStats();
    }

    MeshGPUData* Gpu(Entity e) { return world->GetComponentForWrite<MeshGPUData>(e.GetHandle()); }
};

} // namespace

TEST(HlodEvictionStick, EvictedMemberIsNotReAddedAndSurvivesBothLanes) {
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    EvictionHarness h;
    ASSERT_TRUE(h.Initialize(device.get()));

    Entity e = h.SpawnRenderable(0.0f, 0.0f, 5.0f);

    // Prime to residency (full lanes assign the GPUScene slot).
    for (int i = 0; i < 6; ++i)
        h.StepFrame();
    MeshGPUData* mg = h.Gpu(e);
    ASSERT_NE(mg, nullptr);
    ASSERT_NE(mg->instanceIndex, 0xFFFFFFFFu) << "member never became resident";
    const uint32 slot = mg->instanceIndex;

    // Evict exactly as HLODSelectSystem will: free the slot (batched), tombstone
    // the bridge indices, set the residency flag, force the full lane.
    const uint32 liveBefore = h.rs.GetGPUScene()->GetInstanceCount();
    h.rs.GetGPUScene()->RemoveInstances(std::vector<uint32_t>{slot});
    mg->instanceIndex = 0xFFFFFFFFu;
    mg->meshIndex = 0xFFFFFFFFu;
    mg->materialIndex = 0xFFFFFFFFu;
    mg->hlodEvicted = true;
    h.extraction->RequestFullExtractionNextFrame();

    // The forced full lane must NOT re-add the evicted member.
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u) << "residency request must force the full lane";
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationHlodResidency, 0u);
    mg = h.Gpu(e);
    EXPECT_EQ(mg->instanceIndex, 0xFFFFFFFFu) << "evicted member was re-added by the full lane";
    EXPECT_TRUE(mg->hlodEvicted);
    EXPECT_LT(h.rs.GetGPUScene()->GetInstanceCount(), liveBefore + 1u);

    // Eviction sticks across subsequent (fast) lanes.
    for (int i = 0; i < 4; ++i)
        h.StepFrame();
    mg = h.Gpu(e);
    EXPECT_EQ(mg->instanceIndex, 0xFFFFFFFFu) << "evicted member re-added on a later frame";

    // Proxy-exit: clear the flag + force the full lane -> member re-added the
    // normal way (Op::Add on instanceIndex == 0xFFFFFFFF).
    mg->hlodEvicted = false;
    h.extraction->RequestFullExtractionNextFrame();
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    mg = h.Gpu(e);
    ASSERT_NE(mg, nullptr);
    EXPECT_NE(mg->instanceIndex, 0xFFFFFFFFu) << "member not restored after proxy-exit";
    EXPECT_EQ(mg->meshIndex, 0u);
}

TEST(HlodEvictionStick, DefaultMeshGpuDataIsNotEvicted) {
    // Neutrality: the residency flag defaults false, so a scene with no HLOD
    // volumes pays no behavioral change (the extraction guard is a no-op).
    MeshGPUData mg{};
    EXPECT_FALSE(mg.hlodEvicted);
}
