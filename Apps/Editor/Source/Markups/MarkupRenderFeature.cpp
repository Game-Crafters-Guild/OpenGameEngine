#include "Markups/MarkupRenderFeature.h"

#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Markups/MarkupEditorBridge.h"
#include "MarkupECS/MarkupRegionMesh.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "SceneView/SceneViewGizmos.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace GameEngine::Editor
{

using namespace Rendering;

namespace
{

constexpr int32_t kOverlayPhase = static_cast<int32_t>(PassPhase::kOverlay);
// markup_glow's camera block: set 0, binding 5, as every Scene View overlay binds it.
constexpr uint32_t kCameraBinding = 5;
// markup_glow's scene depth (the contact line): set 0, binding 1.
constexpr uint32_t kSceneDepthBinding = 1;

// markup_glow's push constants (Shaders/markup_glow.vert, .frag).
struct GlowPush
{
    float Model[16];
    float Color[4];
    float Rim[4];
};
static_assert(sizeof(GlowPush) == 96, "GlowPush must match markup_glow's push constant block");

// The stages of the glow package at `path` for `kind`; false when the package is missing (logged).
bool LoadGlowShaderBytes(const char* path, ShaderSourceKind kind, std::vector<uint8_t>& outVs,
                         std::vector<uint8_t>& outFs)
{
    ShaderPackage package{};
    std::string error;
    if (LoadShaderPkg(path, kind, package, &error))
    {
        if (const auto vs = package.stageBytes.find("vs"); vs != package.stageBytes.end())
            outVs = std::move(vs->second);
        if (const auto fs = package.stageBytes.find("fs"); fs != package.stageBytes.end())
            outFs = std::move(fs->second);
    }
    if (outVs.empty() || outFs.empty())
    {
        Logger::Log::Error("Mark-up glow: {} did not load ({}); the Scene View draws mark-ups with the gizmo "
                           "fill. Rebuild the Editor target to stage it.",
                           path, error);
        return false;
    }
    return true;
}

DescriptorSetLayoutDesc GlowCameraLayout()
{
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "SV_MarkupGlow_Set0";
    DescriptorBinding binding{};
    binding.binding = kCameraBinding;
    binding.type = DescriptorType::UniformBuffer;
    binding.count = 1;
    binding.shaderStages = kShaderStageVertex | kShaderStageFragment;
    layout.bindings.push_back(binding);
    DescriptorBinding depth{};
    depth.binding = kSceneDepthBinding;
    depth.type = DescriptorType::CombinedImageSampler;
    depth.count = 1;
    depth.shaderStages = kShaderStageFragment;
    layout.bindings.push_back(depth);
    return layout;
}

// The stride of a region mesh's vertex (MarkupECS::MarkupRegionVertex): xyz and the rim.
constexpr uint32_t kRegionVertexStride = static_cast<uint32_t>(sizeof(MarkupECS::MarkupRegionVertex));

// The body's pipeline: generated vertices (or, for `meshInput`, a region mesh's vertex buffer), no
// culling (the fragment drops the far side), tested against scene depth with `depthCompare` and
// never writing it, premultiplied alpha-over.
PipelineDesc BuildGlowPipelineDesc(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& fs,
                                   const DescriptorSetLayoutDesc& layout, CompareOp depthCompare, const char* name,
                                   bool meshInput)
{
    PipelineDesc desc{};
    desc.type = PipelineType::Graphics;
    desc.vertexShader = vs;
    desc.pixelShader = fs;
    desc.debugName = name;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.AddDynamicState(DynamicState::Viewport);
    desc.AddDynamicState(DynamicState::Scissor);
    desc.colorAttachmentFormats.push_back(0u); // from the pass's format key
    desc.depthAttachmentFormat = 0;
    desc.rasterizationSamples = 0;
    desc.SetCullingMode(CullModeFlagBits::None);
    // Reverse-Z: GreaterOrEqual passes in front of scene geometry, Less behind it.
    desc.EnableDepthTest(true, depthCompare);
    desc.depthStencilState.depthWriteEnable = false;
    desc.EnableBlending(true, BlendFactor::One, BlendFactor::OneMinusSrcAlpha);
    desc.descriptorSetLayouts.push_back(layout);
    if (meshInput)
    {
        desc.vertexBindings.push_back(VertexInputBinding{0u, kRegionVertexStride, 0u});
        desc.vertexAttributes.push_back(VertexInputAttribute{0u, 0u, Format::R32G32B32A32_FLOAT, 0u});
    }
    PipelineDesc::PushConstantRangeDesc range{};
    range.offset = 0;
    range.size = static_cast<uint32_t>(sizeof(GlowPush));
    range.stagesMask = kShaderStageVertex | kShaderStageFragment;
    desc.pushConstantRanges = {range};
    return desc;
}

// A region's draw: its display's mesh in world space, its rim along the top edge.
MarkupRenderFeature::Draw MakeRegionDraw(const MarkupDrawItem& item)
{
    MarkupRenderFeature::Draw draw;
    for (int axis = 0; axis < 4; ++axis)
        draw.Model[axis * 5] = 1.0f;
    const std::array<float32, 3> rgb = item.Hovered ? MarkupHoverRgb(item.Rgb) : item.Rgb;
    draw.Color[0] = rgb[0];
    draw.Color[1] = rgb[1];
    draw.Color[2] = rgb[2];
    draw.Color[3] = item.Hovered ? MarkupRenderFeature::kHoveredBodyAlpha : MarkupRenderFeature::kBodyAlpha;
    draw.Rim[0] = MarkupRenderFeature::kRimPower;
    draw.Rim[1] = item.Selected ? MarkupRenderFeature::kSelectedRimGain : 1.0f;
    draw.Rim[2] = MarkupRenderFeature::kRimBudget;
    draw.Rim[3] = 2.0f;
    draw.VertexCount = static_cast<std::uint32_t>(item.Region->Mesh.Vertices.size());
    draw.Region = item.Region;
    draw.RegionEntity = item.Entity;
    return draw;
}

MarkupRenderFeature::Draw MakeDraw(const MarkupDrawItem& item)
{
    if (item.Region)
        return MakeRegionDraw(item);
    MarkupRenderFeature::Draw draw;
    const MarkupWorldVolume& volume = item.Volume;
    // A mirrored transform (axes with a negative determinant) would wind every triangle
    // back-facing (markup_glow.frag keeps front faces). Both shapes are symmetric: flipping the
    // third axis draws the same volume with a positive determinant.
    const float handedness =
        Mathematics::Vector3::Dot(Mathematics::Vector3::Cross(volume.Axes[0], volume.Axes[1]), volume.Axes[2]) < 0.0f
            ? -1.0f
            : 1.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float sign = axis == 2 ? handedness : 1.0f;
        const Mathematics::Vector3 column =
            volume.Axes[axis] * (volume.HalfExtents[static_cast<std::size_t>(axis)] * sign);
        draw.Model[axis * 4 + 0] = column.x;
        draw.Model[axis * 4 + 1] = column.y;
        draw.Model[axis * 4 + 2] = column.z;
    }
    draw.Model[12] = volume.Center.x;
    draw.Model[13] = volume.Center.y;
    draw.Model[14] = volume.Center.z;
    draw.Model[15] = 1.0f;
    // A hovered mark-up lifts its body, the same on a box and a sphere; a selected one reads by
    // its stronger rim (and the gizmo's doubled outline) at the resting body.
    const std::array<float32, 3> rgb = item.Hovered ? MarkupHoverRgb(item.Rgb) : item.Rgb;
    draw.Color[0] = rgb[0];
    draw.Color[1] = rgb[1];
    draw.Color[2] = rgb[2];
    draw.Color[3] = item.Hovered ? MarkupRenderFeature::kHoveredBodyAlpha : MarkupRenderFeature::kBodyAlpha;
    const bool sphere = volume.Shape == Components::MarkupVolumeShape::Sphere;
    draw.Rim[0] = MarkupRenderFeature::kRimPower;
    draw.Rim[1] = item.Selected ? MarkupRenderFeature::kSelectedRimGain : 1.0f;
    draw.Rim[2] = MarkupRenderFeature::kRimBudget;
    draw.Rim[3] = sphere ? 1.0f : 0.0f;
    draw.VertexCount = sphere ? MarkupRenderFeature::kSphereVertexCount : MarkupRenderFeature::kBoxVertexCount;
    return draw;
}

// The record of the body pass: every draw behind scene geometry dimmed, then every draw in front.
struct GlowPassRecord
{
    GraphicsPipelineId Visible;
    GraphicsPipelineId Occluded;
    GraphicsPipelineId VisibleMesh;
    GraphicsPipelineId OccludedMesh;
    DescriptorSetLayoutDesc Layout;
    RenderGraph::RGTexture SceneDepth;
    SamplerHandle SceneDepthSampler; // texelFetch ignores it; the combined binding needs one
    BufferHandle CameraBuffer;
    uint64_t CameraOffset = 0;
    std::vector<MarkupRenderFeature::Draw> Draws;
    uint32_t Width = 0;
    uint32_t Height = 0;

    void operator()(RenderGraph::RGContext& context) const
    {
        CommandList* commands = context.Cmd;
        IDevice* device = context.GetDevice();
        if (!commands || !device)
            return;
        const PipelineHandle visible = context.GetOrCreatePipelineVariant(Visible);
        const PipelineHandle occluded = context.GetOrCreatePipelineVariant(Occluded);
        const PipelineHandle visibleMesh = context.GetOrCreatePipelineVariant(VisibleMesh);
        const PipelineHandle occludedMesh = context.GetOrCreatePipelineVariant(OccludedMesh);
        if (!visible.IsValid() || !occluded.IsValid() || !visibleMesh.IsValid() || !occludedMesh.IsValid())
            return;
        DescriptorSetDesc setDesc{};
        setDesc.layout = Layout;
        setDesc.transient = true;
        setDesc.debugName = "SV_MarkupGlow_DS";
        const DescriptorSetHandle set = device->CreateDescriptorSet(setDesc);
        if (!set.IsValid())
            return;
        device->UpdateBufferBinding(set, kCameraBinding, CameraBuffer, static_cast<uint32_t>(CameraOffset),
                                    static_cast<uint32_t>(sizeof(CameraData)));
        device->UpdateCombinedImageSamplerBinding(set, kSceneDepthBinding, context.GetTexture(SceneDepth),
                                                  SceneDepthSampler);
        commands->SetViewport(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
        commands->SetScissor(0, 0, static_cast<int>(Width), static_cast<int>(Height));
        DrawAll(*commands, occluded, occludedMesh, set, SceneTools::kGizmoOccludedAlphaScale);
        DrawAll(*commands, visible, visibleMesh, set, 1.0f);
    }

    // Every draw in order, switching between the generated shapes' pipeline and the region
    // meshes' as the draws do.
    void DrawAll(CommandList& commands, PipelineHandle shapes, PipelineHandle meshes, DescriptorSetHandle set,
                 float dim) const
    {
        PipelineHandle bound{};
        for (const MarkupRenderFeature::Draw& draw : Draws)
        {
            const bool mesh = draw.MeshBuffer.IsValid();
            const PipelineHandle pipeline = mesh ? meshes : shapes;
            if (!(pipeline == bound))
            {
                commands.SetPipeline(pipeline);
                commands.BindDescriptorSet(0, set, pipeline);
                bound = pipeline;
            }
            if (mesh)
                commands.SetVertexBuffer(draw.MeshBuffer, 0);
            GlowPush push{};
            std::copy(std::begin(draw.Model), std::end(draw.Model), push.Model);
            std::copy(std::begin(draw.Color), std::end(draw.Color), push.Color);
            std::copy(std::begin(draw.Rim), std::end(draw.Rim), push.Rim);
            push.Color[3] *= dim;
            push.Rim[2] *= dim;
            commands.SetPushConstants(push);
            commands.Draw(draw.VertexCount, 1u, mesh ? draw.FirstVertex : 0u, 0u);
        }
    }
};

} // namespace

MarkupRenderFeature* MarkupRenderFeature::Ensure(Engine::Renderer::RenderServices& services)
{
    if (IsCompatShaderProfile())
        return nullptr;
    return &services.EnsureFeature<MarkupRenderFeature>();
}

void MarkupRenderFeature::BuildDraws(std::span<const MarkupDrawItem> items, std::size_t bodies, std::vector<Draw>& draws)
{
    draws.clear();
    m_Order.clear();
    for (std::size_t index = 0; index < std::min(bodies, items.size()); ++index)
    {
        // A path's line is the gizmo's; it has no body to glow.
        if (items[index].DrawsBody() && !items[index].Path)
            m_Order.emplace_back(items[index].DistanceSq, index);
    }
    // Back to front: overlapping bodies blend in view order.
    std::sort(m_Order.begin(), m_Order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    draws.reserve(m_Order.size());
    for (const auto& [distanceSq, index] : m_Order)
        draws.push_back(MakeDraw(items[index]));
}

bool MarkupRenderFeature::EnsurePipelines(IDevice& device)
{
    if (m_PipelinesTried)
        return m_CompositePipeline.IsValid();
    m_PipelinesTried = true;
    std::vector<uint8_t> vs;
    std::vector<uint8_t> fs;
    std::vector<uint8_t> msVs;
    std::vector<uint8_t> msFs;
    std::vector<uint8_t> meshVs;
    std::vector<uint8_t> meshFs;
    std::vector<uint8_t> meshMsVs;
    std::vector<uint8_t> meshMsFs;
    if (!LoadGlowShaderBytes("Shaders/markup_glow.shaderpkg", device.PreferredShaderSource(), vs, fs) ||
        !LoadGlowShaderBytes("Shaders/markup_glow_ms.shaderpkg", device.PreferredShaderSource(), msVs, msFs) ||
        !LoadGlowShaderBytes("Shaders/markup_glow_mesh.shaderpkg", device.PreferredShaderSource(), meshVs, meshFs) ||
        !LoadGlowShaderBytes("Shaders/markup_glow_mesh_ms.shaderpkg", device.PreferredShaderSource(), meshMsVs,
                             meshMsFs))
        return false;
    PipelineDesc composite{};
    if (!RenderGraph::LoadCopyPipelineDesc(device.PreferredShaderSource(), "SV_MarkupGlow_Composite",
                                           "SV_MarkupGlow_Composite_Set0", m_CompositeLayout, composite))
    {
        Logger::Log::Error("Mark-up glow: Shaders/copy.shaderpkg did not load; the Scene View draws mark-ups "
                           "with the gizmo fill.");
        return false;
    }
    composite.EnableBlending(true, BlendFactor::One, BlendFactor::OneMinusSrcAlpha);

    m_GlowLayout = GlowCameraLayout();
    const auto intern = [&device, this](const std::vector<uint8_t>& stageVs, const std::vector<uint8_t>& stageFs,
                                        const char* visibleName, const char* occludedName, bool meshInput) {
        return GlowPipelines{
            PipelineDescTranslator::InternGraphics(device, BuildGlowPipelineDesc(stageVs, stageFs, m_GlowLayout,
                                                                                 CompareOp::GreaterOrEqual, visibleName,
                                                                                 meshInput)),
            PipelineDescTranslator::InternGraphics(device, BuildGlowPipelineDesc(stageVs, stageFs, m_GlowLayout,
                                                                                 CompareOp::Less, occludedName,
                                                                                 meshInput))};
    };
    const GlowPipelines single = intern(vs, fs, "SV_MarkupGlow", "SV_MarkupGlow_Occluded", false);
    const GlowPipelines multisampled = intern(msVs, msFs, "SV_MarkupGlow_MS", "SV_MarkupGlow_MS_Occluded", false);
    const GlowPipelines singleMesh =
        intern(meshVs, meshFs, "SV_MarkupGlow_Mesh", "SV_MarkupGlow_Mesh_Occluded", true);
    const GlowPipelines multisampledMesh =
        intern(meshMsVs, meshMsFs, "SV_MarkupGlow_Mesh_MS", "SV_MarkupGlow_Mesh_MS_Occluded", true);
    const GraphicsPipelineId compositeId = PipelineDescTranslator::InternGraphics(device, composite);
    for (const GlowPipelines& pipelines : {single, multisampled, singleMesh, multisampledMesh})
    {
        if (!pipelines.Visible.IsValid() || !pipelines.Occluded.IsValid())
            return false;
    }
    if (!compositeId.IsValid())
        return false;
    m_SingleSamplePipelines = single;
    m_MultisamplePipelines = multisampled;
    m_SingleSampleMeshPipelines = singleMesh;
    m_MultisampleMeshPipelines = multisampledMesh;
    m_CompositePipeline = compositeId;
    return true;
}

bool MarkupRenderFeature::DeclareView(RenderGraph::RGFrame& frame, RenderGraph::RGTexture output,
                                      RenderGraph::RGTexture depth, SamplerHandle compositeSampler,
                                      const CameraData& camera, ECS::World& world, const MarkupEditorBridge& bridge,
                                      std::uint64_t frameNumber)
{
    IDevice* device = frame.Device();
    if (device)
        m_RegionBuffers.Collect(*device, frameNumber,
                                (world.GetWorldId() << 32) ^ world.GetLifecycleResetGeneration());
    const Mathematics::Vector3 cameraPos(camera.cameraPos[0], camera.cameraPos[1], camera.cameraPos[2]);
    Mathematics::Vector4 frustumPlanes[6];
    ExtractFrustumPlanes(Mathematics::Matrix4x4::FromColumnMajor(camera.viewProj), frustumPlanes);
    m_Bodies = CollectMarkupDrawItems(world, bridge, bridge.GetHighlight(), cameraPos, frustumPlanes, frameNumber,
                                      m_Items, m_ExcludedScratch);
    BuildDraws(m_Items, m_Bodies, m_Draws);
    // The hand gate: no body in view, no pass.
    if (m_Draws.empty())
        return false;

    if (!device || !EnsurePipelines(*device))
        return false;
    // A region whose mesh found no buffer this frame draws its gizmo outline only.
    std::erase_if(m_Draws, [&](Draw& draw) {
        return draw.Region && !m_RegionBuffers.Place(*device, frame, frameNumber, draw);
    });
    if (m_Draws.empty())
        return false;
    auto cameraAlloc = frame.AllocUpload<CameraData>();
    if (!cameraAlloc.Valid() || !compositeSampler.IsValid())
        return false;
    *cameraAlloc.Ptr = camera;

    const RenderGraph::RGResourceDesc outputDesc = frame.Graph().ResourceDesc(output.Id);
    const RenderGraph::RGResourceDesc depthDesc = frame.Graph().ResourceDesc(depth.Id);
    const uint32_t samples = std::max(depthDesc.SampleCount, 1u);
    const GlowPipelines& pipelines = samples > 1 ? m_MultisamplePipelines : m_SingleSamplePipelines;
    const GlowPipelines& meshPipelines = samples > 1 ? m_MultisampleMeshPipelines : m_SingleSampleMeshPipelines;
    GlowPassRecord record;
    record.Visible = pipelines.Visible;
    record.Occluded = pipelines.Occluded;
    record.VisibleMesh = meshPipelines.Visible;
    record.OccludedMesh = meshPipelines.Occluded;
    record.SceneDepth = depth;
    record.SceneDepthSampler = compositeSampler;
    record.Width = std::max(outputDesc.Width, 1u);
    record.Height = std::max(outputDesc.Height, 1u);
    record.Layout = m_GlowLayout;
    record.CameraBuffer = cameraAlloc.Buffer;
    record.CameraOffset = cameraAlloc.Offset;
    record.Draws = m_Draws;

    TextureDesc target{};
    target.width = record.Width;
    target.height = record.Height;
    target.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    target.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    target.sampleCount = 1;
    target.debugName = "SV.MarkupGlow";
    const RenderGraph::RGTexture glow = frame.CreateTexture("SV.MarkupGlow", target);
    RenderGraph::RGTexture glowMsaa{};
    if (samples > 1)
    {
        TextureDesc multisampled = target;
        multisampled.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
        multisampled.sampleCount = samples;
        multisampled.debugName = "SV.MarkupGlowMSAA";
        glowMsaa = frame.CreateTexture("SV.MarkupGlowMSAA", multisampled);
    }

    frame.AddPass(
        "SV.MarkupGlow", kOverlayPhase,
        [&](RenderGraph::RGPassBuilder& pass)
        {
            RenderGraph::RGAttachmentOps colorOps{};
            colorOps.Load = RenderGraph::RGLoadOp::Clear;
            colorOps.Store = RenderGraph::RGStoreOp::Store;
            if (samples > 1)
                pass.AttachColorResolve(0, glowMsaa, glow, colorOps);
            else
                pass.AttachColor(0, glow, colorOps);
            RenderGraph::RGAttachmentOps depthOps{};
            depthOps.Load = RenderGraph::RGLoadOp::Load;
            pass.AttachDepth(depth, depthOps, RenderGraph::RGDepthAccess::ReadOnly);
            pass.Read(depth, RenderGraph::RGTextureRead::Sampled); // the contact line
        },
        std::move(record));

    const RenderGraph::RGFullscreenTextureInput glowInput{0, glow, {}, {}, compositeSampler, {}, true};
    RenderGraph::RGFullscreenDesc compositePass{};
    compositePass.Name = "SV.MarkupGlow_Composite";
    compositePass.Phase = kOverlayPhase;
    compositePass.Pipeline = m_CompositePipeline;
    compositePass.Layout = m_CompositeLayout;
    compositePass.Textures = std::span(&glowInput, 1);
    compositePass.Target = output;
    compositePass.Ops.Load = RenderGraph::RGLoadOp::Load; // over the scene
    compositePass.Ops.Store = RenderGraph::RGStoreOp::Store;
    RenderGraph::AddFullscreenPass(frame, compositePass);
    return true;
}

void MarkupRenderFeature::OnDeviceRebuilt(IDevice*)
{
    m_RegionBuffers.Drop();
}

bool MarkupRenderFeature::RegionBuffers::Place(IDevice& device, RenderGraph::RGFrame& frame,
                                               std::uint64_t frameNumber, Draw& draw)
{
    const MarkupECS::MarkupRegionDisplayCache::Entry& display = *draw.Region;
    const std::vector<MarkupECS::MarkupRegionVertex>& vertices = display.Mesh.Vertices;
    const std::size_t bytes = vertices.size() * sizeof(MarkupECS::MarkupRegionVertex);
    if (bytes == 0)
        return false;
    // Changed this frame or the last: the upload ring. 256-aligned offsets are stride-divisible,
    // so the offset folds into the first vertex (SetVertexBuffer takes no offset).
    if (frameNumber < display.ChangedFrame + 2)
    {
        const RenderGraph::RGUploadRing::Alloc alloc = frame.AllocUpload(bytes, 256);
        if (!alloc.Valid())
            return false;
        std::memcpy(alloc.Ptr, vertices.data(), bytes);
        draw.MeshBuffer = alloc.Buffer;
        draw.FirstVertex = static_cast<std::uint32_t>(alloc.Offset / kRegionVertexStride);
        return true;
    }
    Owned& owned = m_Owned.GetOrInsert(draw.RegionEntity.id);
    if (!owned.Buffer.IsValid() || owned.MeshRevision != display.MeshRevision)
    {
        if (owned.Buffer.IsValid())
            m_Retired.push_back({owned.Buffer, frameNumber});
        BufferDesc desc{};
        desc.size = bytes;
        desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
        desc.memoryUsage = BufferMemoryUsage::UploadDeviceLocalPreferred;
        desc.debugName = "SV.MarkupRegionMesh";
        owned.Buffer = device.CreateBuffer(desc);
        owned.MeshRevision = display.MeshRevision;
        if (!owned.Buffer.IsValid())
            return false;
        device.UpdateBuffer(owned.Buffer, 0, bytes, vertices.data());
    }
    owned.LastUsedFrame = frameNumber;
    draw.MeshBuffer = owned.Buffer;
    draw.FirstVertex = 0;
    return true;
}

void MarkupRenderFeature::RegionBuffers::Collect(IDevice& device, std::uint64_t frameNumber, std::uint64_t worldKey)
{
    const bool worldChanged = worldKey != m_WorldKey;
    m_WorldKey = worldKey;
    m_Owned.EraseIf([&](std::uint32_t, const Owned& owned) {
        if (!worldChanged &&
            frameNumber <= owned.LastUsedFrame + MarkupECS::MarkupRegionDisplayCache::kEvictAfterFrames)
            return false;
        if (owned.Buffer.IsValid())
            m_Retired.push_back({owned.Buffer, frameNumber});
        return true;
    });
    // A buffer retired on frame N may be read by the frames in flight submitted up to N.
    const std::uint64_t framesInFlight = std::max<std::uint64_t>(device.GetFramesInFlight(), 1u);
    std::erase_if(m_Retired, [&](const Retired& retired) {
        if (frameNumber <= retired.Frame + framesInFlight)
            return false;
        device.DestroyBuffer(retired.Buffer);
        return true;
    });
}

void MarkupRenderFeature::RegionBuffers::Drop()
{
    m_Owned.Clear();
    m_Retired.clear();
}

} // namespace GameEngine::Editor
