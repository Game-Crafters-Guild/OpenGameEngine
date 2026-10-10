#include <gtest/gtest.h>

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h" // Mesh + Vertex for the registry submesh
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include "RGPassQuery.h"
#include "TestDeviceHelper.h"

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;
using GameEngine::Testing::RGQuery::Family;
using GameEngine::Testing::RGQuery::Subtree;

namespace Nodes = GameEngine::Engine::Renderer::Pipeline::Nodes;

// Both passes TransmissivePassNode declares composite over the lit opaque scene: glass, and the
// late transparents (smoke, ocean spray) that follow the Ocean node. Each must depth-test against
// the opaque depth that produced that scene, so a surface behind an occluder disappears rather than
// painting over it. The attachment is the whole test: the material pipeline already carries
// depthTest=on with GreaterOrEqual (reverse-Z, MaterialSystem InternBaseMaterialPipeline), so
// binding depth is what turns the test on, and NOT binding it disables the test silently — no
// validation error, no assert, just glass or particles drawn over the terrain in front of them.
namespace
{

struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(Rendering::IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 65536) {}
};

TextureDesc ColorTargetDesc(uint32_t samples)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = samples;
    d.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    d.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    return d;
}

TextureDesc DepthTargetDesc(uint32_t samples)
{
    TextureDesc d = ColorTargetDesc(samples);
    d.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    d.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    return d;
}

Rendering::MeshGPUHandle RegisterTestMesh(RenderServices& rs)
{
    Mesh m{};
    m.Name = "TransmissiveTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 1.0f;
    v1.Position[0] = -1.0f; v1.Position[1] = -1.0f;
    v2.Position[0] = 1.0f;  v2.Position[1] = -1.0f;
    v0.Normal[2] = 1.0f; v1.Normal[2] = 1.0f; v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    return rs.GetMeshGPURegistry().RegisterSubmesh({GUID::Generate(), 0}, m);
}

// What one composite-over-the-opaque-scene pass declared for depth in a frame.
struct PassDepthDecl
{
    bool PassDeclared = false;     // positive control: the pass ran at all
    bool AttachedDepth = false;
    bool ReadOnly = false;
    RenderGraph::RGLoadOp Load = RenderGraph::RGLoadOp::DontCare;
    RenderGraph::RGStoreOp Store = RenderGraph::RGStoreOp::DontCare;
    // Whether the pass ALSO declared a fragment-Sampled read of the very resource it attached as
    // depth. Only meaningful when View.DepthResolved aliases View.Depth (no DepthResolve node):
    // that is the case where the attach is the sole declaration and its DepthRead scope alone
    // would leave a ge_sceneDepth texelFetch reading non-visible memory.
    bool SampledAttachedDepth = false;
    // The attached depth's sample count and format, and whether it is the view's own depth.
    uint32_t DepthSamples = 0;
    uint32_t DepthFormat = 0;
    bool AttachedViewDepth = false;
    // Whether the blueprint's DepthResolve published View.DepthResolved (its shaders loaded).
    bool DepthResolvedPublished = false;
    // The late pass's depth copy, when it attaches one: a pass that writes the attached depth,
    // what it samples, and whether it is declared before the late pass.
    bool CopyWritesAttachedDepth = false;
    bool CopySamplesViewDepth = false;
    bool CopySamplesDepthResolved = false;
    bool CopyBeforeLatePass = false;
};

// The depth attachment the named pass declared, if any, plus whether the same resource is also
// declared as a shader-sampled read by that pass.
PassDepthDecl CollectDepthDecl(const RenderGraph::RGFrame& frame,
                               const std::vector<RenderGraph::RGPassId>& passes)
{
    PassDepthDecl out{};
    out.PassDeclared = passes.size() == 1u;
    if (!out.PassDeclared)
        return out;
    RenderGraph::RGResourceId depthRes = RenderGraph::kInvalidId;
    for (const auto& att : frame.Attachments())
    {
        if (att.Pass != passes.front() || !att.IsDepth)
            continue;
        out.AttachedDepth = true;
        out.ReadOnly = att.ReadOnly;
        out.Load = att.Ops.Load;
        out.Store = att.Ops.Store;
        depthRes = att.Tex;
        const RenderGraph::RGResourceDesc& desc = frame.Graph().ResourceDesc(att.Tex);
        out.DepthSamples = desc.SampleCount;
        out.DepthFormat = static_cast<uint32_t>(desc.Format);
    }
    if (depthRes != RenderGraph::kInvalidId)
    {
        for (const RenderGraph::RGAccessRecord& a : frame.Graph().Accesses())
        {
            if (a.Pass == passes.front() && a.Resource == depthRes &&
                a.Access == RenderGraph::RGAccess::Sampled)
            {
                out.SampledAttachedDepth = true;
                break;
            }
        }
    }
    return out;
}

/// WorldRender + TransmissiveRender over one view holding a transmissive batch.
///
/// `samples` > 1 gives the view an MSAA colour+depth pair plus a single-sample resolve, which is
/// the configuration where the world pass resolves and the transmissive pass must composite into
/// the resolved (1-sample) colour — a colour the MSAA depth cannot legally share a pass with.
///
/// The view requests clearDepth and the blueprint declares NO depth prepass on purpose: that is
/// the arrangement in which an unguarded transmissive pass would CLEAR the opaque depth it is
/// supposed to test against.
PassDepthDecl DeclareTransmissiveWorld(Rendering::IDevice* device, uint32_t samples)
{
    PassDepthDecl out{};
    RenderServices rs;
    if (!rs.Initialize(device))
        return out;

    const CameraId camId = rs.Views().AllocateCamera("TransCam");
    CameraData cd{};
    for (int i = 0; i < 16; i += 5)
    {
        cd.view[i] = 1.0f;
        cd.proj[i] = 1.0f;
        cd.viewProj[i] = 1.0f;
    }
    rs.Views().SetCameraData(camId, cd);
    const ViewId viewId = rs.Views().AllocateView("TransView", camId);
    rs.Views().SetViewRenderLayerMask(viewId, 1u);
    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f; // reverse-Z far
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    RenderPipelineNodeRegistry registry;
    if (!registry.Register("WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); },
                           true)
        || !registry.Register("TransmissiveRender",
                              [] { return std::make_unique<Nodes::TransmissivePassNode>(); }, true))
    {
        rs.Shutdown();
        return out;
    }

    RenderPipelineBlueprint bp;
    bp.pipelineName = "TransmissiveDepthTest";
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "World";
        p.type = "WorldRender";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"World","type":"WorldRender"})";
        bp.passes.push_back(p);
    }
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "Transmissive";
        p.type = "TransmissiveRender";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"Transmissive","type":"TransmissiveRender"})";
        bp.passes.push_back(p);
    }
    bp.outputs.push_back({"FinalColor", "View.Resolve"});

    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(bp);

    // A material carrying the Transmission keyword is the ONLY activation term: the machinery
    // keys on material presence in the view's batch keys, not on any per-instance flag.
    Material glass = Material::TestFactory::Create(GUID::Generate(), "GlassMat", 32u);
    Material::TestFactory::SetGraphicsPipelineId(glass, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(glass, 1u);
    {
        Rendering::ShaderVariantKey key{};
        key.materialKeywords |= Rendering::MaterialKeyword::Transmission;
        Material::TestFactory::SetVariantKey(glass, key);
    }

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RenderGraph::RGTexture color =
        frame.ImportPersistentTexture("TD.Color", ColorTargetDesc(samples));
    RenderGraph::RGTexture depth =
        frame.ImportPersistentTexture("TD.Depth", DepthTargetDesc(samples));
    RenderGraph::RGTexture resolve{};
    if (samples > 1u)
        resolve = frame.ImportPersistentTexture("TD.Resolve", ColorTargetDesc(1u));

    const Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);
    rs.BeginWorldDrawFrame();
    WorldSubmissionRecord rec{};
    rec.viewId = viewId;
    rec.meshHandle = mesh;
    rec.material = &glass;
    rec.renderLayerMask = 1u;
    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rec, 1u));
    rs.BuildWorldBatchKeys();

    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, resolve}};
    const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                 rs.Views().GetViews().end());
    instance.Declare(frame, targets, views);

    out = CollectDepthDecl(frame, GameEngine::Testing::RGQuery::DeclaredIds(
                                      frame.Graph(), Subtree{"TransmissiveRenderEntities"}));

    frame.MarkOutput(samples > 1u ? resolve : color);
    frame.Execute();
    device->WaitForIdle();

    rs.Shutdown();
    return out;
}

/// WorldRender + TransmissiveRender over one view that holds a late-forward contributor draw and
/// NO transmissive material — the late-transparent branch on its own (smoke, ocean spray).
///
/// The late pass composites into View.Resolve, the single-sample colour the world pass resolved
/// and the Ocean node made authoritative. `samples` > 1 therefore gives the view an MSAA
/// colour+depth pair plus that 1-sample resolve, which is the arrangement where the view's depth
/// cannot legally share a pass with the colour the late contributors draw into.
///
/// The view requests clearDepth, but nothing on this entry point consumes it — the forward
/// contributor path hardcodes RGLoadOp::Load, so unlike the transmissive scope there is no clear
/// suppression to defeat here. The assertion pins that hardcoded Load against future clear
/// plumbing rather than proving a live suppression.
///
/// `withDepthResolve` puts the shipped pipeline's DepthResolve node first, which publishes the
/// single-sample View.DepthResolved the late pass copies its depth from under MSAA.
PassDepthDecl DeclareLateTransparentWorld(Rendering::IDevice* device, uint32_t samples,
                                          bool withDepthResolve = false)
{
    PassDepthDecl out{};
    RenderServices rs;
    if (!rs.Initialize(device))
        return out;

    const CameraId camId = rs.Views().AllocateCamera("LateCam");
    CameraData cd{};
    for (int i = 0; i < 16; i += 5)
    {
        cd.view[i] = 1.0f;
        cd.proj[i] = 1.0f;
        cd.viewProj[i] = 1.0f;
    }
    rs.Views().SetCameraData(camId, cd);
    const ViewId viewId = rs.Views().AllocateView("LateView", camId);
    rs.Views().SetViewRenderLayerMask(viewId, 1u);
    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f; // reverse-Z far
    rs.Views().SetViewTargets(viewId, 0, 0, 0, clear);

    RenderPipelineNodeRegistry registry;
    if (!registry.Register("WorldRender", [] { return std::make_unique<Nodes::WorldRenderNode>(); },
                           true)
        || !registry.Register("TransmissiveRender",
                              [] { return std::make_unique<Nodes::TransmissivePassNode>(); }, true)
        || !registry.Register("DepthResolve", [] { return std::make_unique<Nodes::DepthResolveNode>(); },
                              true))
    {
        rs.Shutdown();
        return out;
    }

    RenderPipelineBlueprint bp;
    bp.pipelineName = "LateTransparentDepthTest";
    if (withDepthResolve)
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "DepthResolve";
        p.type = "DepthResolve";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"DepthResolve","type":"DepthResolve"})";
        bp.passes.push_back(p);
    }
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "World";
        p.type = "WorldRender";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"World","type":"WorldRender"})";
        bp.passes.push_back(p);
    }
    {
        RenderPipelineBlueprint::Pass p;
        p.id = "Transmissive";
        p.type = "TransmissiveRender";
        p.enabled = true;
        p.perView = true;
        p.passJson = R"({"id":"Transmissive","type":"TransmissiveRender"})";
        bp.passes.push_back(p);
    }
    bp.outputs.push_back({"FinalColor", "View.Resolve"});

    RenderPipelineInstance instance(rs, registry);
    instance.SetBlueprint(bp);

    // Opaque, NOT transmissive: it rasterizes the depth the late pass tests against, and its
    // absence from the transmissive batch keys is what isolates the late-forward branch.
    Material opaque = Material::TestFactory::Create(GUID::Generate(), "OpaqueMat", 33u);
    Material::TestFactory::SetGraphicsPipelineId(opaque, Rendering::GraphicsPipelineId{1u});
    Material::TestFactory::SetGpuSceneMaterialIndex(opaque, 1u);

    FramePools pools(device);
    RenderGraph::RGFrame frame(device, &pools.Persistent, &pools.Transient, &pools.Ring);
    frame.BeginFrame(0);

    RenderGraph::RGTexture color =
        frame.ImportPersistentTexture("LT.Color", ColorTargetDesc(samples));
    RenderGraph::RGTexture depth =
        frame.ImportPersistentTexture("LT.Depth", DepthTargetDesc(samples));
    RenderGraph::RGTexture resolve{};
    if (samples > 1u)
        resolve = frame.ImportPersistentTexture("LT.Resolve", ColorTargetDesc(1u));

    const Rendering::MeshGPUHandle mesh = RegisterTestMesh(rs);
    rs.BeginWorldDrawFrame();
    WorldSubmissionRecord rec{};
    rec.viewId = viewId;
    rec.meshHandle = mesh;
    rec.material = &opaque;
    rec.renderLayerMask = 1u;
    rs.SubmitWorldSubmissions(std::span<const WorldSubmissionRecord>(&rec, 1u));
    rs.BuildWorldBatchKeys();

    // The late-forward stream is the whole activation term for the branch under test. Declaration
    // never inspects a command's payload, so a default-constructed one is enough — and it must be
    // emitted after BeginWorldDrawFrame, which resets the per-view streams.
    const DrawCommand lateCommand{};
    rs.EmitLateForwardCommand(viewId, lateCommand);

    const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, resolve}};
    const std::vector<Rendering::ViewDesc> views(rs.Views().GetViews().begin(),
                                                 rs.Views().GetViews().end());
    instance.Declare(frame, targets, views);

    out = CollectDepthDecl(
        frame, GameEngine::Testing::RGQuery::DeclaredIds(frame.Graph(), Subtree{"LateTransparent"}));
    const std::vector<RenderGraph::RGPassId> latePasses =
        GameEngine::Testing::RGQuery::DeclaredIds(frame.Graph(), Subtree{"LateTransparent"});
    for (const auto& att : frame.Attachments())
    {
        if (att.IsDepth && att.Tex == depth.Id && latePasses.size() == 1u && att.Pass == latePasses.front())
            out.AttachedViewDepth = true;
    }
    RenderGraph::RGResourceId depthResolved = RenderGraph::kInvalidId;
    if (const auto* fr = instance.FrameResourcesFor(&frame))
    {
        const auto it = fr->Textures.find({viewId, "View.DepthResolved"});
        out.DepthResolvedPublished = it != fr->Textures.end() && it->second.Id != depth.Id;
        if (out.DepthResolvedPublished)
            depthResolved = it->second.Id;
    }
    const std::vector<RenderGraph::RGPassId> copyPasses =
        GameEngine::Testing::RGQuery::DeclaredIds(frame.Graph(), Family{"LateTransparentDepthCopy"});
    RenderGraph::RGResourceId lateDepth = RenderGraph::kInvalidId;
    for (const auto& att : frame.Attachments())
    {
        if (att.IsDepth && latePasses.size() == 1u && att.Pass == latePasses.front())
            lateDepth = att.Tex;
    }
    if (copyPasses.size() == 1u && lateDepth != RenderGraph::kInvalidId)
    {
        const RenderGraph::RGPassId copyPass = copyPasses.front();
        for (const auto& att : frame.Attachments())
        {
            if (att.Pass == copyPass && att.IsDepth && att.Tex == lateDepth && !att.ReadOnly)
                out.CopyWritesAttachedDepth = true;
        }
        for (const RenderGraph::RGAccessRecord& a : frame.Graph().Accesses())
        {
            if (a.Pass != copyPass || a.Access != RenderGraph::RGAccess::Sampled)
                continue;
            out.CopySamplesViewDepth = out.CopySamplesViewDepth || a.Resource == depth.Id;
            out.CopySamplesDepthResolved = out.CopySamplesDepthResolved || a.Resource == depthResolved;
        }
        out.CopyBeforeLatePass = latePasses.size() == 1u && copyPass < latePasses.front();
    }

    frame.MarkOutput(samples > 1u ? resolve : color);
    frame.Execute();
    device->WaitForIdle();

    rs.Shutdown();
    return out;
}

} // namespace

// The fix this file exists for. Without a depth attachment the transmissive pass has no hardware
// z-test at all, so glass draws over whatever occludes it. Read-only is not a detail: the recorder
// issues vkCmdSetDepthWriteEnable(VK_FALSE) exactly on a read-only pass, which is what keeps a
// material's own depthWriteEnable from writing glass into the shared depth.
TEST(TransmissiveDepthAttachmentTests, AttachesOpaqueDepthReadOnlyWhenSampleCountsMatch)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const PassDepthDecl d = DeclareTransmissiveWorld(device.get(), /*samples=*/1u);
    ASSERT_TRUE(d.PassDeclared)
        << "positive control: a transmissive material in the view must declare exactly one "
           "transmissive pass — without it every assertion below is vacuous";
    EXPECT_TRUE(d.AttachedDepth)
        << "1-sample colour + 1-sample depth: the pass must bind depth or it has no z-test";
    EXPECT_TRUE(d.ReadOnly) << "glass tests depth but never writes it";
    EXPECT_EQ(d.Load, RenderGraph::RGLoadOp::Load)
        << "the pass composites over an already-rasterized opaque scene: clearing the depth it "
           "tests against would both destroy the opaque depth and disable the occlusion";
    EXPECT_EQ(d.Store, RenderGraph::RGStoreOp::None)
        << "a read-only attach preserves contents and records no attachment write";

    device->Shutdown();
}

// The reason the attach is conditional rather than unconditional. Under MSAA the pass composites
// into the RESOLVED single-sample colour, and DeriveFormatKey takes the sample count from ANY
// attachment > 1 — so binding the MSAA depth would build the pipeline multisampled against a
// 1-sample colour and trip the SetPipeline sample-count assert. There is no single-sample depth
// to substitute (View.DepthResolved is R32F, not an attachable depth format), so the pass stays
// attachment-less and glass falls back to the in-shader compare in adapter_forward.glsl.
TEST(TransmissiveDepthAttachmentTests, WithholdsDepthWhenColorResolvedAndDepthMultisampled)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const PassDepthDecl d = DeclareTransmissiveWorld(device.get(), /*samples=*/4u);
    ASSERT_TRUE(d.PassDeclared)
        << "positive control: the MSAA arm must still declare the pass, else 'no depth "
           "attachment' would be trivially true";
    EXPECT_FALSE(d.AttachedDepth)
        << "MSAA depth beside the resolved 1-sample colour is an illegal mixed-sample pass";

    device->Shutdown();
}

// The late-transparent sibling of the case above. Smoke and ocean spray composite into the same
// resolved colour after the Ocean node, and shipped with no depth attachment at all — so an
// occluded particle painted straight over the geometry in front of it. Read-only is the whole
// contract here: these contributors are registered writesDepth=false, and a ReadWrite attach would
// declare a depth WRITE (RGPassBuilder::AttachDepth issues Graph.Write for it) that every later
// depth reader would then be ordered against, besides letting a non-Blend contributor material's
// depthWriteEnable through.
TEST(TransmissiveDepthAttachmentTests, LateTransparentAttachesOpaqueDepthReadOnly)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const PassDepthDecl d = DeclareLateTransparentWorld(device.get(), /*samples=*/1u);
    ASSERT_TRUE(d.PassDeclared)
        << "positive control: a late-forward command in the view must declare exactly one "
           "LateTransparent pass — without it every assertion below is vacuous";
    EXPECT_TRUE(d.AttachedDepth)
        << "1-sample colour + 1-sample depth: the pass must bind depth or late transparents have "
           "no z-test and draw through occluders";
    EXPECT_TRUE(d.ReadOnly) << "late transparents test depth but never own it";
    EXPECT_EQ(d.Load, RenderGraph::RGLoadOp::Load)
        << "the view requests clearDepth, but this pass composites over an already-rasterized "
           "opaque scene: it must load that depth, never clear it";
    EXPECT_EQ(d.Store, RenderGraph::RGStoreOp::None)
        << "a read-only attach preserves contents and records no attachment write";

    device->Shutdown();
}

// This blueprint declares no DepthResolve node, so View.DepthResolved aliases View.Depth and the
// id-match arm in AddForwardCommandPassForView skips the ge_sceneDepth declaration. The read-only
// attach alone scopes the barrier to EARLY/LATE_FRAGMENT_TESTS + DEPTH_STENCIL_ATTACHMENT_READ,
// which does not make the prior depth write VISIBLE to the fragment shader — and the late
// contributor's own surface shader texelFetches ge_sceneDepth (gpu_fog_particles.glsl) for its
// soft-particle depth fade. The pass must therefore declare the sampled read too; the union is
// what puts FRAGMENT_SHADER + SHADER_READ in the consumer scope.
TEST(TransmissiveDepthAttachmentTests, LateTransparentSamplesTheDepthItAttachesReadOnly)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const PassDepthDecl d = DeclareLateTransparentWorld(device.get(), /*samples=*/1u);
    ASSERT_TRUE(d.PassDeclared) << "positive control: the LateTransparent pass must declare";
    ASSERT_TRUE(d.AttachedDepth)
        << "positive control: without an attachment there is no aliased resource to sample, and "
           "the assertion below would be vacuous";
    ASSERT_TRUE(d.ReadOnly)
        << "positive control: the sampled read is only safe beside a READ-ONLY attach — beside a "
           "ReadWrite one it would be a same-pass feedback loop";
    EXPECT_TRUE(d.SampledAttachedDepth)
        << "the attach's DepthRead scope alone leaves a ge_sceneDepth texelFetch reading memory "
           "that is available but not visible to the fragment stage";

    device->Shutdown();
}

// Under MSAA the late pass draws into the resolved 1-sample colour, which the multisampled view depth
// cannot share a pass with. It attaches a 1-sample D32 copy instead, read-only and loaded, written
// before it by a pass that samples the multisampled view depth itself, so the copy holds everything
// the opaque passes wrote by then — terrain, grass and the ocean as well as the prepass. That holds
// with or without a DepthResolve node: View.DepthResolved, resolved right after the prepass, is never
// the source.
TEST(TransmissiveDepthAttachmentTests, LateTransparentAttachesACopyOfTheViewDepthUnderMsaa)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    for (const bool withDepthResolve : {false, true})
    {
        SCOPED_TRACE(withDepthResolve ? "with a DepthResolve node" : "without a DepthResolve node");
        const PassDepthDecl d = DeclareLateTransparentWorld(device.get(), /*samples=*/4u, withDepthResolve);
        ASSERT_TRUE(d.PassDeclared) << "positive control: the MSAA arm must declare the LateTransparent pass";
        EXPECT_TRUE(d.AttachedDepth) << "under MSAA the late transparents must still have a depth test";
        EXPECT_FALSE(d.AttachedViewDepth) << "the multisampled view depth cannot share the resolved colour's pass";
        EXPECT_EQ(d.DepthSamples, 1u);
        EXPECT_EQ(d.DepthFormat, static_cast<uint32_t>(TextureFormat::D32_FLOAT));
        EXPECT_TRUE(d.ReadOnly) << "late transparents test depth but never own it";
        EXPECT_EQ(d.Load, RenderGraph::RGLoadOp::Load) << "the copy holds the opaque depth: load it, never clear it";
        EXPECT_TRUE(d.CopyWritesAttachedDepth) << "nothing writes the depth the late pass tests against";
        EXPECT_TRUE(d.CopySamplesViewDepth) << "the copy must read the multisampled view depth";
        EXPECT_FALSE(d.CopySamplesDepthResolved)
            << "View.DepthResolved is resolved after the prepass and misses terrain, grass and the ocean";
        EXPECT_TRUE(d.CopyBeforeLatePass);
    }

    device->Shutdown();
}
