#include "Engine/Rendering/Pipeline/Nodes/SkyRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Components/Rendering/SkyEnvironment.h" // Components::SkyMode
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Sky/SkyRenderer.h"
#include "Rendering/Sky/SkySettings.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstring>
#include <numbers>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

static constexpr float kDefaultHalfFovDeg = 30.0f;
static constexpr float kDefaultAspect = 16.0f / 9.0f;
static constexpr uint32_t kFallbackViewportWidth = 1280;
static constexpr uint32_t kFallbackViewportHeight = 720;
static constexpr uint32_t kSkyComputeWorkgroupSize = 8;
static constexpr float kProjectionEpsilon = 1e-6f;
// sky_render.frag: starSizeRange.w >= 0 selects HDRI path (intensity may be 0 for black sky).
// Sentinel selects procedural/analytic sky so zero intensity is not confused with "no HDRI".
static constexpr float kSkyHdriProceduralSentinel = -1.0f;

// Scene Skybox with an equirectangular HDRI: the fragment shader samples the environment
// map directly by view direction. Procedural transmittance / sky-view LUT work is unused.
static bool IsHdriBackdropSky(const Rendering::SkySettings& settings)
{
    return settings.environmentTexture.IsValid();
}

// Gradient sky renders a stylized 3-color gradient instead of the atmosphere, so — like the
// HDRI backdrop — it needs neither the sky-view LUT compute nor the star billboards. HDRI wins
// if both somehow resolve (a gradient sky never sets an environment texture).
static bool IsGradientSky(const Rendering::SkySettings& settings)
{
    return settings.skyMode == static_cast<uint32_t>(Components::SkyMode::Gradient) &&
           !settings.environmentTexture.IsValid();
}

static constexpr uint32_t DivCeil(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static void FillAtmosphereFromSettings(const Rendering::SkySettings& settings, Rendering::AtmosphereParametersGPU& atmo)
{
    Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);
    atmo.groundAlbedo[0] = settings.groundAlbedo[0];
    atmo.groundAlbedo[1] = settings.groundAlbedo[1];
    atmo.groundAlbedo[2] = settings.groundAlbedo[2];
    atmo.groundBrightness = settings.groundBrightness;
    atmo.groundNightColor[0] = settings.groundNightColor[0];
    atmo.groundNightColor[1] = settings.groundNightColor[1];
    atmo.groundNightColor[2] = settings.groundNightColor[2];
    atmo.groundNightColorStd140Pad = 0.0f;
    atmo.groundHorizonColor[0] = settings.groundHorizonColor[0];
    atmo.groundHorizonColor[1] = settings.groundHorizonColor[1];
    atmo.groundHorizonColor[2] = settings.groundHorizonColor[2];
    atmo.groundHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonNightColor[0] = settings.groundHorizonNightColor[0];
    atmo.groundHorizonNightColor[1] = settings.groundHorizonNightColor[1];
    atmo.groundHorizonNightColor[2] = settings.groundHorizonNightColor[2];
    atmo.nightSkyHorizonColor[0] = settings.nightSkyHorizonColor[0];
    atmo.nightSkyHorizonColor[1] = settings.nightSkyHorizonColor[1];
    atmo.nightSkyHorizonColor[2] = settings.nightSkyHorizonColor[2];
    atmo.nightSkyHorizonColorStd140Pad = 0.0f;
    atmo.groundHorizonDayCosWidth = settings.groundHorizonCosWidth;
    atmo.groundHorizonNightCosWidth = settings.groundHorizonNightCosWidth;
    atmo.belowHorizonBlendSharpness = settings.belowHorizonBlendSharpness;
    atmo.belowHorizonDarkness = settings.belowHorizonDarkness;
    atmo.belowHorizonDarkColor[0] = settings.belowHorizonDarkColor[0];
    atmo.belowHorizonDarkColor[1] = settings.belowHorizonDarkColor[1];
    atmo.belowHorizonDarkColor[2] = settings.belowHorizonDarkColor[2];
    atmo.belowHorizonMode = settings.belowHorizonMode;
    atmo.groundHazeStrength = settings.groundHazeStrength;
}

// Extracts camera basis vectors and position from a column-major LH view matrix
// (glm::lookAtLH). The upper-left 3x3 rows are right/up/forward directly in LH;
// no sign flip is needed on forward.
static void ExtractCameraVectors(const Rendering::CameraData& cam,
                                 float outPos[3], float outFwd[3],
                                 float outUp[3], float outRight[3])
{
    const float* v = cam.view;
    outRight[0] = v[0]; outRight[1] = v[4]; outRight[2] = v[8];
    outUp[0]    = v[1]; outUp[1]    = v[5]; outUp[2]    = v[9];
    outFwd[0]   = v[2]; outFwd[1]   = v[6]; outFwd[2]   = v[10];

    outPos[0] = -(v[0] * v[12] + v[1] * v[13] + v[2]  * v[14]);
    outPos[1] = -(v[4] * v[12] + v[5] * v[13] + v[6]  * v[14]);
    outPos[2] = -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]);
}

// Perspective LH projections leave proj[15] (m[3][3]) at 0 because clip.w = view.z.
// Orthographic projections preserve w = 1, so proj[15] == 1 distinguishes the two.
static bool IsOrthographicProjection(const Rendering::CameraData& cam)
{
    return std::abs(cam.proj[15] - 1.0f) < kProjectionEpsilon;
}

// Returns 0.0 for orthographic projections as a sentinel; the sky/star shaders
// treat tanHalfFovY <= 0 as "render the 2D backdrop path" (vertical gradient
// for the sky, NDC-mapped star positions) instead of the perspective ray fan.
static float ExtractTanHalfFovY(const Rendering::CameraData& cam)
{
    if (IsOrthographicProjection(cam))
        return 0.0f;
    float p5 = cam.proj[5];
    if (std::abs(p5) < kProjectionEpsilon)
        return std::tan(kDefaultHalfFovDeg * std::numbers::pi_v<float> / 180.0f);
    return 1.0f / std::abs(p5);
}

static float ExtractAspect(const Rendering::CameraData& cam)
{
    float p0 = cam.proj[0];
    float p5 = cam.proj[5];
    if (std::abs(p0) < kProjectionEpsilon)
        return kDefaultAspect;
    return std::abs(p5) / std::abs(p0);
}

static constexpr float kBaseStarRadius    = 0.225f;
static constexpr float kBaseDiamondRadius = 0.5625f;
static constexpr float kBaseCellScale     = 256.0f; // Shader clamps to [64, 4096]
static constexpr float kTwinkleMinFreq    = 0.3f;
static constexpr float kTwinkleMaxFreq    = 1.5f;
static constexpr float kTwinklePhaseScale = 1.2f;
static constexpr float kHorizonFadeStart  = 0.004f;
static constexpr float kHorizonFadeWidth  = 0.02f;
static constexpr float kDefaultStarSizeMin = 0.5f;  // Shader clamps to [0.1, 4.0]
static constexpr float kDefaultStarSizeMax = 2.0f;  // Shader clamps to [0.1, 4.0]

// Populates SkyUBOGPU fields from settings. Camera and viewport fields
// are left at zero and must be filled by the caller for passes that need them.
static void FillSkyUBOFromSettings(const Rendering::SkySettings& settings, SkyUBOGPU& ubo)
{
    ubo.exposureEV = settings.exposureEV;
    ubo.pad0 = settings.sunSize2D;
    ubo.pad1 = settings.moonSize2D;
    ubo.skyPan2D = settings.skyPan2D;
    ubo.sunDirection[0] = settings.scatteringSunDir[0];
    ubo.sunDirection[1] = settings.scatteringSunDir[1];
    ubo.sunDirection[2] = settings.scatteringSunDir[2];
    // The one place the drawn sun size is decided: the physical radius scaled by the stylistic
    // multiplier, so sky_render.frag draws whatever it is given. Radius and brightness stay one
    // decision — the shader derives the disc's radiance from the solid angle the drawn radius
    // covers, so an oversized radius does not brighten the sun, it flattens it. A hidden sun
    // still uploads exactly 0: the shader treats that as "no disc", and sun_disc.glsl floors the
    // radius it divides by so the zero cannot reach SceneColor as a NaN.
    ubo.sunAngularRadius = settings.showSunDisk ? kSunAngularRadiusRad * settings.sunSize : 0.0f;
    ubo.moonDirection[0] = settings.moonDirWS[0];
    ubo.moonDirection[1] = settings.moonDirWS[1];
    ubo.moonDirection[2] = settings.moonDirWS[2];
    ubo.moonIntensity = settings.showMoonDisk ? settings.moonIntensity : 0.0f;
    ubo.moonAngularRadius = settings.moonAngularRadius;
    ubo.skyRotateAroundZenithRadians = 0.0f;
    ubo.moonUboPad1 = settings.moonPhase01;
    ubo.moonUboPad2 = settings.moonExposureEV;
    ubo.sunColor[0] = settings.primarySunColor[0];
    ubo.sunColor[1] = settings.primarySunColor[1];
    ubo.sunColor[2] = settings.primarySunColor[2];
    ubo.sunIntensity = settings.primarySunIntensity;
    ubo.timeOfDayHours = settings.timeOfDayHours;

    ubo.nightSkyBlend = settings.nightSkyBlend;
    ubo.skyTimeSeconds = settings.skyTimeSeconds;

    float sizeClamped = std::max(settings.starSize, 0.1f);
    ubo.starSizeShape[0] = kBaseCellScale / sizeClamped;
    ubo.starSizeShape[1] = kBaseStarRadius * sizeClamped;
    ubo.starSizeShape[2] = kBaseDiamondRadius * sizeClamped;
    ubo.starSizeShape[3] = settings.starDiamondShape;

    ubo.starTwinkleParams[0] = kTwinkleMinFreq * settings.twinkleSpeed;
    ubo.starTwinkleParams[1] = kTwinkleMaxFreq * settings.twinkleSpeed;
    ubo.starTwinkleParams[2] = kTwinklePhaseScale;
    ubo.starTwinkleParams[3] = settings.starGlowFalloff;

    ubo.starTwinkleAmp[0] = settings.twinkleIntensity;
    ubo.starTwinkleAmp[1] = 1.2f;
    ubo.starTwinkleAmp[2] = 0.0f; // Multi-cell flag: 0 = disabled
    ubo.starTwinkleAmp[3] = 0.0f; // vec4 padding

    ubo.starDensityHorizon[0] = settings.starDensity;
    ubo.starDensityHorizon[1] = kHorizonFadeStart;
    ubo.starDensityHorizon[2] = kHorizonFadeWidth;
    ubo.starDensityHorizon[3] = settings.starBrightness;

    ubo.starSizeRange[0] = kDefaultStarSizeMin;
    ubo.starSizeRange[1] = kDefaultStarSizeMax;
    ubo.starSizeRange[2] = settings.environmentTexture.IsValid() ? settings.skyboxRotationRadians : settings.starCoreSize;
    ubo.starSizeRange[3] = settings.environmentTexture.IsValid() ? settings.hdriIntensity : kSkyHdriProceduralSentinel;

    ubo.fallingStarParams[0] = settings.fallingStarsEnabled ? 1.0f : 0.0f;
    ubo.fallingStarParams[1] = settings.fallingStarAmount;
    ubo.fallingStarParams[2] = settings.fallingStarFrequency;
    ubo.fallingStarParams[3] = settings.fallingStarSpeed;
    ubo.fallingStarShapeParams[0] = settings.fallingStarLength;
    ubo.fallingStarShapeParams[1] = settings.fallingStarThickness;
    ubo.fallingStarShapeParams[2] = settings.fallingStarDotSize;
    ubo.fallingStarShapeParams[3] = settings.fallingStarDotSize2D;

    // Gradient sky: .rgb already scene-linear (intensity folded in by the system). The mode
    // sentinel rides gradientSkyTop.w: an HDRI backdrop wins (it is a separate component path),
    // so gradient is only active when there is no environment texture.
    const bool gradientMode =
        settings.skyMode == static_cast<uint32_t>(Components::SkyMode::Gradient) &&
        !settings.environmentTexture.IsValid();
    for (int i = 0; i < 3; ++i)
    {
        ubo.gradientSkyTop[i]     = settings.gradientSkyTopColor[i];
        ubo.gradientSkyHorizon[i] = settings.gradientSkyHorizonColor[i];
        ubo.gradientSkyBottom[i]  = settings.gradientSkyBottomColor[i];
    }
    ubo.gradientSkyTop[3]     = gradientMode ? 1.0f : -1.0f;
    ubo.gradientSkyHorizon[3] = 0.0f;
    ubo.gradientSkyBottom[3]  = 0.0f;
}

// Digest of the SkyViewLUT compute inputs. Used by the activation predicate
// to skip the dispatch when nothing the LUT depends on has changed. Saves
// ~0.026 ms / frame on static skies (the common editor-idle case).
//
// IMPORTANT: only hash the fields the sky_view_lut.comp shader actually
// reads. SkyEnvironmentSystem updates settings.skyTimeSeconds and several
// other fields every frame (for stars/falling-star animation in the
// sky_render.frag stage), but the LUT shader doesn't sample those — hashing
// them would force redundant LUT recomputes every frame and defeat the gate.
//
// LUT shader inputs (see sky_view_lut.comp main()):
//   - uSky.sunDirectionWS   (settings.scatteringSunDir)
//   - uSky.sunColor         (settings.primarySunColor)
//   - uSky.sunIntensity     (settings.primarySunIntensity)
//   - all uAtmos.* fields   (filled from settings via FillAtmosphereFromSettings)
//
// Note: uSky.cameraPositionWS is referenced by the shader but the LUT path
// in FillSkyUBOFromSettings leaves it at (0,0,0); the LUT is computed from
// the planet origin, not the camera. So we don't need to include camera state.
static uint64_t ComputeSkyViewLutInputDigest(const Rendering::SkySettings& settings)
{
    Rendering::AtmosphereParametersGPU atmo{};
    FillAtmosphereFromSettings(settings, atmo);

    // FNV-1a 64-bit. Stable across runs; sufficient quality for change detection.
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](const void* data, size_t bytes)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };

    // Atmosphere block in its entirety.
    mix(&atmo, sizeof(atmo));

    // Only the sun-related SkyUBO fields the LUT shader actually consumes.
    mix(settings.scatteringSunDir, sizeof(settings.scatteringSunDir));
    mix(settings.primarySunColor, sizeof(settings.primarySunColor));
    mix(&settings.primarySunIntensity, sizeof(settings.primarySunIntensity));

    return h;
}

void SkyRenderNode::EnsureDescriptorSetDescs(const Rendering::SkyRenderer::SkyBindingSlots& slots)
{
    // Cached once: the execute callbacks reuse these, never heap-allocating a
    // bindings vector per frame. Binding indices come from the renderer's
    // reflected slots (single source of truth with the pipeline layouts); the
    // descriptor types stay explicit.
    if (m_DescriptorSetDescsBuilt)
        return;
    m_DescriptorSetDescsBuilt = true;

    using Rendering::DescriptorType;
    constexpr uint32_t kCompute = Rendering::kShaderStageCompute;
    constexpr uint32_t kFragment = Rendering::kShaderStageFragment;
    constexpr uint32_t kVertFrag = Rendering::kShaderStageVertex | Rendering::kShaderStageFragment;

    // The storage texel format has to reach the layout: WebGPU bind group
    // layouts declare it and reject a view whose format differs, while
    // Vulkan and Metal carry it on the view and ignore the layout. Left at
    // 0 the backend assumes RGBA8Unorm and every LUT dispatch is refused.
    constexpr uint32_t kLutStorageFormat =
        static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);

    m_TransmittanceDSD.layout.bindings = {
        {slots.Transmittance.TransLUT, DescriptorType::StorageImage, 1, kCompute},
        {slots.Transmittance.Atmos, DescriptorType::UniformBuffer, 1, kCompute}
    };
        m_TransmittanceDSD.layout.bindings[0].storageTexelFormat = kLutStorageFormat;
    m_TransmittanceDSD.layout.debugName = "Sky.Transmittance";
    m_TransmittanceDSD.transient = true;
    m_TransmittanceDSD.debugName = "Sky.Transmittance.Set0";

    m_MultiscatterDSD.layout.bindings = {
        {slots.Multiscatter.MultiscatterLUT, DescriptorType::StorageImage, 1, kCompute},
        {slots.Multiscatter.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, kCompute},
        {slots.Multiscatter.Atmos, DescriptorType::UniformBuffer, 1, kCompute}
    };
        m_MultiscatterDSD.layout.bindings[0].storageTexelFormat = kLutStorageFormat;
    // The LUT chain filters rgba16f LUTs with linear samplers from compute.
    for (auto& b : m_MultiscatterDSD.layout.bindings)
        if (b.type == DescriptorType::CombinedImageSampler)
            b.imageFilterableFloat = true;
    m_MultiscatterDSD.layout.debugName = "Sky.MultiscatterLUT";
    m_MultiscatterDSD.transient = true;
    m_MultiscatterDSD.debugName = "Sky.MultiscatterLUT.Set0";

    m_SkyViewLutDSD.layout.bindings = {
        {slots.SkyViewLut.SkyViewLUT, DescriptorType::StorageImage, 1, kCompute},
        {slots.SkyViewLut.TransmittanceLUT, DescriptorType::CombinedImageSampler, 1, kCompute},
        {slots.SkyViewLut.Atmos, DescriptorType::UniformBuffer, 1, kCompute},
        {slots.SkyViewLut.Sky, DescriptorType::UniformBuffer, 1, kCompute},
        {slots.SkyViewLut.MultiscatterLUT, DescriptorType::CombinedImageSampler, 1, kCompute}
    };
        m_SkyViewLutDSD.layout.bindings[0].storageTexelFormat = kLutStorageFormat;
    for (auto& b : m_SkyViewLutDSD.layout.bindings)
        if (b.type == DescriptorType::CombinedImageSampler)
            b.imageFilterableFloat = true;
    m_SkyViewLutDSD.layout.debugName = "Sky.SkyViewLUT";
    m_SkyViewLutDSD.transient = true;
    m_SkyViewLutDSD.debugName = "Sky.SkyViewLUT.Set0";

    m_SkyRenderDSD.layout.bindings = {
        {slots.SkyRender.Sky, DescriptorType::UniformBuffer, 1, kFragment},
        {slots.SkyRender.SkyViewLUT, DescriptorType::CombinedImageSampler, 1, kFragment},
        {slots.SkyRender.Atmos, DescriptorType::UniformBuffer, 1, kFragment},
        {slots.SkyRender.TransLUT, DescriptorType::CombinedImageSampler, 1, kFragment},
        {slots.SkyRender.MoonFull, DescriptorType::CombinedImageSampler, 1, kFragment},
    };
    m_SkyRenderDSD.layout.debugName = "Sky.SkyRender";
    m_SkyRenderDSD.transient = true;
    m_SkyRenderDSD.debugName = "Sky.SkyRender.Set0";

    m_StarBillboardDSD.layout.bindings = {
        {slots.StarBillboard.Stars, DescriptorType::StorageBuffer, 1, kVertFrag},
        {slots.StarBillboard.Sky, DescriptorType::UniformBuffer, 1, kVertFrag}
    };
    m_StarBillboardDSD.layout.debugName = "Sky.StarBillboard";
    m_StarBillboardDSD.transient = true;
    m_StarBillboardDSD.debugName = "Sky.StarBillboard.Set0";
}

bool SkyRenderNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        const auto j = nlohmann::json::parse(m_Json);
        if (j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("SkyRenderNode JSON parse error: ") + e.what();
        return false;
    }

    return true;
}

void SkyRenderNode::RebuildPassNames(const std::string& pipelineName)
{
    m_CachedPipelineName = pipelineName;
    std::string prefix = "Pipeline." + pipelineName + "." + m_Id + ".";
    m_TransmittancePassName = prefix + "Transmittance";
    m_MultiscatterPassName = prefix + "MultiscatterLUT";
    m_SkyViewLutPassName = prefix + "SkyViewLUT";
}

Rendering::BufferHandle SkyRenderNode::GetOrCreatePerViewSkyUBO(
    Rendering::IDevice* dev, uint32_t viewKey, uint32_t slot)
{
    auto& entry = m_PerViewUBOs[viewKey];
    if (!entry.skyUBO[slot].IsValid())
    {
        Rendering::BufferDesc bd{};
        bd.size = sizeof(SkyUBOGPU);
        bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::Uniform);
        bd.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "Sky_PerView_SkyUBO";
        entry.skyUBO[slot] = dev->CreateBuffer(bd);
    }
    return entry.skyUBO[slot];
}

SkyRenderNode::~SkyRenderNode()
{
    if (m_Device)
    {
        for (auto& [viewKey, perView] : m_PerViewUBOs)
        {
            for (auto& buf : perView.skyUBO)
            {
                if (buf.IsValid())
                    m_Device->DestroyBuffer(buf);
                buf = {};
            }
        }
    }
    m_PerViewUBOs.clear();
}

void SkyRenderNode::Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx)
{
    auto& rs = ctx.Services;
    auto* device = rs.GetDevice();
    if (!device)
        return;
    m_Device = device;

    auto& sky = rs.EnsureFeature<SkyRenderFeature>();
    if (!sky.IsInitialized() && !sky.Initialize(device))
    {
        LOG_ERROR("SkyRenderNode: failed to initialize SkyRenderFeature");
        return;
    }

    // Inactive sky declares NOTHING — the Disable/Reenable/Invalidate dances
    // die here: the world arm re-decides Clear-vs-Load per frame from the
    // backdrop flag, which DeclareForView simply doesn't set.
    if (!sky.HasActiveSettings())
    {
        if (!m_WarnedNoComponent)
        {
            LOG_WARNING("SkyRender pass is active but no SkyEnvironment component found in scene.");
            m_WarnedNoComponent = true;
        }
        return;
    }
    m_WarnedNoComponent = false;

    const auto& settings = sky.GetSettings();
    if (IsHdriBackdropSky(settings) || IsGradientSky(settings))
        return; // backdrop-only: the per-view draw samples the env texture / gradient directly

    auto& renderer = sky.GetRenderer();
    EnsureDescriptorSetDescs(renderer.GetBindingSlots());
    const auto& pipelineName = instance.GetBlueprint().pipelineName;
    if (pipelineName != m_CachedPipelineName)
        RebuildPassNames(pipelineName);

    // LUT imports at their HONEST state: never-computed content is
    // discardable (Common → Undefined first transition); computed LUTs are
    // sampled-persisted across frames. Dedup-by-handle makes the per-view
    // draw's re-imports land on these same ids.
    const uint64_t digest = ComputeSkyViewLutInputDigest(settings);
    const bool transCurrent = renderer.IsTransmittanceComputed();
    const bool multiscatterCurrent = renderer.IsMultiscatterCurrentFor(digest);
    const bool skyViewCurrent = renderer.IsSkyViewLutCurrentFor(digest);
    const RenderGraph::RGTexture trans = ctx.Frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", renderer.GetTransmittanceLutTexture(),
        transCurrent ? Rendering::ResourceState::ShaderResource
                     : Rendering::ResourceState::Common);
    const RenderGraph::RGTexture multiscatter = ctx.Frame.ImportExternalTexture(
        "Sky_Multiscatter_LUT", renderer.GetMultiscatterLutTexture(),
        multiscatterCurrent ? Rendering::ResourceState::ShaderResource
                            : Rendering::ResourceState::Common);
    const RenderGraph::RGTexture skyView = ctx.Frame.ImportExternalTexture(
        "Sky_View_LUT", renderer.GetSkyViewLutTexture(),
        skyViewCurrent ? Rendering::ResourceState::ShaderResource
                       : Rendering::ResourceState::Common);

    // Dirty gates AT DECLARATION (the activation predicates' immediate-mode
    // form). Settings-only digests — never extend to camera/depth inputs.
    auto* rendererPtr = &renderer;
    auto* skyPtr = &sky;
    if (!transCurrent)
    {
        ctx.Frame.AddComputePass(
            m_TransmittancePassName.c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p) { p.Write(trans, RenderGraph::RGTextureWrite::Storage); },
            [this, rendererPtr](RenderGraph::RGContext& c)
            {
                auto* cl = c.Cmd;
                auto* dev = c.GetDevice();
                if (!cl || !dev)
                    return;

                Rendering::AtmosphereParametersGPU atmo{};
                Rendering::SkyRenderer::FillDefaultAtmosphere(atmo);

                const uint32_t slot = rendererPtr->GetFrameSlot(dev);
                Rendering::BufferHandle buf = rendererPtr->GetTransmittanceAtmoUBO(slot);
                void* data = dev->MapBuffer(buf);
                if (!data)
                    return;
                std::memcpy(data, &atmo, sizeof(atmo));
                dev->UnmapBuffer(buf);

                auto pipe = c.GetOrCreatePipelineVariant(rendererPtr->EnsureTransmittanceId(*dev));
                if (!pipe.IsValid())
                    return; // no latch: the dirty gate re-declares next frame
                const auto& slots = rendererPtr->GetBindingSlots().Transmittance;
                auto ds = dev->CreateDescriptorSet(m_TransmittanceDSD);
                dev->UpdateStorageImageBinding(ds, slots.TransLUT,
                                               rendererPtr->GetTransmittanceLutTexture());
                dev->UpdateBufferBinding(ds, slots.Atmos, buf, 0, sizeof(atmo));

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                const auto& cfg = rendererPtr->GetConfig();
                cl->Dispatch(DivCeil(cfg.transmittanceLutWidth, kSkyComputeWorkgroupSize),
                             DivCeil(cfg.transmittanceLutHeight, kSkyComputeWorkgroupSize), 1);
                rendererPtr->MarkTransmittanceComputed();
            });
    }
    if (!multiscatterCurrent)
    {
        ctx.Frame.AddComputePass(
            m_MultiscatterPassName.c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                // Read the transmittance LUT (Sampled) and write the multiscatter
                // LUT (Storage). These access edges make RG order this pass after
                // transmittance and before sky-view (which reads multiscatter).
                p.Read(trans, RenderGraph::RGTextureRead::Sampled);
                p.Write(multiscatter, RenderGraph::RGTextureWrite::Storage);
            },
            [this, rendererPtr, skyPtr](RenderGraph::RGContext& c)
            {
                auto* cl = c.Cmd;
                auto* dev = c.GetDevice();
                if (!cl || !dev)
                    return;

                Rendering::AtmosphereParametersGPU atmo{};
                FillAtmosphereFromSettings(skyPtr->GetSettings(), atmo);

                const uint32_t slot = rendererPtr->GetFrameSlot(dev);
                Rendering::BufferHandle bAtmo = rendererPtr->GetMultiscatterAtmoUBO(slot);
                void* pAtmo = dev->MapBuffer(bAtmo);
                if (!pAtmo)
                    return;
                std::memcpy(pAtmo, &atmo, sizeof(atmo));
                dev->UnmapBuffer(bAtmo);

                auto pipe = c.GetOrCreatePipelineVariant(rendererPtr->EnsureMultiscatterId(*dev));
                if (!pipe.IsValid())
                    return; // no latch: the dirty gate re-declares next frame
                const auto& slots = rendererPtr->GetBindingSlots().Multiscatter;
                auto ds = dev->CreateDescriptorSet(m_MultiscatterDSD);
                dev->UpdateStorageImageBinding(ds, slots.MultiscatterLUT,
                                               rendererPtr->GetMultiscatterLutTexture());
                dev->UpdateCombinedImageSamplerBinding(ds, slots.TransmittanceLUT,
                                                       rendererPtr->GetTransmittanceLutTexture(),
                                                       rendererPtr->GetLinearSampler());
                dev->UpdateBufferBinding(ds, slots.Atmos, bAtmo, 0, sizeof(atmo));

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                const auto& cfg = rendererPtr->GetConfig();
                cl->Dispatch(DivCeil(cfg.multiscatterLutSize, kSkyComputeWorkgroupSize),
                             DivCeil(cfg.multiscatterLutSize, kSkyComputeWorkgroupSize), 1);
                rendererPtr->MarkMultiscatterComputed(
                    ComputeSkyViewLutInputDigest(skyPtr->GetSettings()));
            });
    }
    if (!skyViewCurrent)
    {
        ctx.Frame.AddComputePass(
            m_SkyViewLutPassName.c_str(), Rendering::PassPhase::kEarlySetup,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(trans, RenderGraph::RGTextureRead::Sampled);
                p.Read(multiscatter, RenderGraph::RGTextureRead::Sampled);
                p.Write(skyView, RenderGraph::RGTextureWrite::Storage);
            },
            [this, rendererPtr, skyPtr](RenderGraph::RGContext& c)
            {
                auto* cl = c.Cmd;
                auto* dev = c.GetDevice();
                if (!cl || !dev)
                    return;

                const auto& s = skyPtr->GetSettings();
                SkyUBOGPU skyUbo{};
                FillSkyUBOFromSettings(s, skyUbo);
                Rendering::AtmosphereParametersGPU atmo{};
                FillAtmosphereFromSettings(s, atmo);

                const uint32_t slot = rendererPtr->GetFrameSlot(dev);
                Rendering::BufferHandle bSky = rendererPtr->GetSkyViewLutSkyUBO(slot);
                void* pSky = dev->MapBuffer(bSky);
                if (!pSky)
                    return;
                std::memcpy(pSky, &skyUbo, sizeof(skyUbo));
                dev->UnmapBuffer(bSky);
                Rendering::BufferHandle bAtmo = rendererPtr->GetSkyViewLutAtmoUBO(slot);
                void* pAtmo = dev->MapBuffer(bAtmo);
                if (!pAtmo)
                    return;
                std::memcpy(pAtmo, &atmo, sizeof(atmo));
                dev->UnmapBuffer(bAtmo);

                auto pipe = c.GetOrCreatePipelineVariant(rendererPtr->EnsureSkyViewLutId(*dev));
                if (!pipe.IsValid())
                    return; // no latch: the dirty gate re-declares next frame
                const auto& slots = rendererPtr->GetBindingSlots().SkyViewLut;
                auto ds = dev->CreateDescriptorSet(m_SkyViewLutDSD);
                dev->UpdateStorageImageBinding(ds, slots.SkyViewLUT,
                                               rendererPtr->GetSkyViewLutTexture());
                dev->UpdateCombinedImageSamplerBinding(ds, slots.TransmittanceLUT,
                                                       rendererPtr->GetTransmittanceLutTexture(),
                                                       rendererPtr->GetLinearSampler());
                dev->UpdateBufferBinding(ds, slots.Atmos, bAtmo, 0, sizeof(atmo));
                dev->UpdateBufferBinding(ds, slots.Sky, bSky, 0, sizeof(skyUbo));
                dev->UpdateCombinedImageSamplerBinding(ds, slots.MultiscatterLUT,
                                                       rendererPtr->GetMultiscatterLutTexture(),
                                                       rendererPtr->GetLinearSampler());

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                const auto& cfg = rendererPtr->GetConfig();
                cl->Dispatch(DivCeil(cfg.skyViewLutWidth, kSkyComputeWorkgroupSize),
                             DivCeil(cfg.skyViewLutHeight, kSkyComputeWorkgroupSize), 1);
                rendererPtr->MarkSkyViewLutComputed(
                    ComputeSkyViewLutInputDigest(skyPtr->GetSettings()));
            });
    }
}

void SkyRenderNode::DeclareForView(ViewDeclare& d)
{
    auto& rs = d.Services;
    auto* skyFeat = rs.GetFeature<SkyRenderFeature>();
    if (!skyFeat || !skyFeat->IsInitialized() || !skyFeat->HasActiveSettings())
        return;
    auto& sky = *skyFeat;
    auto& renderer = sky.GetRenderer();
    EnsureDescriptorSetDescs(renderer.GetBindingSlots());
    const Rendering::ViewId viewId = d.View.id;
    const uint32_t viewKey = static_cast<uint32_t>(viewId);

    const RenderGraph::RGTexture out = d.ResolveTexture(m_OutputRef);
    if (!out.IsValid())
        return;

    // AT DECLARATION: the world arm (later in blueprint order) reads this
    // when deciding Clear-vs-Load for its color attach.
    rs.Views().MarkViewSkyBackdropScheduled(viewId);

    const auto& settings = sky.GetSettings();
    const Rendering::TextureHandle skySourceTex = settings.environmentTexture.IsValid()
                                                      ? settings.environmentTexture
                                                      : renderer.GetSkyViewLutTexture();
    // Dedup-by-handle lands these on the frame-scope Declare's ids when the
    // LUT path is active; the HDRI env texture imports fresh as sampled.
    const RenderGraph::RGTexture skySource = d.Frame.ImportExternalTexture(
        "Sky_Backdrop_Source", skySourceTex, Rendering::ResourceState::ShaderResource);
    const RenderGraph::RGTexture trans = d.Frame.ImportExternalTexture(
        "Sky_Transmittance_LUT", renderer.GetTransmittanceLutTexture(),
        renderer.IsTransmittanceComputed() ? Rendering::ResourceState::ShaderResource
                                           : Rendering::ResourceState::Common);

    // Viewport snapshot at declaration (the old exec's GetTextureSize +
    // fallback chain dies).
    uint32_t vpX = 0, vpY = 0;
    uint32_t vpW = d.RenderWidth, vpH = d.RenderHeight;
    const ViewLetterbox letterbox = rs.Views().GetViewLetterbox(viewId);
    if (letterbox.active)
    {
        vpX = letterbox.x;
        vpY = letterbox.y;
        vpW = letterbox.width;
        vpH = letterbox.height;
    }

    auto* rendererPtr = &renderer;
    auto* skyPtr = &sky;
    auto* rsPtr = &rs;
    d.Frame.AddPass(
        d.PassName().c_str(), Rendering::PassPhase::kSkyRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Clear;
            ops.Store = RenderGraph::RGStoreOp::Store;
            ops.Clear.Color[3] = 1.0f;
            p.AttachColor(0, out, ops);
            p.Read(skySource, RenderGraph::RGTextureRead::Sampled);
            p.Read(trans, RenderGraph::RGTextureRead::Sampled);
        },
        [this, rendererPtr, skyPtr, rsPtr, viewId, viewKey, vpX, vpY, vpW,
         vpH](RenderGraph::RGContext& passCtx)
        {
            auto* cl = passCtx.Cmd;
            auto* dev = passCtx.GetDevice();
            if (!cl || !dev)
                return;

            Rendering::CameraId cameraId{0};
            for (const auto& v : rsPtr->Views().GetViews())
            {
                if (v.id == viewId)
                {
                    cameraId = v.cameraId;
                    break;
                }
            }
            const auto* camData = rsPtr->Views().FindCameraData(cameraId);
            if (!camData)
            {
                LOG_WARNING("SkyRenderNode: no camera data for viewId={} cameraId={}",
                            (uint32_t)viewId, (uint32_t)cameraId);
                return;
            }

            float camPos[3], camFwd[3], camUp[3], camRight[3];
            ExtractCameraVectors(*camData, camPos, camFwd, camUp, camRight);
            const float tanHalfFovY = ExtractTanHalfFovY(*camData);
            const float aspect = ExtractAspect(*camData);

            const float vw = static_cast<float>(std::max(vpW, 1u));
            const float vh = static_cast<float>(std::max(vpH, 1u));

            const auto& settings = skyPtr->GetSettings();
            SkyUBOGPU skyUbo{};
            FillSkyUBOFromSettings(settings, skyUbo);
            skyUbo.cameraPosition[0] = camPos[0];
            skyUbo.cameraPosition[1] = camPos[1];
            skyUbo.cameraPosition[2] = camPos[2];
            skyUbo.cameraRight[0] = camRight[0];
            skyUbo.cameraRight[1] = camRight[1];
            skyUbo.cameraRight[2] = camRight[2];
            skyUbo.cameraUp[0] = camUp[0];
            skyUbo.cameraUp[1] = camUp[1];
            skyUbo.cameraUp[2] = camUp[2];
            skyUbo.cameraForward[0] = camFwd[0];
            skyUbo.cameraForward[1] = camFwd[1];
            skyUbo.cameraForward[2] = camFwd[2];
            skyUbo.viewportAspect = (vh > 0.0f) ? (vw / vh) : aspect;
            skyUbo.tanHalfFovY = tanHalfFovY;
            skyUbo.skyRotateAroundZenithRadians =
                (tanHalfFovY <= 0.0f && camData->cameraPos[3] > 0.5f) ? 1.0f : 0.0f;
            skyUbo.viewportResolution[0] = vw;
            skyUbo.viewportResolution[1] = vh;

            const uint32_t slot = rendererPtr->GetFrameSlot(dev);
            Rendering::BufferHandle bSky = GetOrCreatePerViewSkyUBO(dev, viewKey, slot);
            void* pSky = dev->MapBuffer(bSky);
            if (!pSky)
                return;
            std::memcpy(pSky, &skyUbo, sizeof(skyUbo));
            dev->UnmapBuffer(bSky);

            Rendering::AtmosphereParametersGPU atmo{};
            FillAtmosphereFromSettings(settings, atmo);
            Rendering::BufferHandle bAtmo = rendererPtr->GetRenderAtmoUBO(slot);
            void* pAtmo = dev->MapBuffer(bAtmo);
            if (!pAtmo)
                return;
            std::memcpy(pAtmo, &atmo, sizeof(atmo));
            dev->UnmapBuffer(bAtmo);

            auto pipeVar = passCtx.GetOrCreatePipelineVariant(rendererPtr->EnsureSkyRenderId(*dev));
            if (!pipeVar.IsValid())
                return;
            cl->SetPipeline(pipeVar);
            cl->SetViewport(static_cast<float>(vpX), static_cast<float>(vpY), vw, vh);
            cl->SetScissor(vpX, vpY, vpW, vpH);

            const auto& slots = rendererPtr->GetBindingSlots().SkyRender;
            auto ds = dev->CreateDescriptorSet(m_SkyRenderDSD);
            dev->UpdateBufferBinding(ds, slots.Sky, bSky, 0, sizeof(skyUbo));
            dev->UpdateCombinedImageSamplerBinding(
                ds, slots.SkyViewLUT,
                settings.environmentTexture.IsValid() ? settings.environmentTexture
                                                      : rendererPtr->GetSkyViewLutTexture(),
                rendererPtr->GetSkyViewSampler());
            dev->UpdateBufferBinding(ds, slots.Atmos, bAtmo, 0, sizeof(atmo));
            dev->UpdateCombinedImageSamplerBinding(ds, slots.TransLUT,
                                                   rendererPtr->GetTransmittanceLutTexture(),
                                                   rendererPtr->GetLinearSampler());
            dev->UpdateCombinedImageSamplerBinding(ds, slots.MoonFull,
                                                   rendererPtr->EnsureMoonFullTexture(),
                                                   rendererPtr->GetLinearSampler());

            cl->BindDescriptorSet(0, ds, pipeVar);
            cl->Draw(3, 1);

            if (IsHdriBackdropSky(settings) || IsGradientSky(settings))
                return;

            const float nightBlend = settings.nightSkyBlend;
            const uint32_t starCount = rendererPtr->GetStarInstanceCount(settings.starDensity);
            if (nightBlend > 0.0f && starCount > 0 && rendererPtr->GetStarSSBO().IsValid())
            {
                auto starPipe =
                    passCtx.GetOrCreatePipelineVariant(rendererPtr->EnsureStarBillboardId(*dev));
                if (!starPipe.IsValid())
                    return;
                cl->SetPipeline(starPipe);
                const auto& starSlots = rendererPtr->GetBindingSlots().StarBillboard;
                auto starDS = dev->CreateDescriptorSet(m_StarBillboardDSD);
                dev->UpdateStorageBufferBinding(starDS, starSlots.Stars, rendererPtr->GetStarSSBO(), 0,
                                                starCount * sizeof(Rendering::StarInstanceGPU));
                dev->UpdateBufferBinding(starDS, starSlots.Sky, bSky, 0, sizeof(skyUbo));
                cl->BindDescriptorSet(0, starDS, starPipe);
                cl->Draw(6, starCount);
            }
        });
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
