#pragma once

// The extraction fast-path test environment, shared by ExtractionFastPathTests.cpp (the gate) and
// ExtractionFastPathMicrobenchmark.cpp (the opt-in measurement): a device-backed RenderServices,
// a feed-subscribed world (the primary-world shape), one matching view, one registered mesh and
// material. Fixed GUIDs keep digests comparable across harness instances.

#include <gtest/gtest.h>

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h" // Mesh/Vertex for registry fixtures
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "TestDeviceHelper.h"

#include <memory>

namespace GameEngine::Testing::ExtractionFastPath
{
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::ECS;
using namespace GameEngine::Components;

inline constexpr float32 kDt = 1.0f / 60.0f;

inline GUID FixedGuid(uint8 tag)
{
    uint8 bytes[16] = {tag, 0x5A, 0x2B, tag, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, tag};
    return GUID::FromBytes(bytes);
}

inline Transform MakeTransformAt(float x, float y, float z)
{
    return Transform::FromTRS(Mathematics::Vector3{x, y, z}, Mathematics::Quaternion{},
                              Mathematics::Vector3{1.0f, 1.0f, 1.0f});
}

inline void SetIdentityMatrix(float* m)
{
    for (int i = 0; i < 16; ++i)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

inline CameraData MakeIdentityCamera()
{
    CameraData data{};
    SetIdentityMatrix(data.view);
    SetIdentityMatrix(data.proj);
    SetIdentityMatrix(data.viewProj);
    return data;
}

struct ExtractionHarness
{
    RenderServices rs;
    std::unique_ptr<World> world;
    TransformHierarchySystem hierarchy;
    std::unique_ptr<RenderExtractionSystem> extraction;
    MeshGPUHandle meshHandle;
    GUID materialGuid;
    Material* material = nullptr;
    ViewId viewId{};
    CameraId cameraId{};
    uint64 worldId = 0;

    // `pool` becomes the world's job system: extraction forks its parallel
    // lanes on it (null keeps every lane inline, as thumbnail worlds run).
    bool Initialize(Rendering::IDevice* device, bool fastPath,
                    JobSystem::WorkStealingThreadPool* pool = nullptr)
    {
        if (!rs.Initialize(device))
            return false;

        world = std::make_unique<World>(pool);
        worldId = world->GetWorldId();
        world->EnableComponentDirtyFeed(GetComponentTypeId<WorldTransform>());

        Mesh mesh{};
        mesh.Name = "FastPathTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        mesh.Vertices = {v0, v1, v2};
        mesh.Indices = {0, 1, 2};
        meshHandle = rs.GetMeshGPURegistry().RegisterSubmesh({FixedGuid(0x11), 0}, mesh);

        materialGuid = FixedGuid(0x22);
        MaterialDocument doc{};
        doc.materialName = "FastPathMaterial";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "surfaces/standard_surface.glsl";
        material = rs.Materials().Registry().Register(materialGuid, doc);
        if (!material)
            return false;
        Material::TestFactory::SetGraphicsPipelineId(*material, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(*material, 0u);

        cameraId = rs.Views().AllocateCamera("FastPathCamera");
        rs.Views().SetCameraData(cameraId, MakeIdentityCamera());
        viewId = rs.Views().AllocateView("FastPathView", cameraId);
        rs.Views().SetViewWorldId(viewId, worldId);
        rs.Views().SetViewRenderLayerMask(viewId, 0x1u);

        extraction = std::make_unique<RenderExtractionSystem>(&rs, fastPath);
        return true;
    }

    ~ExtractionHarness()
    {
        extraction.reset();
        if (world)
            world.reset();
        rs.Shutdown();
    }

    ECS::Entity SpawnRenderable(float x, float y, float z)
    {
        ECS::Entity e = world->Create();
        e.Set(MakeTransformAt(x, y, z));
        MeshRenderer mr{};
        mr.meshGpuHandleId = static_cast<uint64>(meshHandle);
        mr.renderLayerMask = 0x1u;
        mr.materialAssetGuid.Set(materialGuid);
        e.Set(mr);
        world->ProcessCommands();
        return e;
    }

    void StepFrame()
    {
        world->ProcessCommands();
        hierarchy.Update(*world, kDt);
        extraction->Update(*world, kDt);
        world->SwapComponentDirtyFeed();
    }

    const RenderServices::RenderExtractionStats& Stats() const
    {
        return rs.GetRenderExtractionStats();
    }

    // Runs full frames until the fast path engages; fails the calling test
    // if it never does.
    bool PrimeToFastPath(int maxFrames = 6)
    {
        for (int i = 0; i < maxFrames; ++i)
        {
            StepFrame();
            if (Stats().FastFrame != 0)
                return true;
        }
        return false;
    }
};

class ExtractionFastPathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        if (m_Device)
            m_Device->Shutdown();
    }

    std::unique_ptr<Rendering::IDevice> m_Device;
};
} // namespace GameEngine::Testing::ExtractionFastPath
