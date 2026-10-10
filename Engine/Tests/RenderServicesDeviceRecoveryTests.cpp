// Q6 slice 3a + slice 4 — device-gated re-provision + resume test.
//
// After an injected VK_ERROR_DEVICE_LOST rebuilds the device in place,
// RenderServices::OnDeviceRebuilt (registered in Initialize) must re-provision the
// RenderServices-owned GPU systems: recreate the default textures + bindless set,
// re-bake every material's default bindless indices to LIVE slots, recreate
// GPUScene's buffers, and (slice 4) re-upload every surviving MeshGPURegistry entry
// into its ORIGINAL handle + GPUScene mesh-row so component-resident handles stay
// valid. Slice 4 then calls NotifyReprovisionComplete, so the full chain resumes to
// Healthy without an external stand-in.
//
// Skips when no Vulkan device is available (headless CI without a GPU).

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Rendering/Core/AccelerationStructure.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Assets/ModelAsset.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "TestDeviceHelper.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

namespace
{
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

// A trivial device frame (BeginFrame + empty graphics submit + Present). The
// injected loss fires on a graphics submit; TickDeviceRecovery (called every tick
// before BeginFrame, mirroring the render loop) drives the rebuild retry — the M3
// fix moved it off BeginFrame so a suppressed-rendering failed rebuild cannot starve.
void RunDeviceFrame(IDevice& dev)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
        return;
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

MaterialDocument MakeTestPBRDocument(const std::string& name = "RebuildTestPBR")
{
    MaterialDocument doc{};
    doc.schemaVersion = 2;
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    doc.properties["baseColor"] = std::vector<float>{0.8f, 0.2f, 0.1f, 1.0f};
    doc.properties["metallic"] = 0.0f;
    doc.properties["roughness"] = 0.5f;
    return doc;
}

// Minimal single-triangle mesh so RegisterSubmesh allocates a bucket + writes a
// GPUScene mesh row (indexCount > 0).
Mesh MakeTriangleMesh()
{
    Mesh m{};
    m.Name = "RebuildTestTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[0] = 0.0f;  v0.Position[1] = 1.0f;  v0.Position[2] = 0.0f;  v0.Normal[2] = 1.0f;
    v1.Position[0] = -1.0f; v1.Position[1] = -1.0f; v1.Position[2] = 0.0f;  v1.Normal[2] = 1.0f;
    v2.Position[0] = 1.0f;  v2.Position[1] = -1.0f; v2.Position[2] = 0.0f;  v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    m.MaterialIndex = 0;
    m.MinBounds[0] = -1.0f; m.MinBounds[1] = -1.0f; m.MinBounds[2] = 0.0f;
    m.MaxBounds[0] = 1.0f;  m.MaxBounds[1] = 1.0f;  m.MaxBounds[2] = 0.0f;
    return m;
}
} // namespace

TEST(RenderServicesDeviceRecovery, ReprovisionsRenderServicesOwnedSystems)
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
    ASSERT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);

    // Baseline: the default white texture + its live bindless index, and a
    // material baked against the pre-rebuild default indices.
    const TextureHandle whiteBefore = rs.Textures().GetDefaultWhiteTexture();
    ASSERT_TRUE(whiteBefore.IsValid());
    const uint32_t whiteIdxBefore = rs.Textures().DefaultWhiteBindlessIndex();
    ASSERT_NE(whiteIdxBefore, 0u);

    const GUID matGuid = GUID::Generate();
    Material* mat = rs.Materials().Registry().Register(matGuid, MakeTestPBRDocument());
    ASSERT_NE(mat, nullptr);
    // A material with a baseColor property but no albedo TEXTURE bakes the default
    // white index into its albedo slot.
    EXPECT_EQ(mat->GetBindlessTextureIndex(TextureSlot::kAlbedo), whiteIdxBefore);

    // Slice-4 mesh baseline. A "procedural" submesh with a retained CPU picking
    // mirror stands in for asset-backed geometry: slice 4 re-uploads it from the
    // mirror (no AssetManager asset needed in this unit context). A second submesh
    // with NO source (synthetic GUID, no mirror) must be tombstoned by re-provision.
    auto& registry = rs.GetMeshGPURegistry();
    const GUID meshGuidLive = GUID::Generate();
    const GUID meshGuidDead = GUID::Generate();
    const MeshGPUHandle liveHandle =
        registry.RegisterSubmesh(MeshGPUKey{meshGuidLive, 0}, MakeTriangleMesh(),
                                 /*retainCpuMesh=*/true);
    const MeshGPUHandle deadHandle =
        registry.RegisterSubmesh(MeshGPUKey{meshGuidDead, 0}, MakeTriangleMesh(),
                                 /*retainCpuMesh=*/false);
    ASSERT_TRUE(liveHandle.IsValid());
    ASSERT_TRUE(deadHandle.IsValid());
    const MeshGPUEntry* liveBefore = registry.Find(liveHandle);
    ASSERT_NE(liveBefore, nullptr);
    const uint32_t liveGpuMeshIndexBefore = liveBefore->gpuMeshIndex;
    EXPECT_NE(liveGpuMeshIndexBefore, ~0u) << "a triangle submesh should own a GPUScene mesh row";
    MeshGPUEntryBindings liveBindingsBefore{};
    EXPECT_TRUE(registry.TryGetDrawableBindings(*liveBefore, liveBindingsBefore));
    EXPECT_TRUE(liveBindingsBefore.coreVB.IsValid());

    // Drive frames: the injected loss fires on a graphics submit, latches Lost, and
    // the next BeginFrame rebuilds in place. Slice 4 completes the chain and calls
    // NotifyReprovisionComplete, so the device returns straight to Healthy with a
    // bumped rebuild generation. Loop on the generation (the transient
    // AwaitingReprovision state flips to Healthy inside the same BeginFrame).
    bool rebuilt = false;
    for (int i = 0; i < 16 && !rebuilt; ++i)
    {
        RunDeviceFrame(*device);
        rebuilt = (device->GetDeviceRebuildGeneration() != 0u);
    }
    ASSERT_TRUE(rebuilt) << "injected loss should rebuild the device in place";
    EXPECT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy)
        << "slice 4 must resume to Healthy after the full re-provision chain";

    // (1) Default textures recreated: a FRESH, valid handle (the pre-rebuild one
    // was freed by the teardown). A different handle proves re-provision ran.
    const TextureHandle whiteAfter = rs.Textures().GetDefaultWhiteTexture();
    EXPECT_TRUE(whiteAfter.IsValid());
    EXPECT_NE(whiteAfter, whiteBefore)
        << "default white texture should be a fresh handle after the rebuild";

    // (2) Bindless manager + set recreated and functional: the default index is
    // live, and a fresh texture registers to a non-zero slot (a dead set / stale
    // cache would return the sentinel 0).
    const uint32_t whiteIdxAfter = rs.Textures().DefaultWhiteBindlessIndex();
    EXPECT_NE(whiteIdxAfter, 0u);
    {
        TextureDesc td{};
        td.width = 1;
        td.height = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
        td.debugName = "PostRebuildBindlessProbe";
        TextureHandle probe = device->CreateTexture(td);
        ASSERT_TRUE(probe.IsValid());
        EXPECT_NE(rs.Textures().GetBindlessIndex(probe), 0u);
        device->DestroyTexture(probe);
    }

    // (3) The material's baked albedo slot now points at the LIVE default index
    // (re-baked by OnDeviceRebuilt's per-material InitBindlessDefaults), not a
    // stale index from the dead device.
    EXPECT_EQ(mat->GetBindlessTextureIndex(TextureSlot::kAlbedo), whiteIdxAfter);

    // (4) GPUScene survived and its buffers were recreated on the fresh device.
    // Device-level proof: create + upload + read back a host-visible buffer.
    ASSERT_NE(rs.GetGPUScene(), nullptr);
    {
        const uint32_t pattern[4] = {0xC0FFEEu, 0xBADF00Du, 0x1234u, 0xABCDu};
        BufferHandle buf = device->CreateUploadBuffer(sizeof(pattern), "RSRebuildProofBuffer");
        ASSERT_TRUE(buf.IsValid());
        device->UpdateBuffer(buf, 0, sizeof(pattern), pattern);
        void* mapped = device->MapBuffer(buf);
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(std::memcmp(mapped, pattern, sizeof(pattern)), 0);
        device->UnmapBuffer(buf);
        device->DestroyBuffer(buf);
    }

    // (5) Slice 4: the live submesh survived the rebuild in place. Its generational
    // handle still resolves, its GPUScene mesh-row index is UNCHANGED (F9), and its
    // vertex/index buffers are fresh + valid (re-uploaded from the CPU mirror into
    // the recreated bucket pools).
    const MeshGPUEntry* liveAfter = registry.Find(liveHandle);
    ASSERT_NE(liveAfter, nullptr) << "the live mesh handle must stay valid across the rebuild";
    EXPECT_EQ(liveAfter->gpuMeshIndex, liveGpuMeshIndexBefore)
        << "slice 4 must preserve the GPUScene mesh-row index in place (F9)";
    MeshGPUEntryBindings liveBindings{};
    EXPECT_TRUE(registry.TryGetDrawableBindings(*liveAfter, liveBindings))
        << "the re-provisioned mesh must be drawable again: fresh stamp, fresh pools";
    EXPECT_TRUE(liveBindings.coreVB.IsValid())
        << "the live mesh core VB should be re-uploaded to a fresh live buffer";
    EXPECT_TRUE(liveBindings.indexBuffer.IsValid());
    EXPECT_EQ(liveAfter->indexCount, 3u);
    EXPECT_NE(liveAfter->uploadSeq, 0u)
        << "re-provision must mint a fresh upload stamp, not inherit the pre-rebuild one";

    // (6) Slice 4: the sourceless submesh was tombstoned — the handle still resolves
    // (the table survives) but resolves to a valid-but-empty entry whose bindings are
    // invalid, so extraction / the draw scatter skip it gracefully.
    const MeshGPUEntry* deadAfter = registry.Find(deadHandle);
    ASSERT_NE(deadAfter, nullptr);
    EXPECT_EQ(deadAfter->bucketKey, VertexAttributeFlags::None);
    MeshGPUEntryBindings deadBindings{};
    deadBindings.coreVB = liveBindings.coreVB; // pre-dirtied: refusal must overwrite it
    EXPECT_FALSE(registry.TryGetDrawableBindings(*deadAfter, deadBindings));
    EXPECT_FALSE(deadBindings.coreVB.IsValid());
    EXPECT_EQ(deadAfter->uploadSeq, 0u) << "a tombstone carries no upload stamp";

    // (7) Slice 4 resumed the chain: the device is Healthy directly out of the
    // rebuild (OnDeviceRebuilt called NotifyReprovisionComplete). A redundant call is
    // a CAS-guarded no-op.
    EXPECT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);
    device->NotifyReprovisionComplete();
    EXPECT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);

    // The rebuilt + re-provisioned + resumed device frames cleanly (no crash /
    // no second loss — the one-shot injection was consumed at first fire).
    RunDeviceFrame(*device);
    RunDeviceFrame(*device);
    EXPECT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);

    rs.Shutdown();
    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

// Per-view light UBO across an in-place rebuild.
//
// ReprovisionsRenderServicesOwnedSystems above runs the same rebuild but never
// allocates a view, so its PerView map is empty and it is structurally blind to
// this: it passes identically whether or not the rebuild resets per-view state.
// A view is therefore the whole point of this test.
//
// WriteViewLightBuffer creates the buffer lazily behind `!IsValid()`, and a
// handle stays non-null once its device is gone, so nothing about the handle
// tells the guard to re-fire. If the rebuild does not clear it, the buffer is
// never recreated and every material-set write that binds it drops with
// "buffer handle did not resolve" — lit surfaces render black for the life of
// the process.
TEST(RenderServicesDeviceRecovery, PerViewLightBufferIsForgottenSoItIsRecreatedAfterRebuild)
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

    const Rendering::ViewId viewId =
        rs.Views().AllocateView("LightBufferRebuildTestView", Rendering::CameraId{});
    rs.WriteViewLightBuffer(viewId);

    const auto* pv = rs.Views().FindPerView(viewId);
    ASSERT_NE(pv, nullptr);
    const auto beforeSlot = std::find_if(pv->LightBuffers.begin(), pv->LightBuffers.end(),
                                         [](const auto& b) { return b.IsValid(); });
    ASSERT_NE(beforeSlot, pv->LightBuffers.end())
        << "the fixture must have created a light buffer, or the test proves nothing";

    // Drive the injected loss and the rebuild that follows it. Loop on the
    // rebuild generation rather than a fixed frame count: the loss fires on a
    // graphics submit and the rebuild happens on a later BeginFrame, so how many
    // frames it takes is not something the test gets to assume.
    bool rebuilt = false;
    for (int i = 0; i < 16 && !rebuilt; ++i)
    {
        RunDeviceFrame(*device);
        rebuilt = (device->GetDeviceRebuildGeneration() != 0u);
    }
    ASSERT_TRUE(rebuilt) << "the injected loss should rebuild the device in place";
    ASSERT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);

    // The handles must be GONE, not merely stale. A stale handle still reports
    // IsValid(), which is exactly why the lazy create could not re-fire.
    const auto* pvAfter = rs.Views().FindPerView(viewId);
    ASSERT_NE(pvAfter, nullptr);
    for (const auto& buf : pvAfter->LightBuffers)
        EXPECT_FALSE(buf.IsValid())
            << "a handle surviving the rebuild leaves the lazy create permanently inert";

    // And the lazy path must actually produce a live buffer on the new device.
    // Inside an acquired frame, as production does: the light buffer is one slot
    // of a per-frame ring, and the loop above left the device rotated onto a slot
    // whose fence no BeginFrame has waited.
    ASSERT_TRUE(device->BeginFrame());
    rs.WriteViewLightBuffer(viewId);
    const auto* pvRecreated = rs.Views().FindPerView(viewId);
    ASSERT_NE(pvRecreated, nullptr);
    const auto afterSlot = std::find_if(pvRecreated->LightBuffers.begin(),
                                        pvRecreated->LightBuffers.end(),
                                        [](const auto& b) { return b.IsValid(); });
    EXPECT_NE(afterSlot, pvRecreated->LightBuffers.end())
        << "forgetting is only half the fix: the write must recreate on the live device";

    rs.Shutdown();
    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

// Per-view draw streams across an in-place rebuild (#2867).
//
// The forward and depth streams hold recorded draws, and a recorded draw names the
// buffers and descriptor sets of the device it was recorded on. A stream that
// survives the rebuild is replayed once by the first pass that reads it before the
// producers emit again: the live editor's camera prepass drew a terrain head from
// the lost device once after every recovery, dropping eight descriptor writes.
TEST(RenderServicesDeviceRecovery, PerViewDrawStreamsAreForgottenAtRebuild)
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
    const Rendering::ViewId viewId = rs.Views().AllocateView("DrawStreamRebuildTestView", Rendering::CameraId{});
    const DrawCommand draw{};
    const DrawCommand head{};
    rs.EmitForwardCommand(viewId, draw, ForwardDrawDepth::Prepass, &head);
    ASSERT_EQ(rs.GetForwardCommands(viewId).size(), 1u);
    ASSERT_EQ(rs.GetDepthCommands(viewId, DepthPassType::Prepass).size(), 1u);

    bool rebuilt = false;
    for (int i = 0; i < 16 && !rebuilt; ++i)
    {
        RunDeviceFrame(*device);
        rebuilt = (device->GetDeviceRebuildGeneration() != 0u);
    }
    ASSERT_TRUE(rebuilt) << "the injected loss should rebuild the device in place";

    EXPECT_TRUE(rs.GetForwardCommands(viewId).empty()) << "a forward draw from the lost device survived the rebuild";
    EXPECT_TRUE(rs.GetDepthCommands(viewId, DepthPassType::Prepass).empty())
        << "a prepass head from the lost device survived the rebuild";

    rs.Shutdown();
    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

// Readback tickets across an in-place rebuild — the editor's post-rebuild
// "readback never completed" screenshot wedge. Three arms:
//   control      — a ticket on the healthy device resolves (fixture proof);
//   in-flight    — a ticket stamped before the loss can never resolve (its
//                  buffer and its token's timeline died with the device) and
//                  must fail OBSERVABLY (IsConsumed) instead of polling forever;
//   post-rebuild — a fresh ticket after the rebuild must resolve again, which
//                  requires RGFrame::BeginFrame to re-arm its per-queue
//                  timeline semaphores on the rebuild-generation change.
TEST(RenderServicesDeviceRecovery, ReadbackTicketsAcrossDeviceRebuild)
{
    // Frame ordinals are device BeginFrame counts (1-based): frames 1-2 carry
    // the control and in-flight ticket frames; the loss fires on the first
    // graphics submit at or after device frame 4 (a plain drive frame).
    SetEnvVar("GE_VK_FORCE_DEVICE_LOST", "4");
    auto device = CreateVulkanDeviceFast();
    if (!device)
    {
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        GTEST_SKIP() << "No Vulkan device available";
    }
    {
        RenderGraph::RGResourcePool persistent(device.get());
        RenderGraph::RGTransientPool transient(device.get());
        RenderGraph::RGUploadRing uploadRing(device.get(), 2, 4096);
        RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &uploadRing);

        // One device frame carrying one RG frame with a cleared 8x8 color
        // target and a readback ticket of it, stamped the way the frame
        // drivers do (OnFrameSubmittedReadbacksRG right after Execute).
        auto runTicketFrame = [&](uint64_t rgFrameIndex, const char* name)
            -> std::shared_ptr<Rendering::RGReadbackTicket>
        {
            device->TickDeviceRecovery();
            if (!device->BeginFrame())
                return nullptr;
            frame.BeginFrame(rgFrameIndex);
            TextureDesc td{};
            td.width = 8;
            td.height = 8;
            td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
            td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource
                                             | TextureUsage::RenderTarget);
            RenderGraph::RGTexture color = frame.CreateTexture("TicketRebuild.Color", td);
            frame.AddPass(
                "TicketRebuild.Clear", 0,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    RenderGraph::RGAttachmentOps ops{};
                    ops.Load = RenderGraph::RGLoadOp::Clear;
                    p.AttachColor(0, color, ops);
                },
                [](RenderGraph::RGContext&) {});
            frame.MarkOutput(color);
            auto ticket = Rendering::RequestTextureReadbackRG(device.get(), frame, color, name);
            frame.Execute();
            Rendering::OnFrameSubmittedReadbacksRG(frame, frame.SubmissionToken());
            device->Present();
            return ticket;
        };

        // Control: a healthy-device ticket resolves — proves the fixture can
        // tell a resolving ticket from a wedged one.
        auto control = runTicketFrame(0, "TicketRebuild.Control");
        ASSERT_TRUE(control);
        device->WaitForIdle();
        ViewReadbackResult controlRes{};
        ASSERT_TRUE(control->TryGet(controlRes))
            << "control arm: a healthy-device ticket must resolve";
        EXPECT_EQ(controlRes.width, 8u);

        // In-flight: declared + stamped on device frame 2, before the loss.
        auto inflight = runTicketFrame(1, "TicketRebuild.InFlight");
        ASSERT_TRUE(inflight);
        ASSERT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy)
            << "the injected loss must not fire before the in-flight declare";

        // Injected loss -> in-place rebuild. No RenderServices consumer is
        // registered here, so the device parks at AwaitingReprovision and this
        // test stands in via NotifyReprovisionComplete (DeviceRecoveryTests
        // pattern).
        bool reachedAwaiting = false;
        for (int i = 0; i < 12 && !reachedAwaiting; ++i)
        {
            RunDeviceFrame(*device);
            reachedAwaiting = (device->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
        }
        ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";
        device->NotifyReprovisionComplete();
        ASSERT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);

        // The dead readback fails observably instead of polling forever: its
        // buffer and its token's timeline were freed by the rebuild teardown.
        ViewReadbackResult deadRes{};
        EXPECT_FALSE(inflight->TryGet(deadRes));
        EXPECT_TRUE(inflight->IsConsumed())
            << "a readback whose device was rebuilt must fail observably "
               "(holders retry or error), not wedge its holder forever";

        // Post-rebuild: a fresh ticket resolves again. This is the live defect:
        // every post-rebuild screenshot readback reported 'never completed'.
        auto post = runTicketFrame(2, "TicketRebuild.PostRebuild");
        ASSERT_TRUE(post);
        device->WaitForIdle();
        ViewReadbackResult postRes{};
        EXPECT_TRUE(post->TryGet(postRes))
            << "post-rebuild readback must resolve — a stale RGFrame queue "
               "timeline wedges every later readback for the process lifetime";
        if (!postRes.pixels.empty())
            EXPECT_EQ(postRes.width, 8u);
    }
    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}

// Acceleration structures across an in-place rebuild (#2758).
//
// Every BLAS and TLAS alive at a loss must be destroyed against the lost device
// before it is destroyed; the validation layer reports each survivor at
// vkDestroyDevice, and a later destroy of one against the new device as an
// invalid object. The backend object and its TLAS slots survive the rebuild, so
// consumers' cached backend pointers and channels stay valid. The validation
// layer's object tracking is the counter here; with its default assert a
// survivor aborts the test, and GE_VK_VALIDATION_ASSERT=0 reads the counts.
TEST(RenderServicesDeviceRecovery, AccelerationStructuresAreReleasedWithTheLostDevice)
{
    SetEnvVar("GE_VK_FORCE_DEVICE_LOST", "2");
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDebugLayer = true;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && !device->Initialize(desc))
        device.reset();
    if (!device || !device->GetCapabilities().supportsRayQuery || !device->GetValidationStats().Enabled)
    {
        if (device)
            device->Shutdown();
        UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
        GTEST_SKIP() << "needs a Vulkan device with ray query and the validation layer";
    }

    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    SceneAccelerationStructureService* sceneAS = rs.EnsureSceneAccelerationStructureService();
    ASSERT_NE(sceneAS, nullptr);
    IAccelerationStructureBackend* backend = device->GetAccelerationStructureBackend();
    ASSERT_EQ(sceneAS->GetBackend(), backend);
    // One TLAS and one BLAS alive at the loss. Sizing a BLAS reads only counts,
    // so no vertex data is needed to create it.
    const TlasSlotHandle channel = sceneAS->AcquireTlasChannel("RecoveryTest");
    ASSERT_TRUE(backend->PrepareTlas(channel, 4));
    BlasTriangleGeometry geometry{};
    geometry.VertexStrideBytes = 12;
    geometry.MaxVertex = 2;
    geometry.IndexCount = 3;
    ASSERT_TRUE(backend->CreateBlas(geometry).IsValid());
    // And one BLAS the pool owns, built through the pool's own sweep, so the test
    // pins that RenderServices' rebuild reaches the pool and it forgets the BLAS.
    rs.GetMeshGPURegistry().RegisterSubmesh(MeshGPUKey{GUID::Generate(), 0}, MakeTriangleMesh(),
                                            /*retainCpuMesh=*/true);
    sceneAS->BeginFrame();
    std::shared_ptr<SceneAccelerationStructureService::BuildConfirmToken> poolBuild;
    ASSERT_EQ(sceneAS->CollectPendingBuilds(poolBuild).size(), 1u);
    poolBuild->MarkExecuted();
    sceneAS->BeginFrame();
    ASSERT_EQ(sceneAS->GetBlasCount(), 1u);

    device->ResetValidationStats();
    bool rebuilt = false;
    for (int i = 0; i < 16 && !rebuilt; ++i)
    {
        RunDeviceFrame(*device);
        rebuilt = device->GetDeviceRebuildGeneration() != 0u;
    }
    ASSERT_TRUE(rebuilt) << "injected loss should rebuild the device in place";

    uint64_t leaked = 0;
    uint64_t foreignDestroys = 0;
    for (const ValidationVuidStat& v : device->GetValidationStats().Vuids)
    {
        if (v.Vuid == "VUID-vkDestroyDevice-device-05137")
            leaked += v.Count;
        if (v.Vuid == "VUID-vkDestroyAccelerationStructureKHR-accelerationStructure-parameter")
            foreignDestroys += v.Count;
    }
    EXPECT_EQ(leaked, 0u) << "objects the lost device was destroyed with";
    EXPECT_EQ(foreignDestroys, 0u) << "old-device objects destroyed against the new device";

    // The consumers' backend and TLAS channel survive and work on the new device.
    EXPECT_EQ(device->GetAccelerationStructureBackend(), backend);
    EXPECT_EQ(sceneAS->GetBackend(), backend);
    EXPECT_EQ(backend->GetLiveBlasCount(), 0u) << "BLASes die with the device; the pool cold-rebuilds";
    EXPECT_EQ(sceneAS->GetBlasCount(), 0u) << "RenderServices' rebuild did not make the pool forget its BLAS";
    ASSERT_TRUE(backend->PrepareTlas(channel, 4));
    EXPECT_NE(backend->GetTlasDeviceAddress(channel), 0u);

    // One TLAS build on the rebuilt device: it draws on the backend's scratch, which
    // must have died with the old device rather than carry its address across.
    BufferDesc instancesDesc{};
    instancesDesc.size = 4 * sizeof(TlasInstanceData);
    instancesDesc.usage = static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress |
                                                BufferUsage::AccelerationStructureBuildInput);
    instancesDesc.memoryUsage = BufferMemoryUsage::Upload;
    instancesDesc.debugName = "RecoveryTest.TlasInstances";
    const BufferHandle instances = device->CreateBuffer(instancesDesc);
    ASSERT_TRUE(instances.IsValid());
    device->ResetValidationStats();
    {
        auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        backend->RecordPreBuildBarrier(*cl);
        backend->RecordTlasBuild(*cl, channel, instances, 0, 0);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        device->ExecuteCommandLists(lists);
        device->WaitForIdle();
    }
    EXPECT_EQ(device->GetValidationStats().ErrorCount, 0u) << "the TLAS build on the rebuilt device";
    EXPECT_EQ(device->GetDeviceHealth(), DeviceHealth::Healthy);
    EXPECT_TRUE(backend->IsTlasBuilt(channel));
    device->DestroyBuffer(instances);

    sceneAS->ReleaseTlasChannel(channel);
    rs.Shutdown();
    device->Shutdown();
    UnsetEnvVar("GE_VK_FORCE_DEVICE_LOST");
}
