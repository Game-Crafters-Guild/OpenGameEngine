// Q6 slice 3b — ECS component-handle recovery pass tests.
//
// After an in-place device rebuild, RecoverEcsComponentHandlesAfterDeviceRebuild
// (driven by RenderingLoop off the device rebuild generation) must:
//   1. Zero procedurally-generated component-resident GPU handles (MorphTargetWeights
//      runtime meshes) so the owning system re-registers them on its next tick.
//   2. Force-dirty the render columns so the change-gated RenderExtractionSystem
//      re-fires the full lane for every entity — proven here via a Changed<> gate.
//   3. Flag the RenderServices full-extraction hook.
// A separate case proves the device-rebuild generation the pass keys off actually
// bumps across a real injected loss.
//
// Device cases skip when no Vulkan device is available (headless CI without a GPU).

#include <gtest/gtest.h>

#include "Engine/Rendering/DeviceLostEcsRecovery.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "ECSModules/Rendering/Systems/MorphTargetSystem.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"

#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include "TestDeviceHelper.h"

using namespace GameEngine;
using GameEngine::Engine::Renderer::MorphTargetSystem;
using GameEngine::Engine::Renderer::RenderServices;
using GameEngine::Engine::Renderer::RecoverEcsComponentHandlesAfterDeviceRebuild;
using GameEngine::Engine::Renderer::ShouldRunHealthyGatedDeviceRecovery;
using GameEngine::Rendering::DeviceHealth;

namespace
{
constexpr float32 kDt = 1.0f / 60.0f;

void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
void UnsetEnvVar(const char* key)
{
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

void RunDeviceFrame(Rendering::IDevice& dev)
{
    // Mirror the render loop: TickDeviceRecovery (unconditional per-tick poll, before
    // BeginFrame) drives the rebuild retry after the M3 fix moved it off BeginFrame.
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
        return;
    auto cl = dev.CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<Rendering::CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

// Entities visited by a Changed<C>-gated scan since `gate` — the extraction
// probe shape (const-bound so the probe itself never stamps).
template <typename C>
size_t VisitedSince(ECS::World& world, uint64 gate)
{
    ECS::ChangeGate g;
    g.LastRunVersion = gate;
    size_t visited = 0;
    auto q = world.Query<ECS::Read<C>>();
    q.template Changed<C>(g);
    q.BatchEach([&](const C*, std::size_t count) { visited += count; });
    return visited;
}

// A one-mesh model with a single morph target that raises vertex 0.
Mesh MakeMorphSourceMesh()
{
    Mesh m{};
    m.Name = "Slice3bMorphTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 1.0f;
    v1.Position[0] = -1.0f;
    v1.Position[1] = -1.0f;
    v2.Position[0] = 1.0f;
    v2.Position[1] = -1.0f;
    v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    m.MinBounds[0] = -1.0f; m.MinBounds[1] = -1.0f; m.MinBounds[2] = 0.0f;
    m.MaxBounds[0] = 1.0f;  m.MaxBounds[1] = 1.0f;  m.MaxBounds[2] = 0.0f;
    MorphTarget target{};
    target.Name = "raise";
    target.VertexIndices = {0u};
    target.PositionDeltas = {0.0f, 2.0f, 0.0f};
    m.MorphTargets.push_back(std::move(target));
    return m;
}
} // namespace

// The pass zeroes the morph runtime handle, force-dirties the render columns
// (a Changed<> gate sampled before the pass fires afterward), flags the full-
// extraction hook, and MorphTargetSystem re-registers a fresh runtime mesh on
// its next tick.
TEST(DeviceLostEcsRecovery, ZeroesMorphHandleForceDirtiesAndReRegisters)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    const GUID modelGuid = GUID::Generate();
    auto model = std::make_unique<ModelAsset>(
        modelGuid, std::filesystem::path("Synthetic://Slice3bMorph"));
    Vector<Mesh> meshes;
    meshes.push_back(MakeMorphSourceMesh());
    model->SetMeshesForTest(std::move(meshes));
    ASSERT_TRUE(model->IsLoaded());

    MorphTargetSystem morphSystem(&rs);
    morphSystem.SetModelResolverForTest(
        [&](const GUID& guid) { return guid == modelGuid ? model.get() : nullptr; });

    ECS::World world(nullptr);
    ECS::Entity e = world.Create();
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    mr.meshId = 0;
    e.Set(mr);
    Components::MorphTargetWeights morph{};
    morph.weights[0] = 1.0f; // non-zero: registers a runtime morphed mesh
    morph.version = 2;
    e.Set(morph);
    e.Set(Components::LocalBounds{});
    world.ProcessCommands();

    // Drive to a registered runtime morphed mesh.
    morphSystem.Update(world, kDt);
    const auto* weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    const uint64 runtimeHandleBefore = weights->runtimeMeshGpuHandleId;
    ASSERT_NE(runtimeHandleBefore, 0u) << "setup: a runtime morphed mesh must be registered";

    // Simulate what a device rebuild triggers: sample a change gate, run the pass.
    const uint64 gate = world.GetGlobalSystemVersion();
    ASSERT_FALSE(rs.ConsumeHlodResidencyExtractionPending()); // clean slate
    const auto report = RecoverEcsComponentHandlesAfterDeviceRebuild(world, rs);

    // (1) Runtime handle zeroed so the owning system treats it as missing.
    weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    EXPECT_EQ(weights->runtimeMeshGpuHandleId, 0u);
    EXPECT_EQ(report.MorphRuntimeHandlesCleared, 1u);

    // (2) The render columns are force-dirtied: a Changed<> gate sampled before
    // the pass now visits the entity on both probe columns.
    EXPECT_GT(VisitedSince<Components::MeshRenderer>(world, gate), 0u)
        << "MeshRenderer column was not force-dirtied — extraction would skip it";
    EXPECT_GT(VisitedSince<Components::LocalBounds>(world, gate), 0u)
        << "LocalBounds column was not force-dirtied";
    EXPECT_EQ(report.MeshRenderersDirtied, 1u);

    // (3) The full-extraction hook is flagged (consumed once).
    EXPECT_TRUE(report.FullExtractionRequested);
    EXPECT_TRUE(rs.ConsumeHlodResidencyExtractionPending());

    // (4) On its next tick the owning system re-registers a fresh runtime mesh.
    morphSystem.Update(world, kDt);
    weights = world.GetComponent<Components::MorphTargetWeights>(e.GetHandle());
    ASSERT_NE(weights, nullptr);
    EXPECT_NE(weights->runtimeMeshGpuHandleId, 0u)
        << "MorphTargetSystem must re-register the runtime mesh after the pass zeroed it";

    rs.Shutdown();
    device->Shutdown();
}

// Force-dirty is a no-op-safe pass on an empty world and reports zero work.
TEST(DeviceLostEcsRecovery, EmptyWorldIsSafeNoWork)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world(nullptr);
    const auto report = RecoverEcsComponentHandlesAfterDeviceRebuild(world, rs);
    EXPECT_EQ(report.MorphRuntimeHandlesCleared, 0u);
    EXPECT_EQ(report.MeshRenderersDirtied, 0u);
    EXPECT_TRUE(report.FullExtractionRequested);
    EXPECT_TRUE(rs.ConsumeHlodResidencyExtractionPending());

    rs.Shutdown();
    device->Shutdown();
}

// The rebuild generation the pass keys off starts at 0 and bumps across a real
// injected device loss + in-place rebuild.
TEST(DeviceLostEcsRecovery, DeviceRebuildGenerationBumps)
{
    SetEnvVar("GE_VK_FORCE_DEVICE_LOST", "2");
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        GTEST_SKIP() << "No Vulkan device available";
    }

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    EXPECT_EQ(device->GetDeviceRebuildGeneration(), 0u)
        << "a never-rebuilt device reports generation 0";
    const uint64 genBefore = device->GetDeviceRebuildGeneration();

    // Slice 4 completes the re-provision chain and calls NotifyReprovisionComplete,
    // so the device resumes to Healthy (the transient AwaitingReprovision flips
    // inside the same BeginFrame). Poll on the generation the rebuild bumps.
    bool rebuilt = false;
    for (int i = 0; i < 16 && !rebuilt; ++i)
    {
        RunDeviceFrame(*device);
        rebuilt = (device->GetDeviceRebuildGeneration() != genBefore);
    }
    ASSERT_TRUE(rebuilt) << "injected loss should rebuild the device in place";

    EXPECT_GT(device->GetDeviceRebuildGeneration(), genBefore)
        << "a successful in-place rebuild must bump the generation the 3b poll keys off";
    EXPECT_EQ(device->GetDeviceHealth(), Rendering::DeviceHealth::Healthy)
        << "slice 4 resumes to Healthy after the full re-provision chain";

    device->NotifyReprovisionComplete(); // redundant no-op once already Healthy
    rs.Shutdown();
    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

// The HLOD recovery gate (SceneDocumentManager::PollDeviceRebuildAndRecoverHlod
// delegates to this) must DEFER while the device is suppressed — RegisterSubmesh /
// UploadMesh through the dead MeshGPURegistry pools would be an upload-time UAF —
// and fire exactly once on the first Healthy tick after re-provision. Proven at the
// decision level (no device needed): a false return is a "did NOT reconcile" tick,
// so no RegisterSubmesh runs while suppressed; a true return is the deferred reconcile
// landing after NotifyReprovisionComplete (which slice 4 will call).
TEST(DeviceLostEcsRecovery, HlodGateDefersWhileSuppressedThenFiresOnceOnHealthy)
{
    uint64_t lastGen = 0;

    // A rebuild bumped the generation to 1, but the device is still suppressed —
    // defer WITHOUT consuming the generation (so it is not lost before Healthy).
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::AwaitingReprovision, 1, lastGen));
    EXPECT_EQ(lastGen, 0u) << "deferral must not consume the generation";
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Rebuilding, 1, lastGen));
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Lost, 1, lastGen));
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Failed, 1, lastGen));
    EXPECT_EQ(lastGen, 0u) << "no suppressed state may consume the generation";

    // First Healthy tick after re-provision: the deferred recovery fires exactly once.
    EXPECT_TRUE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Healthy, 1, lastGen));
    EXPECT_EQ(lastGen, 1u);
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Healthy, 1, lastGen))
        << "must not re-fire for the same generation";

    // A never-rebuilt device (generation 0) never fires.
    uint64_t freshGen = 0;
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Healthy, 0, freshGen));
    EXPECT_EQ(freshGen, 0u);

    // A second rebuild that happens during suppression is honored on the next Healthy
    // tick, not swallowed by the earlier deferred reads.
    EXPECT_FALSE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::AwaitingReprovision, 2, lastGen));
    EXPECT_EQ(lastGen, 1u);
    EXPECT_TRUE(ShouldRunHealthyGatedDeviceRecovery(DeviceHealth::Healthy, 2, lastGen));
    EXPECT_EQ(lastGen, 2u);
}
