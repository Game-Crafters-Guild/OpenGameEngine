#include "ShaderGraph/GraphPreviewAtlas.h"

#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/PipelineVariantCache.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PassBindingContext.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/HashUtils.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "UI/UIManager.h"
#include "UI/UITextureSpace.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Editor
{
namespace
{
namespace RG = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::MaterialKeyword;

// Stem of every per-instance resource name. Each atlas appends its own id, so
// two open material graphs address different textures and pass keys.
constexpr const char* kResourceStem = "editor_graph_preview_atlas";
constexpr const char* kDebugStem = "Editor.GraphPreviewAtlas";

/// Distinguishes the atlases of concurrently open material-graph panels.
uint32_t NextInstanceId()
{
    static std::atomic<uint32_t> s_next{1};
    return s_next.fetch_add(1, std::memory_order_relaxed);
}

// Base light world for the atlas views; each instance offsets by its own id.
// Never an ECS world id — the atlas submits its two preview lights directly.
constexpr uint64 kAtlasLightWorldBase = 0x4752'4150'4154'4C53ull; // "GRAPATLS"

// Camera framing, matching the material thumbnail preview's rig so the atlas
// look tracks the slot look it replaces.
constexpr float kFovRad = 45.0f * 3.1415926535f / 180.0f;
constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 10.0f;
constexpr float kEyeX = 0.9f;
constexpr float kEyeY = 0.9f;
constexpr float kEyeZ = 1.8f;

// Preview key light direction at orbit yaw zero (ModelThumbnailHandler's rig).
constexpr float kKeyLightDirX = -0.267f;
constexpr float kKeyLightDirY = -0.802f;
constexpr float kKeyLightDirZ = -0.535f;

// Exposure of the IBL-off preview: the key/ambient rig is authored around
// reference white, so the tonemap runs at unity.
constexpr float kPreviewExposure = 1.0f;

// PC block of adapter_vertex.glsl's non-instanced path.
struct alignas(16) PreviewPushConstants
{
    float uM[16];
    float uN0[4];
    float uN1[4];
    float uN2[4];
    float uExtra[4];
};
static_assert(sizeof(PreviewPushConstants) == 128,
              "must match adapter_vertex.glsl's PC block");

/// Uniform sphere scale whose projected silhouette spans `fill` of the cell.
///
/// The frame's full height at distance d is 2*d*tan(fov/2) and the sphere's
/// apparent diameter is 2*d*tan(theta), so diameter/frameHeight is
/// tan(theta)/tan(fov/2) — the halves cancel. The silhouette's half-angle comes
/// from sin(theta) = r/d, not r/d directly: at this framing the tangent-vs-ratio
/// difference is several percent of the cell.
float SphereScaleForFill(float fill)
{
    const float camDist = std::sqrt(kEyeX * kEyeX + kEyeY * kEyeY + kEyeZ * kEyeZ);
    const float tanHalfFov = std::tan(kFovRad * 0.5f);
    const float tanTheta = std::clamp(fill, 0.05f, 0.99f) * tanHalfFov;
    const float sinTheta = tanTheta / std::sqrt(1.0f + tanTheta * tanTheta);
    // PrimitiveGenerator's sphere has radius 0.5.
    return (camDist * sinTheta) / 0.5f;
}

} // namespace

void GraphPreviewAtlas::Initialize(Engine::Renderer::RenderServices* services)
{
    m_Services = services;
    if (!m_UiResourceName.empty())
        return;

    const uint32_t instanceId = NextInstanceId();
    const std::string suffix = std::to_string(instanceId);
    m_UiResourceName = std::string(kResourceStem) + "_" + suffix;
    const std::string debugStem = std::string(kDebugStem) + "." + suffix;
    m_AtlasResourceKey = debugStem;
    m_MsaaResourceKey = debugStem + ".ColorMSAA";
    m_DepthResourceKey = debugStem + ".Depth";
    m_HdrResourceKey = debugStem + ".HDR";
    m_PassName = debugStem;
    m_TonemapPassName = debugStem + ".Tonemap";
    m_LightWorldId = kAtlasLightWorldBase + instanceId;
}

void GraphPreviewAtlas::Shutdown()
{
    if (m_Services)
    {
        // View before camera: ReleaseCamera refuses while any view still
        // references the camera, so the reverse order would leak the slot.
        if (m_ViewId != 0)
            m_Services->Views().ReleaseView(m_ViewId);
        if (m_CameraId != 0)
            m_Services->Views().ReleaseCamera(m_CameraId);
    }
    m_ViewId = 0;
    m_CameraId = 0;
    if (m_Services && m_AtlasTexture.IsValid())
    {
        if (auto* device = m_Services->GetDevice())
            device->DestroyTexture(m_AtlasTexture);
    }
    m_AtlasTexture = {};
    m_AtlasInitialized = false;
    m_Requests.clear();
    m_Services = nullptr;
}

bool GraphPreviewAtlas::SetRequests(uint64_t windowId, std::vector<NodeRequest> requests)
{
    m_WindowId = windowId;
    if (m_Requests.size() == requests.size()
        && std::equal(m_Requests.begin(), m_Requests.end(), requests.begin(),
                      [](const NodeRequest& a, const NodeRequest& b)
                      { return a.NodeId == b.NodeId && a.MaterialGuid == b.MaterialGuid; }))
    {
        return false;
    }

    /* A re-sync rebuilds the request list from the model, which knows nothing
       about how far a cell has been spun. Carry each surviving cell's angle
       across or the preview snaps back to zero on every rebuild. */
    for (NodeRequest& incoming : requests)
    {
        for (const NodeRequest& previous : m_Requests)
        {
            if (previous.NodeId == incoming.NodeId)
            {
                incoming.YawRadians = previous.YawRadians;
                break;
            }
        }
    }

    m_Requests = std::move(requests);

    std::vector<std::string> keys;
    keys.reserve(m_Requests.size());
    for (const NodeRequest& r : m_Requests)
        keys.push_back(r.NodeId);
    m_Layout.Reconcile(keys);
    m_Dirty = true;
    return true;
}

bool GraphPreviewAtlas::TakeDeferredRebuild(bool gestureActive)
{
    if (!m_RebuildDeferred)
    {
        m_RebuildDeferredFrames = 0;
        return false;
    }
    ++m_RebuildDeferredFrames;
    if (gestureActive && m_RebuildDeferredFrames <= kMaxDeferredRebuildFrames)
        return false;
    m_RebuildDeferred = false;
    m_RebuildDeferredFrames = 0;
    return true;
}

void GraphPreviewAtlas::SetRequestYaw(const std::string& nodeId, float yawRadians)
{
    for (NodeRequest& request : m_Requests)
    {
        if (request.NodeId != nodeId)
            continue;
        if (request.YawRadians == yawRadians)
            return;
        request.YawRadians = yawRadians;
        m_Dirty = true;
        return;
    }
}

bool GraphPreviewAtlas::ClearRequests()
{
    if (m_Requests.empty())
        return false;
    m_Requests.clear();
    m_Layout.Reconcile({});
    m_Dirty = true;
    return true;
}

bool GraphPreviewAtlas::TryGetBinding(const std::string& nodeId,
                                      GraphNodePreviewBinding& out) const
{
    // Deliberately not gated on the atlas having rendered: the thumb binds the
    // moment its cell exists, and the UI simply paints nothing while the name is
    // unresolved. Gating here would bind "no preview" on the rebind that follows
    // a rebuild — before the atlas has ever run — and nothing would rebind after.
    uint32_t cell = 0;
    if (!m_Layout.TryGetCell(nodeId, cell))
        return false;
    const GraphPreviewAtlasLayout::CellRect rect = m_Layout.BackgroundRectForCell(cell);
    out.Resource = m_UiResourceName;
    out.SizeXPercent = rect.SizeXPercent;
    out.SizeYPercent = rect.SizeYPercent;
    out.PosXPercent = rect.PosXPercent;
    out.PosYPercent = rect.PosYPercent;
    return true;
}

bool GraphPreviewAtlas::ContentDigestMoved()
{
    uint64_t digest = Rendering::HashUtils::HashValue(
        0x9E37'79B9'7F4A'7C15ull, Engine::Renderer::Material::GetGlobalContentEpoch());
    auto& registry = m_Services->Materials().Registry();
    for (const NodeRequest& r : m_Requests)
    {
        const Engine::Renderer::Material* mat = registry.Find(r.MaterialGuid);
        digest = Rendering::HashUtils::HashValue(digest, mat ? mat->GetVersion() : 0u);
        digest = Rendering::HashUtils::HashValue(
            digest, (mat && mat->GetGraphicsPipelineId().IsValid()) ? 1u : 0u);
    }
    if (digest == m_ContentDigest)
        return false;
    m_ContentDigest = digest;
    return true;
}

void GraphPreviewAtlas::EnsureView()
{
    if (m_CameraId == 0)
        m_CameraId = m_Services->Views().AllocateCamera("Editor Graph Preview Atlas Camera");
    if (m_ViewId == 0)
    {
        m_ViewId = m_Services->Views().AllocateView("Editor Graph Preview Atlas View", m_CameraId,
                                                    Rendering::ViewPurpose::EditorPreview);
        m_Services->Views().SetViewWorldId(m_ViewId, m_LightWorldId);
    }
}

bool GraphPreviewAtlas::EnsureAtlasTexture(uint32_t width, uint32_t height)
{
    if (m_AtlasTexture.IsValid() && m_AtlasWidth == width && m_AtlasHeight == height)
        return true;

    auto* device = m_Services->GetDevice();
    if (!device || width == 0 || height == 0)
        return false;

    if (m_AtlasTexture.IsValid())
        device->DestroyTexture(m_AtlasTexture);

    Rendering::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    // Display-referred LINEAR, like the thumbnail slots: the editor's terminal
    // FinalSRGBEncode pass owns the only OETF, so an sRGB atlas would encode
    // twice. RGBA16F keeps the darks off the 8-bit linear banding floor.
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget)
               | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = m_AtlasResourceKey.c_str();
    m_AtlasTexture = device->CreateTexture(td);
    if (!m_AtlasTexture.IsValid())
        return false;

    m_AtlasWidth = width;
    m_AtlasHeight = height;
    m_AtlasInitialized = false;
    m_UiBound = false;
    return true;
}

void GraphPreviewAtlas::SubmitPreviewLights()
{
    using ModelThumb = ModelThumbnailHandler;

    Components::Light key{};
    key.Type = Components::LightType::Directional;
    key.Color[0] = key.Color[1] = key.Color[2] = 1.0f;
    key.Intensity = std::clamp(ModelThumb::GetPreviewLightIntensity(), 0.25f, 4.0f);
    key.CastsShadows = false;

    Engine::Renderer::ExtractedLight keyOut{};
    keyOut.SortId = 1;
    keyOut.type = key.Type;
    Components::ResolveLightColorIntensity(key, keyOut.color, keyOut.intensity);
    keyOut.directionWS[0] = kKeyLightDirX;
    keyOut.directionWS[1] = kKeyLightDirY;
    keyOut.directionWS[2] = kKeyLightDirZ;
    keyOut.castsShadows = 0;
    m_Services->SubmitLight(m_LightWorldId, keyOut);

    Components::Light ambient{};
    ambient.Type = Components::LightType::Ambient;
    ambient.Color[0] = ambient.Color[1] = ambient.Color[2] = 1.0f;
    ambient.Intensity = std::max(0.0f, ModelThumb::GetPreviewAmbientIntensity());
    ambient.CastsShadows = false;

    Engine::Renderer::ExtractedLight ambientOut{};
    ambientOut.SortId = 2;
    ambientOut.type = ambient.Type;
    Components::ResolveLightColorIntensity(ambient, ambientOut.color, ambientOut.intensity);
    m_Services->SubmitLight(m_LightWorldId, ambientOut);

    m_Services->FinalizeWorldLights(m_LightWorldId);
    m_Services->WriteViewLightBuffer(m_ViewId);
}

Rendering::CameraData GraphPreviewAtlas::BuildCameraData() const
{
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const Vector3 eye(kEyeX, kEyeY, kEyeZ);
    const Vector3 target(0.0f, 0.0f, 0.0f);
    const Vector3 up(0.0f, 1.0f, 0.0f);

    const Matrix4x4 viewM = GameEngine::Mathematics::MakeLookAtLH(eye, target, up);
    // Cells are square, so the projection is aspect 1 whatever the atlas shape.
    const Matrix4x4 projM =
        GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(kFovRad, 1.0f, kNearZ, kFarZ);
    const Matrix4x4 viewProjM = projM * viewM;

    Rendering::CameraData cam{};
    const float* viewSrc = viewM.Data();
    const float* projSrc = projM.Data();
    const float* viewProjSrc = viewProjM.Data();
    for (int i = 0; i < 16; ++i)
    {
        cam.view[i] = viewSrc[i];
        cam.proj[i] = projSrc[i];
        cam.viewProj[i] = viewProjSrc[i];
        // Render origin is inactive here (sector 0), so the rebased matrices
        // equal the full-world ones and the shader takes the full-world path.
        cam.viewRel[i] = viewSrc[i];
        cam.viewProjRel[i] = viewProjSrc[i];
    }
    cam.cameraPos[0] = kEyeX;
    cam.cameraPos[1] = kEyeY;
    cam.cameraPos[2] = kEyeZ;
    cam.cameraPos[3] = 0.0f;
    return cam;
}

void GraphPreviewAtlas::SetIblEnabled(bool enabled)
{
    if (m_IblEnabled == enabled)
        return;
    m_IblEnabled = enabled;
    m_Dirty = true;
}

bool GraphPreviewAtlas::IblAvailable() const
{
    if (!m_Services)
        return false;
    const auto* ibl = m_Services->GetFeature<Engine::Renderer::ImageBasedLightingFeature>();
    return ibl != nullptr && ibl->IsInitialized();
}

void GraphPreviewAtlas::TickRG(uint64_t windowId, UIManager* ui, RG::RGFrame& frame)
{
    if (!m_Services || windowId == 0 || windowId != m_WindowId)
        return;

    /* Cold IBL variants skip their draw at record time (the publish-gate
       enqueues the compile); the atomic brings the retry back to this thread. */
    if (m_RecordSkippedDraws.exchange(0, std::memory_order_relaxed) != 0)
        m_Dirty = true;

    if (m_Requests.empty())
    {
        if (m_UiBound && ui)
        {
            ui->RemoveExternalTexture(m_UiResourceName);
            m_UiBound = false;
        }
        return;
    }

    if (!EnsureAtlasTexture(m_Layout.WidthPx(), m_Layout.HeightPx()))
        return;
    EnsureView();
    if (m_ViewId == 0)
        return;

    // The atlas rides the graph even on frames it is not redrawn: the UI pass
    // resolves the name against this frame's publish, which is also the edge
    // that orders the atlas write before the UI sample.
    const RG::RGTexture atlas = frame.ImportExternalTexture(
        m_AtlasResourceKey.c_str(), m_AtlasTexture,
        m_AtlasInitialized ? Rendering::ResourceState::ShaderResource
                           : Rendering::ResourceState::Undefined,
        Rendering::TextureFormat::R16G16B16A16_FLOAT);
    if (!atlas.IsValid())
        return;

    // Publishing an atlas that has never been rendered would hand the UI a
    // texture whose contents are undefined, so the name stays unresolved (the
    // thumb simply paints nothing) until the first render lands.
    const auto publishToUi = [&]
    {
        if (!ui || !m_AtlasInitialized)
            return;
        ui->SetExternalTextureRG(m_UiResourceName, m_AtlasWidth, m_AtlasHeight,
                                 UI::UITextureSpace::DisplayLinearSdr(),
                                 Rendering::TextureFormat::R16G16B16A16_FLOAT);
        ui->PublishExternalTextureRG(m_UiResourceName, frame, atlas);
        m_UiBound = true;
    };

    const bool contentMoved = ContentDigestMoved();
    if (!m_Dirty && !contentMoved && m_UndrawnCells == 0)
    {
        publishToUi();
        return;
    }

    auto& meshRegistry = m_Services->GetMeshGPURegistry();
    const Rendering::MeshGPUEntry* sphere = meshRegistry.FindByKey(
        {Engine::Renderer::PrimitiveGenerator::SphereGuid(), 0});
    Rendering::MeshGPUEntryBindings sphereBindings{};
    if (!sphere || sphere->indexCount == 0
        || !meshRegistry.TryGetDrawableBindings(*sphere, sphereBindings))
    {
        return;
    }

    // Collect the cells whose material can actually be drawn this frame. A
    // material whose base pipeline is still compiling simply skips its cell and
    // the pass re-declares next frame — that is the whole readiness protocol.
    std::vector<CellDraw> draws;
    draws.reserve(m_Requests.size());
    uint32_t undrawn = 0;
    auto& registry = m_Services->Materials().Registry();
    for (const NodeRequest& request : m_Requests)
    {
        uint32_t cell = 0;
        if (!m_Layout.TryGetCell(request.NodeId, cell))
            continue;
        const Engine::Renderer::Material* mat = registry.Find(request.MaterialGuid);
        if (!mat || !mat->GetGraphicsPipelineId().IsValid() || !mat->GetShaderMeta())
        {
            ++undrawn;
            continue;
        }
        draws.push_back(
            CellDraw{cell, request.MaterialGuid, request.YawRadians, request.FillFraction});
    }
    if (draws.empty())
    {
        m_UndrawnCells = undrawn;
        publishToUi();
        return;
    }

    SubmitPreviewLights();

    // Same sample count as the thumbnail slots this replaces: a 256 px sphere
    // silhouette is all edge, so dropping MSAA here would be a visible
    // regression against the previews it takes over.
    const uint32_t samples = m_Services->GetDefaultMSAASampleCount();
    const bool msaa = samples > 1u;

    Rendering::TextureDesc td{};
    td.width = m_AtlasWidth;
    td.height = m_AtlasHeight;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;

    RG::RGTexture msaaColor{};
    if (msaa)
    {
        td.sampleCount = samples;
        // Must match the resolve target's format, or the resolve is invalid.
        td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget);
        td.debugName = m_MsaaResourceKey.c_str();
        msaaColor = frame.ImportPersistentTexture(m_MsaaResourceKey.c_str(), td);
    }

    td.sampleCount = msaa ? samples : 1u;
    td.format = static_cast<uint32_t>(m_Services->GetDepthFormat());
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
               | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = m_DepthResourceKey.c_str();
    const RG::RGTexture depth =
        frame.ImportPersistentTexture(m_DepthResourceKey.c_str(), td);

    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget)
               | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = m_HdrResourceKey.c_str();
    const RG::RGTexture hdr = frame.ImportPersistentTexture(m_HdrResourceKey.c_str(), td);

    if (!depth.IsValid() || !hdr.IsValid() || (msaa && !msaaColor.IsValid()))
        return;

    // Recording the IBL keyword without the feature's cubes would skip every
    // draw, and the undrawn-cell retry would re-declare the pass every frame
    // with nothing ever landing. Fall back to the single-light rig instead.
    const bool useIbl = m_IblEnabled && IblAvailable();

    // Per-pass resources the base (keyword-less) material variant reflects:
    // camera, per-view light UBO, view params, and the shared material SSBO.
    // Textures resolve through the global bindless set, which the binder
    // short-circuits.
    Engine::Renderer::ResolvedPassResources resources;
    {
        const auto camUpload = frame.AllocUpload<Rendering::CameraData>();
        if (!camUpload.Valid())
            return;
        *camUpload.Ptr = BuildCameraData();
        resources.Buffers.push_back(
            {HashStringId("Cam"), camUpload.Buffer, camUpload.Offset, sizeof(Rendering::CameraData)});

        const auto viewParamsUpload = frame.AllocUpload<Rendering::ViewParamsUBO>();
        if (!viewParamsUpload.Valid())
            return;
        Rendering::ViewParamsUBO vp{};
        const Rendering::CameraData cam = BuildCameraData();
        std::memcpy(vp.ge_view, cam.view, sizeof(vp.ge_view));
        std::memcpy(vp.ge_viewProj, cam.viewProj, sizeof(vp.ge_viewProj));
        std::memcpy(vp.ge_proj, cam.proj, sizeof(vp.ge_proj));
        std::memcpy(vp.ge_prevViewProj, cam.viewProj, sizeof(vp.ge_prevViewProj));
        vp.ge_nearFar[0] = kNearZ;
        vp.ge_nearFar[1] = kFarZ;
        vp.ge_cameraPosWS[0] = kEyeX;
        vp.ge_cameraPosWS[1] = kEyeY;
        vp.ge_cameraPosWS[2] = kEyeZ;
        vp.ge_screenSize[0] = static_cast<float>(GraphPreviewAtlasLayout::kCellPx);
        vp.ge_screenSize[1] = static_cast<float>(GraphPreviewAtlasLayout::kCellPx);
        vp.ge_screenSize[2] = 1.0f / vp.ge_screenSize[0];
        vp.ge_screenSize[3] = 1.0f / vp.ge_screenSize[1];
        *viewParamsUpload.Ptr = vp;
        resources.Buffers.push_back({HashStringId("ViewParams"), viewParamsUpload.Buffer,
                                     viewParamsUpload.Offset, sizeof(Rendering::ViewParamsUBO)});

        if (const auto* perView = m_Services->Views().FindPerView(m_ViewId))
        {
            const uint32_t frameSlot = m_Services->GetDevice()->GetFrameIndex();
            const auto lightBuffer = perView->LightBuffers[frameSlot];
            if (lightBuffer.IsValid())
            {
                resources.Buffers.push_back({HashStringId("LightUBO"), lightBuffer, 0, 0});
                resources.Buffers.push_back({HashStringId("Light"), lightBuffer, 0, 0});
            }
        }

        const auto materialParams = m_Services->Materials().PackedMaterialParams();
        if (!materialParams.Buffer.IsValid())
            return;
        resources.Buffers.push_back({HashStringId("MaterialParams"), materialParams.Buffer,
                                     materialParams.Offset, materialParams.Size});

        /* IBL: the environment cubes, BRDF LUT and EnvData UBO are
           engine-shared and view-independent — bound exactly the way the
           world pass binds them (RenderServicesWorldPass, IBL keyword block). */
        if (useIbl)
        {
            auto* ibl = m_Services->GetFeature<Engine::Renderer::ImageBasedLightingFeature>();
            const Rendering::BufferHandle envData = ibl->UploadEnvData(m_Services->GetDevice());
            resources.Textures.push_back({HashStringId("ge_irradianceCube"),
                                          ibl->GetIrradianceCube(), ibl->GetCubeSampler()});
            resources.Textures.push_back({HashStringId("ge_prefilterCube"),
                                          ibl->GetPrefilterCube(), ibl->GetCubeSampler()});
            resources.Textures.push_back(
                {HashStringId("ge_brdfLUT"), ibl->GetBrdfLut(), ibl->GetLutSampler()});
            if (envData.IsValid())
                resources.Buffers.push_back({HashStringId("Env"), envData, 0, 0});
        }
    }

    const uint32_t cellPx = GraphPreviewAtlasLayout::kCellPx;

    frame.AddPass(
        m_PassName.c_str(), Rendering::PassPhase::kWorldRender,
        [&](RG::RGPassBuilder& p)
        {
            RG::RGAttachmentOps color{};
            color.Load = RG::RGLoadOp::Clear;
            color.Store = RG::RGStoreOp::Store;
            if (msaa)
                p.AttachColorResolve(0, msaaColor, hdr, color);
            else
                p.AttachColor(0, hdr, color);

            RG::RGAttachmentOps depthOps{};
            depthOps.Load = RG::RGLoadOp::Clear;
            depthOps.Store = RG::RGStoreOp::Store;
            // Reverse-Z: the far value is 0.
            depthOps.Clear.Depth = 0.0f;
            p.AttachDepth(depth, depthOps, RG::RGDepthAccess::ReadWrite);
        },
        [this, draws = std::move(draws), resources = std::move(resources), sphere, sphereBindings,
         ibl = useIbl](RG::RGContext& ctx) mutable
        {
            auto* cl = ctx.Cmd;
            if (!cl)
                return;

            auto& binder = m_Services->Materials().Binder();
            auto pass = binder.BeginPass(*cl, m_ViewId, ctx,
                                         ibl ? MaterialKeyword::IBL : MaterialKeyword::None,
                                         std::move(resources), 0);

            const auto setVb = [&](Rendering::BufferHandle vb, uint32_t slot)
            {
                if (vb.IsValid())
                    cl->SetVertexBuffer(vb, slot);
            };
            setVb(sphereBindings.coreVB, 0);
            setVb(sphereBindings.tangentVB, 1);
            setVb(sphereBindings.colorVB, 2);
            setVb(sphereBindings.uv1VB, 3);
            cl->SetIndexBuffer(sphereBindings.indexBuffer,
                               static_cast<Rendering::IndexType>(sphere->indexType));

            auto& registry = m_Services->Materials().Registry();
            for (const CellDraw& draw : draws)
            {
                const Engine::Renderer::Material* mat = registry.Find(draw.MaterialGuid);
                if (!mat)
                    continue;
                // The sphere's vertex layout, not the material's authored one:
                // the base pipeline is interned for the material's variant-key
                // flags and must be re-derived for the geometry actually bound.
                Rendering::PipelineHandle pipeline{};
                // The serve unit the handle was resolved from (current, or the
                // previous one during a rebuild window) — its meta and set
                // layouts always match the bound pipeline.
                const Engine::Renderer::PipelineVariantCache::VariantServeUnit* variantEntry =
                    nullptr;
                if (ibl)
                {
                    // The IBL keyword variant compiles async behind the
                    // publish-gate; a miss skips the cell and the atomic
                    // re-marks the atlas dirty so it retries next frame.
                    auto variant = m_Services->Materials().Variants().GetOrCompileColorVariant(
                        *mat, sphere->vertexFlags,
                        Rendering::PrimitiveTopology::TriangleList, MaterialKeyword::IBL,
                        Rendering::FrontFace::CounterClockwise, ctx);
                    pipeline = variant.first;
                    variantEntry = variant.second;
                    if (!pipeline.IsValid())
                    {
                        m_RecordSkippedDraws.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
                else
                {
                    pipeline = ctx.GetOrCreatePipelineVariant(
                        mat->GetGraphicsPipelineIdForFlags(sphere->vertexFlags,
                                                           m_Services->GetDevice()));
                }
                if (!pipeline.IsValid())
                    continue;

                uint32_t cellX = 0;
                uint32_t cellY = 0;
                m_Layout.CellOrigin(draw.Cell, cellX, cellY);
                cl->SetViewport(static_cast<float>(cellX), static_cast<float>(cellY),
                                static_cast<float>(cellPx), static_cast<float>(cellPx));
                cl->SetScissor(cellX, cellY, cellPx, cellPx);

                /* Column-major yaw about Y, scaled uniformly. Rotating the
                   sphere rather than the camera keeps one camera for the whole
                   pass, which is what lets every cell share one draw setup. */
                const float sphereScale = SphereScaleForFill(draw.FillFraction);
                const float cy = std::cos(draw.YawRadians);
                const float sy = std::sin(draw.YawRadians);
                PreviewPushConstants pc{};
                pc.uM[0] = sphereScale * cy;
                pc.uM[2] = sphereScale * -sy;
                pc.uM[8] = sphereScale * sy;
                pc.uM[10] = sphereScale * cy;
                pc.uM[5] = sphereScale;
                pc.uM[15] = 1.0f;
                // Uniform scale: the normal matrix is the rotation part, which
                // the shader renormalizes, so the scale cancels.
                pc.uN0[0] = cy;
                pc.uN0[2] = -sy;
                pc.uN1[1] = 1.0f;
                pc.uN2[0] = sy;
                pc.uN2[2] = cy;
                const uint32_t materialIndex = mat->GetGpuSceneMaterialIndex();
                std::memcpy(&pc.uExtra[0], &materialIndex, sizeof(materialIndex));

                Engine::Renderer::DrawBindings bindings{};
                bindings.PushConstants = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(&pc), sizeof(pc));

                const auto bound = binder.BindMaterialForDraw(
                    pass, *mat, sphere->vertexFlags, bindings, pipeline,
                    variantEntry ? variantEntry->VariantMeta.get() : mat->GetShaderMeta().get(),
                    /*pipelineSetCount=*/0,
                    variantEntry
                        ? std::span<const Rendering::DescriptorSetLayoutId>(variantEntry->SetLayouts)
                        : std::span<const Rendering::DescriptorSetLayoutId>{});
                if (!bound.IsValid())
                    continue;

                cl->DrawIndexed(sphere->indexCount, 1, sphere->firstIndex,
                                static_cast<int32_t>(sphere->vertexOffset), 0);
            }

            binder.EndPass(pass);
        });

    Rendering::Passes::TonemapParams tmParams{};
    tmParams.preserveAlpha = 1;
    tmParams.exposure = kPreviewExposure;
    const RG::RGPass tonemap = Rendering::Passes::AddTonemapPassRG(
        frame, hdr, atlas, tmParams, m_TonemapPassName.c_str());
    if (!tonemap.IsValid())
        return;

    frame.MarkOutput(atlas, RG::RGImageLayout::ShaderReadOnly);
    m_AtlasInitialized = true;
    m_Dirty = false;
    m_UndrawnCells = undrawn;
    publishToUi();
}

} // namespace GameEngine::Editor
