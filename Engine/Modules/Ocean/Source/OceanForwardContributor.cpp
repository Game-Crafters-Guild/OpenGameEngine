#include "Ocean/OceanForwardContributor.h"
#include "Ocean/OceanRenderFeature.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "Ocean/OceanTypes.h"

#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Logger/Logger.h"
#include "AssetCore/GUID.h"

#include <array>
#include <cstring>

namespace GameEngine::Ocean
{

namespace
{
// Asset previews render isolated worlds and must never grow an ocean surface.
// Keyed on the view's declared PURPOSE, not its debug name — the old strcmp
// list had already rotted (one compared name matched no existing view).
bool IsAssetPreviewView(const ::GameEngine::Rendering::ViewDesc* view)
{
    return view && view->purpose == ::GameEngine::Rendering::ViewPurpose::EditorPreview;
}
} // namespace

OceanForwardContributor::OceanForwardContributor(
    OceanRenderFeature& feature, Engine::Renderer::RenderServices& renderServices)
    : m_Feature(feature)
    , m_RenderServices(renderServices)
{
    // Do not inject the ocean into reflection-probe captures. The surface also
    // consumes those probes, so capturing it creates recursive self-reflection;
    // with a camera-following LOD mesh the six cube faces then expose different
    // ring boundaries as large wedges on the main ocean. This matches the
    // reference setup, where ocean tiles do not participate in light/reflection
    // probes. Planar reflections already exclude the ocean for the same reason.
}

bool OceanForwardContributor::EnsureFallbackCascade(::GameEngine::Rendering::IDevice& device)
{
    if (m_FallbackCascade.IsValid() && m_FallbackCascadeSampler.IsValid())
        return true;

    using namespace ::GameEngine::Rendering;
    TextureDesc d{};
    d.width = 1;
    d.height = 1;
    d.arrayLayers = 1;
    d.mipLevels = 1;
    d.format = static_cast<uint32>(TextureFormat::R16G16B16A16_FLOAT);
    d.usage = static_cast<uint32>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    d.flags = TextureCreateFlags::ForceArrayView;
    d.persistent = true;
    d.initialState = ResourceState::ShaderResource;
    d.debugName = "Ocean_FallbackCascade";
    m_FallbackCascade = device.CreateTexture(d);
    if (m_FallbackCascade.IsValid())
    {
        const uint16_t zeroTexel[4] = {0, 0, 0, 0};
        UploadTexture2D(&device, m_FallbackCascade, zeroTexel, 1, 1, sizeof(zeroTexel),
                        "Ocean_FallbackCascade_Upload");
    }
    if (!m_FallbackCascadeSampler.IsValid())
        m_FallbackCascadeSampler =
            device.CreateSampler(SamplerDesc::MaterialLinearRepeat("Ocean_FallbackCascadeSampler"));
    return m_FallbackCascade.IsValid() && m_FallbackCascadeSampler.IsValid();
}

bool OceanForwardContributor::EnsureMaterial()
{
    if (m_Material)
        return true;
    if (m_MaterialInitAttempted)
        return false;
    m_MaterialInitAttempted = true;

    auto& registry = m_RenderServices.Materials().Registry();
    m_Material = registry.Find(OceanSurfaceMaterialGuid());
    if (m_Material)
        return true;

    m_Material = m_RenderServices.Materials().RegisterMaterialFromDocument(
        OceanSurfaceMaterialGuid(), BuildOceanSurfaceMaterialDocument(),
        OceanSurfaceMaterialKeywords(Rendering::IsCompatShaderProfile()));

    if (!m_Material)
    {
        Logger::Log::Error("OceanForwardContributor: failed to register material");
        return false;
    }


    Logger::Log::Info("OceanForwardContributor: material registered and compiled");
    return true;
}

bool OceanForwardContributor::BuildForwardCommand(
    Engine::Renderer::ForwardEmitContext& ctx, Engine::Renderer::DrawCommand& outCommand,
    std::span<const ::GameEngine::Rendering::RenderGraph::RGTexture>* outSampledCascades)
{
    if (!m_Feature.IsInitialized() || !m_Feature.HasOcean())
        return false;

    const auto viewId = ctx.ViewId;
    if (IsAssetPreviewView(m_RenderServices.Views().FindViewDesc(viewId)))
        return false;

    if (!EnsureMaterial())
        return false;

    auto* device = ctx.Device;
    if (!device)
        return false;

    const uint32 frameIndex = ctx.FrameIndex;

    // Never emit the surface into auxiliary ocean scene-capture views. The
    // reflection view must not recurse the ocean into its own mirror, and the
    // raster-depth view must not capture the water plane as seabed.
    const auto reflectionViewId = m_Feature.GetPlanarReflection().GetReflectionViewId();
    if (reflectionViewId != 0 && viewId == reflectionViewId)
        return false;
    const auto rasterDepthViewId = m_Feature.GetRasterDepthCapture().GetCaptureViewId();
    if (rasterDepthViewId != 0 && viewId == rasterDepthViewId)
        return false;

    const auto gridVB = m_Feature.GetGridVB();
    const auto gridIB = m_Feature.GetGridIB();
    const uint32 indexCount = m_Feature.GetGridIndexCount();
    if (!gridVB.IsValid() || !gridIB.IsValid() || indexCount == 0)
        return false;

    // Refraction grab: bound only when the node built a grab texture this frame
    // (SceneColor resolved + copy shader present). When it declined, the surface
    // reads opaque — the flag stamped into the params buffer gates the shader.
    const bool refractionReady = m_Feature.GetSceneGrab().IsReady(viewId, frameIndex);

    // Seabed depth: bound + sampled only when the bake is ready AND at least one
    // depth content was supplied this frame. Otherwise the surface reads deep
    // water (the shallow/shoreline terms stay off), stamped via
    // SeabedDepthAvailable below.
    const bool seabedReady =
        m_Feature.IsSeabedDepthReady() && m_Feature.GetSeabedDepth().HasSeabeds();

    // Caustics modulate the grabbed scene colour, so they only contribute when
    // refraction is active. Gate on the texture being available (procedural OR a
    // user override) and the refraction grab being bound this frame.
    const bool causticsReady =
        (m_Feature.IsCausticsReady() || m_Feature.HasUserCausticsTexture()) && refractionReady;
    // A user-assigned caustics image overrides the procedural web. It has no encoded
    // R,G distortion channel, so the surface skips the distortion for it (flag).
    const bool causticsTextureReady = m_Feature.HasUserCausticsTexture() && refractionReady;

    // Flow: bound + sampled only when the bake is ready, the authored toggle is on,
    // AND at least one flow source was tagged this frame. Otherwise the surface
    // reads no flow (detail UVs stay stationary), stamped via FlowAvailable below.
    const bool flowReady = m_Feature.IsFlowReady() && m_Feature.IsFlowEnabled() &&
                           m_Feature.GetFlow().HasSources();

    // Dynamic waves: bound + sampled only while the sim is active this frame.
    // Once quiescent, the render node intentionally skips BeginFrame/dispatch;
    // sampling that stale camera-snapped cascade while panning makes the field
    // swim against the FFT surface. Stamped via DynamicWavesAvailable below.
    const bool dynWavesReady = m_Feature.IsDynWavesReady() && m_Feature.IsDynWavesEnabled() &&
                               !m_Feature.GetDynWaves().IsQuiescent();

    // Wave mask: bound + sampled only when at least one local wave override source
    // was tagged this frame. Otherwise the surface uses default multipliers (1,1).
    const bool waveMaskReady = m_Feature.IsWaveMaskReady() && m_Feature.GetWaveMask().HasSources();
    const uint32 localFFTReadyMask = m_Feature.GetLocalFFTReadyMaskForFrame(frameIndex);
    const bool localFFTReady = localFFTReadyMask != 0u;

    // Clip: bound + sampled only when the bake is ready, the authored toggle is on,
    // AND at least one clip source was tagged this frame. Otherwise the surface
    // reads the default clip state (solid), stamped via ClipAvailable below.
    const bool clipReady = m_Feature.IsClipReady() && m_Feature.IsClipEnabled() &&
                           m_Feature.GetClip().HasSources();

    // Albedo: bound + sampled only when the bake is ready, the authored toggle is
    // on, AND at least one albedo source was tagged this frame. Otherwise the
    // surface keeps its base water colour, stamped via AlbedoAvailable below.
    const bool albedoReady = m_Feature.IsAlbedoReady() && m_Feature.IsAlbedoEnabled() &&
                             m_Feature.GetAlbedo().HasSources();

    // User foam-bubble texture: bound + sampled (in place of the analytic Worley)
    // whenever the component slot resolved to a valid texture this frame. With no
    // user texture the surface uses the procedural foam (user-controllable via the
    // OceanSurface foam parameters).
    const bool foamTextureReady = m_Feature.HasUserFoamTexture();
    // Planar reflections: bound + sampled when the authored toggle is on AND the
    // capture produced a reflection texture this frame. Otherwise the surface
    // reads the procedural sky dome (PlanarReflectionAvailable stays 0).
    const bool allowPlanarReflection =
        ctx.Purpose != Engine::Renderer::ForwardEmitPurpose::ReflectionProbeCapture;
    const bool planarReflectionReady =
        allowPlanarReflection &&
        m_Feature.GetParams().PlanarReflections != 0u &&
        m_Feature.GetPlanarReflection().IsReady(frameIndex);

    // Combine cascade: currently only bound if a world-stable bake was scheduled.
    // Otherwise the surface sums the FFT cascades directly (CombineWavesAvailable 0).
    const bool combineReady = m_Feature.IsCombineReadyForFrame(frameIndex);

    // Per-view params live in the frame's upload ring (host-coherent, written
    // here at declare/emit time) — no per-view persistent buffer ring.
    if (!ctx.Frame)
        return false;
    const auto paramsAlloc = ctx.Frame->AllocUpload<OceanParamsGPU>();
    if (!paramsAlloc.Valid())
        return false;
    m_Feature.FillViewParams(*paramsAlloc.Ptr, viewId, frameIndex, refractionReady, seabedReady,
                             causticsReady, flowReady, dynWavesReady, waveMaskReady, clipReady,
                             albedoReady, foamTextureReady, causticsTextureReady,
                             planarReflectionReady);
    paramsAlloc.Ptr->NormalTextureAvailable = m_Feature.HasUserNormalTexture() ? 1u : 0u;
    paramsAlloc.Ptr->RefractionDepthMatched =
        refractionReady && m_Feature.GetSceneGrab().IsDepthMatched(viewId) ? 1u : 0u;

    // Two-sided whenever the authored underwater look is enabled: the surface's
    // underside must remain rasterizable even if the per-view submersion flag is
    // late/missing for an editor view. The shader still uses the per-view
    // Underwater flag to decide when to flip underside normals and add submerged
    // lighting; this only controls culling.
    const bool twoSided = m_Feature.IsUnderwaterEnabled() ||
                          m_Feature.GetParamsForView(viewId).Underwater != 0u;

    // Build the vec4 clipmap-vertex pipeline variant (material base id has the
    // standard mesh layout; ocean vertices carry local XZ, LOD and skirt scale).
    // Rebuild both cached
    // variants on a material recompile so the cull mode can be selected per view.
    const uint32_t materialVersion = m_Material->GetVersion();
    if (m_PipelineMaterialVersion != materialVersion)
    {
        m_OceanPipelineIds[0] = {};
        m_OceanPipelineIds[1] = {};
        m_PipelineMaterialVersion = materialVersion;
    }

    auto& oceanPipelineId = m_OceanPipelineIds[twoSided ? 1 : 0];
    if (!oceanPipelineId.IsValid())
    {
        const auto* base = device->LookupGraphicsPipeline(m_Material->GetGraphicsPipelineId());
        if (!base)
            return false;
        Rendering::GraphicsPipelineDesc gd = *base;
        gd.VertexBindings = {{0, sizeof(float) * 4, 0}};
        gd.VertexAttributes = {{0, 0, Rendering::Format::R32G32B32A32_FLOAT, 0}};
        if (twoSided)
            gd.Rasterization.cullMode = Rendering::CullModeFlagBits::None;
        oceanPipelineId = device->InternGraphicsPipeline(std::move(gd));
    }

    using namespace Engine::Renderer;

    // Sweep view scratch whose view stopped emitting (forward-contributor scratch pattern).
    constexpr uint32_t kStaleFrameThreshold = 8;
    if (frameIndex > kStaleFrameThreshold)
    {
        const uint32_t cutoff = frameIndex - kStaleFrameThreshold;
        for (auto it = m_Scratch.begin(); it != m_Scratch.end();)
        {
            if (it->second.LastFrameUsed < cutoff)
                it = m_Scratch.erase(it);
            else
                ++it;
        }
    }
    auto& scratch = m_Scratch[viewId];
    scratch.LastFrameUsed = frameIndex;
    scratch.Buffers.clear();
    scratch.Buffers.push_back({HashStringId("OceanParamsBuffer"), paramsAlloc.Buffer,
                               paramsAlloc.Offset, static_cast<uint64_t>(sizeof(OceanParamsGPU))});

    uint64 materialBytes = 0;
    const auto materialUpload = m_Feature.UploadWaterMaterials(*ctx.Frame, materialBytes);
    if (!materialUpload.Valid())
        return false;
    scratch.Buffers.push_back(
        {HashStringId("OceanWaterMaterials"), materialUpload.Buffer, materialUpload.Offset, materialBytes});

    // Collect the RG import for each DYNAMIC cascade bound below, so the
    // consuming pass declares real sampled reads (layout transition + hazard
    // edge). Collection mirrors the bind conditions exactly — a texture is
    // declared iff it is bound.
    scratch.SampledRG.clear();
    auto collectRG = [&](auto&& importFn)
    {
        if (!ctx.Frame)
            return; // no frame stream — nothing to import into or declare
        const ::GameEngine::Rendering::RenderGraph::RGTexture t = importFn();
        if (t.IsValid())
            scratch.SampledRG.push_back(t);
    };

    // Bind the FFT displacement cascade array (sampled in the vertex modifier and
    // for the surface normal). The OceanParamsBuffer's FFTCascadeCount gates
    // whether the shader actually samples it.
    scratch.Textures.clear();
    // The fallback cascade completes the compat set-2 contract: the compat
    // shader profile declares displacement + foam unconditionally, and WebGPU
    // refuses a bind group with a declared entry unwritten. Sampling stays
    // gated by the availability flags either way.
    const bool haveFallbackCascade = EnsureFallbackCascade(*device);
    bool boundDisplacement = false;
    if (m_Feature.IsFFTReady())
    {
        const auto dispTex = m_Feature.GetDisplacementTexture();
        if (dispTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanDisplacement");
            tex.Texture = dispTex;
            tex.Sampler = m_Feature.GetDisplacementSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetFFT().ImportDisplacementRG(*ctx.Frame); });
            boundDisplacement = true;
        }
    }
    if (!boundDisplacement && haveFallbackCascade)
    {
        DrawBindings::TextureEntry tex{};
        tex.Name = HashStringId("uOceanDisplacement");
        tex.Texture = m_FallbackCascade;
        tex.Sampler = m_FallbackCascadeSampler;
        tex.ArrayIndex = 0;
        scratch.Textures.push_back(tex);
    }
    if (localFFTReady)
    {
        const auto localNames = std::array{
            HashStringId("uOceanLocalDisplacement0"),
            HashStringId("uOceanLocalDisplacement1"),
            HashStringId("uOceanLocalDisplacement2"),
            HashStringId("uOceanLocalDisplacement3"),
            HashStringId("uOceanLocalDisplacement4"),
            HashStringId("uOceanLocalDisplacement5"),
            HashStringId("uOceanLocalDisplacement6"),
            HashStringId("uOceanLocalDisplacement7")};
        for (uint32 stream = 0; stream < kMaxOceanLocalFFTStreams; ++stream)
        {
            if ((localFFTReadyMask & (1u << stream)) == 0u)
                continue;
            const auto localDispTex = m_Feature.GetLocalDisplacementTexture(stream);
            if (!localDispTex.IsValid())
                continue;
            DrawBindings::TextureEntry tex{};
            tex.Name = localNames[stream];
            tex.Texture = localDispTex;
            tex.Sampler = m_Feature.GetLocalDisplacementSampler(stream);
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetLocalFFT(stream).ImportDisplacementRG(*ctx.Frame); });
        }
        if (waveMaskReady)
        {
            const auto localMaskNames = std::array{
                HashStringId("uOceanLocalFFTMask0"),
                HashStringId("uOceanLocalFFTMask1")};
            for (uint32 page = 0u; page < kOceanLocalFFTMaskPages; ++page)
            {
                const auto localFFTMaskTex = m_Feature.GetWaveMask().GetLocalFFTMaskTexture(page);
                if (!localFFTMaskTex.IsValid())
                    continue;
                DrawBindings::TextureEntry tex{};
                tex.Name = localMaskNames[page];
                tex.Texture = localFFTMaskTex;
                tex.Sampler = m_Feature.GetWaveMaskSampler();
                tex.ArrayIndex = 0;
                scratch.Textures.push_back(tex);
                collectRG([&] { return m_Feature.GetWaveMask().ImportLocalFFTMaskRG(*ctx.Frame, page); });
            }
        }
    }

    // Bind the simulated foam cascade (set 2 binding 3) and the layout of every
    // camera-snapped cascade the surface samples (set 2 binding 4, one layout per
    // cascade). Each sim snaps on its own schedule (a bake whenever it is
    // considered, the foam and dynamic waves only on the frames they step), so
    // each texture is sampled with the layout it was written with. The layout
    // UBO is ALWAYS bound; a cascade whose sim is not running keeps LodCount 0,
    // which its sample helper reads as unavailable (without the UBO the binder's
    // fallback would feed a garbage LodCount). The foam texture itself binds only
    // when ready (the default-slot fallback covers the unbound case exactly like
    // uOceanDisplacement).
    bool boundFoam = false;
    if (m_Feature.IsFoamReady())
    {
        const auto foamTex = m_Feature.GetFoamTexture();
        if (foamTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanFoam");
            tex.Texture = foamTex;
            tex.Sampler = m_Feature.GetFoamSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetFoamSim().ImportWriteRG(*ctx.Frame); });
            boundFoam = true;
        }
    }
    if (!boundFoam && haveFallbackCascade)
    {
        // Completes the compat set-2 contract (see uOceanDisplacement above);
        // LodCount 0 in the layout keeps every foam sample on its fallback path.
        DrawBindings::TextureEntry tex{};
        tex.Name = HashStringId("uOceanFoam");
        tex.Texture = m_FallbackCascade;
        tex.Sampler = m_FallbackCascadeSampler;
        tex.ArrayIndex = 0;
        scratch.Textures.push_back(tex);
    }
    OceanRenderFeature::SampledCascadeAvailability available{};
    available[OceanSampledCascade::Foam] = boundFoam;
    available[OceanSampledCascade::SeabedDepth] = seabedReady;
    available[OceanSampledCascade::Flow] = flowReady;
    available[OceanSampledCascade::DynWaves] = dynWavesReady;
    available[OceanSampledCascade::WaveMask] = waveMaskReady;
    available[OceanSampledCascade::Clip] = clipReady;
    available[OceanSampledCascade::Albedo] = albedoReady;
    if (const auto layoutAlloc = ctx.Frame->AllocUpload<OceanSampledCascadeLayoutsGPU>();
        layoutAlloc.Valid())
    {
        *layoutAlloc.Ptr = m_Feature.SampledCascadeLayouts(available);
        scratch.Buffers.push_back({HashStringId("OceanCascadeLayout"), layoutAlloc.Buffer,
                                   layoutAlloc.Offset,
                                   static_cast<uint64_t>(sizeof(OceanSampledCascadeLayoutsGPU))});
    }

    // Combined displacement cascade (set 2 binding 14) + its own snapped layout (set
    // 2 binding 15). Bound only when a stable combine bake was scheduled; otherwise
    // the binder's default-slot fallback covers the off case. The layout UBO is
    // always bound (zeroed LodCount when off) so the binder never feeds garbage. The
    // shader gates the path on uCombineWavesAvailable (stamped in UploadParams).
    if (combineReady)
    {
        const auto combTex = m_Feature.GetCombineSim().GetTexture();
        if (combTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanCombinedDisplacement");
            tex.Texture = combTex;
            tex.Sampler = m_Feature.GetCombineSim().GetSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetCombineSim().ImportRG(*ctx.Frame); });
        }
    }
    OceanCascadeLayoutGPU combineLayout{};
    if (combineReady)
        combineLayout = m_Feature.GetCombineSim().GetLayout();
    if (const auto combineAlloc = ctx.Frame->AllocUpload<OceanCascadeLayoutGPU>();
        combineAlloc.Valid())
    {
        *combineAlloc.Ptr = m_Feature.ClampCascadeLayout(combineLayout);
        scratch.Buffers.push_back({HashStringId("OceanCombineCascadeLayout"), combineAlloc.Buffer,
                                   combineAlloc.Offset,
                                   static_cast<uint64_t>(sizeof(OceanCascadeLayoutGPU))});
    }

    // Scene-colour grab (set 2 binding 5): the opaque scene snapshot the surface
    // refracts. Bound only when ready; the shader's RefractionAvailable flag (set
    // above) gates the sample, so the binder's default-slot fallback covers the
    // unbound case exactly like uOceanDisplacement / uOceanFoam.
    if (refractionReady)
    {
        const auto grabTex = m_Feature.GetSceneGrab().GetGrabTexture(static_cast<uint32_t>(viewId));
        if (grabTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanSceneColor");
            tex.Texture = grabTex;
            tex.Sampler = m_Feature.GetSceneGrab().GetSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetSceneGrab().ImportForSampling( *ctx.Frame, static_cast<uint32_t>(viewId), frameIndex); });
        }
    }

    // Sea-floor depth cascade (set 2 binding 6): the top-down seabed depth the
    // surface reads for the shallow-water colour + shoreline foam. Bound only when
    // ready + a seabed is present; the shader's SeabedDepthAvailable flag (set
    // above) gates the sample, so the binder's default-slot fallback covers the
    // unbound case exactly like uOceanDisplacement / uOceanFoam.
    if (seabedReady)
    {
        const auto depthTex = m_Feature.GetSeabedDepthTexture();
        if (depthTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanSeabedDepth");
            tex.Texture = depthTex;
            tex.Sampler = m_Feature.GetSeabedDepthSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetSeabedDepth().ImportRG(*ctx.Frame); });
        }
    }

    // Procedural caustics (set 2 binding 7): the underwater light-focusing web the
    // surface multiplies into the refracted scene colour. Bound only when ready +
    // refraction is active; the shader's CausticsAvailable flag (set above) gates
    // the sample, so the binder's default-slot fallback covers the unbound case
    // exactly like the other ocean sim textures.
    if (causticsReady)
    {
        // A user-assigned caustics image (if any) overrides the procedural web on the
        // same uOceanCaustics slot; otherwise the procedural texture is bound.
        const auto userCaustics = m_Feature.GetUserCausticsTexture();
        const auto causticsTex = userCaustics.IsValid() ? userCaustics : m_Feature.GetCausticsTexture();
        if (causticsTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanCaustics");
            tex.Texture = causticsTex;
            tex.Sampler = userCaustics.IsValid() ? m_Feature.GetUserCausticsSampler()
                                                 : m_Feature.GetCausticsSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
        }
    }

    // User foam-bubble texture (set 2 binding 12): the optional whitecap/bubble image
    // the surface samples for the foam dissolve instead of the analytic Worley. Bound
    // only when the component slot resolved; the shader's FoamTextureAvailable flag
    // gates the sample, so the binder's default-slot fallback covers the unbound case.
    if (m_Feature.HasUserNormalTexture())
    {
        DrawBindings::TextureEntry tex{};
        tex.Name = HashStringId("uOceanDetailNormal");
        tex.Texture = m_Feature.GetUserNormalTexture();
        tex.Sampler = m_Feature.GetUserFoamSampler();
        tex.ArrayIndex = 0;
        scratch.Textures.push_back(tex);
    }
    if (foamTextureReady)
    {
        DrawBindings::TextureEntry tex{};
        tex.Name = HashStringId("uOceanFoamBubble");
        tex.Texture = m_Feature.GetUserFoamTexture();
        tex.Sampler = m_Feature.GetUserFoamSampler();
        tex.ArrayIndex = 0;
        scratch.Textures.push_back(tex);
    }

    // Planar reflection capture (set 2 binding 13): the mirrored opaque scene the
    // surface samples in place of the procedural sky dome. Bound only when ready;
    // the shader's PlanarReflectionAvailable flag (set above) gates the sample, so
    // the binder's default-slot fallback covers the unbound case.
    if (planarReflectionReady)
    {
        const auto reflTex = m_Feature.GetPlanarReflection().GetReflectionTexture();
        if (reflTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanReflection");
            tex.Texture = reflTex;
            tex.Sampler = m_Feature.GetPlanarReflection().GetSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetPlanarReflection().ImportForSampling(*ctx.Frame, frameIndex); });
        }
    }

    // Image-based lighting: ocean uses an interned custom grid pipeline, so it
    // does not get recompiled through the ordinary per-draw world-pass variant
    // path. Bind the shared IBL resources directly under the same reflected names
    // as RenderServices so the ocean can match the active sky/environment.
    auto& ibl = m_RenderServices.EnsureFeature<Engine::Renderer::ImageBasedLightingFeature>();
    if (!ibl.IsInitialized())
        ibl.Initialize(device);
    if (ibl.IsInitialized())
    {
        if (auto* sky = m_RenderServices.GetFeature<Engine::Renderer::SkyRenderFeature>();
            sky && sky->HasActiveSettings())
        {
            ibl.SetIblIntensity(sky->GetSettings().iblIntensity);
        }
        if (auto* iblSource = ibl.GetEnvironmentSource(); !iblSource || iblSource->InputDigest() == 0)
            ibl.SetIblIntensity(0.0f);
        const auto envBuffer = ibl.UploadEnvData(device);

        const auto cubeSampler = ibl.GetCubeSampler();
        const auto lutSampler = ibl.GetLutSampler();
        if (cubeSampler.IsValid())
        {
            if (const auto irradiance = ibl.GetIrradianceCube(); irradiance.IsValid())
            {
                DrawBindings::TextureEntry tex{};
                tex.Name = HashStringId("ge_irradianceCube");
                tex.Texture = irradiance;
                tex.Sampler = cubeSampler;
                scratch.Textures.push_back(tex);
            }
            if (const auto prefilter = ibl.GetPrefilterCube(); prefilter.IsValid())
            {
                DrawBindings::TextureEntry tex{};
                tex.Name = HashStringId("ge_prefilterCube");
                tex.Texture = prefilter;
                tex.Sampler = cubeSampler;
                scratch.Textures.push_back(tex);
            }
        }
        if (lutSampler.IsValid())
        {
            if (const auto brdfLut = ibl.GetBrdfLut(); brdfLut.IsValid())
            {
                DrawBindings::TextureEntry tex{};
                tex.Name = HashStringId("ge_brdfLUT");
                tex.Texture = brdfLut;
                tex.Sampler = lutSampler;
                scratch.Textures.push_back(tex);
            }
        }
        if (envBuffer.IsValid())
            scratch.Buffers.push_back({HashStringId("Env"), envBuffer, 0u, 0u});
    }

    // The per-view accumulated field is declared after the engine shadow pass.
    const auto shadowInfo = ctx.Frame->AllocUpload<OceanShadowSamplingGPU>();
    if (shadowInfo.Valid())
    {
        const bool ready =
            m_Feature.GetShadows().FillSampling(viewId, ctx.Frame->FrameIndex(), *shadowInfo.Ptr);
        scratch.Buffers.push_back({HashStringId("OceanShadowInfo"), shadowInfo.Buffer, shadowInfo.Offset,
                                   sizeof(OceanShadowSamplingGPU)});
        if (ready)
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanShadows");
            tex.Texture = m_Feature.GetShadows().GetTexture(viewId);
            tex.Sampler = m_Feature.GetShadows().GetSampler();
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetShadows().ImportRG(*ctx.Frame, viewId); });
        }
    }

    // Flow cascade (set 2 binding 8): the horizontal current the surface reads to
    // scroll its detail-normal UVs (the foam sim advects by it separately). Bound
    // only when ready + the toggle is on + a source is present; the shader's
    // FlowAvailable flag (set above) gates the sample, so the binder's default-slot
    // fallback covers the unbound case exactly like the other ocean sim textures.
    if (flowReady)
    {
        const auto flowTex = m_Feature.GetFlowTexture();
        if (flowTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanFlow");
            tex.Texture = flowTex;
            tex.Sampler = m_Feature.GetFlowSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetFlow().ImportRG(*ctx.Frame); });
        }
    }

    // Dynamic-wave cascade (set 2 binding 9): the interactive (height, velocity)
    // state the surface reads for the displacement add (vertex) + normal fold
    // (fragment). Bound only when the same frame scheduled the sim; the shader's
    // DynamicWavesAvailable flag (set above) gates the sample, so the binder's
    // default-slot fallback covers the unbound case.
    if (dynWavesReady)
    {
        const auto dynTex = m_Feature.GetDynWavesTexture();
        if (dynTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanDynWaves");
            tex.Texture = dynTex;
            tex.Sampler = m_Feature.GetDynWavesSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetDynWaves().ImportWriteRG(*ctx.Frame); });
        }
    }

    // Wave-mask cascade (set 2 binding 16): local wave-weight/chop multipliers
    // baked from water-body wave overrides. Bound only when a source exists; the
    // shader's WaveMaskAvailable flag gates sampling and otherwise returns (1,1,0,0).
    if (waveMaskReady)
    {
        const auto maskTex = m_Feature.GetWaveMaskTexture();
        if (maskTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanWaveMask");
            tex.Texture = maskTex;
            tex.Sampler = m_Feature.GetWaveMaskSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetWaveMask().ImportMaskRG(*ctx.Frame); });
        }
    }

    // Clip cascade (set 2 binding 10): the surface clip state the surface reads to
    // discard fragments inside cut holes. Bound only when ready + the toggle is on +
    // a source is present; the shader's ClipAvailable flag (set above) gates the
    // sample, so the binder's default-slot fallback covers the unbound case exactly
    // like the other ocean sim textures.
    if (clipReady)
    {
        const auto clipTex = m_Feature.GetClipTexture();
        if (clipTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanClip");
            tex.Texture = clipTex;
            tex.Sampler = m_Feature.GetClipSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetClip().ImportRG(*ctx.Frame); });
        }
    }

    // Albedo cascade (set 2 binding 11): the surface paint the surface blends into
    // baseColor before lighting + under foam. Bound only when ready + the toggle is
    // on + a source is present; the shader's AlbedoAvailable flag (set above) gates
    // the sample, so the binder's default-slot fallback covers the unbound case.
    if (albedoReady)
    {
        const auto albedoTex = m_Feature.GetAlbedoTexture();
        if (albedoTex.IsValid())
        {
            DrawBindings::TextureEntry tex{};
            tex.Name = HashStringId("uOceanAlbedo");
            tex.Texture = albedoTex;
            tex.Sampler = m_Feature.GetAlbedoSampler();
            tex.ArrayIndex = 0;
            scratch.Textures.push_back(tex);
            collectRG([&] { return m_Feature.GetAlbedo().ImportRG(*ctx.Frame); });
        }
    }

    Rendering::MaterialKeyword passKeywords = Rendering::MaterialKeyword::None;
    if (auto kw = m_RenderServices.GetWorldPassKeywords(viewId))
        passKeywords = *kw;

    DrawBindings drawBindings{};
    drawBindings.Buffers = scratch.Buffers;
    drawBindings.Textures = scratch.Textures;

    outCommand = {};
    outCommand.InternedPipeline = oceanPipelineId;
    outCommand.Material = m_Material;
    outCommand.VertexFlags = Rendering::VertexAttributeFlags::None;
    outCommand.PassKeywords = passKeywords;
    outCommand.Geometry.AltGeom = {gridVB, gridIB, Rendering::IndexType::Uint32};
    outCommand.IndexCount = indexCount;
    outCommand.InstanceCount = 1;
    outCommand.FirstIndex = 0;
    outCommand.FirstInstance = 0;
    outCommand.VertexOffset = 0;
    outCommand.Bindings = drawBindings;
    if (outSampledCascades)
        *outSampledCascades = scratch.SampledRG;
    return true;
}

} // namespace GameEngine::Ocean
