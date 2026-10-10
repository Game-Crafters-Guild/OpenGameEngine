#include <gtest/gtest.h>

#include <set>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h" // required for template implementations (AddComponent<>, etc.)
#include "JobSystem/WorkStealingThreadPool.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Components/Transform.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MeshGPUData.h"

#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Materials/MaterialDocument.h"

#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Mathematics/MatrixOps.h"

#include <array>
#include <random>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::ECS;
using namespace GameEngine::Components;

#include "TestDeviceHelper.h"

static void SetIdentityMatrix4x4(float outM[16])
{
    for (int i = 0; i < 16; ++i)
        outM[i] = 0.0f;
    outM[0] = 1.0f;
    outM[5] = 1.0f;
    outM[10] = 1.0f;
    outM[15] = 1.0f;
}

static CameraData MakeIdentityCameraData()
{
    CameraData data{};
    SetIdentityMatrix4x4(data.view);
    SetIdentityMatrix4x4(data.proj);
    SetIdentityMatrix4x4(data.viewProj);
    return data;
}

// Register a real mesh + material via RenderServices and return a properly
// configured MeshRenderer component that the extraction system can resolve.
struct TestRenderableSetup
{
    MeshGPUHandle meshHandle;
    GUID materialGuid;
    Material* material = nullptr;
};

static TestRenderableSetup RegisterTestRenderable(RenderServices& rs)
{
    TestRenderableSetup out{};

    // Register a triangle mesh.
    Mesh mesh{};
    mesh.Name = "TestTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[0] = 0.0f; v0.Position[1] = 1.0f; v0.Position[2] = 0.0f;
    v1.Position[0] = -1.0f; v1.Position[1] = -1.0f; v1.Position[2] = 0.0f;
    v2.Position[0] = 1.0f; v2.Position[1] = -1.0f; v2.Position[2] = 0.0f;
    v0.Normal[2] = 1.0f; v1.Normal[2] = 1.0f; v2.Normal[2] = 1.0f;
    mesh.Vertices = {v0, v1, v2};
    mesh.Indices = {0, 1, 2};

    GUID meshGuid = GUID::Generate();
    out.meshHandle = rs.GetMeshGPURegistry().RegisterSubmesh({meshGuid, 0}, mesh);

    // Register a minimal material directly with the runtime registry. The
    // extraction tests do not need RenderServices' asset-manager-backed
    // RegisterMaterialFromDocument path.
    out.materialGuid = GUID::Generate();
    MaterialDocument doc{};
    doc.materialName = "TestExtraction";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    out.material = rs.Materials().Registry().Register(out.materialGuid, doc);

    // Give the material a synthetic valid pipeline so the extraction system
    // doesn't skip it.
    if (out.material)
    {
        Material::TestFactory::SetGraphicsPipelineId(*out.material, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(*out.material, 0u);
    }

    return out;
}

static MeshRenderer MakeTestMeshRenderer(const TestRenderableSetup& setup)
{
    MeshRenderer mr{};
    mr.meshGpuHandleId = static_cast<uint64>(setup.meshHandle);
    mr.renderLayerMask = 0x1u;

    mr.materialAssetGuid.Set(setup.materialGuid);

    return mr;
}

static Material* RegisterNamedMaterial(RenderServices& rs, const GUID& guid, const char* name)
{
    MaterialDocument doc{};
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    return rs.Materials().RegisterMaterialFromDocument(guid, doc);
}

// ---------------------------------------------------------------------------
// Material SSBO free-list and reverse lookup tests
// ---------------------------------------------------------------------------

TEST(RenderServicesWorldMaterialTests, MultipleFreeListCycles)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    // Register 4 materials: indices 0, 1, 2, 3.
    GUID guids[4];
    Material* mats[4];
    for (int i = 0; i < 4; ++i)
    {
        guids[i] = GUID::Generate();
        mats[i] = RegisterNamedMaterial(rs, guids[i], "Mat");
        ASSERT_NE(mats[i], nullptr);
        EXPECT_EQ(mats[i]->GetGpuSceneMaterialIndex(), static_cast<uint32_t>(i));
    }

    // Unregister 1 and 3 — their indices should be recycled.
    rs.Materials().Registry().Unregister(guids[1]);
    rs.Materials().Registry().Unregister(guids[3]);

    // Register two new materials — should reuse the freed indices (1 and 3).
    GUID guidX = GUID::Generate();
    GUID guidY = GUID::Generate();
    Material* matX = RegisterNamedMaterial(rs, guidX, "MatX");
    Material* matY = RegisterNamedMaterial(rs, guidY, "MatY");
    ASSERT_NE(matX, nullptr);
    ASSERT_NE(matY, nullptr);

    const std::set<uint32_t> freedIndices = {1u, 3u};
    EXPECT_TRUE(freedIndices.count(matX->GetGpuSceneMaterialIndex()));
    EXPECT_TRUE(freedIndices.count(matY->GetGpuSceneMaterialIndex()));
    EXPECT_NE(matX->GetGpuSceneMaterialIndex(), matY->GetGpuSceneMaterialIndex());

    // Next registration should get a fresh index (free-list exhausted).
    GUID guidZ = GUID::Generate();
    Material* matZ = RegisterNamedMaterial(rs, guidZ, "MatZ");
    ASSERT_NE(matZ, nullptr);
    EXPECT_EQ(matZ->GetGpuSceneMaterialIndex(), 4u);

    // Verify no collisions: all active materials have unique indices.
    std::set<uint32_t> activeIndices;
    activeIndices.insert(mats[0]->GetGpuSceneMaterialIndex());
    activeIndices.insert(mats[2]->GetGpuSceneMaterialIndex());
    activeIndices.insert(matX->GetGpuSceneMaterialIndex());
    activeIndices.insert(matY->GetGpuSceneMaterialIndex());
    activeIndices.insert(matZ->GetGpuSceneMaterialIndex());
    EXPECT_EQ(activeIndices.size(), 5u);

    rs.Shutdown();
    device->Shutdown();
}

TEST(RenderServicesWorldMaterialTests, ReRegisterSameGuidKeepsIndex)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    GUID guid = GUID::Generate();
    Material* mat1 = RegisterNamedMaterial(rs, guid, "MatA");
    ASSERT_NE(mat1, nullptr);
    const uint32_t idx = mat1->GetGpuSceneMaterialIndex();

    // Re-registering the same GUID returns the same material with the same index.
    Material* mat2 = RegisterNamedMaterial(rs, guid, "MatA");
    EXPECT_EQ(mat1, mat2);
    EXPECT_EQ(mat2->GetGpuSceneMaterialIndex(), idx);

    rs.Shutdown();
    device->Shutdown();
}

TEST(RenderServicesWorldMaterialTests, DoubleUnregisterIsHarmless)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    GUID guidA = GUID::Generate();
    GUID guidB = GUID::Generate();
    Material* matA = RegisterNamedMaterial(rs, guidA, "MatA");
    ASSERT_NE(matA, nullptr);
    const uint32_t idxA = matA->GetGpuSceneMaterialIndex();

    rs.Materials().Registry().Unregister(guidA);
    // Second unregister of the same GUID should be a no-op (material already gone).
    rs.Materials().Registry().Unregister(guidA);

    // Register B — should get A's recycled index. If double-unregister pushed the
    // index twice, a subsequent registration would also get the same index, causing
    // a collision. Register two materials to verify no duplication.
    Material* matB = RegisterNamedMaterial(rs, guidB, "MatB");
    ASSERT_NE(matB, nullptr);
    EXPECT_EQ(matB->GetGpuSceneMaterialIndex(), idxA);

    GUID guidC = GUID::Generate();
    Material* matC = RegisterNamedMaterial(rs, guidC, "MatC");
    ASSERT_NE(matC, nullptr);
    // If the free-list had a duplicate, matC would also get idxA — that must not happen.
    EXPECT_NE(matC->GetGpuSceneMaterialIndex(), idxA);

    rs.Shutdown();
    device->Shutdown();
}

// End-to-end sanity: a simple ECS world with a parent and child entity using
// Transform + Parent + MeshRenderer should produce a WorldTransform for the
// child, a GPUScene instance, and a batch key on the view that matches its world
// and layer, and none on a view of another world or another layer. This
// exercises TransformHierarchySystem + RenderExtractionSystem using the default
// world mesh/material provided by RenderServices.
TEST(RenderServicesWorldMaterialTests, ECSWorldMeshExtraction_UsesWorldTransformHierarchy)
{
	auto device = CreateVulkanDeviceFast();
	if (!device)
	{
	    GTEST_SKIP() << "No Vulkan device available";
	}

	RenderServices rs;
	ASSERT_TRUE(rs.Initialize(device.get()));

	auto setup = RegisterTestRenderable(rs);
	ASSERT_NE(setup.material, nullptr);

	// ECS world with a parent and a child. The child is offset along +Z in local
	// space; world-space position should be parent + local once hierarchy is
	// resolved.
	auto worldPtr = std::make_unique<World>(nullptr);
	World& world = *worldPtr;
	const uint64 worldId = world.GetWorldId();

	// Minimal camera + view so that RenderExtractionSystem sees at least one view.
	CameraId cameraId = rs.Views().AllocateCamera("TestCamera");
	CameraData cam = MakeIdentityCameraData();
	rs.Views().SetCameraData(cameraId, cam);

	ViewId viewId = rs.Views().AllocateView("TestView", cameraId);
	rs.Views().SetViewWorldId(viewId, worldId);
	rs.Views().SetViewRenderLayerMask(viewId, 0x1u);

	// Views that must receive no world draw items: another world, another layer.
	World otherWorld(nullptr);
	ViewId viewWrongWorld = rs.Views().AllocateView("ViewWrongWorld", cameraId);
	rs.Views().SetViewWorldId(viewWrongWorld, otherWorld.GetWorldId());
	rs.Views().SetViewRenderLayerMask(viewWrongWorld, 0x1u);
	ViewId viewWrongLayer = rs.Views().AllocateView("ViewWrongLayer", cameraId);
	rs.Views().SetViewWorldId(viewWrongLayer, worldId);
	rs.Views().SetViewRenderLayerMask(viewWrongLayer, 0x2u);

	Entity parent = world.Create();
	Entity child  = world.Create();

	Transform parentLocal = Transform::FromTRS(
	    Mathematics::Vector3{1.0f, 2.0f, 3.0f},
	    Mathematics::Quaternion{},
	    Mathematics::Vector3{1.0f, 1.0f, 1.0f});
	Transform childLocal = Transform::FromTRS(
	    Mathematics::Vector3{0.0f, 0.0f, 5.0f},
	    Mathematics::Quaternion{},
	    Mathematics::Vector3{1.0f, 1.0f, 1.0f});

	parent.Set(parentLocal);

	Parent parentComp{};
	parentComp.parent = parent.GetHandle();

	MeshRenderer mr = MakeTestMeshRenderer(setup);

	child.Set(childLocal);
	child.Set(parentComp);
	child.Set(mr);

	world.ProcessCommands();

	TransformHierarchySystem hierarchy;
	hierarchy.Update(world, 0.0f);

	// World transform should equal parent (1,2,3) plus child local (0,0,5).
	auto* childWorld = world.GetComponent<WorldTransform>(child.GetHandle());
	ASSERT_NE(childWorld, nullptr);
	EXPECT_NEAR(childWorld->matrix[12], 1.0f, 1e-4f);
	EXPECT_NEAR(childWorld->matrix[13], 2.0f, 1e-4f);
	EXPECT_NEAR(childWorld->matrix[14], 8.0f, 1e-4f);

		RenderExtractionSystem extraction(&rs);
	extraction.Update(world, 0.0f);

	GPUScene* scene = rs.GetGPUScene();
	ASSERT_NE(scene, nullptr);
	EXPECT_GE(scene->GetInstanceCount(), 1u);

	// MeshGPUData should have been attached to the child and cache the
	// GPUScene instance index used for DrawCommand emission.
	auto* meshGpu = world.GetComponent<MeshGPUData>(child.GetHandle());
	ASSERT_NE(meshGpu, nullptr);
	EXPECT_NE(meshGpu->instanceIndex, 0xFFFFFFFFu);
	EXPECT_LT(meshGpu->instanceIndex, scene->GetInstanceCount());

	const auto& instances = scene->GetInstances();
	ASSERT_GT(instances.size(), meshGpu->instanceIndex);
	const GPUInstance& inst = instances[meshGpu->instanceIndex];

	// Fallback bounds path in RenderExtractionSystem uses the translation from
	// WorldTransform for boundingCenter.
	EXPECT_NEAR(inst.boundingCenter.x, 1.0f, 1e-4f);
	EXPECT_NEAR(inst.boundingCenter.y, 2.0f, 1e-4f);
	EXPECT_NEAR(inst.boundingCenter.z, 8.0f, 1e-4f);

		// Build per-view batch keys once for this frame so we can query them.
		rs.BuildWorldBatchKeys();

		// At least one batch key should reference this instance's material+mesh.
		auto keysForView = rs.GetEntityBatchKeys(viewId);
		ASSERT_FALSE(keysForView.empty());
		bool found = false;
		for (const auto& key : keysForView)
		{
		    if (key.material == setup.material && key.mesh == setup.meshHandle)
		    {
		        found = true;
		        break;
		    }
		}
		EXPECT_TRUE(found);
		EXPECT_TRUE(rs.GetEntityBatchKeys(viewWrongWorld).empty());
		EXPECT_TRUE(rs.GetEntityBatchKeys(viewWrongLayer).empty());

	rs.Shutdown();
	device->Shutdown();
}

TEST(RenderServicesWorldMaterialTests, DisabledRendererClearsCachedGPUSceneInstance)
{
	auto device = CreateVulkanDeviceFast();
	if (!device)
	{
	    GTEST_SKIP() << "No Vulkan device available";
	}

	RenderServices rs;
	ASSERT_TRUE(rs.Initialize(device.get()));

	auto setup = RegisterTestRenderable(rs);
	ASSERT_NE(setup.material, nullptr);

	auto worldPtr = std::make_unique<World>(nullptr);
	World& world = *worldPtr;
	const uint64 worldId = world.GetWorldId();

	CameraId cameraId = rs.Views().AllocateCamera("DisableTestCamera");
	rs.Views().SetCameraData(cameraId, MakeIdentityCameraData());

	ViewId viewId = rs.Views().AllocateView("DisableTestView", cameraId);
	rs.Views().SetViewWorldId(viewId, worldId);
	rs.Views().SetViewRenderLayerMask(viewId, 0x1u);

	Entity entity = world.Create();
	entity.Set(Transform::FromTRS(
	    Mathematics::Vector3{0.0f, 0.0f, 0.0f},
	    Mathematics::Quaternion{},
	    Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
	entity.Set(MakeTestMeshRenderer(setup));
	world.ProcessCommands();

	TransformHierarchySystem hierarchy;
	hierarchy.Update(world, 0.0f);

	RenderExtractionSystem extraction(&rs);
	extraction.Update(world, 0.0f);

	GPUScene* scene = rs.GetGPUScene();
	ASSERT_NE(scene, nullptr);

	auto* meshGpu = world.GetComponent<MeshGPUData>(entity.GetHandle());
	ASSERT_NE(meshGpu, nullptr);
	EXPECT_NE(meshGpu->instanceIndex, 0xFFFFFFFFu);
	EXPECT_EQ(scene->GetLiveInstanceCount(), 1u);
	EXPECT_EQ(scene->GetInstanceCount(), 1u);
	const uint32_t originalInstanceIndex = meshGpu->instanceIndex;

	world.AddComponentImmediate<ECS::Disabled>(entity.GetHandle(), ECS::Disabled{});
	extraction.Update(world, 0.0f);

	meshGpu = world.GetComponent<MeshGPUData>(entity.GetHandle());
	ASSERT_NE(meshGpu, nullptr);
	EXPECT_EQ(meshGpu->instanceIndex, originalInstanceIndex);
	EXPECT_EQ(meshGpu->meshIndex, 0xFFFFFFFFu);
	EXPECT_EQ(meshGpu->materialIndex, 0xFFFFFFFFu);
	ASSERT_LT(originalInstanceIndex, scene->GetInstances().size());
	EXPECT_EQ(scene->GetInstances()[originalInstanceIndex].meshIndex, 0xFFFFFFFFu);
	EXPECT_EQ(scene->GetInstances()[originalInstanceIndex].materialIndex, 0xFFFFFFFFu);
	EXPECT_EQ(scene->GetLiveInstanceCount(), 1u);
	EXPECT_EQ(scene->GetInstanceCount(), 1u)
	    << "Disabled renderers keep a stable slot, but the slot must be tombstoned.";

	world.RemoveComponentImmediate<ECS::Disabled>(entity.GetHandle());
	extraction.Update(world, 0.0f);

	meshGpu = world.GetComponent<MeshGPUData>(entity.GetHandle());
	ASSERT_NE(meshGpu, nullptr);
	EXPECT_EQ(meshGpu->instanceIndex, originalInstanceIndex);
	EXPECT_EQ(scene->GetLiveInstanceCount(), 1u);
	EXPECT_EQ(scene->GetInstanceCount(), 1u);

	ASSERT_NE(world.GetComponent<MeshRenderer>(entity.GetHandle()), nullptr);
	entity.SetEnabled<MeshRenderer>(false);
	extraction.Update(world, 0.0f);

	// The switch moves the entity to another archetype; read MeshGPUData afresh.
	meshGpu = world.GetComponent<MeshGPUData>(entity.GetHandle());
	ASSERT_NE(meshGpu, nullptr);
	EXPECT_EQ(meshGpu->instanceIndex, originalInstanceIndex);
	EXPECT_EQ(meshGpu->meshIndex, 0xFFFFFFFFu);
	EXPECT_EQ(meshGpu->materialIndex, 0xFFFFFFFFu);
	EXPECT_EQ(scene->GetInstances()[originalInstanceIndex].meshIndex, 0xFFFFFFFFu);
	EXPECT_EQ(scene->GetInstances()[originalInstanceIndex].materialIndex, 0xFFFFFFFFu);
	EXPECT_EQ(scene->GetLiveInstanceCount(), 1u);
	EXPECT_EQ(scene->GetInstanceCount(), 1u);

	rs.Shutdown();
	device->Shutdown();
}

// Regression for the editor thumbnail blank-output bug.
//
// Before commits 65438b71 / 8ac4167f the GPUScene per-frame upload was driven
// by CullingSystem (a per-ECS-world tick). When the editor's thumbnail world
// extracted entities AFTER the main world's BuildFrameGraph had finalised the
// upload for that frame, the thumbnail entities were silently dropped — the
// thumbnail rendered blank.
//
// The fix hoisted the GPUScene lifecycle into RenderServices and made
// ScheduleBucketerDispatchesForView call FlushGPUBuffers() (guarded by
// IsDirty()) so any mutation between the main BuildFrameGraph and a per-view
// bucketer dispatch still reaches the GPU.
//
// This test exercises the underlying GPUScene contract that the fix relies on:
// after the main frame's flush, a second-world extraction must leave
// IsDirty() == true so a subsequent flush picks it up.
TEST(RenderServicesWorldMaterialTests, LateExtractionRedirtiesGPUSceneForBucketer)
{
	auto device = CreateVulkanDeviceFast();
	if (!device)
		GTEST_SKIP() << "No Vulkan device available";

	RenderServices rs;
	ASSERT_TRUE(rs.Initialize(device.get()));

	auto setup = RegisterTestRenderable(rs);
	ASSERT_NE(setup.material, nullptr);

	GPUScene* scene = rs.GetGPUScene();
	ASSERT_NE(scene, nullptr);

	// Shared camera; per-world views with matching worldIds so extraction
	// accepts each world's renderable.
	CameraId cameraId = rs.Views().AllocateCamera("TestCamera");
	CameraData camData = MakeIdentityCameraData();
	rs.Views().SetCameraData(cameraId, camData);

	// --- Main world: allocate matching view, one entity, extract. ---
	auto mainWorldPtr = std::make_unique<World>(nullptr);
	World& mainWorld = *mainWorldPtr;
	{
		ViewId viewMain = rs.Views().AllocateView("MainTestView", cameraId);
		rs.Views().SetViewWorldId(viewMain, mainWorld.GetWorldId());
		rs.Views().SetViewRenderLayerMask(viewMain, 0x1u);

		Entity e = mainWorld.Create();
		e.Set(Transform::FromTRS(
		    Mathematics::Vector3{1.0f, 0.0f, 0.0f},
		    Mathematics::Quaternion{},
		    Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
		e.Set(MakeTestMeshRenderer(setup));
		mainWorld.ProcessCommands();

		TransformHierarchySystem hierarchy;
		hierarchy.Update(mainWorld, 0.0f);
		RenderExtractionSystem extraction(&rs);
		extraction.Update(mainWorld, 0.0f);
	}

	const uint32_t mainOnlyInstanceCount = scene->GetInstanceCount();
	ASSERT_GE(mainOnlyInstanceCount, 1u);
	EXPECT_TRUE(scene->IsDirty())
	    << "GPUScene must be dirty after extraction populates instances.";

	// Simulate the main world's per-frame flush
	// (BuildFrameGraph -> AdvanceFrameSlot + FlushGPUBuffers).
	scene->BeginFrame();
	EXPECT_FALSE(scene->IsDirty())
	    << "GPUScene must be clean after BeginFrame (advance + flush).";

	// --- Thumbnail world: extracts AFTER the main flush. ---
	// This is exactly the editor thumbnail timing that previously triggered
	// the blank-output bug.
	auto thumbnailWorldPtr = std::make_unique<World>(nullptr);
	World& thumbnailWorld = *thumbnailWorldPtr;
	{
		ViewId viewThumb = rs.Views().AllocateView("ThumbTestView", cameraId);
		rs.Views().SetViewWorldId(viewThumb, thumbnailWorld.GetWorldId());
		rs.Views().SetViewRenderLayerMask(viewThumb, 0x1u);

		Entity e = thumbnailWorld.Create();
		e.Set(Transform::FromTRS(
		    Mathematics::Vector3{2.0f, 0.0f, 0.0f},
		    Mathematics::Quaternion{},
		    Mathematics::Vector3{1.0f, 1.0f, 1.0f}));
		e.Set(MakeTestMeshRenderer(setup));
		thumbnailWorld.ProcessCommands();

		TransformHierarchySystem hierarchy;
		hierarchy.Update(thumbnailWorld, 0.0f);
		RenderExtractionSystem extraction(&rs);
		extraction.Update(thumbnailWorld, 0.0f);
	}

	EXPECT_TRUE(scene->IsDirty())
	    << "Late extraction must re-dirty GPUScene so the next per-view "
	       "bucketer dispatch flushes its data. Before commit 8ac4167f this "
	       "mutation was silently lost when CullingSystem owned the upload.";

	const uint32_t totalInstanceCount = scene->GetInstanceCount();
	EXPECT_GT(totalInstanceCount, mainOnlyInstanceCount)
	    << "Thumbnail extraction must add at least one instance to GPUScene.";

	// Second flush picks up the late mutation — this is what
	// ScheduleBucketerDispatchesForView's guarded FlushGPUBuffers does.
	scene->FlushGPUBuffers();
	EXPECT_FALSE(scene->IsDirty())
	    << "Second flush must clear the late-extraction dirty state.";

	rs.Shutdown();
	device->Shutdown();
}

// W1 cascade-skip helpers. ComputeCascadeWorldAABB back-projects the 8 NDC
// corners of a light-VP into world space; AABBsIntersect is a standard
// slab test. Both must work correctly with the engine's reverse-Z LH
// orthographic VP convention (near=1, far=0 in NDC z).
TEST(CascadeWorldAABBTests, IdentityVPYieldsNDCBoxInWorldSpace)
{
	const Mathematics::Matrix4x4 identity = Mathematics::Matrix4x4::Identity();

	Mathematics::Vector3 mn{}, mx{};
	ShadowMapRenderFeature::ComputeCascadeWorldAABB(identity, mn, mx);

	// Identity VP → world == NDC. AABB is [-1, -1, 0] .. [1, 1, 1] (reverse-Z).
	EXPECT_FLOAT_EQ(mn.x, -1.0f);
	EXPECT_FLOAT_EQ(mn.y, -1.0f);
	EXPECT_FLOAT_EQ(mn.z, 0.0f);
	EXPECT_FLOAT_EQ(mx.x, 1.0f);
	EXPECT_FLOAT_EQ(mx.y, 1.0f);
	EXPECT_FLOAT_EQ(mx.z, 1.0f);
}

TEST(CascadeWorldAABBTests, AABBsIntersectMatchesSlabTest)
{
	using V = Mathematics::Vector3;
	// Overlapping AABBs.
	EXPECT_TRUE(ShadowMapRenderFeature::AABBsIntersect(
	    V{0, 0, 0}, V{2, 2, 2}, V{1, 1, 1}, V{3, 3, 3}));
	// Edge-touching (closed intervals).
	EXPECT_TRUE(ShadowMapRenderFeature::AABBsIntersect(
	    V{0, 0, 0}, V{1, 1, 1}, V{1, 0, 0}, V{2, 1, 1}));
	// Disjoint on X.
	EXPECT_FALSE(ShadowMapRenderFeature::AABBsIntersect(
	    V{0, 0, 0}, V{1, 1, 1}, V{2, 0, 0}, V{3, 1, 1}));
	// Disjoint on Z.
	EXPECT_FALSE(ShadowMapRenderFeature::AABBsIntersect(
	    V{0, 0, 0}, V{1, 1, 1}, V{0, 0, 5}, V{1, 1, 6}));
}

// ── Caster-set reduction: plane-tightening math ──
//
// TightenCascadeSidePlanes replaces a cascade's four side planes with its
// caster footprint, here the light-NDC rectangle of a camera-visible slice. Under an orthographic light,
// a caster casts its shadow onto receivers at the SAME light-space XY for all
// depths, so the property that MUST hold is: every caster whose light-space XY
// overlaps the visible footprint survives the tightened planes (no missing
// shadow), while casters whose XY lies well outside the inflated footprint are
// cullable. These tests drive a real perspective-camera frustum through the
// slice corner extraction the fit uses, fit an orthographic light to it
// (mirroring ComputeCascadeLightVP minus the texel/band snapping), and verify
// the property with the production TestSphereFrustum cull test.
namespace
{
using Mathematics::Matrix4x4;
using Mathematics::Vector3;
using Mathematics::Vector4;

// World slack the tightening adds around the footprint in these tests.
constexpr float kP2TestSlack = 4.0f;

struct FittedLight
{
	Matrix4x4 VP;
	float HalfExtent = 0.0f;
};

// Square-AABB orthographic fit over the slice corners — the core of
// ComputeCascadeLightVP without the snapping/hysteresis that stabilises it
// across frames (irrelevant to the tightening property).
FittedLight FitOrthoLight(const Vector3 corners[8], Vector3 lightDir)
{
	using namespace Mathematics;
	Vector3 ld = lightDir.Normalize();
	Vector3 up = std::abs(Vector3::Dot(ld, Vector3{0, 1, 0})) > 0.99f ? Vector3{0, 0, 1}
	                                                                  : Vector3{0, 1, 0};
	Matrix4x4 lightRot = MakeLookAtLH(Vector3{0, 0, 0}, ld, up);
	Vector3 mn{1e30f, 1e30f, 1e30f};
	Vector3 mx{-1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; ++i)
	{
		Vector4 ls = lightRot.Transform(Vector4{corners[i].x, corners[i].y, corners[i].z, 1.0f});
		mn.x = std::min(mn.x, ls.x); mx.x = std::max(mx.x, ls.x);
		mn.y = std::min(mn.y, ls.y); mx.y = std::max(mx.y, ls.y);
		mn.z = std::min(mn.z, ls.z); mx.z = std::max(mx.z, ls.z);
	}
	const float halfExtent = std::max((mx.x - mn.x) * 0.5f, (mx.y - mn.y) * 0.5f);
	const Vector3 centerLS{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
	Matrix4x4 invRot = Mathematics::Inverse(lightRot);
	Vector4 cw = invRot.Transform(Vector4{centerLS.x, centerLS.y, centerLS.z, 1.0f});
	const Vector3 center{cw.x, cw.y, cw.z};
	const float backExt = halfExtent * 1.5f;
	const float nearZ = mn.z - backExt;
	const float depth = mx.z - nearZ;
	const Vector3 lightPos = center - ld * (centerLS.z - nearZ);
	Matrix4x4 lightView = MakeLookAtLH(lightPos, lightPos + ld, up);
	Matrix4x4 lightProj = MakeOrthographicLH_ZO_ReverseZ(-halfExtent, halfExtent,
	                                                     -halfExtent, halfExtent, 0.0f, depth);
	return FittedLight{lightProj * lightView, halfExtent};
}

Matrix4x4 MakeTestCameraViewProj()
{
	using namespace Mathematics;
	Matrix4x4 view = MakeLookAtLH(Vector3{0, 40, -120}, Vector3{0, 0, 60}, Vector3{0, 1, 0});
	Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(60.0f * 3.14159265f / 180.0f,
	                                               16.0f / 9.0f, 0.5f, 400.0f);
	return proj * view;
}

// Project a world point through the light VP, returning NDC xy (and w for the
// depth-slab gate).
struct NdcXYW { float x, y, w; float z; };
NdcXYW ProjectNdc(const Matrix4x4& vp, const Vector3& p)
{
	Vector4 c = vp.Transform(Vector4{p.x, p.y, p.z, 1.0f});
	const float invW = 1.0f / c.w;
	return NdcXYW{c.x * invW, c.y * invW, c.w, c.z * invW};
}

// The slice corners' light-NDC rectangle {xMin, yMin, xMax, yMax}: the caster
// footprint of a cascade fitted to that slice.
std::array<float, 4> SliceFootprintNdc(const Matrix4x4& vp, const Vector3 corners[8])
{
	std::array<float, 4> rect{1e30f, 1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; ++i)
	{
		const NdcXYW n = ProjectNdc(vp, corners[i]);
		rect[0] = std::min(rect[0], n.x);
		rect[1] = std::min(rect[1], n.y);
		rect[2] = std::max(rect[2], n.x);
		rect[3] = std::max(rect[3], n.y);
	}
	return rect;
}
} // namespace

TEST(ShadowCasterReductionTests, TightenedPlanesKeepEveryReceiverVisibleCaster)
{
	const Matrix4x4 camVP = MakeTestCameraViewProj();
	Vector3 corners[8];
	ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(camVP, 0.5f, 400.0f, 120.0f, 260.0f, corners);

	const Vector3 lightDir{0.35f, -1.0f, 0.25f};
	const FittedLight fit = FitOrthoLight(corners, lightDir);

	Vector4 planes[6]{};
	ExtractFrustumPlanes(fit.VP, planes);
	ShadowMapRenderFeature::TightenCascadeSidePlanes(fit.VP, SliceFootprintNdc(fit.VP, corners).data(), fit.HalfExtent,
	                                                 kP2TestSlack, planes);

	// Every visible slice corner is a receiver — it must survive the tightened
	// planes (culling it would drop its shadow).
	for (int i = 0; i < 8; ++i)
		EXPECT_TRUE(TestSphereFrustum(corners[i], 0.5f, planes)) << "corner " << i;

	// A caster sharing a corner's light-space XY but pulled toward the light
	// (an off-screen caster whose shadow still lands on that visible corner)
	// must ALSO survive — the ortho Z-independence guarantee the whole scheme
	// rests on. The shift stays within the 1.5*halfExtent back-extension.
	const Vector3 ld = lightDir.Normalize();
	for (int i = 0; i < 8; ++i)
	{
		const Vector3 shifted = corners[i] - ld * fit.HalfExtent;
		EXPECT_TRUE(TestSphereFrustum(shifted, 0.5f, planes)) << "back-shifted corner " << i;
	}
}

TEST(ShadowCasterReductionTests, TighteningIsStrictSubsetAndCullsOutsideFootprint)
{
	const Matrix4x4 camVP = MakeTestCameraViewProj();
	Vector3 corners[8];
	ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(camVP, 0.5f, 400.0f, 120.0f, 260.0f, corners);

	const Vector3 lightDir{0.35f, -1.0f, 0.25f};
	const FittedLight fit = FitOrthoLight(corners, lightDir);

	Vector4 original[6]{};
	ExtractFrustumPlanes(fit.VP, original);
	Vector4 tightened[6];
	std::memcpy(tightened, original, sizeof(original));
	ShadowMapRenderFeature::TightenCascadeSidePlanes(fit.VP, SliceFootprintNdc(fit.VP, corners).data(), fit.HalfExtent,
	                                                 kP2TestSlack, tightened);

	// Footprint NDC AABB (what the corners project to inside the [-1,1] box).
	float fxMin = 1e30f, fxMax = -1e30f, fyMin = 1e30f, fyMax = -1e30f;
	for (int i = 0; i < 8; ++i)
	{
		const NdcXYW n = ProjectNdc(fit.VP, corners[i]);
		fxMin = std::min(fxMin, n.x); fxMax = std::max(fxMax, n.x);
		fyMin = std::min(fyMin, n.y); fyMax = std::max(fyMax, n.y);
	}
	const float slackNdc = kP2TestSlack / fit.HalfExtent;
	const Matrix4x4 invVP = Mathematics::Inverse(fit.VP);
	auto worldAt = [&](float nx, float ny, float nz)
	{
		Vector4 w = invVP.Transform(Vector4{nx, ny, nz, 1.0f});
		const float invW = 1.0f / w.w;
		return Vector3{w.x * invW, w.y * invW, w.z * invW};
	};

	// The square ortho fit leaves the footprint a strict subset of the box on at
	// least one side. For each side with headroom past the slack, a caster just
	// beyond the tightened bound (but still inside the original box) must be
	// culled by the tightened planes yet kept by the original — proving the
	// tightening is a strict, correct subset.
	std::vector<std::array<float, 2>> outsidePts;
	const float midY = (fyMin + fyMax) * 0.5f;
	const float midX = (fxMin + fxMax) * 0.5f;
	if (fxMax + slackNdc < 0.9f) outsidePts.push_back({(fxMax + slackNdc + 1.0f) * 0.5f, midY});
	if (fxMin - slackNdc > -0.9f) outsidePts.push_back({(fxMin - slackNdc - 1.0f) * 0.5f, midY});
	if (fyMax + slackNdc < 0.9f) outsidePts.push_back({midX, (fyMax + slackNdc + 1.0f) * 0.5f});
	if (fyMin - slackNdc > -0.9f) outsidePts.push_back({midX, (fyMin - slackNdc - 1.0f) * 0.5f});

	ASSERT_FALSE(outsidePts.empty()) << "footprint filled the box — no tightening to verify";
	for (const std::array<float, 2>& p : outsidePts)
	{
		const Vector3 w = worldAt(p[0], p[1], 0.5f);
		EXPECT_FALSE(TestSphereFrustum(w, 0.25f, tightened)) << "outside-footprint caster survived";
		EXPECT_TRUE(TestSphereFrustum(w, 0.25f, original)) << "original frustum wrongly culled it";
	}
}

TEST(ShadowCasterReductionTests, RandomCasterPropertyRespectsFootprint)
{
	const Matrix4x4 camVP = MakeTestCameraViewProj();
	Vector3 corners[8];
	ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(camVP, 0.5f, 400.0f, 120.0f, 260.0f, corners);

	const Vector3 lightDir{0.35f, -1.0f, 0.25f};
	const FittedLight fit = FitOrthoLight(corners, lightDir);

	Vector4 tightened[6]{};
	ExtractFrustumPlanes(fit.VP, tightened);
	ShadowMapRenderFeature::TightenCascadeSidePlanes(fit.VP, SliceFootprintNdc(fit.VP, corners).data(), fit.HalfExtent,
	                                                 kP2TestSlack, tightened);

	float fxMin = 1e30f, fxMax = -1e30f, fyMin = 1e30f, fyMax = -1e30f;
	for (int i = 0; i < 8; ++i)
	{
		const NdcXYW n = ProjectNdc(fit.VP, corners[i]);
		fxMin = std::min(fxMin, n.x); fxMax = std::max(fxMax, n.x);
		fyMin = std::min(fyMin, n.y); fyMax = std::max(fyMax, n.y);
	}
	const float slackNdc = kP2TestSlack / fit.HalfExtent;

	std::mt19937 rng(1234567u);
	std::uniform_real_distribution<float> ux(-500.0f, 500.0f);
	std::uniform_real_distribution<float> uy(-80.0f, 80.0f);
	std::uniform_real_distribution<float> uz(-150.0f, 350.0f);
	const float radius = 0.5f;
	const float rNdc = (radius * 1.5f) / fit.HalfExtent; // TestSphereFrustum inflation

	int insideChecked = 0, outsideChecked = 0;
	for (int i = 0; i < 4000; ++i)
	{
		const Vector3 c{ux(rng), uy(rng), uz(rng)};
		const NdcXYW n = ProjectNdc(fit.VP, c);
		if (n.w < 1e-3f)
			continue;
		// Stay inside the depth slab so the near/far planes don't dominate the
		// side-plane classification we're checking.
		if (n.z < 0.05f || n.z > 0.95f)
			continue;

		const bool insideFootprint = n.x > fxMin + 0.05f && n.x < fxMax - 0.05f &&
		                             n.y > fyMin + 0.05f && n.y < fyMax - 0.05f;
		const float m = slackNdc + rNdc + 0.05f;
		const bool outsideInflated = n.x < fxMin - m || n.x > fxMax + m ||
		                             n.y < fyMin - m || n.y > fyMax + m;

		const bool survived = TestSphereFrustum(c, radius, tightened);
		if (insideFootprint)
		{
			EXPECT_TRUE(survived) << "receiver-visible caster culled (ndc " << n.x << "," << n.y << ")";
			++insideChecked;
		}
		else if (outsideInflated)
		{
			EXPECT_FALSE(survived) << "far-outside caster survived (ndc " << n.x << "," << n.y << ")";
			++outsideChecked;
		}
	}
	// The random sweep must actually exercise both branches.
	EXPECT_GT(insideChecked, 20);
	EXPECT_GT(outsideChecked, 20);
}

TEST(ShadowCasterReductionTests, DegenerateExtentLeavesFullFrustumUntouched)
{
	const Matrix4x4 camVP = MakeTestCameraViewProj();
	Vector3 corners[8];
	ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(camVP, 0.5f, 400.0f, 120.0f, 260.0f, corners);
	const FittedLight fit = FitOrthoLight(corners, Vector3{0.35f, -1.0f, 0.25f});

	Vector4 planes[6]{};
	ExtractFrustumPlanes(fit.VP, planes);
	Vector4 before[6];
	std::memcpy(before, planes, sizeof(before));

	// Zero ortho extent is degenerate — the tightening must no-op (never
	// over-cull), leaving the original full-frustum planes verbatim.
	ShadowMapRenderFeature::TightenCascadeSidePlanes(fit.VP, SliceFootprintNdc(fit.VP, corners).data(), 0.0f,
	                                                 kP2TestSlack, planes);
	for (int i = 0; i < 6; ++i)
	{
		EXPECT_FLOAT_EQ(planes[i].x, before[i].x) << "plane " << i;
		EXPECT_FLOAT_EQ(planes[i].y, before[i].y) << "plane " << i;
		EXPECT_FLOAT_EQ(planes[i].z, before[i].z) << "plane " << i;
		EXPECT_FLOAT_EQ(planes[i].w, before[i].w) << "plane " << i;
	}
}

// GPUScene's lazy world-bounds accessor: empty scene returns false, populated
// scene returns the union of all live instance spheres, and the cache
// reflects the most recent mutation.
TEST(GPUSceneWorldBoundsTests, ReportsCorrectAABBAcrossAddAndRemove)
{
	auto device = CreateVulkanDeviceFast();
	if (!device)
		GTEST_SKIP() << "No Vulkan device available";

	GPUScene scene(device.get());
	ASSERT_TRUE(scene.Initialize(16u, 4u));

	Mathematics::Vector3 mn{}, mx{};
	EXPECT_FALSE(scene.GetInstancesWorldBounds(mn, mx)) << "Empty scene has no bounds.";

	GPUInstance inst{};
	inst.boundingCenter = Vector3{10.0f, 5.0f, -3.0f};
	inst.boundingRadius = 2.0f;
	const uint32_t slot0 = scene.AddInstance(inst);

	ASSERT_TRUE(scene.GetInstancesWorldBounds(mn, mx));
	EXPECT_FLOAT_EQ(mn.x, 8.0f);  EXPECT_FLOAT_EQ(mx.x, 12.0f);
	EXPECT_FLOAT_EQ(mn.y, 3.0f);  EXPECT_FLOAT_EQ(mx.y, 7.0f);
	EXPECT_FLOAT_EQ(mn.z, -5.0f); EXPECT_FLOAT_EQ(mx.z, -1.0f);

	// Add a second instance further out — AABB expands.
	GPUInstance inst2{};
	inst2.boundingCenter = Vector3{-50.0f, 0.0f, 0.0f};
	inst2.boundingRadius = 1.0f;
	scene.AddInstance(inst2);

	ASSERT_TRUE(scene.GetInstancesWorldBounds(mn, mx));
	EXPECT_FLOAT_EQ(mn.x, -51.0f);
	EXPECT_FLOAT_EQ(mx.x, 12.0f);

	// Remove the first instance — AABB shrinks to the remaining one.
	scene.RemoveInstance(slot0);
	ASSERT_TRUE(scene.GetInstancesWorldBounds(mn, mx));
	EXPECT_FLOAT_EQ(mn.x, -51.0f);
	EXPECT_FLOAT_EQ(mx.x, -49.0f);
	EXPECT_FLOAT_EQ(mn.y, -1.0f);
	EXPECT_FLOAT_EQ(mx.y, 1.0f);

	scene.Shutdown();
	device->Shutdown();
}

// R1.2: LODGroup.Bias is the per-entity LOD knob, plumbed by extraction into
// GPUInstance.lodBias (log2 coverage scale for ge_SelectLOD). Covers: default
// zero without the component, the plumbed value with it, and that a Bias edit
// releases the R1.1 instance-skip gate (the instance actually re-uploads).
TEST(RenderServicesWorldMaterialTests, ECSWorldMeshExtraction_LODGroupBiasReachesInstance)
{
	auto device = CreateVulkanDeviceFast();
	if (!device)
	{
	    GTEST_SKIP() << "No Vulkan device available";
	}

	RenderServices rs;
	ASSERT_TRUE(rs.Initialize(device.get()));
	auto setup = RegisterTestRenderable(rs);
	ASSERT_NE(setup.material, nullptr);

	auto worldPtr = std::make_unique<World>(nullptr);
	World& world = *worldPtr;

	CameraId cameraId = rs.Views().AllocateCamera("TestCamera");
	rs.Views().SetCameraData(cameraId, MakeIdentityCameraData());
	ViewId viewId = rs.Views().AllocateView("TestView", cameraId);
	rs.Views().SetViewWorldId(viewId, world.GetWorldId());
	rs.Views().SetViewRenderLayerMask(viewId, 0x1u);

	Entity plain = world.Create();
	plain.Set(Transform::FromTRS({0.0f, 0.0f, 0.0f}, Mathematics::Quaternion{}, {1.0f, 1.0f, 1.0f}));
	plain.Set(MakeTestMeshRenderer(setup));

	Entity biased = world.Create();
	biased.Set(Transform::FromTRS({5.0f, 0.0f, 0.0f}, Mathematics::Quaternion{}, {1.0f, 1.0f, 1.0f}));
	biased.Set(MakeTestMeshRenderer(setup));
	LODGroup lg{};
	lg.Bias = 1.5f;
	biased.Set(lg);

	world.ProcessCommands();

	TransformHierarchySystem hierarchy;
	RenderExtractionSystem extraction(&rs);
	hierarchy.Update(world, 0.0f);
	extraction.Update(world, 0.0f);

	GPUScene* scene = rs.GetGPUScene();
	ASSERT_NE(scene, nullptr);

	auto instanceOf = [&](Entity e) -> const GPUInstance& {
	    auto* meshGpu = world.GetComponent<MeshGPUData>(e.GetHandle());
	    EXPECT_NE(meshGpu, nullptr);
	    EXPECT_NE(meshGpu->instanceIndex, 0xFFFFFFFFu);
	    return scene->GetInstances()[meshGpu->instanceIndex];
	};

	EXPECT_FLOAT_EQ(instanceOf(plain).lodBias, 0.0f);
	EXPECT_FLOAT_EQ(instanceOf(biased).lodBias, 1.5f);

	// A Bias edit must defeat the instance-skip gate and reach the GPU copy.
	lg.Bias = -0.75f;
	biased.Set(lg);
	world.ProcessCommands();
	hierarchy.Update(world, 0.0f);
	extraction.Update(world, 0.0f);
	EXPECT_FLOAT_EQ(instanceOf(biased).lodBias, -0.75f);

	// An unchanged frame keeps the cached value (skip path) without corruption.
	hierarchy.Update(world, 0.0f);
	extraction.Update(world, 0.0f);
	EXPECT_FLOAT_EQ(instanceOf(biased).lodBias, -0.75f);
	EXPECT_FLOAT_EQ(instanceOf(plain).lodBias, 0.0f);
}

// ---------------------------------------------------------------------------
// TransformHierarchySystem path parity.
//
// Path selection is data-driven (flat fast-lane when no parent edges exist ->
// WorldTransform = Transform fanned out over the job system, skipping the
// serial path's sort/gather/CSR machinery; serial resolve for real
// hierarchies). The forceSerial construction seam pins the serial path so a
// single process can compare both. These tests pin the contract:
// bitwise-identical output vs the serial path, exact Version-bump semantics,
// and correct reparent handling.
// ---------------------------------------------------------------------------

namespace {

// Deterministic, non-identity, per-index-distinct local transform so a first
// pass always changes WorldTransform (Version 0 -> 1) and any byte divergence
// between the two paths is caught.
Transform MakeVariedLocalTransform(uint32 i)
{
	const float fi = static_cast<float>(i);
	const Mathematics::Vector3 pos{fi * 0.5f, std::sin(fi) * 2.0f, -fi * 0.25f};
	const Mathematics::Quaternion rot =
	    Components::QuaternionFromEulerXYZDegrees(fi * 3.0f, fi * 7.0f, fi * 11.0f);
	const Mathematics::Vector3 scale{
	    1.0f + static_cast<float>(i % 4) * 0.1f,
	    1.0f + static_cast<float>(i % 3) * 0.2f,
	    1.0f + static_cast<float>(i % 5) * 0.05f};
	return Transform::FromTRS(pos, rot, scale);
}

// Byte-exact comparison of a resolved WorldTransform against expectations.
void ExpectWorldTransformBitwiseEqual(const WorldTransform& a, const WorldTransform& b, uint32 index)
{
	EXPECT_EQ(0, std::memcmp(a.matrix, b.matrix, sizeof(a.matrix)))
	    << "WorldTransform.matrix diverged at entity index " << index;
	EXPECT_EQ(a.Version, b.Version)
	    << "WorldTransform.Version diverged at entity index " << index;
}

} // namespace

// Flat 4000-entity world resolved by the serial path and by the parallel path
// (with a real multi-thread pool). Every WorldTransform must be bitwise-equal:
// same matrix bytes AND same Version. This is the core determinism contract —
// level-parallel preserves per-entity op order, and for roots there is no
// floating-point op at all (WorldTransform = Transform memcpy).
TEST(TransformHierarchyParallelTests, FlatDeterminism_ParallelMatchesSerialBitwise)
{
	constexpr uint32 kCount = 4000;

	World worldSerial(nullptr);
	JobSystem::WorkStealingThreadPool pool(4);
	World worldParallel(&pool);

	std::vector<EntityHandle> serialHandles;
	std::vector<EntityHandle> parallelHandles;
	serialHandles.reserve(kCount);
	parallelHandles.reserve(kCount);

	for (uint32 i = 0; i < kCount; ++i)
	{
		const Transform t = MakeVariedLocalTransform(i);
		serialHandles.push_back(worldSerial.Create(t).GetHandle());
		parallelHandles.push_back(worldParallel.Create(t).GetHandle());
	}
	worldSerial.ProcessCommands();
	worldParallel.ProcessCommands();

	TransformHierarchySystem serial(/*forceSerial=*/true);
	TransformHierarchySystem parallel(/*forceSerial=*/false);
	serial.Update(worldSerial, 0.0f);
	parallel.Update(worldParallel, 0.0f);

	for (uint32 i = 0; i < kCount; ++i)
	{
		const WorldTransform* ws = worldSerial.GetComponent<WorldTransform>(serialHandles[i]);
		const WorldTransform* wp = worldParallel.GetComponent<WorldTransform>(parallelHandles[i]);
		ASSERT_NE(ws, nullptr);
		ASSERT_NE(wp, nullptr);
		ExpectWorldTransformBitwiseEqual(*ws, *wp, i);
		// A non-identity local always bumps Version exactly once on first pass.
		EXPECT_EQ(wp->Version, 1u) << "entity index " << i;
	}
}

// Version bump is memcmp-gated: the first pass bumps 0 -> 1, a second pass with
// no local change leaves it at 1, and this holds identically for both paths.
TEST(TransformHierarchyParallelTests, VersionBumpParity_BumpsOnceThenStable)
{
	constexpr uint32 kCount = 512;

	JobSystem::WorkStealingThreadPool pool(4);
	World world(&pool);
	std::vector<EntityHandle> handles;
	handles.reserve(kCount);
	for (uint32 i = 0; i < kCount; ++i)
		handles.push_back(world.Create(MakeVariedLocalTransform(i)).GetHandle());
	world.ProcessCommands();

	TransformHierarchySystem parallel(/*forceSerial=*/false);

	parallel.Update(world, 0.0f);
	for (uint32 i = 0; i < kCount; ++i)
		EXPECT_EQ(world.GetComponent<WorldTransform>(handles[i])->Version, 1u) << "after pass 1, index " << i;

	// No local edits -> second pass must not bump (skip gate holds).
	parallel.Update(world, 0.0f);
	for (uint32 i = 0; i < kCount; ++i)
		EXPECT_EQ(world.GetComponent<WorldTransform>(handles[i])->Version, 1u) << "after pass 2, index " << i;

	// Editing one local bumps only that entity, and only by one. A value-only
	// edit (no archetype change) can be written in place.
	{
		auto* t = world.GetComponentForWrite<Transform>(handles[3]);
		ASSERT_NE(t, nullptr);
		t->Translate(10.0f, 0.0f, 0.0f);
	}
	parallel.Update(world, 0.0f);
	EXPECT_EQ(world.GetComponent<WorldTransform>(handles[3])->Version, 2u);
	EXPECT_EQ(world.GetComponent<WorldTransform>(handles[4])->Version, 1u);
}

// A parented chain forces the data-driven path onto its serial fallback (a
// valid parent edge exists). Reparenting mid-run must rebuild topology and
// propagate: the moved child follows its new parent. Exercised with the
// default (data-driven) selection to prove the fallback stays correct across
// a structural change.
TEST(TransformHierarchyParallelTests, ReparentTopologyRebuild_ChildFollowsNewParent)
{
	JobSystem::WorkStealingThreadPool pool(4);
	World world(&pool);

	Entity a = world.Create(Transform::FromTRS({10.0f, 0.0f, 0.0f}, Mathematics::Quaternion{}, {1, 1, 1}));
	Entity b = world.Create(Transform::FromTRS({0.0f, 5.0f, 0.0f}, Mathematics::Quaternion{}, {1, 1, 1}));
	Entity c = world.Create(Transform::FromTRS({0.0f, 0.0f, 3.0f}, Mathematics::Quaternion{}, {1, 1, 1}));

	Parent bToA{};
	bToA.parent = a.GetHandle();
	b.Set(bToA);
	Parent cToB{};
	cToB.parent = b.GetHandle();
	c.Set(cToB);
	world.ProcessCommands();

	TransformHierarchySystem parallel(/*forceSerial=*/false);
	parallel.Update(world, 0.0f);

	// b = a(10,0,0) + local(0,5,0); c = b + local(0,0,3).
	const WorldTransform* wc = world.GetComponent<WorldTransform>(c.GetHandle());
	ASSERT_NE(wc, nullptr);
	EXPECT_FLOAT_EQ(wc->matrix[12], 10.0f);
	EXPECT_FLOAT_EQ(wc->matrix[13], 5.0f);
	EXPECT_FLOAT_EQ(wc->matrix[14], 3.0f);

	// Reparent c directly under a: c world = a(10,0,0) + local(0,0,3).
	Parent cToA{};
	cToA.parent = a.GetHandle();
	c.Set(cToA);
	world.ProcessCommands();
	parallel.Update(world, 0.0f);

	wc = world.GetComponent<WorldTransform>(c.GetHandle());
	ASSERT_NE(wc, nullptr);
	EXPECT_FLOAT_EQ(wc->matrix[12], 10.0f);
	EXPECT_FLOAT_EQ(wc->matrix[13], 0.0f);
	EXPECT_FLOAT_EQ(wc->matrix[14], 3.0f);
}

// A mixed world (roots + a parented chain) must resolve bitwise-identically
// with forceSerial pinned and with data-driven selection. Both use the serial
// resolve here (a parent edge exists), so any divergence would be a real
// regression.
TEST(TransformHierarchyParallelTests, MixedHierarchy_OffAndOnBitwiseIdentical)
{
	World worldOff(nullptr);
	JobSystem::WorkStealingThreadPool pool(4);
	World worldOn(&pool);

	auto build = [](World& w, std::vector<EntityHandle>& handles) {
		// 200 flat roots.
		for (uint32 i = 0; i < 200; ++i)
			handles.push_back(w.Create(MakeVariedLocalTransform(i)).GetHandle());
		// A 3-deep parented chain appended after the roots.
		Entity root = w.Create(MakeVariedLocalTransform(1000));
		Entity mid = w.Create(MakeVariedLocalTransform(1001));
		Entity leaf = w.Create(MakeVariedLocalTransform(1002));
		Parent pm{}; pm.parent = root.GetHandle(); mid.Set(pm);
		Parent pl{}; pl.parent = mid.GetHandle(); leaf.Set(pl);
		handles.push_back(root.GetHandle());
		handles.push_back(mid.GetHandle());
		handles.push_back(leaf.GetHandle());
		w.ProcessCommands();
	};

	std::vector<EntityHandle> offHandles;
	std::vector<EntityHandle> onHandles;
	build(worldOff, offHandles);
	build(worldOn, onHandles);

	TransformHierarchySystem off(/*forceSerial=*/true);
	TransformHierarchySystem on(/*forceSerial=*/false);
	off.Update(worldOff, 0.0f);
	on.Update(worldOn, 0.0f);

	ASSERT_EQ(offHandles.size(), onHandles.size());
	for (size_t i = 0; i < offHandles.size(); ++i)
	{
		const WorldTransform* a = worldOff.GetComponent<WorldTransform>(offHandles[i]);
		const WorldTransform* b = worldOn.GetComponent<WorldTransform>(onHandles[i]);
		ASSERT_NE(a, nullptr);
		ASSERT_NE(b, nullptr);
		ExpectWorldTransformBitwiseEqual(*a, *b, static_cast<uint32>(i));
	}
}
