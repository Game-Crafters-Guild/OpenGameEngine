#include "Ocean/OceanUnderwater.h"
#include "Ocean/OceanFootprintFrustum.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Ocean/OceanTypes.h"
#include "OceanShaderDirectory.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kUnderwaterFragmentStage = kShaderStageFragment;

// std140 mirror of OceanUnderwaterParams in ocean_underwater.frag. Filled CPU-
// side each frame; the shader reads it at set 0 binding 2.
struct alignas(16) OceanUnderwaterParamsGPU
{
    float InvViewProj[16]{};
    float ViewProj[16]{};    // world -> clip (god-ray sun projection)
    float View[16]{};        // world -> view (shadow cascade selection)
    float CameraPos[4]{};    // xyz world camera; w unused
    float FogDensity[4]{};   // rgb extinction; w = max fog distance (m)
    float FogFalloff[4]{};   // x start, y end, z power, w mode
    float DeepTint[4]{};     // rgb deep body colour; a unused
    float ShallowTint[4]{};  // rgb shallow scatter; a = meniscus brightness
    float Misc[4]{};         // x seaLevel, y meniscusWidth, z submergedDepth, w time
    float SunDirection[4]{}; // xyz toward the sun
    float SunColor[4]{};     // rgb light colour * intensity
    float FFTParams[4]{};    // x legacy waveMode, y fftCascadeCount, z shapeWeight, w maxVertical
    float LocalFFT[4]{}; // x = ready local FFT stream bit mask, yzw pad
    float LocalFFTMaskCascadeOriginScale[kMaxOceanLodCascades * 4]{};
    float LocalFFTMaskCascadeMeta[4]{}; // x lod count
    float Shape[4]{};        // x maxHorizontal, y iorWater, z probeReach, w waterlineEdge
    float Inscatter[4]{};    // x enable, y strength, z HG phase g, w camera-depth falloff
    float Distortion[4]{};   // x enable, y lensK1, z lensK2, w intersection strength
    float GodRays[4]{};      // x mode, y strength, z density, w secondary density
    float ReflectedCaustics[4]{}; // x strength, y height, z falloff, w pad
    float Caustics[4]{};     // scale, average, strength, focal depth
    float CausticsFocus[4]{}; // depth of field, distortion strength/scale, pad
    float Gerstner[4]{};     // legacy ABI pad
    float Portal[4]{};       // x enabled, y volume count, z exclusion count, w polygon count
    float PortalExtra[4]{};  // x occluder count, y ribbon count, z 1 = dry camera (portal-only), w pad
    float PortalVolumeBoxes[kMaxOceanUnderwaterPortalVolumes * 8]{};       // min/max vec4 pairs
    float PortalExclusionBoxes[kMaxOceanUnderwaterPortalExclusions * 8]{}; // min/max vec4 pairs
    float PortalOccluderBoxes[kMaxOceanUnderwaterPortalOccluders * 8]{};   // min/max vec4 pairs
    float PortalPolygons[kMaxOceanUnderwaterPortalPolygons *
                         (1u + kMaxOceanUnderwaterPortalPolygonPoints) * 4u]{}; // meta + point vec4s
    float Waves[128]{};      // legacy ABI pad
};
static_assert(sizeof(OceanUnderwaterParamsGPU) == 2368,
              "OceanUnderwaterParamsGPU must match the std140 UBO in ocean_underwater.frag");

// Never-read dummy bindings: a 1x1 zero texture, array-viewed for the FFT
// cascade slots and plain 2D for the caustics slot.
TextureHandle CreateZeroTexture(IDevice& device, const char* debugName, bool forceArrayView)
{
    TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.arrayLayers = 1;
    td.mipLevels = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    if (forceArrayView)
        td.flags = TextureCreateFlags::ForceArrayView;
    td.initialState = ResourceState::Undefined;
    td.debugName = debugName;

    TextureHandle texture = device.CreateTexture(td);
    if (!texture.IsValid())
        return {};

    const uint16_t zeroHalfRgba[4] = {};
    UploadTexture2D(&device, texture, zeroHalfRgba, 1, 1, sizeof(zeroHalfRgba),
                    "Ocean_Dummy_Upload");
    return texture;
}
} // namespace

OceanUnderwater::~OceanUnderwater()
{
    if (!m_Device)
        return;
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
    if (m_DummyArrayTexture.IsValid())
        m_Device->DestroyTexture(m_DummyArrayTexture);
    if (m_DummyTexture2D.IsValid())
        m_Device->DestroyTexture(m_DummyTexture2D);
}

bool OceanUnderwater::EnsurePipeline(IDevice& device)
{
    if (!m_PipelineLoadAttempted)
    {
        m_PipelineLoadAttempted = true; // one attempt: missing sources decline the overlay

        // --- Overlay program: runtime-compiled vs+fs from the Ocean shader dir ---
        const fs::path shaderDir = OceanShaderDirectory("ocean_underwater.frag");
        if (!shaderDir.empty())
        {
            const fs::path includeDir = shaderDir.parent_path(); // for "Ocean/..." includes
            const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

            ShaderProgramCompileRequest req{};
            req.debugName = "ocean_underwater";
            req.baseDirectory = shaderDir;
            req.cacheRoot = cacheRoot;
            req.includeDirs = {includeDir};
            req.stages = {{"vs", "ocean_fullscreen.vert", "main", {}},
                          {"fs", "ocean_underwater.frag", "main", {}}};

            ShaderProgramCompileResult result{};
            std::string err;
            if (LoadOceanShaderProgram(req, device.PreferredShaderSource(), result, &err))
            {
                auto vs = result.stageBytes.find("vs");
                auto fs = result.stageBytes.find("fs");
                if (vs != result.stageBytes.end() && fs != result.stageBytes.end() &&
                    !vs->second.empty() && !fs->second.empty())
                {
                    m_Layout.debugName = "Ocean.Underwater.Set0";
                    m_Layout.bindings = {
                        {0, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {1, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {2, DescriptorType::UniformBuffer, 1, kUnderwaterFragmentStage},
                        {3, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {4, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {5, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {6, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {7, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {10, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {11, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {12, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {13, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {14, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {15, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {16, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {17, DescriptorType::StorageBuffer, 1, kUnderwaterFragmentStage},
                        {18, DescriptorType::StorageBuffer, 1, kUnderwaterFragmentStage}};
                    m_Pipeline = RenderGraph::MakeFullscreenPipelineDesc("Ocean.Underwater");
                    m_Pipeline.vertexShader = vs->second;
                    m_Pipeline.pixelShader = fs->second;
                    Rendering::ApplyMetaImageShapeToLayout(result.meta, m_Layout);
                    m_Pipeline.descriptorSetLayouts.push_back(m_Layout);
                }
            }
            else if (!m_WarnedLoadFailed)
            {
                m_WarnedLoadFailed = true;
                Logger::Log::Warning("OceanUnderwater: overlay compile failed: {}", err);
            }

            ShaderProgramCompileRequest shadowReq{};
            shadowReq.debugName = "ocean_underwater_shadowed";
            shadowReq.baseDirectory = shaderDir;
            shadowReq.cacheRoot = cacheRoot;
            shadowReq.includeDirs = {includeDir};
            shadowReq.stages = {{"vs", "ocean_fullscreen.vert", "main", {}},
                                {"fs", "ocean_underwater.frag", "main",
                                 {"OCEAN_UNDERWATER_SHADOWS"}}};

            ShaderProgramCompileResult shadowResult{};
            std::string shadowErr;
            if (LoadOceanShaderProgram(shadowReq, device.PreferredShaderSource(), shadowResult, &shadowErr))
            {
                auto vs = shadowResult.stageBytes.find("vs");
                auto fs = shadowResult.stageBytes.find("fs");
                if (vs != shadowResult.stageBytes.end() &&
                    fs != shadowResult.stageBytes.end() && !vs->second.empty() &&
                    !fs->second.empty())
                {
                    m_ShadowLayout.debugName = "Ocean.Underwater.Shadowed.Set0";
                    m_ShadowLayout.bindings = {
                        {0, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {1, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {2, DescriptorType::UniformBuffer, 1, kUnderwaterFragmentStage},
                        {3, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {4, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {5, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {6, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {7, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {8, DescriptorType::UniformBuffer, 1, kUnderwaterFragmentStage},
                        {9, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {10, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {11, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {12, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {13, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {14, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {15, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {16, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage},
                        {17, DescriptorType::StorageBuffer, 1, kUnderwaterFragmentStage},
                        {18, DescriptorType::StorageBuffer, 1, kUnderwaterFragmentStage}};
                    m_ShadowPipeline =
                        RenderGraph::MakeFullscreenPipelineDesc("Ocean.Underwater.Shadowed");
                    m_ShadowPipeline.vertexShader = vs->second;
                    m_ShadowPipeline.pixelShader = fs->second;
                    Rendering::ApplyMetaImageShapeToLayout(shadowResult.meta, m_ShadowLayout);
                    m_ShadowPipeline.descriptorSetLayouts.push_back(m_ShadowLayout);
                }
            }
            else
            {
                Logger::Log::Warning("OceanUnderwater: shadowed overlay compile failed: {}",
                                     shadowErr);
            }
        }
        if (m_Pipeline.vertexShader.empty() && !m_WarnedLoadFailed)
        {
            m_WarnedLoadFailed = true;
            Logger::Log::Warning(
                "OceanUnderwater: overlay sources unavailable; underwater declines.");
        }

        // --- Scene-copy program: the stock fullscreen copy (samples set 0
        // binding 0 -> outputs it). Snapshots SceneColor so the overlay can read
        // the scene while writing back into the same target. ---
        RenderGraph::LoadCopyPipelineDesc(device.PreferredShaderSource(), "Ocean.Underwater.Copy",
                                          "Ocean.Underwater.Copy.Set0", m_CopyLayout,
                                          m_CopyPipeline);

        // --- MSAA resolve program (sampler2DMS averaging): used for the scene copy
        // when SceneColor is multisampled, so the overlay works with MSAA on. ---
        if (!shaderDir.empty())
        {
            ShaderProgramCompileRequest rreq{};
            rreq.debugName = "ocean_underwater_resolve";
            rreq.baseDirectory = shaderDir;
            rreq.cacheRoot = fs::path(".Cache") / "Shaders";
            rreq.includeDirs = {shaderDir.parent_path()};
            rreq.stages = {{"vs", "ocean_fullscreen.vert", "main", {}},
                           {"fs", "ocean_resolve.frag", "main", {}}};
            ShaderProgramCompileResult rres{};
            std::string rerr;
            if (LoadOceanShaderProgram(rreq, device.PreferredShaderSource(), rres, &rerr))
            {
                auto rvs = rres.stageBytes.find("vs");
                auto rfs = rres.stageBytes.find("fs");
                if (rvs != rres.stageBytes.end() && rfs != rres.stageBytes.end() &&
                    !rvs->second.empty() && !rfs->second.empty())
                {
                    m_ResolveLayout.debugName = "Ocean.Underwater.Resolve.Set0";
                    m_ResolveLayout.bindings = {
                        {0, DescriptorType::CombinedImageSampler, 1, kUnderwaterFragmentStage}};
                    m_ResolvePipeline =
                        RenderGraph::MakeFullscreenPipelineDesc("Ocean.Underwater.Resolve");
                    m_ResolvePipeline.vertexShader = rvs->second;
                    m_ResolvePipeline.pixelShader = rfs->second;
                    Rendering::ApplyMetaImageShapeToLayout(rres.meta, m_ResolveLayout);
                    m_ResolvePipeline.descriptorSetLayouts.push_back(m_ResolveLayout);
                }
            }
        }
    }
    if (!m_PipelineId.IsValid() && !m_Pipeline.vertexShader.empty())
        m_PipelineId = PipelineDescTranslator::InternGraphics(device, m_Pipeline);
    if (!m_ShadowPipelineId.IsValid() && !m_ShadowPipeline.vertexShader.empty())
        m_ShadowPipelineId = PipelineDescTranslator::InternGraphics(device, m_ShadowPipeline);
    if (!m_CopyPipelineId.IsValid() && !m_CopyPipeline.vertexShader.empty())
        m_CopyPipelineId = PipelineDescTranslator::InternGraphics(device, m_CopyPipeline);
    if (!m_ResolvePipelineId.IsValid() && !m_ResolvePipeline.vertexShader.empty())
        m_ResolvePipelineId = PipelineDescTranslator::InternGraphics(device, m_ResolvePipeline);
    if (m_PipelineId.IsValid() && !m_Sampler.IsValid())
        m_Sampler = device.CreateSampler(SamplerDesc::MaterialLinearClamp("Ocean_Underwater_Sampler"));
    if (m_PipelineId.IsValid() && !m_DummyArrayTexture.IsValid())
        m_DummyArrayTexture = CreateZeroTexture(device, "Ocean_Underwater_DummyArray", true);
    if (m_PipelineId.IsValid() && !m_DummyTexture2D.IsValid())
        m_DummyTexture2D = CreateZeroTexture(device, "Ocean_Underwater_Dummy2D", false);
    return m_PipelineId.IsValid() && m_CopyPipelineId.IsValid() && m_Sampler.IsValid() &&
           m_DummyArrayTexture.IsValid() && m_DummyTexture2D.IsValid();
}

namespace
{
// Finalize this frame's UBO contents. Called at DECLARE time; the result lands
// in a render-graph upload-ring allocation and the composite pass captures only
// {buffer, offset} — no per-class buffer ring.
void FillUnderwaterParams(OceanUnderwaterParamsGPU& out, const OceanParamsGPU& params,
                          float submergedDepth, const float* invViewProj, const float* viewProj,
                          const float* view, const float* cameraPos, bool causticsAvailable,
                          const OceanUnderwaterSettings& settings, uint32_t localFFTReadyMask,
                          const OceanCascadeLayoutGPU* localFFTMaskLayout,
                          const OceanUnderwaterPortalData* portalData)
{
    OceanUnderwaterParamsGPU u{};
    std::memcpy(u.InvViewProj, invViewProj, sizeof(float) * 16);
    std::memcpy(u.ViewProj, viewProj, sizeof(float) * 16);
    std::memcpy(u.View, view, sizeof(float) * 16);
    u.CameraPos[0] = cameraPos[0];
    u.CameraPos[1] = cameraPos[1];
    u.CameraPos[2] = cameraPos[2];

    // Reuse the surface DepthFogDensity from below; the w lane caps the fog reach
    // so sky / far geometry saturate to the deep body. Scaled off the densities so
    // strong water still reaches full extinction at the cap.
    u.FogDensity[0] = params.DepthFogDensity[0];
    u.FogDensity[1] = params.DepthFogDensity[1];
    u.FogDensity[2] = params.DepthFogDensity[2];
    const float maxDensity = std::max(
        {params.DepthFogDensity[0], params.DepthFogDensity[1], params.DepthFogDensity[2], 1e-3f});
    const float fogStart = std::max(params.DepthFogStartDistance, 0.0f);
    const float autoFogRange = 8.0f / maxDensity; // ~e^-8 -> fully fogged at the cap
    const float authoredFogEnd =
        params.DepthFogEndDistance > fogStart ? params.DepthFogEndDistance : 0.0f;
    u.FogDensity[3] = authoredFogEnd > 0.0f ? authoredFogEnd : fogStart + autoFogRange;
    // Clear local water must not inherit a shorter global extinction cap.
    if (authoredFogEnd == 0.0f && portalData)
        for (const auto& material : portalData->Materials)
        {
            const float localDensity = std::max({material.Fog[0], material.Fog[1], material.Fog[2], 1e-3f});
            u.FogDensity[3] = std::max(u.FogDensity[3], fogStart + 8.0f / localDensity);
        }
    u.FogFalloff[0] = fogStart;
    u.FogFalloff[1] = authoredFogEnd;
    u.FogFalloff[2] = std::max(params.DepthFogFalloffPower, 0.01f);
    u.FogFalloff[3] = params.DepthFogFalloffMode;

    // Deep body = the authored deep-water colour; the alpha lane carries an
    // overall tint strength that ramps with how deep the camera sits.
    u.DeepTint[0] = params.DeepColor[0];
    u.DeepTint[1] = params.DeepColor[1];
    u.DeepTint[2] = params.DeepColor[2];

    // Shallow scatter tint (the near-surface "alive" colour) also drives the
    // meniscus brightness in the alpha lane.
    u.ShallowTint[0] = params.SubSurfaceShallowCol[0];
    u.ShallowTint[1] = params.SubSurfaceShallowCol[1];
    u.ShallowTint[2] = params.SubSurfaceShallowCol[2];
    u.ShallowTint[3] = 0.9f;

    u.Misc[0] = params.SeaLevel;
    u.Misc[1] = params.MeniscusWidth;
    u.Misc[2] = submergedDepth;
    u.Misc[3] = params.Time;
    // The per-view Underwater flag is set only for a submerged camera; without it
    // the pass runs for portal volumes alone and the shader skips every pixel
    // that sees no water.
    u.PortalExtra[2] = params.Underwater != 0u ? 0.0f : 1.0f;

    // Sun (for inscattering + god-ray projection) — fed from the same scene light
    // the surface uses (OceanParamsGPU.SunDirection points toward the sun).
    u.SunDirection[0] = params.SunDirection[0];
    u.SunDirection[1] = params.SunDirection[1];
    u.SunDirection[2] = params.SunDirection[2];
    u.SunColor[0] = params.SunColor[0];
    u.SunColor[1] = params.SunColor[1];
    u.SunColor[2] = params.SunColor[2];

    // FFT surface-height inputs so the waterline conforms to the same shaped/clamped
    // wave height the visible surface uses.
    u.FFTParams[0] = static_cast<float>(params.WaveMode);
    u.FFTParams[1] = static_cast<float>(params.FFTCascadeCount);
    u.FFTParams[2] = params.Weight;
    u.FFTParams[3] = params.MaxVerticalDisplacement;
    u.LocalFFT[0] = static_cast<float>(localFFTReadyMask & ((1u << kMaxOceanLocalFFTStreams) - 1u));
    if (localFFTReadyMask != 0u && localFFTMaskLayout)
    {
        const uint32_t lodCount =
            std::min<uint32_t>(localFFTMaskLayout->LodCount, kMaxOceanLodCascades);
        for (uint32_t i = 0; i < lodCount; ++i)
        {
            std::memcpy(u.LocalFFTMaskCascadeOriginScale + i * 4u,
                        localFFTMaskLayout->CascadeOriginScale[i], sizeof(float) * 4u);
        }
        u.LocalFFTMaskCascadeMeta[0] = static_cast<float>(lodCount);
    }

    u.Shape[0] = (causticsAvailable && settings.CausticsOnGeometry) ? 1.0f : 0.0f; // caustics-on-geometry enable
    u.Shape[1] = params.IorWater; // Snell refraction into water for inscatter
    u.Shape[2] = 1.0f;            // probe reach (m) along the view ray
    u.Shape[3] = std::max(settings.WaterlineFadeDistance, 0.001f); // waterline edge softness (m)

    // Effect strengths from the authored OceanSurface settings (routed via the
    // feature). The Crest/Wicked presets are just different values of these.
    u.Inscatter[0] = settings.Inscatter ? 1.0f : 0.0f;
    u.Inscatter[1] = settings.Inscatter_Strength;
    u.Inscatter[2] = settings.Inscatter_PhaseG; // HG phase g
    u.Inscatter[3] = 0.1f;                       // camera-depth falloff

    // The lens (barrel and waterline pinch) belongs to a submerged camera. A dry
    // camera sees water only through a portal, and with the lens off the shader's
    // per-pixel portal test uses exactly the ray the full path shades.
    u.Distortion[0] = settings.Distortion && params.Underwater != 0u ? 1.0f : 0.0f;
    u.Distortion[1] = 0.035f * std::clamp(settings.Distortion_Strength, 0.0f, 2.0f);                       // lens K1 (Brown-Conrady)
    u.Distortion[2] = -0.02f * std::clamp(settings.Distortion_Strength, 0.0f, 2.0f);                      // lens K2
    u.Distortion[3] = settings.Distortion_Strength; // intersection-band squeeze

    u.GodRays[0] = settings.GodRays ? 1.0f : 0.0f;
    u.GodRays[1] = settings.GodRay_Strength;
    u.GodRays[2] = settings.GodRay_Density;        // angular frequency
    u.GodRays[3] = settings.GodRay_Density * 0.6f; // secondary octave (derived)
    u.ReflectedCaustics[0] = std::max(settings.ReflectedCaustics_Strength, 0.0f);
    u.ReflectedCaustics[1] = std::max(settings.ReflectedCaustics_Height, 0.0f);
    u.ReflectedCaustics[2] = std::max(settings.ReflectedCaustics_Falloff, 0.01f);

    u.Caustics[0] = std::max(params.CausticsScale, 0.01f);
    u.Caustics[1] = params.CausticsAverage;
    u.Caustics[2] = std::max(params.CausticsStrength, 0.0f);
    u.Caustics[3] = params.CausticsFocalDepth;
    u.CausticsFocus[0] = std::max(params.CausticsDepthOfField, 0.0f);
    u.CausticsFocus[1] = params.CausticsDistortionStrength;
    u.CausticsFocus[2] = std::max(params.CausticsDistortionScale, 0.01f);

    if (portalData && portalData->Enabled)
    {
        const uint32_t volumeCount = std::min<uint32_t>(
            static_cast<uint32_t>(portalData->Volumes.size()), kMaxOceanUnderwaterPortalVolumes);
        const uint32_t exclusionCount = std::min<uint32_t>(
            static_cast<uint32_t>(portalData->Exclusions.size()), kMaxOceanUnderwaterPortalExclusions);
        const uint32_t polygonCount = std::min<uint32_t>(
            static_cast<uint32_t>(portalData->Polygons.size()), kMaxOceanUnderwaterPortalPolygons);
        const uint32_t occluderCount = std::min<uint32_t>(
            static_cast<uint32_t>(portalData->Occluders.size()), kMaxOceanUnderwaterPortalOccluders);
        u.Portal[0] = (volumeCount > 0u || polygonCount > 0u) ? 1.0f : 0.0f;
        u.Portal[1] = static_cast<float>(volumeCount);
        u.Portal[2] = static_cast<float>(exclusionCount);
        u.Portal[3] = static_cast<float>(polygonCount);
        u.PortalExtra[0] = static_cast<float>(occluderCount);

        auto writeBox = [](float* dst, uint32_t index, const OceanUnderwaterPortalBox& box)
        {
            float* base = dst + index * 8u;
            base[0] = box.CenterX - box.HalfX;
            base[1] = box.CenterY - box.HalfY;
            base[2] = box.CenterZ - box.HalfZ;
            base[3] = 0.0f;
            base[4] = box.CenterX + box.HalfX;
            base[5] = box.CenterY + box.HalfY;
            base[6] = box.CenterZ + box.HalfZ;
            base[7] = 0.0f;
        };

        for (uint32_t i = 0; i < volumeCount; ++i)
            writeBox(u.PortalVolumeBoxes, i, portalData->Volumes[i]);
        for (uint32_t i = 0; i < exclusionCount; ++i)
            writeBox(u.PortalExclusionBoxes, i, portalData->Exclusions[i]);
        for (uint32_t i = 0; i < occluderCount; ++i)
            writeBox(u.PortalOccluderBoxes, i, portalData->Occluders[i]);
        for (uint32_t i = 0; i < polygonCount; ++i)
        {
            const OceanUnderwaterPortalPolygon& src = portalData->Polygons[i];
            const uint32_t base =
                i * (1u + kMaxOceanUnderwaterPortalPolygonPoints) * 4u;
            u.PortalPolygons[base + 0u] = src.SurfaceY;
            u.PortalPolygons[base + 1u] = src.Depth;
            u.PortalPolygons[base + 2u] =
                static_cast<float>(std::min<uint32_t>(src.PointCount,
                                                      kMaxOceanUnderwaterPortalPolygonPoints));
            for (uint32_t p = 0; p < std::min<uint32_t>(src.PointCount,
                                                        kMaxOceanUnderwaterPortalPolygonPoints);
                 ++p)
            {
                const uint32_t pointBase = base + (1u + p) * 4u;
                u.PortalPolygons[pointBase + 0u] = src.X[p];
                u.PortalPolygons[pointBase + 1u] = src.Z[p];
            }
        }
    }

    u.Gerstner[0] = static_cast<float>(params.GerstnerWaveCount);
    u.Gerstner[1] = params.ChoppyScale;
    const uint32_t gw = params.GerstnerWaveCount < 16u ? params.GerstnerWaveCount : 16u;
    for (uint32_t i = 0; i < 16u; ++i)
    {
        const bool active = i < gw;
        const GerstnerWave& w = params.Waves[i];
        u.Waves[i * 8 + 0] = active ? w.DirectionX : 0.0f;
        u.Waves[i * 8 + 1] = active ? w.DirectionZ : 0.0f;
        u.Waves[i * 8 + 2] = active ? w.Amplitude : 0.0f;
        u.Waves[i * 8 + 3] = active ? w.Wavelength : 1.0f;
        u.Waves[i * 8 + 4] = active ? w.Steepness : 0.0f;
        u.Waves[i * 8 + 5] = active ? w.Speed : 1.0f;
    }

    // Single assignment into the (host-coherent, possibly write-combined)
    // upload-ring memory rather than field-by-field writes.
    out = u;
}
} // namespace

bool IsUnderwaterPortalInView(const OceanUnderwaterPortalData& data, const float* viewProj)
{
    const OceanFootprintFrustum frustum(viewProj);
    // The ribbon tree's first record is its root branch, whose A/B carry the
    // world-space bounds (x, y, z) of every underwater ribbon triangle.
    constexpr uint32_t kRibbonBranchFlag = 0x80000000u;
    if (!data.Ribbons.empty())
    {
        const OceanRibbonTriangleGPU& root = data.Ribbons.front();
        if ((root.Meta[0] & kRibbonBranchFlag) == 0u)
            return true;
        if (frustum.Intersects(root.A[0], root.A[1], root.A[2], root.B[0], root.B[1], root.B[2]))
            return true;
    }
    for (const OceanUnderwaterPortalBox& box : data.Volumes)
    {
        if (frustum.Intersects(box.CenterX - box.HalfX, box.CenterY - box.HalfY,
                               box.CenterZ - box.HalfZ, box.CenterX + box.HalfX,
                               box.CenterY + box.HalfY, box.CenterZ + box.HalfZ))
            return true;
    }
    for (const OceanUnderwaterPortalPolygon& polygon : data.Polygons)
    {
        if (frustum.IntersectsPolygon(
                polygon.X, polygon.Z,
                std::min(polygon.PointCount, kMaxOceanUnderwaterPortalPolygonPoints),
                polygon.SurfaceY - polygon.Depth, polygon.SurfaceY, 0.0f))
            return true;
    }
    return false;
}

bool OceanUnderwater::DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d,
                                     const OceanParamsGPU& params, float submergedDepth,
                                     const OceanShapeSampleInputs& shape,
                                     ::GameEngine::Rendering::TextureHandle caustics,
                                     ::GameEngine::Rendering::SamplerHandle causticsSampler,
                                     const OceanUnderwaterSettings& settings,
                                     const OceanUnderwaterPortalData* portalData)
{
    auto* device = d.Services.GetDevice();
    if (!device)
        return false;
    m_Device = device;

    // Resolve the scene colour we tint and the resolved depth we fog by. Decline
    // cleanly (no overlay) when a project rendergraph stripped either.
    static bool s_LoggedDecline = false;
    const RenderGraph::RGTexture sceneColor = d.ResolveTexture(Engine::Renderer::Pipeline::Names::Res::SceneColor);
    if (!sceneColor.IsValid())
    {
        if (!s_LoggedDecline) { Logger::Log::Warning("OceanUnderwater: declined - SceneColor unresolved"); s_LoggedDecline = true; }
        return false;
    }
    RenderGraph::RGTexture depth = d.ResolveTexture(Engine::Renderer::Pipeline::Names::View::DepthResolved);
    if (!depth.IsValid())
        depth = d.ViewDepthResolved.IsValid() ? d.ViewDepthResolved : d.ViewDepth;
    if (!depth.IsValid())
    {
        if (!s_LoggedDecline) { Logger::Log::Warning("OceanUnderwater: declined - depth unresolved"); s_LoggedDecline = true; }
        return false;
    }

    // When SceneColor is multisampled the scene-copy step resolves it via a
    // sampler2DMS pass (a sampler2D copy can't read MSAA), so the overlay works with
    // MSAA on. The resulting single-sample copy then feeds the composite as usual.
    const RenderGraph::RGResourceDesc scd = d.Frame.Graph().ResourceDesc(sceneColor.Id);
    const bool msaa = scd.SampleCount > 1;

    if (!EnsurePipeline(*device))
    {
        if (!s_LoggedDecline) { Logger::Log::Warning("OceanUnderwater: declined - EnsurePipeline failed (overlay/copy pipeline or sampler invalid)"); s_LoggedDecline = true; }
        return false;
    }
    if (msaa && !m_ResolvePipelineId.IsValid())
    {
        if (!s_LoggedDecline) { Logger::Log::Warning("OceanUnderwater: declined - MSAA resolve pipeline unavailable"); s_LoggedDecline = true; }
        return false;
    }

    const CameraData* camera = d.Services.Views().FindCameraData(d.View.cameraId);
    if (!camera)
        return false;

    Mathematics::Matrix4x4 vp{};
    std::memcpy(vp.Data(), camera->viewProj, sizeof(float) * 16);
    const Mathematics::Matrix4x4 invVp = Mathematics::Inverse(vp);

    uint32_t validLocalFFTMask =
        shape.LocalFFTReadyMask & ((1u << kMaxOceanLocalFFTStreams) - 1u);
    if (!shape.LocalFFTMaskLayout || shape.LocalFFTMaskLayout->LodCount == 0u)
    {
        validLocalFFTMask = 0u;
    }
    else
    {
        for (uint32_t page = 0; page < kOceanLocalFFTMaskPages; ++page)
        {
            if (shape.LocalFFTMasks[page].IsValid() && shape.LocalFFTMaskSamplers[page].IsValid())
                continue;
            const uint32_t pageMask =
                ((1u << kOceanLocalFFTMaskChannels) - 1u) <<
                (page * kOceanLocalFFTMaskChannels);
            validLocalFFTMask &= ~pageMask;
        }
    }
    for (uint32_t stream = 0; stream < kMaxOceanLocalFFTStreams; ++stream)
    {
        if (!shape.LocalDisplacements[stream].IsValid() ||
            !shape.LocalDisplacementSamplers[stream].IsValid())
        {
            validLocalFFTMask &= ~(1u << stream);
        }
    }
    // Params fill + upload at DECLARE time (the frame's upload ring is
    // host-coherent); the composite pass captures only {buffer, offset}.
    const auto paramsAlloc = d.Frame.AllocUpload<OceanUnderwaterParamsGPU>();
    if (!paramsAlloc.Valid())
        return false;
    FillUnderwaterParams(*paramsAlloc.Ptr, params, submergedDepth, invVp.Data(), vp.Data(),
                         camera->view, camera->cameraPos, caustics.IsValid(), settings,
                         validLocalFFTMask, shape.LocalFFTMaskLayout, portalData);

    const uint32_t renderW = d.RenderWidth;
    const uint32_t renderH = d.RenderHeight;

    const Engine::Renderer::Pipeline::PipelineBufferBindingRG shadowData =
        d.ResolveBuffer(Engine::Renderer::Pipeline::Names::Res::ShadowData);
    const RenderGraph::RGTexture shadowArr =
        d.Services.GetShadowMapArrayRG(d.Frame, d.View.id);
    SamplerHandle shadowSampler{};
    if (const auto* shadowResources = d.Services.Views().GetViewShadowResources(d.View.id))
        shadowSampler = shadowResources->shadowSampler;
    const bool useShadowedGodRays = shadowData.IsValid() && shadowArr.IsValid() &&
                                    shadowSampler.IsValid() && m_ShadowPipelineId.IsValid();

    // Scene copy: the overlay reads the scene-as-built while writing back into the
    // same target, so snapshot it first (mirrors the fullscreen-copy pattern
    // / OceanSceneGrab). A straight passthrough copy via the stock copy program.
    TextureDesc copyDesc{};
    copyDesc.width = renderW;
    copyDesc.height = renderH;
    copyDesc.depth = 1;
    copyDesc.arrayLayers = 1;
    copyDesc.mipLevels = 1;
    copyDesc.sampleCount = 1;
    copyDesc.format = scd.Format;
    copyDesc.usage =
        static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource);
    copyDesc.debugName = "Ocean.Underwater.SceneCopy";
    const std::string base =
        "Ocean.Underwater.View" + std::to_string(static_cast<uint32_t>(d.View.id));
    const std::string sceneCopyName = base + ".SceneCopy";
    const std::string compositeName = base + ".Composite";
    const RenderGraph::RGTexture sceneCopy =
        d.Frame.CreateTexture(sceneCopyName.c_str(), copyDesc);
    const RenderGraph::RGTexture compositeTarget =
        msaa ? d.Frame.CreateTexture((base + ".CompositeColor").c_str(), copyDesc) : sceneColor;
    if (!compositeTarget.IsValid())
        return false;

    const GraphicsPipelineId overlayPipe = useShadowedGodRays ? m_ShadowPipelineId : m_PipelineId;
    const DescriptorSetLayoutDesc overlayLayout = useShadowedGodRays ? m_ShadowLayout : m_Layout;
    const SamplerHandle sampler = m_Sampler;

    // AddCopyPass picks the sampler2DMS resolve fork off the source's
    // SampleCount — the same `msaa` condition used above to gate declining.
    RenderGraph::AddCopyPass(d.Frame, sceneColor, sceneCopy, sceneCopyName.c_str(),
                             Rendering::PassPhase::kPostProcess, m_CopyPipelineId,
                             m_ResolvePipelineId, m_CopyLayout, m_ResolveLayout, sampler,
                             renderW, renderH);

    // Composite: read the snapshot + depth, apply fog-from-below / tint / meniscus,
    // write back into SceneColor when it is single-sample. If a custom pipeline
    // made SceneColor multisampled, write a new single-sample chain target instead:
    // fullscreen post pipelines are 1x and cannot attach an MSAA colour target.
    //
    // Inputs the shader gates off the params UBO (unready FFT streams/masks,
    // absent caustics) still need valid descriptors, so they fall back to the
    // never-read dummies — a per-input fallback, hence the dedicated 2D dummy
    // for caustics instead of reusing another binding's physical.
    std::array<RenderGraph::RGFullscreenTextureInput, 15> textureInputs{};
    uint32_t textureCount = 0;
    {
        auto& in = textureInputs[textureCount++]; // scene snapshot
        in.Binding = 0;
        in.Texture = sceneCopy;
        in.Sampler = sampler;
        in.Required = true;
    }
    {
        auto& in = textureInputs[textureCount++]; // resolved depth
        in.Binding = 1;
        in.Texture = depth;
        in.Sampler = sampler;
        in.Required = true;
    }
    {
        auto& in = textureInputs[textureCount++]; // FFT displacement cascades
        in.Binding = 3;
        in.Texture = shape.DisplacementRG; // declared read: orders after the FFT sim
        in.RawTexture = shape.Displacement;
        in.Sampler = shape.DisplacementSampler;
        in.Fallback = m_DummyArrayTexture;
        in.FallbackSampler = sampler;
    }
    {
        auto& in = textureInputs[textureCount++]; // caustics web (uShape.x gates use)
        in.Binding = 4;
        in.RawTexture = caustics;
        in.Sampler = causticsSampler;
        in.Fallback = m_DummyTexture2D;
        in.FallbackSampler = sampler;
    }
    constexpr uint32_t kLocalBindings[kMaxOceanLocalFFTStreams] =
        {5u, 6u, 7u, 10u, 12u, 13u, 14u, 15u};
    for (uint32_t stream = 0; stream < kMaxOceanLocalFFTStreams; ++stream)
    {
        auto& in = textureInputs[textureCount++]; // local FFT displacement streams
        in.Binding = kLocalBindings[stream];
        if ((validLocalFFTMask & (1u << stream)) != 0u)
        {
            in.Texture = shape.LocalDisplacementsRG[stream];
            in.RawTexture = shape.LocalDisplacements[stream];
            in.Sampler = shape.LocalDisplacementSamplers[stream];
        }
        in.Fallback = m_DummyArrayTexture;
        in.FallbackSampler = sampler;
    }
    constexpr uint32_t kLocalMaskBindings[kOceanLocalFFTMaskPages] = {11u, 16u};
    for (uint32_t page = 0; page < kOceanLocalFFTMaskPages; ++page)
    {
        auto& in = textureInputs[textureCount++]; // local FFT mask pages
        in.Binding = kLocalMaskBindings[page];
        if (shape.LocalFFTMasks[page].IsValid() && shape.LocalFFTMaskSamplers[page].IsValid())
        {
            in.Texture = shape.LocalFFTMasksRG[page];
            in.RawTexture = shape.LocalFFTMasks[page];
            in.Sampler = shape.LocalFFTMaskSamplers[page];
        }
        in.Fallback = m_DummyArrayTexture;
        in.FallbackSampler = sampler;
    }
    if (useShadowedGodRays)
    {
        auto& in = textureInputs[textureCount++]; // CSM array for god-ray shafts
        in.Binding = 9;
        in.Texture = shadowArr;
        in.Sampler = shadowSampler;
        in.Required = true;
    }

    std::array<RenderGraph::RGFullscreenBufferInput, 4> bufferInputs{};
    uint32_t bufferCount = 0;
    {
        auto& b = bufferInputs[bufferCount++]; // params UBO (frame upload ring)
        b.Binding = 2;
        b.Buffer = paramsAlloc.Buffer;
        b.Offset = paramsAlloc.Offset;
        b.Size = sizeof(OceanUnderwaterParamsGPU);
    }
    if (useShadowedGodRays)
    {
        auto& b = bufferInputs[bufferCount++]; // shadow cascade data
        b.Binding = 8;
        b.Buffer = shadowData.Buffer;
        b.Offset = shadowData.Offset;
        b.Size = shadowData.Size;
        b.Declare = shadowData.Graph; // graph-produced: order/barrier the read
    }

    // Continuous spline volumes share the same triangles used for clipping and CPU queries.
    const size_t ribbonCount = portalData ? portalData->Ribbons.size() : 0;
    const size_t ribbonBytes = std::max<size_t>(ribbonCount, 1) * sizeof(OceanRibbonTriangleGPU);
    const auto ribbons = d.Frame.AllocUpload(ribbonBytes);
    if (!ribbons.Valid())
        return false;
    if (ribbonCount)
        std::memcpy(ribbons.Ptr, portalData->Ribbons.data(), ribbonBytes);
    else
        std::memset(ribbons.Ptr, 0, ribbonBytes);
    paramsAlloc.Ptr->PortalExtra[1] = static_cast<float>(ribbonCount);
    if (ribbonCount > 0u)
        paramsAlloc.Ptr->Portal[0] = 1.0f;
    auto &ribbonBinding = bufferInputs[bufferCount++];
    ribbonBinding.Binding = 17;
    ribbonBinding.IsStorage = true;
    ribbonBinding.Buffer = ribbons.Buffer;
    ribbonBinding.Offset = ribbons.Offset;
    ribbonBinding.Size = ribbonBytes;

    const size_t materialCount = portalData ? portalData->Materials.size() : 0;
    const size_t materialBytes = 16 + materialCount * sizeof(OceanWaterMaterialGPU);
    const auto materials = d.Frame.AllocUpload(materialBytes);
    if (!materials.Valid())
        return false;
    std::memset(materials.Ptr, 0, 16);
    static_cast<uint32 *>(materials.Ptr)[0] = static_cast<uint32>(materialCount);
    if (materialCount)
        std::memcpy(static_cast<uint8 *>(materials.Ptr) + 16, portalData->Materials.data(),
                    materialBytes - 16);
    auto &materialBinding = bufferInputs[bufferCount++];
    materialBinding.Binding = 18;
    materialBinding.IsStorage = true;
    materialBinding.Buffer = materials.Buffer;
    materialBinding.Offset = materials.Offset;
    materialBinding.Size = materialBytes;

    RenderGraph::RGFullscreenDesc composite{};
    composite.Name = compositeName.c_str();
    composite.Phase = Rendering::PassPhase::kPostProcess;
    composite.Pipeline = overlayPipe;
    composite.Layout = overlayLayout;
    composite.Textures =
        std::span<const RenderGraph::RGFullscreenTextureInput>(textureInputs.data(), textureCount);
    composite.Buffers =
        std::span<const RenderGraph::RGFullscreenBufferInput>(bufferInputs.data(), bufferCount);
    composite.Target = compositeTarget;
    composite.Ops.Load = RenderGraph::RGLoadOp::DontCare;
    composite.Ops.Store = RenderGraph::RGStoreOp::Store;
    composite.Width = renderW;
    composite.Height = renderH;
    RenderGraph::AddFullscreenPass(d.Frame, composite);

    if (compositeTarget.Id != sceneColor.Id)
        d.PublishTexture(Engine::Renderer::Pipeline::Names::Res::SceneColor, compositeTarget);

    return true;
}

} // namespace GameEngine::Ocean
