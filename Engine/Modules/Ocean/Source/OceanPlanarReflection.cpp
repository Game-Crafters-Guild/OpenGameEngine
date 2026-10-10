#include "Ocean/OceanPlanarReflection.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_access.hpp>

namespace GameEngine::Ocean
{

using namespace ::GameEngine::Rendering;
namespace RG = ::GameEngine::Rendering::RenderGraph;

namespace
{
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector4;

// Scaled reflection colour: HDR so reflected highlights survive the Fresnel
// blend, with alpha as coverage (0 where nothing drew -> sky-dome fallback).
constexpr TextureFormat kReflectionColorFormat = TextureFormat::R16G16B16A16_FLOAT;

constexpr float kReflectionClipBiasMeters = 0.02f;

float SignNonZero(float value)
{
    return (value >= 0.0f) ? 1.0f : -1.0f;
}

Matrix4x4 MakeReverseZObliqueProjection(const Matrix4x4& projection,
                                        const Matrix4x4& view,
                                        float seaLevel)
{
    using GameEngine::Mathematics::Inverse;
    using GameEngine::Mathematics::Transpose;

    // Keep only geometry that was above the original water plane. The reflection
    // view contains a mirror in its view matrix, so transforming this world plane
    // into that view space gives a down-facing plane; points that were above the
    // sea still evaluate positive after the mirrored view transform.
    const float clipY = seaLevel + kReflectionClipBiasMeters;
    const Vector4 worldClipPlane(0.0f, 1.0f, 0.0f, -clipY);
    Vector4 viewClipPlane = Transpose(Inverse(view)).Transform(worldClipPlane);

    const float planeLenSq =
        viewClipPlane.x * viewClipPlane.x + viewClipPlane.y * viewClipPlane.y +
        viewClipPlane.z * viewClipPlane.z;
    if (planeLenSq <= 1e-8f)
        return projection;

    Matrix4x4 oblique = projection;
    const glm::mat4& p = projection.GetGLM();
    const glm::vec4 row3 = glm::row(p, 3);

    // Reverse-Z, depth [0,1]: near is (row3 - row2), far is row2. Replace the
    // near plane with the sea clip plane while keeping the opposite far corner on
    // the far plane. This is the D3D/Vulkan reverse-Z variant of oblique near-
    // plane clipping; the usual OpenGL row replacement is not correct here.
    const glm::vec4 qClip(SignNonZero(viewClipPlane.x), SignNonZero(viewClipPlane.y),
                          0.0f, 1.0f);
    const glm::vec4 qView = Inverse(projection).GetGLM() * qClip;
    const glm::vec4 plane(viewClipPlane.x, viewClipPlane.y, viewClipPlane.z,
                          viewClipPlane.w);
    const float denom = glm::dot(plane, qView);
    if (std::fabs(denom) <= 1e-6f)
        return projection;

    const glm::vec4 scaledPlane = plane * (glm::dot(row3, qView) / denom);
    const glm::vec4 newRow2 = row3 - scaledPlane;
    oblique[0][2] = newRow2.x;
    oblique[1][2] = newRow2.y;
    oblique[2][2] = newRow2.z;
    oblique[3][2] = newRow2.w;

    return oblique;
}
} // namespace

OceanPlanarReflection::~OceanPlanarReflection()
{
    if (m_Device)
    {
        if (m_ColorTexture.IsValid())
            m_Device->DestroyTexture(m_ColorTexture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
    }
    // The reflection view/camera live in the RenderServices registry. The feature
    // (our owner) is destroyed by RenderServices, which still outlives this
    // member's destruction, so releasing here is safe (same as the feature's own
    // producer-subscription Reset in its destructor).
    if (m_RenderServices && m_ViewAllocated)
    {
        if (m_ViewId != 0)
            m_RenderServices->Views().ReleaseView(m_ViewId);
        if (m_CameraId != 0)
            m_RenderServices->Views().ReleaseCamera(m_CameraId);
    }
}

CameraData OceanPlanarReflection::MakeMirroredCamera(const CameraData& main, float seaLevel)
{
    using GameEngine::Mathematics::Matrix4x4;

    // Rebuild the main view matrix (column-major, matches Matrix4x4/glm storage).
    Matrix4x4 viewM;
    std::memcpy(viewM.Data(), main.view, sizeof(float) * 16);
    Matrix4x4 projM;
    std::memcpy(projM.Data(), main.proj, sizeof(float) * 16);

    // Householder reflection across the plane y = seaLevel: (x, y, z) -> (x,
    // 2*sea - y, z). Column-major, column-vector convention (M * p): column 1's
    // y component negates, column 3 carries the +2*sea translation.
    Matrix4x4 mirror; // identity
    mirror[1][1] = -1.0f;
    mirror[3][1] = 2.0f * seaLevel;

    // Reflect the world, THEN view it with the main camera: view' = view * mirror.
    const Matrix4x4 reflView = viewM * mirror;
    const Matrix4x4 reflProj = MakeReverseZObliqueProjection(projM, reflView, seaLevel);
    const Matrix4x4 reflViewProj = reflProj * reflView;

    CameraData out{};
    std::memcpy(out.view, reflView.Data(), sizeof(float) * 16);
    std::memcpy(out.proj, reflProj.Data(), sizeof(float) * 16);
    std::memcpy(out.viewProj, reflViewProj.Data(), sizeof(float) * 16);
    out.cameraPos[0] = main.cameraPos[0];
    out.cameraPos[1] = 2.0f * seaLevel - main.cameraPos[1];
    out.cameraPos[2] = main.cameraPos[2];
    out.cameraPos[3] = main.cameraPos[3];
    return out;
}

void OceanPlanarReflection::EnsureView(Engine::Renderer::RenderServices& rs, const ViewDesc& mainView)
{
    if (m_ViewAllocated)
        return;
    m_RenderServices = &rs;
    m_CameraId = rs.Views().AllocateCamera("Ocean Planar Reflection Camera");
    m_ViewId = rs.Views().AllocateView("Ocean Planar Reflection", m_CameraId, ViewPurpose::UtilityCapture,
                               ViewParticipation::OnDemand);
    // Skip the active pipeline (we drive the world pass ourselves), but keep the
    // main view's mask + world so extraction still feeds the scene to this view.
    rs.Views().SetViewActiveRenderPipeline(m_ViewId, false);
    rs.Views().SetViewRenderLayerMask(m_ViewId, mainView.renderLayerMask);
    rs.Views().SetViewWorldId(m_ViewId, mainView.worldId);
    // Mirror matrix flips winding -> flip the world-pass viewport Y to keep the
    // baked front face correct (the surface flips V on sample to compensate).
    rs.Views().SetViewWorldPassFlipY(m_ViewId, true);
    m_ViewAllocated = true;
}

bool OceanPlanarReflection::EnsureColorTexture(IDevice& device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return false;
    if (m_ColorTexture.IsValid() && width == m_Width && height == m_Height)
        return true;

    if (m_ColorTexture.IsValid())
        device.DestroyTexture(m_ColorTexture);

    TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(kReflectionColorFormat);
    td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    // Rests in ShaderResource so the surface's descriptor-direct sample sees a
    // stable layout and next frame's import starts from a defined state.
    td.initialState = ResourceState::ShaderResource;
    td.debugName = "Ocean_PlanarReflection";
    m_ColorTexture = device.CreateTexture(td);
    if (!m_ColorTexture.IsValid())
        return false;
    m_Width = width;
    m_Height = height;

    if (!m_Sampler.IsValid())
        m_Sampler =
            device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_PlanarReflection_Sampler"));
    return m_Sampler.IsValid();
}

bool OceanPlanarReflection::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                           float seaLevel, float resolutionScale,
                                           uint32_t frameIndex)
{
    m_ReflectionReady = false;
    m_ReadyFrameIndex = UINT32_MAX;

    auto& rs = d.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    const CameraData* mainCam = rs.Views().FindCameraData(d.View.cameraId);
    if (!mainCam)
        return false;
    // Uninitialized camera (e.g. a thumbnail before its first update): inverting a
    // zero projection produces NaNs that poison culling.
    if (mainCam->proj[0] == 0.0f && mainCam->proj[5] == 0.0f)
        return false;

    EnsureView(rs, d.View);
    if (m_ViewId == 0)
        return false;

    // OnDemand view: request extraction for as long as the mirror is active;
    // when reflections turn off (or the ocean goes away) the request lapses
    // and the view stops costing extraction/culling entirely. The first frame
    // after an idle stretch has no submissions yet — the bucketer declines and
    // the surface falls back to the sky dome, same as the first-ever frame.
    rs.Views().RequestViewFrame(m_ViewId);

    const float scale = std::clamp(std::isfinite(resolutionScale) ? resolutionScale : 0.5f,
                                   0.125f, 1.0f);
    const uint32_t w = std::max(1u, static_cast<uint32_t>(
        std::ceil(static_cast<float>(d.RenderWidth) * scale)));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(
        std::ceil(static_cast<float>(d.RenderHeight) * scale)));
    if (!EnsureColorTexture(*device, w, h))
        return false;

    // Mirror the main camera across the sea plane. Set BEFORE building the draw
    // stream so frustum culling + the world pass camera UBO use the mirror.
    rs.Views().SetCameraData(m_CameraId, MakeMirroredCamera(*mainCam, seaLevel));

    // Track the main view's mask/world each frame (cheap; covers a main-view
    // reconfigure). These persist on the view for next frame's extraction.
    rs.Views().SetViewRenderLayerMask(m_ViewId, d.View.renderLayerMask);
    rs.Views().SetViewWorldId(m_ViewId, d.View.worldId);

    // Build the secondary view's draw stream, then schedule its bucketer twin —
    // the same ordering the editor thumbnail path uses (lights -> batch keys ->
    // bucketer, all before AddWorldPassForView). A bucketer refusal means the
    // GPU-driven spine isn't stamped on this frame incarnation: decline cleanly
    // (sky-dome fallback) rather than render a stale/blank reflection.
    rs.WriteViewLightBuffer(m_ViewId);
    rs.BuildWorldBatchKeysForView(m_ViewId);
    if (!rs.ScheduleBucketerDispatchesForView(d.Frame, m_ViewId))
        return false;

    // Clear config on the ViewDesc — AddWorldPassForView reads it at declaration.
    // Transparent background (alpha 0 = no reflected geometry), reverse-Z far.
    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    clear.clearColorValue[0] = 0.0f;
    clear.clearColorValue[1] = 0.0f;
    clear.clearColorValue[2] = 0.0f;
    clear.clearColorValue[3] = 0.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs.Views().SetViewClearConfig(m_ViewId, clear);

    // Targets: the feature-owned colour (imported external, so the contributor can
    // bind it descriptor-direct on the surface draw) + a pooled depth (graph-
    // internal only; a transient would trip BuildPassResourcesRG's ge_sceneDepth
    // physical resolve, so use a persistent pool import like the thumbnail path).
    const RG::RGTexture color = d.Frame.ImportExternalTexture(
        "Ocean_PlanarReflection", m_ColorTexture, ResourceState::ShaderResource,
        kReflectionColorFormat);

    TextureDesc dd{};
    dd.width = w;
    dd.height = h;
    dd.depth = 1;
    dd.mipLevels = 1;
    dd.arrayLayers = 1;
    dd.sampleCount = 1;
    dd.format = static_cast<uint32_t>(rs.GetDepthFormat());
    dd.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    dd.debugName = "Ocean_PlanarReflection_Depth";
    const RG::RGTexture depth = d.Frame.ImportPersistentTexture("Ocean_PlanarReflection_Depth", dd);
    if (!color.IsValid() || !depth.IsValid())
        return false;

    // Emit forward-contributor producers (terrain, ocean surface, …) for this view so
    // they draw into the reflection world pass. Regular meshes already reflect via the
    // GPU-driven bucketer stream set up above; terrain is a FORWARD CONTRIBUTOR whose
    // per-view emit is otherwise only triggered by its own pipeline node — which is
    // skipped here (the reflection view runs no active pipeline). Without this, terrain
    // is absent from the mirror. The ocean surface self-skips the reflection view, so
    // it isn't double-drawn. (Terrain CDLOD patch selection for this view still happens
    // in the terrain node; if distant terrain LOD looks off in the mirror, the terrain
    // node needs a per-view select for the reflection camera too.)
    rs.EmitProducerForwardCommandsForView(m_ViewId);

    Engine::Renderer::RenderServices::WorldPassTargetsRG targets{};
    targets.Color = color;
    targets.Depth = depth;
    // Instanced only (no Shadows): the reflection view is inactive, so no cascade
    // pass ran for it — a Shadows keyword would just bind the dummy array. Matches
    // the thumbnail path. Reflections are sun/sky/ambient lit without self-shadow.
    const auto world =
        rs.AddWorldPassForView(d.Frame, m_ViewId, targets, Rendering::MaterialKeyword::Instanced);
    if (!world.Pass.IsValid())
        return false;

    // The same-frame consumer (OceanSurface) declares its Sampled read via
    // ImportForSampling + the sampledTextures span — a real RAW edge that both
    // orders the surface after this mirror pass and lands the
    // RenderTarget->ShaderReadOnly transition before it records. MarkOutput
    // sink-anchors the hidden reflection span (bucketer -> world pass) against
    // culling and restores ShaderReadOnly on frames where the surface declines.
    d.Frame.MarkOutput(color, RG::RGImageLayout::ShaderReadOnly);

    m_ReflectionReady = true;
    m_ReadyFrameIndex = frameIndex;
    return true;
}

RG::RGTexture OceanPlanarReflection::ImportForSampling(RG::RGFrame& frame,
                                                       uint32_t frameIndex) const
{
    if (!IsReady(frameIndex) || !m_ColorTexture.IsValid())
        return {};
    return frame.ImportExternalTexture("Ocean_PlanarReflection", m_ColorTexture,
                                       ResourceState::ShaderResource, kReflectionColorFormat);
}

} // namespace GameEngine::Ocean
