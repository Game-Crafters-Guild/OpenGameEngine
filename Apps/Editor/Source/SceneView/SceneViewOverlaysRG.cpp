// Slice 7e — the Scene View editor overlays as fresh per-frame RenderGraph
// declarations: gizmos (grid/tools/icons) into a transient GizmoColor,
// selection mask + Sobel outline, and the gizmo composite, all attaching the
// pipeline's ACTUAL FinalColor (GetPipelineOutputRG's out.Out — never the
// caller-imported Color/Resolve, which under FinalCopy elision is a texture
// the UI never samples) with Load. Declaration order is execution order:
// overlay → mask → outline → composite, so the transform gizmo draws on top
// of outlined selections.
//
// Everything the old retained passes did at EXECUTE time against live members
// (gizmo vertex generation, the per-entity selection walk, settings reads)
// happens here at DECLARATION time and is snapshotted: RenderGraph exec lambdas are
// placement-new'd into the frame arena and run after this function returns —
// they capture by value only (the sole pointer capture, &m_GridRenderer, is
// safe because the controller strictly outlives this frame's Execute()).

#include "SceneViewController.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "Engine/GameUI/GameUIHost.h"
#include "UI/UITextureSpace.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "SceneView/SelectionOutlineContributors.h"
#include "Editor/EditorPaths.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupRenderFeature.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Common/Frustum.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewOverlayShared.h"
#include "SceneView/SceneViewTools.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace GameEngine
{

using namespace Rendering;

namespace
{

constexpr int32_t kOverlayPhase = static_cast<int32_t>(PassPhase::kOverlay);

// Gizmo vertices ride the upload ring as 16-byte (xyz + pad) entries: the
// ring's 256-byte alloc alignment is stride-divisible, so the alloc offset
// folds into firstVertex (CommandList::SetVertexBuffer has no offset param).
constexpr uint32_t kGizmoVertexStride = 16;

// Push constants — layouts shared with the old arm's pipelines (same shaders).
struct GizmoPC
{
    float M[16];
    float color[4];
};
static_assert(sizeof(GizmoPC) == 80, "GizmoPC layout must match editor_lines shaders");

struct MaskPC
{
    float uVPM[16];
    float uMaskParams[4];
    float uMaskExtra[4];
    float uWindStrength[4];
    float uWindParams[4];
};
static_assert(sizeof(MaskPC) == 128, "MaskPC layout must match selection_mask shaders");

struct OutlinePush
{
    float outlineColor[4];
    float texelSize[2];
    float edgeThreshold;
    float smokeTint;
    float outlineRadius;
    float pad[3];
};
static_assert(sizeof(OutlinePush) == 48, "OutlinePush layout must match selection_outline.frag");

void MakeIdentity4(float m[16])
{
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// One packed gizmo group: an absolute firstVertex into the ring buffer plus
// the per-group color. Only triangles carry layer/sort keys; alwaysOnTop
// applies to both and selects which depth pass owns the draw.
struct GizmoGroupDraw
{
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    Color color{0.0f, 0.0f, 0.0f, 0.0f};
    int32_t layer = 0;
    float depthKey = 0.0f;
    bool isTransparent = false;
    bool alwaysOnTop = false;
};

// Pack xyz-triplet group vertices into a 16-byte-stride ring alloc. Returns
// false when there is nothing to pack (outDraws empty / no alloc made).
template <class GroupVec>
bool PackGizmoGroups(RenderGraph::RGFrame& frame, const GroupVec& groups, uint32_t minVerts,
                     BufferHandle& outBuf, std::vector<GizmoGroupDraw>& outDraws)
{
    uint64_t totalVerts = 0;
    for (const auto& group : groups)
    {
        const uint32_t vc = static_cast<uint32_t>(group.vertices.size() / 3);
        if (vc >= minVerts)
            totalVerts += vc;
    }
    if (totalVerts == 0)
        return false;

    const RenderGraph::RGUploadRing::Alloc alloc =
        frame.AllocUpload(totalVerts * kGizmoVertexStride, 256);
    if (!alloc.Valid())
        return false;
    // 256-aligned offsets are 16-divisible by construction; the fold below
    // depends on it.
    assert(alloc.Offset % kGizmoVertexStride == 0);
    const uint32_t baseVertex = static_cast<uint32_t>(alloc.Offset / kGizmoVertexStride);

    float* dst = static_cast<float*>(alloc.Ptr);
    uint32_t cursor = 0;
    outDraws.reserve(groups.size());
    for (const auto& group : groups)
    {
        const uint32_t vc = static_cast<uint32_t>(group.vertices.size() / 3);
        if (vc < minVerts)
            continue;

        GizmoGroupDraw draw{};
        draw.firstVertex = baseVertex + cursor;
        draw.vertexCount = vc;
        draw.color = group.color;
        draw.alwaysOnTop =
            (group.depthMode == Editor::SceneTools::GizmoDepthMode::AlwaysOnTop);
        outDraws.push_back(draw);

        const float* src = group.vertices.data();
        float* row = dst + static_cast<size_t>(cursor) * 4u;
        for (uint32_t v = 0; v < vc; ++v)
        {
            row[0] = src[0];
            row[1] = src[1];
            row[2] = src[2];
            row[3] = 0.0f;
            row += 4;
            src += 3;
        }
        cursor += vc;
    }

    outBuf = alloc.Buffer;
    return !outDraws.empty();
}

DescriptorSetLayoutDesc GizmoCameraSet0Layout()
{
    DescriptorSetLayoutDesc set0{};
    set0.debugName = "SV_Gizmos_Set0";
    DescriptorBinding b{};
    b.binding = 5;
    b.type = DescriptorType::UniformBuffer;
    b.count = 1;
    b.shaderStages = kShaderStageVertex;
    set0.bindings.push_back(b);
    return set0;
}

DescriptorSetLayoutDesc FullscreenSamplerSet0Layout(const char* debugName)
{
    DescriptorSetLayoutDesc set0{};
    set0.debugName = debugName;
    DescriptorBinding db{};
    db.binding = 0;
    db.type = DescriptorType::CombinedImageSampler;
    db.count = 1;
    db.shaderStages = kShaderStageFragment;
    set0.bindings.push_back(db);
    return set0;
}

constexpr uint32_t kBonePaletteAtlasBinding = 12;

DescriptorSetLayoutDesc SkinnedMaskSet0Layout()
{
    DescriptorSetLayoutDesc l{};
    l.debugName = "SV_Selection_Mask_Skinned_Set0";
    DescriptorBinding b{};
    b.binding = kBonePaletteAtlasBinding;
    b.type = DescriptorType::StorageBuffer;
    b.count = 1;
    b.shaderStages = kShaderStageVertex;
    l.bindings.push_back(b);
    return l;
}

DescriptorSetLayoutDesc AlphaMaskSet0Layout()
{
    DescriptorSetLayoutDesc l{};
    l.debugName = "SV_Selection_Mask_Alpha_Set0";
    DescriptorBinding b{};
    b.binding = 0;
    b.type = DescriptorType::CombinedImageSampler;
    b.count = 1;
    b.shaderStages = kShaderStageFragment;
    l.bindings.push_back(b);
    return l;
}

struct MaskDraw
{
    float vpm[16];
    uint32_t paletteOffset = 0;
    float alphaCutoff = 0.5f;
    float uvScale[2] = {1.0f, 1.0f};
    float uvOffset[2] = {0.0f, 0.0f};
    float windStrength[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float windParams[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    float windTime = 0.0f;
    float windSeed = 0.0f;
    bool skinned = false;
    bool alphaTest = false;
    BufferHandle coreVB{};
    BufferHandle jointsVB{};
    BufferHandle weightsVB{};
    BufferHandle indexBuffer{};
    TextureHandle alphaTexture{};
    uint32_t indexType = 0;
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
};

PipelineDesc BuildGizmoLinesPipelineDesc(const std::vector<uint8_t>& vs,
                                         const std::vector<uint8_t>& fs,
                                         const DescriptorSetLayoutDesc& set0,
                                         CompareOp depthCompare,
                                         const char* debugName)
{
    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.debugName = debugName;
    pd.topology = PrimitiveTopology::LineList;
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back(0u); // auto from the pass's format key
    pd.depthAttachmentFormat = 0;
    pd.rasterizationSamples = 0;
    // Two-pass depth split against scene depth for depth perception (reverse-Z:
    // near→1.0, so GreaterOrEqual passes where the gizmo is in FRONT of scene
    // geometry, Less where it is behind — same polarity as the grid pipeline).
    // The occluded pass redraws at reduced alpha; neither pass writes depth
    // (the overlay pass binds world depth read-only).
    // Triangle gizmos include camera-facing quads with runtime-generated
    // winding, so both the line and triangle variants must disable culling.
    pd.SetCullingMode(CullModeFlagBits::None);
    pd.EnableDepthTest(true, depthCompare);
    // EnableDepthTest also turns depth WRITES on; the overlay must not write.
    pd.depthStencilState.depthWriteEnable = false;
    // Alpha-over into the transparent gizmo target, kept premultiplied for the composite
    // (One, OneMinusSrcAlpha): color by SrcAlpha, coverage by One. EnableBlending copies the
    // color factors onto alpha; SrcAlpha on coverage would store a² and the composite would read
    // a translucent gizmo as added light.
    pd.EnableBlending(true);
    pd.colorBlendState.attachments[0].srcAlphaBlendFactor = BlendFactor::One;
    VertexInputBinding vb0{};
    vb0.binding = 0;
    vb0.stride = kGizmoVertexStride; // xyz + pad (ring-offset fold, see above)
    vb0.inputRate = 0;
    pd.vertexBindings.push_back(vb0);
    VertexInputAttribute a0{};
    a0.location = 0;
    a0.binding = 0;
    a0.format = Format::R32G32B32_FLOAT;
    a0.offset = 0;
    pd.vertexAttributes.push_back(a0);
    pd.descriptorSetLayouts.push_back(set0);
    PipelineDesc::PushConstantRangeDesc pr{};
    pr.offset = 0;
    pr.size = static_cast<uint32_t>(sizeof(GizmoPC));
    pr.stagesMask = kShaderStageVertex;
    pd.pushConstantRanges = {pr};
    return pd;
}

PipelineDesc BuildFullscreenBlendPipelineDesc(const std::vector<uint8_t>& vs,
                                              const std::vector<uint8_t>& fs,
                                              const DescriptorSetLayoutDesc& set0,
                                              const char* debugName,
                                              BlendFactor srcBlend)
{
    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.debugName = debugName;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back(0u);
    pd.depthAttachmentFormat = 0;
    pd.rasterizationSamples = 0;
    pd.SetCullingMode(CullModeFlagBits::None);
    pd.EnableDepthTest(false, CompareOp::Always);
    pd.EnableBlending(true, srcBlend, BlendFactor::OneMinusSrcAlpha);
    pd.descriptorSetLayouts.push_back(set0);
    return pd;
}

PipelineDesc BuildSelectionMaskPipelineDesc(const std::vector<uint8_t>& vs,
                                            const std::vector<uint8_t>& fs,
                                            bool skinned,
                                            bool alphaTest = false)
{
    PipelineDesc pd{};
    pd.type = PipelineType::Graphics;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.debugName = alphaTest ? "SV_Selection_Mask_Alpha" : (skinned ? "SV_Selection_Mask_Skinned" : "SV_Selection_Mask");
    pd.topology = PrimitiveTopology::TriangleList;
    pd.AddDynamicState(DynamicState::Viewport);
    pd.AddDynamicState(DynamicState::Scissor);
    pd.colorAttachmentFormats.push_back(0u);
    pd.depthAttachmentFormat = 0;
    pd.rasterizationSamples = 0;
    // On-top silhouette: no depth test and culling off, so the outline reads the
    // same as the pre-cache path (x-ray, like Unity/Unreal selection outlines)
    // and honours material double-sidedness (no back-face culling to drop
    // one-sided or double-sided-material faces). Triangle winding is irrelevant
    // with culling off.
    pd.SetCullingMode(CullModeFlagBits::None);
    pd.EnableDepthTest(false, CompareOp::Always);
    pd.EnableBlending(false);

    // Only position from the standard interleaved core VB (stride 32:
    // pos + normal + uv0); joints/weights plug in for the skinned variant —
    // bindings/locations match VertexLayoutBuilder.
    {
        VertexInputBinding vb0{};
        vb0.binding = VertexBinding::Core;
        vb0.stride = 32u;
        vb0.inputRate = 0;
        pd.vertexBindings.push_back(vb0);
        VertexInputAttribute a0{};
        a0.location = VertexLocation::Position;
        a0.binding = VertexBinding::Core;
        a0.format = Format::R32G32B32_FLOAT;
        a0.offset = 0;
        pd.vertexAttributes.push_back(a0);
    }
    if (alphaTest)
    {
        VertexInputAttribute aUv{};
        aUv.location = VertexLocation::UV0;
        aUv.binding = VertexBinding::Core;
        aUv.format = Format::R32G32_FLOAT;
        aUv.offset = 24u;
        pd.vertexAttributes.push_back(aUv);
        pd.descriptorSetLayouts.push_back(AlphaMaskSet0Layout());
    }
    if (skinned)
    {
        {
            VertexInputBinding vbJ{};
            vbJ.binding = VertexBinding::Joints;
            vbJ.stride = 4u * sizeof(uint16_t);
            vbJ.inputRate = 0;
            pd.vertexBindings.push_back(vbJ);
            VertexInputAttribute aJ{};
            aJ.location = VertexLocation::Joints;
            aJ.binding = VertexBinding::Joints;
            aJ.format = Format::R16G16B16A16_UINT;
            aJ.offset = 0;
            pd.vertexAttributes.push_back(aJ);
        }
        {
            VertexInputBinding vbW{};
            vbW.binding = VertexBinding::Weights;
            vbW.stride = 4u * sizeof(float);
            vbW.inputRate = 0;
            pd.vertexBindings.push_back(vbW);
            VertexInputAttribute aW{};
            aW.location = VertexLocation::Weights;
            aW.binding = VertexBinding::Weights;
            aW.format = Format::R32G32B32A32_FLOAT;
            aW.offset = 0;
            pd.vertexAttributes.push_back(aW);
        }
        pd.descriptorSetLayouts.push_back(SkinnedMaskSet0Layout());
    }

    PipelineDesc::PushConstantRangeDesc pr{};
    pr.offset = 0;
    pr.size = static_cast<uint32_t>(sizeof(MaskPC));
    pr.stagesMask = kShaderStageVertex | kShaderStageFragment;
    pd.pushConstantRanges = {pr};
    return pd;
}

} // namespace

void SceneViewController::DeclareOverlaysRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture finalColor,
                                             RenderGraph::RGTexture depth,
                                             Engine::Renderer::RenderServices* rs)
{
    if (!rs || !finalColor.IsValid() || !depth.IsValid() || !m_HasFrameCameraData)
        return;
    IDevice* device = frame.Device();
    if (!device)
        return;

    const RenderGraph::RGResourceDesc outDesc = frame.Graph().ResourceDesc(finalColor.Id);
    const RenderGraph::RGResourceDesc depthDesc = frame.Graph().ResourceDesc(depth.Id);
    const uint32_t w = outDesc.Width > 0 ? outDesc.Width : 1u;
    const uint32_t h = outDesc.Height > 0 ? outDesc.Height : 1u;
    const uint32_t samples = depthDesc.SampleCount > 0 ? depthDesc.SampleCount : 1u;
    if (depthDesc.Width != outDesc.Width || depthDesc.Height != outDesc.Height)
    {
        static bool sWarnedDims = false;
        if (!sWarnedDims)
        {
            sWarnedDims = true;
            Logger::Log::Warning(
                "SceneView overlays (RenderGraph): depth {}x{} != output {}x{} — overlays skipped",
                depthDesc.Width, depthDesc.Height, outDesc.Width, outDesc.Height);
        }
        return;
    }

    const CameraData cam = m_FrameCameraData; // by-value snapshot for exec

    // The mark-up glow draws the volumes' bodies (desktop); where it declares nothing, or does not
    // exist (the compat profile), the mark-up gizmo's fill stands in. Its composite precedes the
    // selection outline and the gizmo composite, so outlines and tools draw over it.
    bool markupGlowDrawn = false;
    const Editor::MarkupRenderFeature* markupCollected = nullptr;
    if (m_ShowGizmos && m_ShowMarkupGizmos)
    {
        Editor::MarkupRenderFeature* markupGlow = Editor::MarkupRenderFeature::Ensure(*rs);
        const Editor::MarkupEditorBridge* markupBridge = Editor::MarkupEditorBridge::TryGet();
        ECS::World* markupWorld = &GetWorld();
        const Application* application = Application::Get();
        if (markupGlow && markupBridge)
        {
            markupGlowDrawn = markupGlow->DeclareView(frame, finalColor, depth,
                                                      rs->Textures().GetSampler(SamplerPreset::LinearClamp), cam,
                                                      *markupWorld, *markupBridge,
                                                      application ? application->GetFrameCount() : 0);
            markupCollected = markupGlow;
        }
    }
    m_MarkupGizmo.SetDrawsFills(!markupGlowDrawn);
    // The gizmo outlines the mark-ups the glow collected for this pane, not a second collection.
    m_MarkupGizmo.UseCollectedItems(markupCollected ? &markupCollected->CollectedItems() : nullptr,
                                    markupCollected ? markupCollected->CollectedBodies() : 0);
    // The mark-up gizmo builds a region's display only while it is in this view.
    Mathematics::Vector4 markupFrustum[6];
    ExtractFrustumPlanes(Mathematics::Matrix4x4::FromColumnMajor(cam.viewProj), markupFrustum);
    m_MarkupGizmo.SetViewFrustum(markupFrustum);

    const auto& svSettings = Editor::SceneViewSettings::Get();
    const Editor::SelectionHighlightStyle highlightStyle = svSettings.GetSelectionHighlightStyle();
    const bool hoverUsesWireBox =
        m_HoveredEntity.IsValid() && highlightStyle != Editor::SelectionHighlightStyle::Outline;

    // ── Gizmo vertex gathering (moved verbatim from the old exec — runs at
    // declaration now; the thread_local group store is produced and consumed
    // on this same thread within this call). ──
    m_ToolContext.SetCamera(m_CameraId);
    m_ToolContext.SetView(m_ViewId);
    Editor::SceneTools::ResetGizmoLineGroups(m_ViewId);
    Editor::SceneTools::ResetGizmoTriangleGroups(m_ViewId);

    Editor::SceneTools::GizmoCollector collector;
    collector.AddGizmo(&m_MeasureSceneGizmo);
    if (m_ShowGizmos)
    {
        collector.AddGizmo(&m_HoverGizmo);
        collector.AddGizmo(&m_SelectionGizmo);
        collector.AddGizmo(&m_NavDebugGizmo);
        collector.AddGizmo(&m_ComponentGizmos);
        collector.AddGizmo(&m_ReflectionProbeGizmo);
        collector.AddGizmo(&m_DDGIVolumeGizmo);
        collector.AddGizmo(&m_TerrainModifierGizmo);
        if (m_ShowLightGizmos)
            collector.AddGizmo(&m_LightGizmo);
        if (m_ShowMarkupGizmos)
            collector.AddGizmo(&m_MarkupGizmo);
        if (m_ShowCameraFrameGuide)
        {
            bool hasActiveCamera = false;
            Engine::Renderer::Camera activeCamera{};
            {
                ECS::World* frameWorld = &GetWorld();
                if (auto found = Engine::Renderer::FindActiveCamera(*frameWorld))
                {
                    activeCamera = *found;
                    hasActiveCamera = true;
                }
            }
            m_CameraFrameGizmo.SetCamera(hasActiveCamera, activeCamera.params,
                                         activeCamera.worldTransform.Data());
            collector.AddGizmo(&m_CameraFrameGizmo);
        }
        collector.AddGizmo(&m_SplineSceneGizmo);
        if (m_ShowTransformGizmos)
        {
            SyncMeasureEndpointTransformTarget();
            m_ToolContext.GatherGizmos(collector);
        }
    }
    else if (hoverUsesWireBox)
    {
        collector.AddGizmo(&m_HoverGizmo);
    }

    {
        const float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
        const float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;
        using Mathematics::Vector3;
        const Vector3 forward =
            Vector3(std::cos(pitR) * std::cos(yawR), std::sin(pitR), std::cos(pitR) * std::sin(yawR)).NormalizeOrZero();
        Vector3 right = Vector3::Cross(Vector3(0.0f, 1.0f, 0.0f), forward);
        if (right.LengthSquared() <= 1.0e-6f)
            right = Vector3(1.0f, 0.0f, 0.0f);
        right = right.NormalizeOrZero();
        const Vector3 cameraUp = Vector3::Cross(forward, right).NormalizeOrZero();

        const Vector3 camPosF(static_cast<float>(m_CamPos[0]),
                              static_cast<float>(m_CamPos[1]),
                              static_cast<float>(m_CamPos[2]));
        Editor::SceneTools::GizmoRenderContext gizmoCtx(m_ViewId, m_CameraId, &camPosF, &cameraUp, &GetWorld());
        gizmoCtx.SetEditor2DMode(m_Is2DMode);
        if (m_Is2DMode || m_Orthographic)
        {
            gizmoCtx.SetOrthoHeight(m_CamDistance);
            if (m_TransformTool)
                m_TransformTool->SetOrthoHeight(m_CamDistance);
        }
        else if (m_TransformTool)
        {
            m_TransformTool->SetOrthoHeight(0.0f);
        }
        for (auto* gizmo : collector.GetGizmos())
        {
            if (gizmo)
                gizmo->Render(gizmoCtx);
        }
    }

    // ── Pack lines/triangles into ring allocs (exact size — the old arm's
    // capacity/grow/clamp machinery has no reason to exist here). ──
    BufferHandle lineBuf{};
    std::vector<GizmoGroupDraw> lineDraws;
    if (const auto* groups = Editor::SceneTools::GetGizmoLineGroups(m_ViewId))
        PackGizmoGroups(frame, *groups, 2u, lineBuf, lineDraws);

    BufferHandle triBuf{};
    std::vector<GizmoGroupDraw> triDraws;
    if (const auto* triGroups = Editor::SceneTools::GetGizmoTriangleGroups(m_ViewId))
    {
        if (PackGizmoGroups(frame, *triGroups, 3u, triBuf, triDraws))
        {
            // Per-group layer/transparency + view-space depth for the
            // transparent back-to-front sort (same metric as the old arm:
            // average view-space Z, larger = further in LH view space).
            size_t di = 0;
            for (const auto& group : *triGroups)
            {
                const uint32_t vc = static_cast<uint32_t>(group.vertices.size() / 3);
                if (vc < 3u)
                    continue;
                GizmoGroupDraw& draw = triDraws[di++];
                draw.layer = group.layer;
                draw.isTransparent = (draw.color.a < 0.999f);
                if (draw.isTransparent)
                {
                    const float* viewM = cam.view;
                    double accumZ = 0.0;
                    for (uint32_t v = 0; v < vc; ++v)
                    {
                        const float x = group.vertices[static_cast<size_t>(v) * 3u + 0u];
                        const float y = group.vertices[static_cast<size_t>(v) * 3u + 1u];
                        const float z = group.vertices[static_cast<size_t>(v) * 3u + 2u];
                        accumZ += static_cast<double>(viewM[2] * x + viewM[6] * y +
                                                      viewM[10] * z + viewM[14]);
                    }
                    draw.depthKey = static_cast<float>(accumZ / static_cast<double>(vc));
                }
            }
            std::sort(triDraws.begin(), triDraws.end(),
                      [](const GizmoGroupDraw& a, const GizmoGroupDraw& b)
                      {
                          if (a.layer != b.layer)
                              return a.layer < b.layer;
                          if (a.isTransparent != b.isTransparent)
                              return !a.isTransparent && b.isTransparent;
                          if (!a.isTransparent && !b.isTransparent)
                              return false;
                          return a.depthKey > b.depthKey;
                      });
        }
    }

    // ── Pass 1: gizmo overlay into the transient GizmoColor. Skipped
    // entirely when there is nothing to draw (no grid, no groups) — dead
    // work simply isn't declared. ──
    const bool anyGizmoContent = m_ShowGrid || !lineDraws.empty() || !triDraws.empty();
    RenderGraph::RGTexture gizmoColor{};
    if (anyGizmoContent)
    {
        std::vector<uint8_t> vs;
        std::vector<uint8_t> fs;
        GraphicsPipelineId idLinesVisible{};
        GraphicsPipelineId idLinesOccluded{};
        GraphicsPipelineId idTrisVisible{};
        GraphicsPipelineId idTrisOccluded{};
        GraphicsPipelineId idLinesAlways{};
        GraphicsPipelineId idTrisAlways{};
        if (LoadEditorLinesShaderBytes(device->PreferredShaderSource(), vs, fs))
        {
            const DescriptorSetLayoutDesc set0 = GizmoCameraSet0Layout();
            const auto internPair = [&](CompareOp depthCompare, const char* lineName,
                                        const char* triName, GraphicsPipelineId& outLines,
                                        GraphicsPipelineId& outTris)
            {
                PipelineDesc pdLines =
                    BuildGizmoLinesPipelineDesc(vs, fs, set0, depthCompare, lineName);
                outLines = PipelineDescTranslator::InternGraphics(*device, pdLines);
                PipelineDesc pdTris = pdLines;
                pdTris.debugName = triName;
                pdTris.topology = PrimitiveTopology::TriangleList;
                outTris = PipelineDescTranslator::InternGraphics(*device, pdTris);
            };
            internPair(CompareOp::GreaterOrEqual, "SV_Gizmos_Lines", "SV_Gizmos_Triangles",
                       idLinesVisible, idTrisVisible);
            internPair(CompareOp::Less, "SV_Gizmos_Lines_Occluded", "SV_Gizmos_Triangles_Occluded",
                       idLinesOccluded, idTrisOccluded);
            // Opt-in third pair for GizmoDepthMode::AlwaysOnTop: no depth test,
            // so the group draws once at full strength wherever it lands.
            internPair(CompareOp::Always, "SV_Gizmos_Lines_Always",
                       "SV_Gizmos_Triangles_Always", idLinesAlways, idTrisAlways);
        }

        const bool gizmoPipelinesValid = idLinesVisible.IsValid() && idLinesOccluded.IsValid() &&
                                         idTrisVisible.IsValid() && idTrisOccluded.IsValid() &&
                                         idLinesAlways.IsValid() && idTrisAlways.IsValid();
        auto camAlloc = frame.AllocUpload<CameraData>();
        if ((gizmoPipelinesValid && camAlloc.Valid()) || m_ShowGrid)
        {
            if (camAlloc.Valid())
                *camAlloc.Ptr = cam;

            TextureDesc td{};
            td.width = w;
            td.height = h;
            td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
            td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
            td.sampleCount = 1;
            td.debugName = "SV.GizmoColor";
            gizmoColor = frame.CreateTexture("SV.GizmoColor", td);

            RenderGraph::RGTexture gizmoColorMsaa{};
            if (samples > 1)
            {
                TextureDesc tm = td;
                tm.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
                tm.sampleCount = samples;
                tm.debugName = "SV.GizmoColorMSAA";
                gizmoColorMsaa = frame.CreateTexture("SV.GizmoColorMSAA", tm);
            }

            struct GridSnapshot
            {
                bool Show = false;
                float CamPos[3] = {0, 0, 0};
                bool Is2D = false;
                float CamDistance = 0.0f;
                float Opacity = 1.0f;
                uint32_t Color = 0;
            } grid;
            grid.Show = m_ShowGrid;
            grid.CamPos[0] = static_cast<float>(m_CamPos[0]);
            grid.CamPos[1] = static_cast<float>(m_CamPos[1]);
            grid.CamPos[2] = static_cast<float>(m_CamPos[2]);
            grid.Is2D = m_Is2DMode;
            grid.CamDistance = m_CamDistance;
            grid.Opacity = m_Is2DMode ? svSettings.GetGridOpacity2D() : svSettings.GetGridOpacity3D();
            grid.Color = m_Is2DMode ? svSettings.GetGridColor2D() : svSettings.GetGridColor3D();

            frame.AddPass(
                "SV.Gizmos_Overlay", kOverlayPhase,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    RenderGraph::RGAttachmentOps colorOps{};
                    colorOps.Load = RenderGraph::RGLoadOp::Clear;
                    colorOps.Store = RenderGraph::RGStoreOp::Store;
                    if (samples > 1)
                        p.AttachColorResolve(0, gizmoColorMsaa, gizmoColor, colorOps);
                    else
                        p.AttachColor(0, gizmoColor, colorOps);

                    // World depth is read-only here: the grid and the
                    // depth-split gizmo passes test against scene geometry,
                    // the always-on-top pass ignores it, and none of them write.
                    RenderGraph::RGAttachmentOps depthOps{};
                    depthOps.Load = RenderGraph::RGLoadOp::Load;
                    p.AttachDepth(depth, depthOps, RenderGraph::RGDepthAccess::ReadOnly);
                },
                [idLinesVisible, idLinesOccluded, idTrisVisible, idTrisOccluded,
                 idLinesAlways, idTrisAlways,
                 set0 = GizmoCameraSet0Layout(), camBuf = camAlloc.Buffer,
                 camOff = camAlloc.Offset, camValid = camAlloc.Valid(), lineBuf,
                 lineDraws = std::move(lineDraws), triBuf, triDraws = std::move(triDraws), w, h,
                 cam, grid, gridRenderer = &m_GridRenderer](RenderGraph::RGContext& ctx)
                {
                    auto* cl = ctx.Cmd;
                    auto* dev = ctx.GetDevice();
                    if (!cl || !dev)
                        return;

                    cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                    cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));

                    if (grid.Show)
                        gridRenderer->RecordRG(ctx, &cam, grid.CamPos, grid.Is2D,
                                                grid.CamDistance, static_cast<float>(h),
                                                grid.Opacity, grid.Color);

                    if (!camValid || (!lineDraws.size() && !triDraws.size()))
                        return;
                    const PipelineHandle pipeLinesVisible = ctx.GetOrCreatePipelineVariant(idLinesVisible);
                    const PipelineHandle pipeLinesOccluded = ctx.GetOrCreatePipelineVariant(idLinesOccluded);
                    const PipelineHandle pipeTrisVisible = ctx.GetOrCreatePipelineVariant(idTrisVisible);
                    const PipelineHandle pipeTrisOccluded = ctx.GetOrCreatePipelineVariant(idTrisOccluded);
                    const PipelineHandle pipeLinesAlways = ctx.GetOrCreatePipelineVariant(idLinesAlways);
                    const PipelineHandle pipeTrisAlways = ctx.GetOrCreatePipelineVariant(idTrisAlways);
                    if (!pipeLinesVisible.IsValid() || !pipeLinesOccluded.IsValid() ||
                        !pipeTrisVisible.IsValid() || !pipeTrisOccluded.IsValid() ||
                        !pipeLinesAlways.IsValid() || !pipeTrisAlways.IsValid())
                        return;

                    DescriptorSetDesc dsDesc{};
                    dsDesc.layout = set0;
                    dsDesc.transient = true;
                    dsDesc.debugName = "SV_Gizmos_DS";
                    const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
                    if (!ds.IsValid())
                        return;
                    dev->UpdateBufferBinding(ds, 5, camBuf, static_cast<uint32_t>(camOff),
                                             static_cast<uint32_t>(sizeof(CameraData)));

                    GizmoPC pc{};
                    MakeIdentity4(pc.M);

                    // Draws the subset of `draws` this pass owns. Pipeline and
                    // vertex buffer bind lazily on the first match, so a pass
                    // whose subset is empty issues no commands at all — with
                    // nothing opted in, this records exactly what the two-pass
                    // version did.
                    const auto drawSubset = [&](PipelineHandle pipe, BufferHandle buf,
                                                const std::vector<GizmoGroupDraw>& draws,
                                                bool alwaysOnTop, float alphaScale)
                    {
                        if (draws.empty() || !buf.IsValid())
                            return;
                        bool bound = false;
                        for (const GizmoGroupDraw& draw : draws)
                        {
                            if (draw.alwaysOnTop != alwaysOnTop)
                                continue;
                            if (!bound)
                            {
                                cl->SetPipeline(pipe);
                                cl->BindDescriptorSet(0, ds, pipe);
                                cl->SetVertexBuffer(buf, 0);
                                bound = true;
                            }
                            pc.color[0] = draw.color.r;
                            pc.color[1] = draw.color.g;
                            pc.color[2] = draw.color.b;
                            pc.color[3] = draw.color.a * alphaScale;
                            cl->SetPushConstants(pc);
                            cl->Draw(draw.vertexCount, 1u, draw.firstVertex, 0u);
                        }
                    };

                    // Always-on-top first, then occluded, then visible.
                    //
                    // Always-on-top leads because its members are backdrops: the
                    // spline's width band has to sit UNDER the curve and knots
                    // that stay depth-tested, and this pass blends in submission
                    // order.
                    //
                    // Occluded before visible within the depth split: the depth
                    // test partitions each primitive's pixels between the two,
                    // but where two DIFFERENT primitives land on the same pixel
                    // the full-strength fragment blends on top.
                    struct DepthPass
                    {
                        PipelineHandle Lines;
                        PipelineHandle Tris;
                        float AlphaScale;
                        bool AlwaysOnTop;
                    };
                    const DepthPass depthPasses[3] = {
                        {pipeLinesAlways, pipeTrisAlways, 1.0f, true},
                        {pipeLinesOccluded, pipeTrisOccluded, Editor::SceneTools::kGizmoOccludedAlphaScale, false},
                        {pipeLinesVisible, pipeTrisVisible, 1.0f, false},
                    };
                    for (const DepthPass& pass : depthPasses)
                    {
                        drawSubset(pass.Lines, lineBuf, lineDraws, pass.AlwaysOnTop,
                                   pass.AlphaScale);
                        drawSubset(pass.Tris, triBuf, triDraws, pass.AlwaysOnTop,
                                   pass.AlphaScale);
                    }
                });
        }
        else
        {
            gizmoColor = RenderGraph::RGTexture{}; // pipelines unavailable: no overlay this frame
        }
    }

    // ── Passes 2+3: selection mask + Sobel outline (selection OR hover, and
    // not the Box highlight style — the old EnablePass gate, as an if). ──
    const bool selectionOutlinesVisible = Editor::SceneViewSettings::Get().GetSelectionOutlinesVisible();
    const bool hasSelectionTarget = (selectionOutlinesVisible && !m_SelectedEntities.empty()) ||
                                    m_HoveredEntity.IsValid();
    const bool outlineEnabled =
        highlightStyle != Editor::SelectionHighlightStyle::Box && hasSelectionTarget;
    if (outlineEnabled)
        DeclareSelectionOutlineRG(frame, finalColor, rs, w, h, samples, selectionOutlinesVisible);

    // ── Pass 4: gizmo composite — declared LAST so the transform gizmo
    // draws on top of outlined selections (declaration order = execution
    // order on the shared finalColor chain). ──
    if (gizmoColor.IsValid())
    {
        std::vector<uint8_t> vs;
        std::vector<uint8_t> fs;
        if (!LoadGizmoCompositeShaderBytes(vs, fs))
            return;
        const SamplerHandle linearClamp = rs->Textures().GetSampler(SamplerPreset::LinearClamp);
        if (!linearClamp.IsValid())
            return;
        const DescriptorSetLayoutDesc set0 = FullscreenSamplerSet0Layout("SV_Gizmo_Composite_Set0");
        const GraphicsPipelineId idComposite = PipelineDescTranslator::InternGraphics(
            *device,
            BuildFullscreenBlendPipelineDesc(vs, fs, set0, "SV_Gizmo_Composite",
                                             BlendFactor::One));
        if (!idComposite.IsValid())
            return;

        frame.AddPass(
            "SV.Gizmos_Composite", kOverlayPhase,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Load; // blend over the scene (derives ColorLoad)
                ops.Store = RenderGraph::RGStoreOp::Store;
                p.AttachColor(0, finalColor, ops);
                p.Read(gizmoColor, RenderGraph::RGTextureRead::Sampled);
            },
            [idComposite, set0, gizmoColor, linearClamp, w, h](RenderGraph::RGContext& ctx)
            {
                auto* cl = ctx.Cmd;
                auto* dev = ctx.GetDevice();
                if (!cl || !dev)
                    return;
                const TextureHandle gizmoTex = ctx.GetTexture(gizmoColor);
                if (!gizmoTex.IsValid())
                    return;
                const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(idComposite);
                if (!pipe.IsValid())
                    return;

                cl->SetPipeline(pipe);
                DescriptorSetDesc dsDesc{};
                dsDesc.layout = set0;
                dsDesc.transient = true;
                dsDesc.debugName = "SV_Gizmo_Composite_DS";
                const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
                if (!ds.IsValid())
                    return;
                dev->UpdateCombinedImageSamplerBinding(ds, 0, gizmoTex, linearClamp);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
                cl->Draw(3, 1);
            });
    }

    // Game UI overlay: composite the world's UIDocument HUDs over the editing
    // viewport so the Scene View previews the in-game UI. The GameUI pass runs at
    // phase kUI (400), so it sorts after the gizmo composite's kOverlay (300) into
    // finalColor (composited over the scene). User-toggleable via the Scene View
    // "Show Game UI" setting (default on).
    if (svSettings.GetShowGameUI())
    {
        if (!m_GameUI)
            // The Scene View overlays onto the editor's shared scene+gizmos target,
            // so a Fullscreen UIDocument must NOT clear it (neverClearTarget=true).
            // Font from the staged editor assets next to Editor.exe.
            m_GameUI = GameUIHost::CreateForHost(
                device, &EngineCore::GetInstance().GetAssetManager(),
                &EngineCore::GetInstance().GetJobSystem(),
                Editor::GetEditorGlobalPaths().installAssetsRoot, /*neverClearTarget=*/true);
        m_GameUiScale.Apply(*m_GameUI->GetUIManager());
        // The HUD composites onto the scene pipeline's finalColor: its
        // declared space is the PRODUCER's stamp (#784), not the display's.
        m_GameUI->SyncAndRender(&GetWorld(), frame, finalColor,
                                UI::UITargetSpace::ForPipelineOutput(
                                    rs->GetPipelineOutputSpaceRG(),
                                    device->GetActiveHdrOutputMode()),
                                w, h);
    }
    else if (m_GameUI)
    {
        // Toggled off → free the host (its UIManager, GPU rings, and asset-pinned
        // document subtrees) instead of leaving a full game-UI stack resident and
        // frozen per pane. Re-created on demand when the toggle comes back on.
        m_GameUI.reset();
    }
}

void SceneViewController::DeclareSelectionOutlineRG(RenderGraph::RGFrame& frame,
                                                     RenderGraph::RGTexture finalColor,
                                                     Engine::Renderer::RenderServices* rs,
                                                     uint32_t w, uint32_t h, uint32_t samples,
                                                     bool includeSelectedEntities)
{
    IDevice* device = frame.Device();
    auto* world = &GetWorld();
    if (!device)
        return;

    MeshGPURegistry& meshReg = rs->GetMeshGPURegistry();
    auto& skeletonStore = Engine::Renderer::SkeletonStore::Instance();

    glm::mat4 viewProj;
    std::memcpy(&viewProj[0][0], m_FrameCameraData.viewProj, sizeof(float) * 16);

    // Resolve every selection/hover draw NOW — the exec lambda replays the
    // snapshot with zero ECS/registry access.
    std::vector<MaskDraw> draws;
    uint32_t dbgNoHandle = 0, dbgNoEntry = 0;
    // Set when any contributor produced plugin/procedural mask parts: their
    // content is not in the cheap key, so the resolve marks the cache to
    // re-render every frame while such a contributor is present.
    bool anyPluginContributor = false;
    auto appendMeshDraw = [&](ECS::EntityHandle entity,
                              const Components::WorldTransform& xf,
                              MeshGPUHandle handle,
                              bool allowSkinning,
                              TextureHandle alphaTexture = {},
                              float alphaCutoff = 0.5f,
                              float uvScaleX = 1.0f,
                              float uvScaleY = 1.0f,
                              float uvOffsetX = 0.0f,
                              float uvOffsetY = 0.0f,
                              const float* windStrength = nullptr,
                              const float* windParams = nullptr,
                              float windSeed = 0.0f) -> bool
    {
        if (!handle.IsValid())
        {
            ++dbgNoHandle;
            return false;
        }
        const MeshGPUEntry* entry = meshReg.Find(handle);
        if (!entry)
        {
            ++dbgNoEntry;
            return false;
        }
        MeshGPUEntryBindings entryBindings{};
        if (!meshReg.TryGetDrawableBindings(*entry, entryBindings) || entry->indexCount == 0)
            return false;

        MaskDraw d{};
        d.alphaTest = alphaTexture.IsValid();
        d.alphaTexture = alphaTexture;
        d.alphaCutoff = std::clamp(alphaCutoff, 0.0f, 1.0f);
        d.uvScale[0] = uvScaleX;
        d.uvScale[1] = uvScaleY;
        d.uvOffset[0] = uvOffsetX;
        d.uvOffset[1] = uvOffsetY;
        d.windTime = rs->GetShaderAnimationTimeSeconds();
        d.windSeed = windSeed;
        if (windStrength)
            std::memcpy(d.windStrength, windStrength, sizeof(d.windStrength));
        if (windParams)
            std::memcpy(d.windParams, windParams, sizeof(d.windParams));
        d.skinned = allowSkinning && IsSkinned(entry->vertexFlags) && entryBindings.jointsVB.IsValid() &&
                    entryBindings.weightsVB.IsValid();
        if (d.skinned)
        {
            // Palette offset 0 = the identity block SkinPaletteAtlas seeds
            // each frame, so entities without an active animation render
            // bind pose, not garbage.
            const auto* skelRef = world->GetComponent<Components::SkeletonRef>(entity);
            if (skelRef && skelRef->runtimeId != 0)
            {
                if (const auto* runtime = skeletonStore.GetRuntime(skelRef->runtimeId))
                    d.paletteOffset = runtime->AtlasPaletteOffsetBones;
            }
        }

        glm::mat4 model;
        std::memcpy(&model[0][0], xf.matrix, sizeof(float) * 16);
        const glm::mat4 vpm = viewProj * model;
        std::memcpy(d.vpm, &vpm[0][0], sizeof(d.vpm));

        d.coreVB = entryBindings.coreVB;
        d.jointsVB = entryBindings.jointsVB;
        d.weightsVB = entryBindings.weightsVB;
        d.indexBuffer = entryBindings.indexBuffer;
        d.indexType = entry->indexType;
        d.indexCount = entry->indexCount;
        d.firstIndex = entry->firstIndex;
        d.vertexOffset = static_cast<int32_t>(entry->vertexOffset);
        draws.push_back(d);
        return true;
    };

    std::vector<Editor::SelectionMaskPart> pluginParts; // reused across the per-entity loop
    auto appendEntity = [&](ECS::EntityHandle entity)
    {
        if (!world->IsValid(entity))
            return;
        const auto* xfPtr = world->GetComponent<Components::WorldTransform>(entity);
        if (!xfPtr)
            return;

        const auto* mrPtr = world->GetComponent<Components::MeshRenderer>(entity);
        if (mrPtr && world->IsComponentEnabled(entity, ECS::GetComponentTypeId<Components::MeshRenderer>()))
            (void)appendMeshDraw(entity, *xfPtr, MeshGPUHandle(mrPtr->meshGpuHandleId), true);

        // Plugin-owned runtime meshes: enabled editor plugins contribute mask
        // parts as pure data (mesh handle id, alpha texture, wind), resolved
        // plugin-side against Engine.dll.
        pluginParts.clear();
        Editor::EditorPluginRegistry::Get().CollectSelectionMaskParts(*world, entity, pluginParts);
        if (!pluginParts.empty())
            anyPluginContributor = true;
        for (const Editor::SelectionMaskPart& part : pluginParts)
        {
            (void)appendMeshDraw(entity,
                                 *xfPtr,
                                 MeshGPUHandle(part.MeshGpuHandleId),
                                 part.AllowSkinning,
                                 part.AlphaTexture,
                                 part.AlphaCutoff,
                                 part.UvScale[0],
                                 part.UvScale[1],
                                 part.UvOffset[0],
                                 part.UvOffset[1],
                                 part.WindStrength,
                                 part.WindParams,
                                 part.WindSeed);
        }
    };

    // Ordered contributor set (selection + descendants, hover + descendants),
    // de-duped exactly as the resolve below consumes it, without the entities
    // that are not active in the hierarchy: they render nothing, so they outline
    // nothing. The cheap dirty key is built from THIS set's ids + world
    // transforms + camera VP — without the per-entity mesh/plugin resolution — so
    // a cache hit skips that resolution (the reported hover-and-hold cost), not
    // just the GPU mask pass; an entity switching off or on changes the set.
    std::vector<ECS::EntityHandle> contributors;
    Editor::CollectSelectionOutlineContributors(
        *world,
        {m_SelectedEntities, m_SelectionDescendants, m_HoveredEntity, m_HoveredDescendants, includeSelectedEntities},
        contributors);

    if (contributors.empty())
    {
        m_OutlineMaskValid = false;
        return;
    }

    // ── Cheap dirty key: dimensions + camera VP + per-contributor id, world
    // transform, and (MeshRenderer) mesh identity and on/off state.
    // Skinned/wind silhouettes animate without a transform change, and
    // plugin/procedural contributors (e.g. an EZTree with no MeshRenderer) carry
    // content not folded into the key, so the last resolve records
    // m_OutlineMaskAnimated / m_OutlineMaskHasPlugin and forces a per-frame
    // re-render while either holds. (The mask is on-top, so foreign occluders
    // never affect it.) ──
    uint64_t key = 1469598103934665603ull; // FNV-1a offset basis
    auto foldBytes = [&key](const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i)
        {
            key ^= b[i];
            key *= 1099511628211ull;
        }
    };
    auto foldU64 = [&](uint64_t v) { foldBytes(&v, sizeof(v)); };
    foldU64((static_cast<uint64_t>(w) << 32) | h);
    foldBytes(m_FrameCameraData.viewProj, sizeof(float) * 16);
    for (const ECS::EntityHandle e : contributors)
    {
        foldU64(static_cast<uint64_t>(ECS::EntityHandleHash{}(e)));
        if (const auto* xf = world->GetComponent<Components::WorldTransform>(e))
            foldBytes(xf->matrix, sizeof(float) * 16);
        else
            foldU64(0);
        uint64_t contentWord = 0;
        if (const auto* mr = world->GetComponent<Components::MeshRenderer>(e))
            contentWord ^= static_cast<uint64_t>(mr->meshGpuHandleId) |
                           (world->IsComponentEnabled(e, ECS::GetComponentTypeId<Components::MeshRenderer>())
                                ? (1ull << 62)
                                : 0ull);
        foldU64(contentWord);
    }

    // ── Retained (pool-persistent, per-view) silhouette mask. NOT a transient:
    // transients alias frame-to-frame, so a mask read on a cache-hit frame
    // (when no geometry re-renders) would sample stale/aliased memory. The
    // physical is stable across frames while imported+read every frame; a
    // physical change (resize / age-out / device rebuild) forces a re-render
    // regardless of the content key. ──
    TextureDesc md{};
    md.width = w;
    md.height = h;
    md.format = static_cast<uint32_t>(TextureFormat::R8_UNORM);
    md.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    md.sampleCount = 1;
    md.debugName = "SV.SelectionMask";
    const std::string maskName =
        m_RenderNamePrefix + ".SelectionMask.V" + std::to_string(static_cast<uint64_t>(m_ViewId));
    const RenderGraph::RGTexture maskTex = frame.ImportPersistentTexture(maskName.c_str(), md);
    if (!maskTex.IsValid())
    {
        m_OutlineMaskValid = false;
        return;
    }
    const uint64_t maskPhysical = static_cast<uint64_t>(frame.PhysicalTexture(maskTex));
    const bool physicalChanged = maskPhysical != m_OutlineMaskPhysical || maskPhysical == 0;
    const bool needResolve = !m_OutlineMaskValid || physicalChanged || key != m_OutlineMaskKey ||
                             m_OutlineMaskAnimated || m_OutlineMaskHasPlugin;

    if (needResolve)
    {
        // Resolve the per-entity mesh/plugin draws NOW (the expensive part —
        // component lookups + plugin dispatch + MeshGPURegistry resolves, skipped
        // entirely on a cache hit).
        for (const ECS::EntityHandle e : contributors)
            appendEntity(e);

        if (draws.empty())
        {
            // Nothing resolvable this frame — drop the cache so the next
            // non-empty set forces a fresh render (its physical may have aged
            // out meanwhile).
            m_OutlineMaskValid = false;
            m_OutlineMaskAnimated = false;
            m_OutlineMaskHasPlugin = false;
            if (dbgNoHandle > 0 || dbgNoEntry > 0)
            {
                static bool sLoggedOnce = false;
                if (!sLoggedOnce)
                {
                    sLoggedOnce = true;
                    Logger::Log::Warning(
                        "[SelectionMask] selected entity has no MeshGPU handle/entry (noHandle={}, "
                        "noEntry={}); outline will be missing for primitives whose meshGpuHandleId is 0.",
                        dbgNoHandle, dbgNoEntry);
                }
            }
            return;
        }

        bool anySkinned = false;
        bool anyAnimated = false;
        for (const MaskDraw& d : draws)
        {
            anySkinned = anySkinned || d.skinned;
            const bool hasWind = d.windStrength[0] != 0.0f || d.windStrength[1] != 0.0f ||
                                 d.windStrength[2] != 0.0f || d.windStrength[3] != 0.0f;
            anyAnimated = anyAnimated || d.skinned || hasWind;
        }
        // Skinned/wind content, and any plugin/procedural contributor, re-render
        // every frame (their silhouettes change under a stable cheap key); static
        // MeshRenderer content rides the cache next frame.
        m_OutlineMaskAnimated = anyAnimated;
        m_OutlineMaskHasPlugin = anyPluginContributor;

        // Pipelines + the skinned palette descriptor set, resolved at declaration.
        // The mask is on-top (culling off, no depth), so no winding split.
        std::vector<uint8_t> vsStatic;
        std::vector<uint8_t> fsShared;
        if (!LoadSelectionMaskShaderBytes(vsStatic, fsShared))
            return;
        const GraphicsPipelineId idStatic = PipelineDescTranslator::InternGraphics(
            *device, BuildSelectionMaskPipelineDesc(vsStatic, fsShared, /*skinned*/ false));
        if (!idStatic.IsValid())
            return;

        GraphicsPipelineId idAlpha{};
        DescriptorSetLayoutDesc alphaSet0{};
        SamplerHandle alphaSampler{};
        {
            std::vector<uint8_t> vsAlpha;
            std::vector<uint8_t> fsAlpha;
            if (LoadSelectionMaskAlphaShaderBytes(vsAlpha, fsAlpha))
            {
                alphaSet0 = AlphaMaskSet0Layout();
                idAlpha = PipelineDescTranslator::InternGraphics(
                    *device, BuildSelectionMaskPipelineDesc(vsAlpha, fsAlpha, /*skinned*/ false,
                                                            /*alphaTest*/ true));
                alphaSampler = rs->Textures().GetSampler(SamplerPreset::LinearRepeat);
            }
        }

        GraphicsPipelineId idSkinned{};
        DescriptorSetHandle skinnedDS{};
        if (anySkinned)
        {
            std::vector<uint8_t> vsSkinned;
            if (LoadSelectionMaskSkinnedVertexBytes(vsSkinned))
                idSkinned = PipelineDescTranslator::InternGraphics(
                    *device,
                    BuildSelectionMaskPipelineDesc(vsSkinned, fsShared, /*skinned*/ true));

            // PerFrameWritePool cycles a small fixed ring of persistent palette
            // buffers; one cached (non-transient) DS per distinct buffer handle.
            const BufferHandle paletteBuf = rs->GetSkinPaletteAtlas().GetBuffer();
            if (idSkinned.IsValid() && paletteBuf.IsValid())
            {
                const uint64_t paletteKey = static_cast<uint64_t>(paletteBuf);
                auto it = m_SelectionMaskSkinnedDSByBuffer.find(paletteKey);
                if (it != m_SelectionMaskSkinnedDSByBuffer.end())
                {
                    skinnedDS = it->second;
                }
                else
                {
                    DescriptorSetDesc dsDesc{};
                    dsDesc.layout = SkinnedMaskSet0Layout();
                    dsDesc.transient = false;
                    dsDesc.debugName = "SV_Selection_Mask_Skinned_DS";
                    skinnedDS = device->CreateDescriptorSet(dsDesc);
                    if (skinnedDS.IsValid())
                    {
                        device->UpdateStorageBufferBinding(skinnedDS, kBonePaletteAtlasBinding,
                                                           paletteBuf, 0, 0);
                        m_SelectionMaskSkinnedDSByBuffer.emplace(paletteKey, skinnedDS);
                    }
                }
            }
            // No skinned pipeline/DS: skinned draws fall back to the static path
            // below (bind pose, but never missing) — same policy as the old arm.
            if (!idSkinned.IsValid() || !skinnedDS.IsValid())
            {
                idSkinned = {};
                skinnedDS = {};
            }
        }

        // Anti-alias the silhouette at the view's MSAA level: render into a
        // transient MSAA mask and resolve into the single-sample retained mask
        // (mirrors the gizmo overlay's msaa target). Single-sample views attach
        // the retained mask directly.
        RenderGraph::RGTexture maskMsaa{};
        if (samples > 1u)
        {
            TextureDesc tm = md;
            tm.usage = static_cast<uint32_t>(TextureUsage::RenderTarget);
            tm.sampleCount = samples;
            tm.debugName = "SV.SelectionMaskMsaa";
            maskMsaa = frame.CreateTexture("SV.SelectionMaskMsaa", tm);
        }

        // Pointer-only compare preserved across the RGFrameStamp adoption: this
        // check pre-dates the (pointer, FrameIndex) pair convention, and upgrading
        // it to IsFor would strengthen the guard (behavior change) — follow-up.
        const bool readsPalette = anySkinned && skinnedDS.IsValid() &&
                                  rs->FrameRG().For.Frame == &frame &&
                                  rs->FrameRG().SkinPaletteAtlas.IsValid();
        const RenderGraph::RGBuffer paletteAtlas =
            readsPalette ? rs->FrameRG().SkinPaletteAtlas : RenderGraph::RGBuffer{};

        frame.AddPass(
            "SV.SelectionMask", kOverlayPhase,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                if (maskMsaa.IsValid())
                    p.AttachColorResolve(0, maskMsaa, maskTex, ops);
                else
                    p.AttachColor(0, maskTex, ops);
                // The skinned VS binds the palette atlas descriptor-direct; this
                // declared read is the ONLY thing ordering it after the skinning
                // compute (RenderServices.h GpuDrivenFrameRG contract).
                if (paletteAtlas.IsValid())
                    p.Read(paletteAtlas, RenderGraph::RGBufferRead::Storage);
            },
            [draws = std::move(draws), idStatic, idAlpha, alphaSet0, alphaSampler, idSkinned,
             skinnedDS, w, h](RenderGraph::RGContext& ctx)
            {
                auto* cl = ctx.Cmd;
                auto* dev = ctx.GetDevice();
                if (!cl || !dev)
                    return;
                const PipelineHandle pipeStatic = ctx.GetOrCreatePipelineVariant(idStatic);
                if (!pipeStatic.IsValid())
                    return;
                PipelineHandle pipeSkinned{};
                if (idSkinned.IsValid())
                    pipeSkinned = ctx.GetOrCreatePipelineVariant(idSkinned);
                PipelineHandle pipeAlpha{};
                if (idAlpha.IsValid())
                    pipeAlpha = ctx.GetOrCreatePipelineVariant(idAlpha);

                cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));

                cl->SetPipeline(pipeStatic);
                PipelineHandle currentPipe = pipeStatic;

                MaskPC pcData{};
                for (const MaskDraw& d : draws)
                {
                    const bool drawAlpha = d.alphaTest && pipeAlpha.IsValid() && alphaSampler.IsValid();
                    const bool drawSkinned =
                        !drawAlpha && d.skinned && pipeSkinned.IsValid() && skinnedDS.IsValid();
                    const PipelineHandle desiredPipe =
                        drawAlpha ? pipeAlpha : (drawSkinned ? pipeSkinned : pipeStatic);
                    if (desiredPipe != currentPipe)
                    {
                        cl->SetPipeline(desiredPipe);
                        if (drawSkinned)
                            cl->BindDescriptorSet(0, skinnedDS, pipeSkinned);
                        currentPipe = desiredPipe;
                    }
                    if (drawAlpha)
                    {
                        DescriptorSetDesc dsDesc{};
                        dsDesc.layout = alphaSet0;
                        dsDesc.transient = true;
                        dsDesc.debugName = "SV_Selection_Mask_Alpha_DS";
                        const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
                        if (!ds.IsValid())
                            continue;
                        dev->UpdateCombinedImageSamplerBinding(ds, 0, d.alphaTexture, alphaSampler);
                        cl->BindDescriptorSet(0, ds, pipeAlpha);
                    }

                    std::memcpy(pcData.uVPM, d.vpm, sizeof(pcData.uVPM));
                    pcData.uMaskParams[0] = d.alphaCutoff;
                    pcData.uMaskParams[1] = d.uvScale[0];
                    pcData.uMaskParams[2] = d.uvScale[1];
                    pcData.uMaskParams[3] = static_cast<float>(d.paletteOffset);
                    pcData.uMaskExtra[0] = d.uvOffset[0];
                    pcData.uMaskExtra[1] = d.uvOffset[1];
                    pcData.uMaskExtra[2] = d.windTime;
                    pcData.uMaskExtra[3] = d.windSeed;
                    std::memcpy(pcData.uWindStrength, d.windStrength, sizeof(pcData.uWindStrength));
                    std::memcpy(pcData.uWindParams, d.windParams, sizeof(pcData.uWindParams));
                    cl->SetPushConstants(pcData);

                    cl->SetVertexBuffer(d.coreVB, VertexBinding::Core);
                    if (drawSkinned)
                    {
                        cl->SetVertexBuffer(d.jointsVB, VertexBinding::Joints);
                        cl->SetVertexBuffer(d.weightsVB, VertexBinding::Weights);
                    }
                    cl->SetIndexBuffer(d.indexBuffer, static_cast<IndexType>(d.indexType));
                    cl->DrawIndexed(d.indexCount, 1u, d.firstIndex, d.vertexOffset, 0u);
                }
            });

        m_OutlineMaskKey = key;
        m_OutlineMaskPhysical = maskPhysical;
        m_OutlineMaskValid = true;
    }

    // Cache hit (or a resolve that produced no mask): only draw the Sobel
    // outline when a valid cached mask exists to sample.
    if (!m_OutlineMaskValid)
        return;

    // Outline: fullscreen Sobel over the mask, alpha-blended onto FinalColor.
    std::vector<uint8_t> vsOutline;
    std::vector<uint8_t> fsOutline;
    if (!LoadSelectionOutlineShaderBytes(vsOutline, fsOutline))
        return;
    const SamplerHandle linearClamp = rs->Textures().GetSampler(SamplerPreset::LinearClamp);
    if (!linearClamp.IsValid())
        return;
    const DescriptorSetLayoutDesc set0 = FullscreenSamplerSet0Layout("SV_Selection_Outline_Set0");
    PipelineDesc pdOutline = BuildFullscreenBlendPipelineDesc(vsOutline, fsOutline, set0,
                                                              "SV_Selection_Outline",
                                                              BlendFactor::SrcAlpha);
    {
        PipelineDesc::PushConstantRangeDesc pr{};
        pr.offset = 0;
        pr.size = static_cast<uint32_t>(sizeof(OutlinePush));
        pr.stagesMask = kShaderStageFragment;
        pdOutline.pushConstantRanges = {pr};
    }
    const GraphicsPipelineId idOutline = PipelineDescTranslator::InternGraphics(*device, pdOutline);
    if (!idOutline.IsValid())
        return;

    OutlinePush push{};
    const auto& settings = Editor::SceneViewSettings::Get();
    {
        const uint32_t argb = settings.GetSelectionOutlineColor();
        push.outlineColor[3] = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
        push.outlineColor[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
        push.outlineColor[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
        push.outlineColor[2] = static_cast<float>((argb) & 0xFFu) / 255.0f;
    }
    push.texelSize[0] = 1.0f / static_cast<float>(w);
    push.texelSize[1] = 1.0f / static_cast<float>(h);
    push.edgeThreshold = 0.10f;
    push.smokeTint = 0.0f;
    {
        const float t = settings.GetSelectionOutlineThickness();
        push.outlineRadius = t > 0.5f ? t : 0.5f;
    }

    frame.AddPass(
        "SV.Selection_Outline", kOverlayPhase,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load; // blend over the scene (derives ColorLoad)
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, finalColor, ops);
            p.Read(maskTex, RenderGraph::RGTextureRead::Sampled);
        },
        [idOutline, set0, maskTex, push, linearClamp, w, h](RenderGraph::RGContext& ctx)
        {
            auto* cl = ctx.Cmd;
            auto* dev = ctx.GetDevice();
            if (!cl || !dev)
                return;
            const TextureHandle maskHandle = ctx.GetTexture(maskTex);
            if (!maskHandle.IsValid())
                return;
            const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(idOutline);
            if (!pipe.IsValid())
                return;

            cl->SetPipeline(pipe);
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = set0;
            dsDesc.transient = true;
            dsDesc.debugName = "SV_Selection_Outline_DS";
            const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
            if (!ds.IsValid())
                return;
            dev->UpdateCombinedImageSamplerBinding(ds, 0, maskHandle, linearClamp);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->SetPushConstants(push);
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
            cl->SetScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
            cl->Draw(3, 1);
        });
}

} // namespace GameEngine
