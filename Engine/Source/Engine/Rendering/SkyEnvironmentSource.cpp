#include "Engine/Rendering/SkyEnvironmentSource.h"

#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"

#include "Components/Rendering/SkyEnvironment.h" // Components::SkyMode
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Rendering/Sky/SkySettings.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace GameEngine::Rendering;

namespace
{
constexpr uint32_t kGroup = 8; // convolve compute workgroup edge (matches the .comp files)

// Per-cube-face camera basis fed to sky_capture_cube.frag. Engine-specific
// (LH, Z+ forward, Y+ up) — reproduced componentwise from the proven demo; every
// sign matters (a wrong one mirrors or rotates the captured environment).
struct CubeFaceBasis
{
    float right[3];
    float up[3];
    float forward[3];
};
constexpr CubeFaceBasis kFaces[ImageBasedLightingFeature::kNumCaptureFaces] = {
    {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},   // +X
    {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},   // -X
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},   // +Y
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},   // -Y
    {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},    // +Z
    {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},  // -Z
};

// CPU mirror of sky_capture_cube.frag's SkyUBO block (set 0, binding 0). std140:
// every vec3 is immediately followed by a float, so each row is exactly 16 bytes.
// Only the prefix the capture shader reads is declared; the upload-ring alloc
// is sized to exactly this block (the AtmosphereUBO rides its own alloc).
struct CaptureSkyUbo
{
    float cameraPositionWS[3]; float exposureEV;
    float cameraRightWS[3];    float pad0;
    float cameraUpWS[3];       float pad1;
    float cameraForwardWS[3];  float pad2;
    float sunDirectionWS[3];   float sunAngularRadius;
    float moonDirectionWS[3];  float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    float sunColor[3];         float sunIntensity;
    float timeOfDayHours;      float iblLowerHemisphereDarkness;
    float viewportAspect;      float nightSkyBlend;
    float gradientSkyTop[4];      // .rgb scene-linear; .w >= 0.5 -> bake the gradient sky
    float gradientSkyHorizon[4];
    float gradientSkyBottom[4];
};
static_assert(sizeof(CaptureSkyUbo) == 192, "CaptureSkyUbo must match the GLSL std140 layout");
void FillCaptureUbo(const SkySettings& s, uint32_t face, CaptureSkyUbo& ubo)
{
    ubo = CaptureSkyUbo{};
    const CubeFaceBasis& f = kFaces[face];
    for (int i = 0; i < 3; ++i)
    {
        ubo.cameraPositionWS[i] = 0.0f;
        ubo.cameraRightWS[i] = f.right[i];
        ubo.cameraUpWS[i] = f.up[i];
        ubo.cameraForwardWS[i] = f.forward[i];
        ubo.sunDirectionWS[i] = s.scatteringSunDir[i];
        ubo.sunColor[i] = s.primarySunColor[i];
        ubo.moonDirectionWS[i] = s.moonDirWS[i];
    }
    ubo.exposureEV = s.exposureEV;
    // Exclude the sun DISK from the IBL capture. The disc-free capture never reads
    // sunAngularRadius (the sky-view LUT it samples already excludes the disk), but
    // keep the field at 0 to document intent: the directional light owns the sun's
    // specular highlight; a baked disc would firefly the low-res prefilter cube.
    ubo.sunAngularRadius = 0.0f;
    ubo.sunIntensity = s.primarySunIntensity;
    ubo.viewportAspect = 1.0f;
    ubo.nightSkyBlend = s.nightSkyBlend; // drives the night-sky composite in the capture
    // Ground occlusion is dynamic EnvData consumed after sampling. Capturing an
    // undarkened environment preserves the sky radiance for surfaces (notably
    // the ocean) that intentionally opt out of ground darkening.
    ubo.iblLowerHemisphereDarkness = 0.0f;
    ubo.moonIntensity = 0.0f;            // the disc-free capture does not sample the moon
    ubo.moonUboPad1 = s.skyboxRotationRadians;
    ubo.moonUboPad2 = s.environmentTexture.IsValid() ? s.hdriIntensity : -1.0f;

    // Gradient sky: bake the same scene-linear gradient the dome shows (HDRI wins if both set).
    const bool gradientMode =
        s.skyMode == static_cast<uint32_t>(Components::SkyMode::Gradient) && !s.environmentTexture.IsValid();
    for (int i = 0; i < 3; ++i)
    {
        ubo.gradientSkyTop[i]     = s.gradientSkyTopColor[i];
        ubo.gradientSkyHorizon[i] = s.gradientSkyHorizonColor[i];
        ubo.gradientSkyBottom[i]  = s.gradientSkyBottomColor[i];
    }
    ubo.gradientSkyTop[3]     = gradientMode ? 1.0f : -1.0f;
    ubo.gradientSkyHorizon[3] = 0.0f;
    ubo.gradientSkyBottom[3]  = 0.0f;
}

// Fills the AtmosphereUBO (ground/night/below-horizon composite inputs + planet
// geometry) the capture composites with. Byte-identical to SkyRenderNode's
// FillAtmosphereFromSettings so the baked sky matches the on-screen sky exactly.
void FillCaptureAtmosphere(const SkySettings& s, AtmosphereParametersGPU& atmo)
{
    SkyRenderer::FillDefaultAtmosphere(atmo);
    for (int i = 0; i < 3; ++i)
    {
        atmo.groundAlbedo[i] = s.groundAlbedo[i];
        atmo.groundNightColor[i] = s.groundNightColor[i];
        atmo.groundHorizonColor[i] = s.groundHorizonColor[i];
        atmo.groundHorizonNightColor[i] = s.groundHorizonNightColor[i];
        atmo.nightSkyHorizonColor[i] = s.nightSkyHorizonColor[i];
        atmo.belowHorizonDarkColor[i] = s.belowHorizonDarkColor[i];
    }
    atmo.groundBrightness = s.groundBrightness;
    atmo.groundNightColorStd140Pad = 0.0f;
    atmo.groundHorizonColorStd140Pad = 0.0f;
    atmo.nightSkyHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonDayCosWidth = s.groundHorizonCosWidth;
    atmo.groundHorizonNightCosWidth = s.groundHorizonNightCosWidth;
    atmo.belowHorizonBlendSharpness = s.belowHorizonBlendSharpness;
    atmo.belowHorizonDarkness = s.belowHorizonDarkness;
    atmo.belowHorizonMode = s.belowHorizonMode;
    atmo.groundHazeStrength = s.groundHazeStrength;
}

// Set-0 descriptor layout for the capture graphics pass: SkyUBO (b0), the
// atmospheric sky-view LUT (b1, sampled exactly like sky_render.frag), the
// AtmosphereUBO (b2), and the transmittance LUT (b3). The capture still excludes
// the sun disc; b3 is there for the baked ground bounce, which needs the sun's
// attenuation down to the planet surface. Slot 3 matches sky_render.frag so both
// callers of GE_CompositeGroundFloor read the same LUT from the same binding.
const DescriptorSetLayoutDesc& CaptureDSD()
{
    static const DescriptorSetLayoutDesc d = []() {
        DescriptorSetLayoutDesc x{};
        x.bindings = {
            {0, DescriptorType::UniformBuffer, 1, kShaderStageFragment},
            {1, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
            {2, DescriptorType::UniformBuffer, 1, kShaderStageFragment},
            {3, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment},
        };
        x.debugName = "IBL_Capture_Set0";
        return x;
    }();
    return d;
}


} // namespace

SkyRenderFeature* SkyEnvironmentSource::GetSky() const
{
    return m_Services ? m_Services->GetFeature<SkyRenderFeature>() : nullptr;
}

bool SkyEnvironmentSource::HasActiveSky() const
{
    auto* sky = GetSky();
    return sky && sky->IsInitialized() && sky->HasActiveSettings();
}

uint64_t SkyEnvironmentSource::MaterializedLutDigest() const
{
    auto* sky = GetSky();
    if (!sky || !sky->IsInitialized())
        return 0;
    return sky->GetRenderer().GetSkyViewLutComputedDigest();
}

void SkyEnvironmentSource::SetProbeOverrides(const SkyEnvironmentProbeOverrides& overrides)
{
    SkyEnvironmentProbeOverrides clamped = overrides;
    clamped.Intensity = std::max(0.0f, clamped.Intensity);
    clamped.LowerHemisphereDarkness = std::clamp(clamped.LowerHemisphereDarkness, 0.0f, 1.0f);
    m_ProbeOverrides = clamped;
}

void SkyEnvironmentSource::ClearProbeOverrides()
{
    m_ProbeOverrides.reset();
}

SkySettings SkyEnvironmentSource::GetEffectiveSettings() const
{
    SkySettings settings{};
    if (auto* sky = GetSky(); sky && sky->HasActiveSettings())
        settings = sky->GetSettings();

    if (m_ProbeOverrides)
    {
        settings.iblIntensity = m_ProbeOverrides->Intensity;
        settings.exposureEV += m_ProbeOverrides->ExposureEV;
        settings.skyboxRotationRadians += m_ProbeOverrides->RotationRadians;
        settings.iblLowerHemisphereDarkness = m_ProbeOverrides->LowerHemisphereDarkness;
    }

    return settings;
}

float SkyEnvironmentSource::IblIntensity() const
{
    if (!HasActiveSky())
        return 0.0f;
    return GetEffectiveSettings().iblIntensity;
}

float SkyEnvironmentSource::LowerHemisphereDarkness() const
{
    if (!HasActiveSky())
        return 0.0f;
    return std::clamp(GetEffectiveSettings().iblLowerHemisphereDarkness, 0.0f, 1.0f);
}

void SkyEnvironmentSource::AmbientGradientTint(float outSky[3], float outEquator[3], float outGround[3]) const
{
    const SkySettings s = GetEffectiveSettings();
    for (int i = 0; i < 3; ++i)
    {
        outSky[i]     = s.ambientTintSky[i];
        outEquator[i] = s.ambientTintEquator[i];
        outGround[i]  = s.ambientTintGround[i];
    }
}

uint64_t SkyEnvironmentSource::InputDigest() const
{
    if (!HasActiveSky())
        return 0; // sentinel: no active sky -> never a "real" bake
    const SkySettings s = GetEffectiveSettings();

    if (s.environmentTexture.IsValid())
    {
        uint64_t h = 14695981039346656037ull;
        auto mix = [&h](const void* data, size_t bytes) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < bytes; ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        };

        const uint64_t textureId = static_cast<uint64_t>(s.environmentTexture);
        mix(&textureId, sizeof(textureId));
        mix(&s.hdriIntensity, sizeof(s.hdriIntensity));
        mix(&s.skyboxRotationRadians, sizeof(s.skyboxRotationRadians));
        mix(&s.exposureEV, sizeof(s.exposureEV));
        return h != 0 ? h : 1;
    }

    // FNV-1a over exactly the inputs the capture's baked sky depends on: the
    // sun + the atmosphere/composite block. Deliberately excludes per-frame
    // animated fields (skyTimeSeconds, stars, moon, time-of-day) so a
    // steady sky bakes once.
    //
    // The capture SAMPLES the atmospheric sky-view LUT, which is a function of
    // the sun + the full atmosphere block (see ComputeSkyViewLutInputDigest in
    // SkyRenderNode). Folding the same atmosphere fields here means the IBL
    // re-bakes exactly once whenever the sky-view LUT (and therefore the sky the
    // user sees) changes — sun motion, atmosphere edits, ground/night/horizon
    // tweaks — and never on a frame where the sky is steady. nightSkyBlend +
    // exposure are added because the capture's ground/night composite reads them.
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    // Gradient sky: the capture bakes ONLY the 3 gradient colors + exposure (its branch reads
    // nothing else), so hash exactly those and stop. Deliberately excludes the sun / atmosphere /
    // LUT below: an animated sun or time-of-day must NOT churn the digest and re-bake a
    // byte-identical gradient cube every frame (gradient mode even hides the ToD toggle). The
    // gradient colors already carry GradientSkyIntensity (the system folds it in), so the mode +
    // three colors + exposure fully determine the baked cube.
    if (s.skyMode == static_cast<uint32_t>(Components::SkyMode::Gradient))
    {
        mix(&s.skyMode, sizeof(s.skyMode));
        mix(s.gradientSkyTopColor, sizeof(s.gradientSkyTopColor));
        mix(s.gradientSkyHorizonColor, sizeof(s.gradientSkyHorizonColor));
        mix(s.gradientSkyBottomColor, sizeof(s.gradientSkyBottomColor));
        mix(&s.exposureEV, sizeof(s.exposureEV));
        return h != 0 ? h : 1;
    }

    mix(s.scatteringSunDir, sizeof(s.scatteringSunDir));
    mix(s.primarySunColor, sizeof(s.primarySunColor));
    mix(&s.primarySunIntensity, sizeof(s.primarySunIntensity));

    // The full atmosphere block the LUT + composite consume. Hashing the packed
    // GPU struct covers every ground/night/below-horizon/planet field in one shot
    // and stays in lockstep with what the capture actually uploads.
    AtmosphereParametersGPU atmo{};
    FillCaptureAtmosphere(s, atmo);
    mix(&atmo, sizeof(atmo));

    mix(&s.nightSkyBlend, sizeof(s.nightSkyBlend));
    mix(&s.exposureEV, sizeof(s.exposureEV));
    // showSunDisk is intentionally NOT mixed: the capture always excludes the sun
    // disk, so toggling it would only trigger an identical rebake.

    // Fold in the sky state the sky-view LUT actually CONTAINS: a capture
    // declared on the change frame can sample the not-yet-recomputed LUT (the
    // recompute may land a frame later, or in another window's render stream).
    // The LUT catching up moves this digest and fires the corrective rebake.
    const uint64_t lutDigest = MaterializedLutDigest();
    mix(&lutDigest, sizeof(lutDigest));
    return h != 0 ? h : 1; // never collide with the no-active-sky sentinel
}

bool SkyEnvironmentSource::EnsurePipelines(IDevice& device,
                                           const ImageBasedLightingFeature& feature)
{
    const uint32_t captureResolution = feature.GetEnvCaptureSize();
    const uint32_t prefilterMipCount = feature.GetPrefilterMipCount();
    if (m_PipelinesReady &&
        m_PipelineCaptureResolution == captureResolution &&
        m_PipelinePrefilterMipCount == prefilterMipCount)
        return true;

    std::vector<uint8_t> vs = Utils::LoadShaderFile("Shaders/fullscreen_noinput.vert.spv");
    std::vector<uint8_t> fsCube = Utils::LoadShaderFile("Shaders/sky_capture_cube.frag.spv");
    std::vector<uint8_t> csDownsample = Utils::LoadShaderFile("Shaders/sky_env_mip_downsample.comp.spv");
    std::vector<uint8_t> csDiffuse = Utils::LoadShaderFile("Shaders/sky_diffuse_convolve.comp.spv");
    std::vector<uint8_t> csSpecular = Utils::LoadShaderFile("Shaders/sky_specular_prefilter.comp.spv");
    if (vs.empty() || fsCube.empty() || csDownsample.empty() || csDiffuse.empty() || csSpecular.empty())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Log::Warning("SkyEnvironmentSource: missing bake shader SPIR-V; IBL will keep the ambient fallback");
        }
        return false;
    }

    const DescriptorSetLayoutId captureLayout = device.InternDescriptorSetLayout(CaptureDSD());
    const DescriptorSetLayoutId convolveLayout = device.InternDescriptorSetLayout(ImageBasedLightingFeature::ConvolveSet0());

    GraphicsPipelineDesc gd{};
    gd.Kind = GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(std::move(vs));
    gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(std::move(fsCube));
    gd.DescriptorSetLayouts.push_back(captureLayout);
    gd.Rasterization.cullMode = CullModeFlagBits::None;
    gd.DepthStencil.depthTestEnable = false;
    gd.DepthStencil.depthWriteEnable = false;
    DynamicStateInfo dyn{};
    dyn.states = {DynamicState::Viewport, DynamicState::Scissor};
    gd.DynamicState = dyn;
    gd.DebugName = "IBL_SkyCapture";
    m_CapturePipelineId = device.InternGraphicsPipeline(gd);

    // Env-cube box-downsample: same set-0 layout as the convolves (samplerCube +
    // storage image). The source mip is selected by binding a single-mip view —
    // no push constants.
    ComputePipelineDesc md{};
    md.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csDownsample));
    md.DescriptorSetLayouts.push_back(convolveLayout);
    md.DebugName = "IBL_EnvMipDownsample";
    m_MipDownsamplePipelineId = device.InternComputePipeline(md);

    ComputePipelineDesc dd{};
    dd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csDiffuse));
    dd.DescriptorSetLayouts.push_back(convolveLayout);
    dd.DebugName = "IBL_DiffuseConvolve";
    SpecializationConstants diffuseSpecConstants;
    diffuseSpecConstants.AddConstant<uint32_t>(0u, captureResolution, "kEnvCaptureSize");
    dd.Specialization = std::move(diffuseSpecConstants);
    m_DiffusePipelineId = device.InternComputePipeline(dd);

    ComputePipelineDesc sd{};
    sd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csSpecular));
    sd.DescriptorSetLayouts.push_back(convolveLayout);
    sd.DebugName = "IBL_SpecularPrefilter";
    // Single-source the prefilter base size + mip count: the shader reads these as
    // spec constants 0/1 for its roughness->mip mapping, so the C++ constants here
    // and the dispatch loop can never drift from the GLSL.
    SpecializationConstants specConstants;
    specConstants.AddConstant<uint32_t>(0u, feature.GetPrefilterSize(), "kSpecBaseSize");
    specConstants.AddConstant<uint32_t>(1u, prefilterMipCount, "kSpecMipCount");
    sd.Specialization = std::move(specConstants);
    m_SpecularPipelineId = device.InternComputePipeline(sd);

    m_PipelinesReady = m_CapturePipelineId.IsValid() && m_MipDownsamplePipelineId.IsValid() &&
                       m_DiffusePipelineId.IsValid() && m_SpecularPipelineId.IsValid();
    if (m_PipelinesReady)
    {
        m_PipelineCaptureResolution = captureResolution;
        m_PipelinePrefilterMipCount = prefilterMipCount;
    }
    return m_PipelinesReady;
}

SkyEnvironmentSource::CaptureFaceUploads SkyEnvironmentSource::AllocCaptureFaceUploads(
    RenderGraph::RGFrame& frame, uint32_t face) const
{
    CaptureFaceUploads out{};
    auto* sky = GetSky();
    if (!sky || !sky->HasActiveSettings())
        return out;
    const SkySettings settings = GetEffectiveSettings();

    auto skyAlloc = frame.AllocUpload<CaptureSkyUbo>();
    auto atmoAlloc = frame.AllocUpload<AtmosphereParametersGPU>();
    if (!skyAlloc.Valid() || !atmoAlloc.Valid())
        return out;
    FillCaptureUbo(settings, face, *skyAlloc.Ptr);
    FillCaptureAtmosphere(settings, *atmoAlloc.Ptr);

    out.SkyBuffer = skyAlloc.Buffer;
    out.SkyOffset = skyAlloc.Offset;
    out.AtmoBuffer = atmoAlloc.Buffer;
    out.AtmoOffset = atmoAlloc.Offset;
    return out;
}

void SkyEnvironmentSource::RecordCaptureFace(RenderGraph::RGContext& ctx,
                                             ImageBasedLightingFeature& feature,
                                             const CaptureFaceUploads& uploads) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return;
    auto* sky = GetSky();
    if (!sky || !sky->HasActiveSettings())
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_CapturePipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    const float size = static_cast<float>(feature.GetEnvCaptureSize());
    cl->SetViewport(0.0f, 0.0f, size, size);
    cl->SetScissor(0, 0, static_cast<int>(feature.GetEnvCaptureSize()),
                   static_cast<int>(feature.GetEnvCaptureSize()));

    const SkySettings settings = GetEffectiveSettings();
    SkyRenderer& renderer = sky->GetRenderer();

    // The UBO contents were written into the frame upload ring at declaration
    // (AllocCaptureFaceUploads); only the binds happen here.
    if (!uploads.Valid())
        return;

    DescriptorSetDesc dsd{};
    dsd.layout = CaptureDSD();
    dsd.transient = true;
    dsd.debugName = "IBL_Capture_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateBufferBinding(ds, 0, uploads.SkyBuffer, uploads.SkyOffset, sizeof(CaptureSkyUbo));
    // The source texture is either the active HDRI skybox or the atmospheric sky-view
    // LUT. The capture shader uses the same HDRI lat-long path as the on-screen sky
    // when an environment texture is present.
    const TextureHandle skySourceTexture = settings.environmentTexture.IsValid()
        ? settings.environmentTexture
        : renderer.GetSkyViewLutTexture();
    dev->UpdateCombinedImageSamplerBinding(ds, 1, skySourceTexture, renderer.GetSkyViewSampler());
    dev->UpdateBufferBinding(ds, 2, uploads.AtmoBuffer, uploads.AtmoOffset,
                             sizeof(AtmosphereParametersGPU));
    // Same texture AND same sampler the on-screen sky binds for this LUT — a sampler
    // mismatch here would show up as the baked bounce disagreeing with the visible
    // ground, which is exactly the divergence GE_CompositeGroundFloor exists to prevent.
    dev->UpdateCombinedImageSamplerBinding(ds, 3, renderer.GetTransmittanceLutTexture(),
                                           renderer.GetLinearSampler());
    cl->BindDescriptorSet(0, ds, pipe);
    cl->Draw(3, 1);
}

void SkyEnvironmentSource::RecordMipDownsample(RenderGraph::RGContext& ctx,
                                               ImageBasedLightingFeature& feature,
                                               uint32_t dstMip) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev || dstMip == 0)
        return;

    TextureViewHandle store = feature.GetEnvCaptureStoreView(dstMip);
    // Single-mip source view: the sampled descriptor's claim must cover ONLY
    // the RG-declared Read(mip dstMip-1) — the whole-chain view would claim
    // ShaderReadOnly for the GENERAL-resident destination mip too (VUID-00344).
    TextureViewHandle source = feature.GetEnvCaptureMipSampleView(dstMip - 1);
    if (!store.IsValid() || !source.IsValid())
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_MipDownsamplePipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsd{};
    dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
    dsd.transient = true;
    dsd.debugName = "IBL_EnvMipDownsample_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateCombinedImageSamplerBinding(ds, 0, source, feature.GetCubeSampler());
    dev->UpdateStorageImageBinding(ds, 1, store);
    cl->BindDescriptorSet(0, ds, pipe);

    const uint32_t size = feature.GetEnvCaptureSize() >> dstMip;
    const uint32_t g = ((size > 0 ? size : 1u) + kGroup - 1) / kGroup;
    cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
}

void SkyEnvironmentSource::RecordDiffuseConvolve(RenderGraph::RGContext& ctx,
                                                 ImageBasedLightingFeature& feature) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_DiffusePipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsd{};
    dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
    dsd.transient = true;
    dsd.debugName = "IBL_DiffuseConvolve_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateCombinedImageSamplerBinding(ds, 0, feature.GetEnvCaptureCubeView(), feature.GetCubeSampler());
    dev->UpdateStorageImageBinding(ds, 1, feature.GetIrradianceStoreView());
    cl->BindDescriptorSet(0, ds, pipe);

    const uint32_t g = (ImageBasedLightingFeature::kIrradianceSize + kGroup - 1) / kGroup;
    cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
}

void SkyEnvironmentSource::RecordSpecularPrefilter(RenderGraph::RGContext& ctx,
                                                   ImageBasedLightingFeature& feature,
                                                   uint64_t bakedDigest) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpecularPipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    // One dispatch per mip; the shader derives roughness from the bound mip's
    // dimension, so each mip binds its own View2DArray storage view.
    for (uint32_t mip = 0; mip < feature.GetPrefilterMipCount(); ++mip)
    {
        TextureViewHandle store = feature.GetPrefilterStoreView(mip);
        if (!store.IsValid())
            continue;

        DescriptorSetDesc dsd{};
        dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
        dsd.transient = true;
        dsd.debugName = "IBL_SpecularPrefilter_DS0";
        DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
        dev->UpdateCombinedImageSamplerBinding(ds, 0, feature.GetEnvCaptureCubeView(), feature.GetCubeSampler());
        dev->UpdateStorageImageBinding(ds, 1, store);
        cl->BindDescriptorSet(0, ds, pipe);

        const uint32_t size = feature.GetPrefilterSize() >> mip;
        const uint32_t g = (size + kGroup - 1) / kGroup;
        cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
    }

    // Last bake stage: record the digest the bake was scheduled FOR (captured at
    // declaration, not re-read here) so steady-state frames skip the rebake.
    feature.SetLastBakedDigest(bakedDigest);
    feature.ResetFramesSinceBake();
}

bool SkyEnvironmentSource::ScheduleCaptureFacesOnly(const EnvironmentBakeContext& ctx,
                                                    const char* passPrefix,
                                                    int32_t phase, uint32_t faceFilter)
{
    if (!ctx.Frame || !ctx.Feature || !m_Services)
        return false;
    auto* device = m_Services->GetDevice();
    if (!device || !EnsurePipelines(*device, *ctx.Feature) || !HasActiveSky())
        return false;

    RenderGraph::RGFrame& frame = *ctx.Frame;
    ImageBasedLightingFeature& feature = *ctx.Feature;
    const SkyEnvironmentSource* self = this;
    const char* prefix = (passPrefix && passPrefix[0] != '\0') ? passPrefix : "IBLGen.SkyCaptureOnly";

    const RenderGraph::RGTexture envCube = frame.ImportExternalTexture(
        "IBL_EnvCapture", feature.GetEnvCaptureTex(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, feature.GetEnvCaptureMipCount(),
        ImageBasedLightingFeature::kNumCaptureFaces);
    const RenderGraph::RGTexture skyView = frame.ImportExternalTexture(
        "Sky_View_LUT", GetSky()->GetRenderer().GetSkyViewLutTexture(),
        ResourceState::ShaderResource);
    // The capture's ground bounce samples the transmittance LUT. Dedup-by-handle lands
    // this on SkyRenderNode's id, so RG orders the captures after the transmittance
    // compute and inserts the StorageWrite->SampledRead barrier when it is recomputed.
    const RenderGraph::RGTexture transLut = frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", GetSky()->GetRenderer().GetTransmittanceLutTexture(),
        ResourceState::ShaderResource);

    for (uint32_t face = 0; face < ImageBasedLightingFeature::kNumCaptureFaces; ++face)
    {
        if (faceFilter != UINT32_MAX && face != faceFilter)
            continue;
        const std::string name = std::string(prefix) + ".Face" + std::to_string(face);
        frame.AddPass(
            name.c_str(), phase,
            [envCube, skyView, transLut, face](RenderGraph::RGPassBuilder& p) {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 0.0f;
                ops.Clear.Color[1] = 0.0f;
                ops.Clear.Color[2] = 0.0f;
                ops.Clear.Color[3] = 1.0f;
                RenderGraph::RGRange layer = RenderGraph::RGRange::All();
                layer.BaseMip = 0;
                layer.MipCount = 1;
                layer.BaseLayer = face;
                layer.LayerCount = 1;
                p.AttachColor(0, envCube, ops, layer);
                p.Read(skyView, RenderGraph::RGTextureRead::Sampled);
                p.Read(transLut, RenderGraph::RGTextureRead::Sampled);
                p.PreventCulling();
            },
            [self, feature = &feature,
             uploads = self->AllocCaptureFaceUploads(frame, face)](RenderGraph::RGContext& c) {
                self->RecordCaptureFace(c, *feature, uploads);
            });
    }

    return true;
}

void SkyEnvironmentSource::ScheduleBake(const EnvironmentBakeContext& ctx)
{
    if (!ctx.Frame || !ctx.Feature || !m_Services)
        return;
    auto* device = m_Services->GetDevice();
    if (!device || !EnsurePipelines(*device, *ctx.Feature))
        return;

    // Tick every frame the bake declaration runs so the throttle below can cap the
    // rebake rate during animated time-of-day (the digest changes every frame then).
    ctx.Feature->TickFramesSinceBake();

    // Shared gate AT DECLARATION (the activation-predicate's immediate-mode form):
    // re-bake only when the sky changed (a steady sky bakes once). Not declaring
    // is not running — RG culls the absent passes for free.
    if (!HasActiveSky())
        return;
    // Hash the bake inputs once per frame and reuse the value for the dirty gate and the
    // latched bakedDigest below -- InputDigest() re-evaluates the whole atmosphere block,
    // so computing it twice in one Declare is wasted work.
    const uint64_t digest = InputDigest();
    if (digest == ctx.Feature->GetLastBakedDigest())
        return;

    // The sky changed. During an animated day/night cycle the digest shifts every
    // frame (sub-degree sun drift + slowly-moving day-key colors), but the 20-pass
    // IBL bake is low-frequency ambient -- redoing it every frame is wasteful and a
    // few frames of lag is imperceptible. Cap to one rebake per interval; a static
    // sky's edit bakes at once (the counter is already saturated).
    if (ctx.Feature->GetFramesSinceBake() < ImageBasedLightingFeature::kIblRebakeIntervalFrames)
        return;

    RenderGraph::RGFrame& frame = *ctx.Frame;
    ImageBasedLightingFeature& feature = *ctx.Feature;
    const SkyEnvironmentSource* self = this;
    // The digest VALUE the bake is scheduled for (computed once above); the specular tail
    // latches exactly this (the exec runs in Execute(), after declaration — re-reading
    // InputDigest() there would re-evaluate live SkySettings).
    const uint64_t bakedDigest = digest;

    // Import each feature-owned cube ONCE per frame (dedup-by-handle fuses these
    // with the world-pass imports). Six layers / RGBA16F so per-face AttachColor
    // and the cross-face StorageWrite->Sampled barriers transition the right
    // subresources. The env cube rests at ShaderResource between bakes (the
    // convolves leave it Sampled); the per-face Clear establishes ColorAttachment.
    const RenderGraph::RGTexture envCube = frame.ImportExternalTexture(
        "IBL_EnvCapture", feature.GetEnvCaptureTex(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, feature.GetEnvCaptureMipCount(),
        ImageBasedLightingFeature::kNumCaptureFaces);
    const RenderGraph::RGTexture irrCube = frame.ImportExternalTexture(
        "IBL_Irradiance", feature.GetIrradianceCube(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, 1, ImageBasedLightingFeature::kNumCaptureFaces);
    const RenderGraph::RGTexture prefCube = frame.ImportExternalTexture(
        "IBL_Prefilter", feature.GetPrefilterCube(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, feature.GetPrefilterMipCount(),
        ImageBasedLightingFeature::kNumCaptureFaces);

    // Import the renderer's atmospheric sky-view LUT — the SAME texture SkyRenderNode
    // computes + the on-screen sky samples. Dedup-by-handle lands this on SkyRenderNode's
    // "Sky_View_LUT" id, so the face captures' Read(Sampled) connects to the sky-view
    // compute's Write(Storage): RG orders the captures AFTER the LUT compute and inserts
    // the StorageWrite->SampledRead barrier. On a steady-sky frame the LUT compute isn't
    // declared, the captures read the resting (ShaderResource) LUT, and nothing reorders.
    // ShaderResource is the LUT's resting state (the convolves/world pass leave it sampled).
    const RenderGraph::RGTexture skyView = frame.ImportExternalTexture(
        "Sky_View_LUT", GetSky()->GetRenderer().GetSkyViewLutTexture(),
        ResourceState::ShaderResource);
    // Same dedup-by-handle reasoning as the sky-view LUT above: the capture's ground
    // bounce samples the transmittance LUT, so declaring the read orders these passes
    // after the transmittance compute and inserts its barrier.
    const RenderGraph::RGTexture transLut = frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", GetSky()->GetRenderer().GetTransmittanceLutTexture(),
        ResourceState::ShaderResource);

    // Six sky face captures (one graphics pass per cube layer). Each samples the
    // atmospheric sky-view LUT + composites ground/night, clears + writes only its
    // own layer; no whole-cube init pass — RG derives the per-layer
    // Undefined->ColorAttachment transition from the first Clear.
    for (uint32_t face = 0; face < ImageBasedLightingFeature::kNumCaptureFaces; ++face)
    {
        const std::string name = "IBLGen.SkyCapture.Face" + std::to_string(face);
        frame.AddPass(
            name.c_str(), PassPhase::kEarlySetup,
            [envCube, skyView, transLut, face](RenderGraph::RGPassBuilder& p) {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = 0.0f;
                ops.Clear.Color[1] = 0.0f;
                ops.Clear.Color[2] = 0.0f;
                ops.Clear.Color[3] = 1.0f;
                // Attach exactly mip 0 of this face: the capture renders the base
                // mip only (the downsample fills 1..N-1). The cube now carries a
                // full mip chain, so MipCount must be 1 — a color attachment view
                // is a single level (a kRemaining MipCount would bind all mips).
                RenderGraph::RGRange layer = RenderGraph::RGRange::All();
                layer.BaseMip = 0;
                layer.MipCount = 1;
                layer.BaseLayer = face;
                layer.LayerCount = 1;
                p.AttachColor(0, envCube, ops, layer);
                // Sampling the atmospheric sky-view LUT: orders this capture after the
                // sky-view compute + inserts the StorageWrite->SampledRead barrier.
                p.Read(skyView, RenderGraph::RGTextureRead::Sampled);
                p.Read(transLut, RenderGraph::RGTextureRead::Sampled);
                // The env cube has NO world-side reader (only the convolves sample
                // it), so its producers have no in-graph consumer to anchor cull.
                p.PreventCulling();
            },
            [self, feature = &feature,
             uploads = self->AllocCaptureFaceUploads(frame, face)](RenderGraph::RGContext& c) {
                self->RecordCaptureFace(c, *feature, uploads);
            });
    }

    // Env-cube mip chain: box-downsample mip N from mip N-1 (one compute pass per
    // mip, N = 1 .. feature.GetEnvCaptureMipCount()-1). Per-mip subresource tracking orders
    // these after the face captures (which write mip 0) and before the convolves:
    // each pass Reads mip N-1 (Sampled) and Writes mip N (Storage) of the SAME cube,
    // so RG inserts the Write(mip 0)->Read(mip 0), then chained per-mip barriers,
    // and the convolves' whole-resource Read sees the finished pre-blurred chain.
    for (uint32_t mip = 1; mip < feature.GetEnvCaptureMipCount(); ++mip)
    {
        const std::string name = "IBLGen.EnvMipDownsample.Mip" + std::to_string(mip);
        frame.AddComputePass(
            name.c_str(), PassPhase::kEarlySetup,
            [envCube, mip](RenderGraph::RGPassBuilder& p) {
                RenderGraph::RGRange srcRange = RenderGraph::RGRange::All();
                srcRange.BaseMip = mip - 1;
                srcRange.MipCount = 1;
                RenderGraph::RGRange dstRange = RenderGraph::RGRange::All();
                dstRange.BaseMip = mip;
                dstRange.MipCount = 1;
                p.Read(envCube, RenderGraph::RGTextureRead::Sampled, srcRange);
                p.Write(envCube, RenderGraph::RGTextureWrite::Storage, dstRange);
                p.PreventCulling();
            },
            [self, feature = &feature, mip](RenderGraph::RGContext& c) {
                self->RecordMipDownsample(c, *feature, mip);
            });
    }

    // Diffuse irradiance convolution (env cube -> irradiance cube).
    frame.AddComputePass(
        "IBLGen.DiffuseConvolve", PassPhase::kEarlySetup,
        [envCube, irrCube](RenderGraph::RGPassBuilder& p) {
            p.Read(envCube, RenderGraph::RGTextureRead::Sampled);
            p.Write(irrCube, RenderGraph::RGTextureWrite::Storage);
            p.PreventCulling();
        },
        [self, feature = &feature](RenderGraph::RGContext& c) {
            self->RecordDiffuseConvolve(c, *feature);
        });

    // GGX specular prefilter (env cube -> prefilter cube mip chain). One pass; the
    // per-mip loop runs INSIDE exec, binding the feature's per-mip storage views.
    // The whole-resource Write is hazard-correct: the only read here is envCube (a
    // different physical), so the per-mip writes never self-hazard.
    frame.AddComputePass(
        "IBLGen.SpecularPrefilter", PassPhase::kEarlySetup,
        [envCube, prefCube](RenderGraph::RGPassBuilder& p) {
            p.Read(envCube, RenderGraph::RGTextureRead::Sampled);
            p.Write(prefCube, RenderGraph::RGTextureWrite::Storage);
            p.PreventCulling();
        },
        [self, feature = &feature, bakedDigest](RenderGraph::RGContext& c) {
            self->RecordSpecularPrefilter(c, *feature, bakedDigest);
        });

    // Resting-state contract: the convolves leave both cubes GENERAL on the
    // compute queue, and every import claims ResourceState::ShaderResource (the
    // ones above and the world pass's). The export contract restores
    // ShaderReadOnly at frame end — the sentinel transition records on the
    // queue that last touched the cubes, with derived submission edges — so the
    // claim stays TRUE on bake-only/empty-view frames. The graph-authoritative
    // oldLayout rule records import claims verbatim: a false claim parks the
    // cubes in GENERAL under sampling (VUID-09600 startup storm). The env cube
    // needs no contract (the convolves' sampled reads already leave it
    // ShaderReadOnly).
    frame.MarkOutput(irrCube, RenderGraph::RGImageLayout::ShaderReadOnly);
    frame.MarkOutput(prefCube, RenderGraph::RGImageLayout::ShaderReadOnly);
}

} // namespace Engine::Renderer
} // namespace GameEngine
