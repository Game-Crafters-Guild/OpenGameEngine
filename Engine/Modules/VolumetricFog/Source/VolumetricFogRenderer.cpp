#include "Engine/Rendering/VolumetricFogRenderer.h"

#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Components/Rendering/Light.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
constexpr uint32_t kComputeGroup = 8;
constexpr uint32_t kFogLightListCapacity = 64; // Matches volumetric_fog_cull.comp.

// A froxel's emission equals the view's unless a local volume changes it, so the
// per-froxel grid is only worth allocating when one does. An override volume
// always rewrites emission (it blends toward its own, even zero); an additive one
// only when it emits; a subtractive one never touches emission at all.
bool VolumesModifyEmission(const std::vector<VolumetricFogLocalVolume>& volumes)
{
    for (const VolumetricFogLocalVolume& volume : volumes)
    {
        if (!volume.enabled || volume.densityMode == VolumetricFogDensityMode::Subtractive)
            continue;
        if (volume.densityMode == VolumetricFogDensityMode::Override)
            return true;
        if (volume.emission[0] != 0.0f || volume.emission[1] != 0.0f || volume.emission[2] != 0.0f)
            return true;
    }
    return false;
}
constexpr uint32_t kComputeStage = kShaderStageCompute;
constexpr uint32_t kFragmentStage = kShaderStageFragment;
constexpr uint32_t kJitterAtlasSize = 32;
constexpr uint32_t kDensityNoiseAtlasSize = 64;

struct alignas(16) FogLocalVolumeGpu
{
    float centerShape[4]{};
    float axisXHalfExtent[4]{1.0f, 0.0f, 0.0f, 0.5f};
    float axisYHalfExtent[4]{0.0f, 1.0f, 0.0f, 0.5f};
    float axisZHalfExtent[4]{0.0f, 0.0f, 1.0f, 0.5f};
    float params0[4]{1.0f, 1.0f, 0.02f, 0.0f}; // x=blend, y=weight, z=density, w=mode
    float albedo[4]{0.82f, 0.78f, 0.72f, 0.0f};
    float emission[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float gradientLow[4]{1.0f, 1.0f, 1.0f, 0.0f};
    float gradientHigh[4]{1.0f, 1.0f, 1.0f, 0.0f};
    float params1[4]{0.0f, 0.15f, 0.0f, 0.0f}; // x=threshold, y=softness, z=gradientMode, w=gradientStrength
};

struct alignas(16) FogLocalVolumeBufferGpu
{
    uint32_t header[4]{0u, 0u, 0u, 0u};
    FogLocalVolumeGpu volumes[kMaxVolumetricFogLocalVolumes]{};
};

uint32_t DivCeil(uint32_t v, uint32_t d)
{
    return (v + d - 1u) / d;
}

uint32_t PcgHash(uint32_t v)
{
    v = v * 747796405u + 2891336453u;
    uint32_t word = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (word >> 22u) ^ word;
}

float Fract(float v)
{
    return v - std::floor(v);
}

float Clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

void CopyMatrix(float* dst, const float* src)
{
    std::memcpy(dst, src, sizeof(float) * 16);
}

void MakeIdentity(float* dst)
{
    std::fill(dst, dst + 16, 0.0f);
    dst[0] = dst[5] = dst[10] = dst[15] = 1.0f;
}

void TransformPoint(const float* m, const float in[4], float out[4])
{
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8]  * in[2] + m[12] * in[3];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9]  * in[2] + m[13] * in[3];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14] * in[3];
    out[3] = m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15] * in[3];
}

bool FogRayFromUv(const CameraData& camera, float u, float v, float ray[3])
{
    Mathematics::Matrix4x4 vp{};
    std::memcpy(vp.Data(), camera.viewProj, sizeof(float) * 16);
    Mathematics::Matrix4x4 inv = Mathematics::Inverse(vp);

    const float clip[4] = {u * 2.0f - 1.0f, (1.0f - v) * 2.0f - 1.0f, 0.0f, 1.0f};
    float world[4]{};
    TransformPoint(inv.Data(), clip, world);
    if (std::abs(world[3]) <= 1.0e-6f)
        return false;

    const float invW = 1.0f / world[3];
    ray[0] = world[0] * invW - camera.cameraPos[0];
    ray[1] = world[1] * invW - camera.cameraPos[1];
    ray[2] = world[2] * invW - camera.cameraPos[2];
    const float len = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    if (len <= 1.0e-6f)
        return false;
    const float invLen = 1.0f / len;
    ray[0] *= invLen;
    ray[1] *= invLen;
    ray[2] *= invLen;
    return true;
}

float EstimateVisibleFogLayerDistance(const VolumetricFogSettings& settings, const CameraData& camera)
{
    const float heightFalloff = std::max(settings.heightFalloff, 0.01f);
    const float layerHeights[] = {
        settings.baseHeight,
        settings.baseHeight + heightFalloff,
        settings.baseHeight + heightFalloff * 3.0f,
    };
    const float uvSamples[][2] = {
        {0.50f, 0.50f},
        {0.50f, 0.68f},
        {0.50f, 0.86f},
        {0.25f, 0.86f},
        {0.75f, 0.86f},
    };

    float maxDistance = 0.0f;
    for (const auto& uv : uvSamples)
    {
        float ray[3]{};
        if (!FogRayFromUv(camera, uv[0], uv[1], ray))
            continue;
        for (float layerHeight : layerHeights)
        {
            if (std::abs(ray[1]) <= 1.0e-5f)
                continue;
            const float t = (layerHeight - camera.cameraPos[1]) / ray[1];
            if (t > 0.0f)
                maxDistance = std::max(maxDistance, t + heightFalloff * 6.0f);
        }
    }
    return maxDistance;
}

bool IsFogActive(const VolumetricFogSettings& settings)
{
    return settings.enabled && (settings.density > 0.0f || !settings.localVolumes.empty()) && settings.maxDistance > 0.0f;
}

bool IsTemporalCompatible(const VolumetricFogSettings& a, const VolumetricFogSettings& b)
{
    constexpr float eps = 0.0001f;
    auto near3 = [eps](const float lhs[3], const float rhs[3]) {
        return std::abs(lhs[0] - rhs[0]) <= eps &&
               std::abs(lhs[1] - rhs[1]) <= eps &&
               std::abs(lhs[2] - rhs[2]) <= eps;
    };
    if (a.localVolumes.size() != b.localVolumes.size())
        return false;
    for (size_t i = 0; i < a.localVolumes.size(); ++i)
    {
        const auto& av = a.localVolumes[i];
        const auto& bv = b.localVolumes[i];
        if (av.enabled != bv.enabled ||
            av.shape != bv.shape ||
            av.densityMode != bv.densityMode ||
            av.gradientMode != bv.gradientMode ||
            !near3(av.center, bv.center) ||
            !near3(av.axisX, bv.axisX) ||
            !near3(av.axisY, bv.axisY) ||
            !near3(av.axisZ, bv.axisZ) ||
            !near3(av.halfExtents, bv.halfExtents) ||
            std::abs(av.weight - bv.weight) > eps ||
            std::abs(av.density - bv.density) > eps ||
            std::abs(av.blendDistance - bv.blendDistance) > eps ||
            std::abs(av.densityThreshold - bv.densityThreshold) > eps)
        {
            return false;
        }
    }
    return a.isGlobal == b.isGlobal &&
           a.localVolumeValid == b.localVolumeValid &&
           a.localVolumeShape == b.localVolumeShape &&
           near3(a.localVolumeCenter, b.localVolumeCenter) &&
           near3(a.localVolumeAxisX, b.localVolumeAxisX) &&
           near3(a.localVolumeAxisY, b.localVolumeAxisY) &&
           near3(a.localVolumeAxisZ, b.localVolumeAxisZ) &&
           near3(a.localVolumeHalfExtents, b.localVolumeHalfExtents) &&
           std::abs(a.localVolumeBlendDistance - b.localVolumeBlendDistance) <= eps &&
           a.xyCellSizePixels == b.xyCellSizePixels &&
           a.zSliceCount == b.zSliceCount &&
           std::abs(a.maxDistance - b.maxDistance) <= eps &&
           std::abs(a.depthDistribution - b.depthDistribution) <= eps;
}

float ComputeEffectiveMaxDistance(const VolumetricFogSettings& settings, const CameraData& camera)
{
    const float authoredMax = std::max(settings.maxDistance, 0.01f);
    if (settings.isGlobal)
        return authoredMax;

    const float heightFalloff = std::max(settings.heightFalloff, 0.01f);
    const float heightGap = std::abs(camera.cameraPos[1] - settings.baseHeight);
    const float layerReach = heightGap + heightFalloff * 10.0f;
    const float visibleLayerReach = EstimateVisibleFogLayerDistance(settings, camera);
    const float adaptiveCap = std::max(authoredMax * 64.0f, 65536.0f);
    return std::min(std::max({authoredMax, layerReach, visibleLayerReach}), adaptiveCap);
}

void Normalize3(float v[3])
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float invLen = 1.0f / std::max(len, 0.0001f);
    v[0] *= invLen;
    v[1] *= invLen;
    v[2] *= invLen;
}

float Smoothstep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
        return x >= edge1 ? 1.0f : 0.0f;
    const float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

struct ResolvedFogSun
{
    float direction[3] = {0.35f, -0.65f, 0.68f};
    float color[3] = {1.0f, 0.86f, 0.62f};
    float intensity = 1.0f;
};

ResolvedFogSun ResolveFogSunLighting(
    RenderServices& services,
    ViewId viewId,
    const VolumetricFogSettings& settings)
{
    ResolvedFogSun sun{};
    bool foundDirectional = false;
    if (settings.trackDirectionalLight)
    {
        const ViewDesc* view = services.Views().FindViewDesc(viewId);
        const auto lights = view ? services.GetWorldLights(view->worldId) : std::span<const ExtractedLight>{};
        // Fog tracks THE primary directional (the light the world shades
        // with — SelectPrimaryDirectional), never a secondary one. If the
        // primary is non-emitting the sky-anchor fallback below applies;
        // scanning on to a weaker directional would aim the fog sun at a
        // light the surface shading is not using. "Non-emitting" is what it
        // delivers, not its intensity alone: a sky-driven sun at night with
        // the moon hidden is black at its authored intensity.
        const ExtractedLight* light = SelectLitPrimaryDirectional(lights);
        if (light)
        {
            sun.direction[0] = -light->directionWS[0];
            sun.direction[1] = -light->directionWS[1];
            sun.direction[2] = -light->directionWS[2];
            sun.color[0] = std::max(light->color[0], 0.0f);
            sun.color[1] = std::max(light->color[1], 0.0f);
            sun.color[2] = std::max(light->color[2], 0.0f);
            sun.intensity = std::max(light->intensity, 0.0f);
            foundDirectional = true;
        }
    }

    if (auto* skyFeature = services.GetFeature<SkyRenderFeature>(); skyFeature && skyFeature->HasActiveSettings())
    {
        const auto& sky = skyFeature->GetSettings();
        if (settings.trackDirectionalLight && !foundDirectional)
        {
            sun.direction[0] = sky.primarySunDir[0];
            sun.direction[1] = sky.primarySunDir[1];
            sun.direction[2] = sky.primarySunDir[2];
            // Ground level: with no directional light to read the sky is the only source, and fog
            // sits under the atmosphere, so it takes the extinguished colour rather than the
            // above-atmosphere one.
            sun.color[0] = std::max(sky.primarySunGroundColor[0], 0.0f);
            sun.color[1] = std::max(sky.primarySunGroundColor[1], 0.0f);
            sun.color[2] = std::max(sky.primarySunGroundColor[2], 0.0f);
            sun.intensity = std::max(sky.primarySunIntensity * 0.1f, 0.0f);
        }
        // A tracked light needs nothing from the sky: its colour already IS the ground-level sun
        // colour whenever the sky drives it, so scaling it by the source's chromaticity would apply
        // the same hue twice — the moon's at night, an authored sun tint squared.
    }

    Normalize3(sun.direction);
    sun.intensity *= std::max(settings.sunIntensityScale, 0.0f);
    return sun;
}

bool HasActiveLocalFogLights(RenderServices& services, ViewId viewId)
{
    const ViewDesc* view = services.Views().FindViewDesc(viewId);
    const auto lights = view ? services.GetWorldLights(view->worldId) : std::span<const ExtractedLight>{};
    for (const auto& light : lights)
    {
        if ((light.type == GameEngine::Components::LightType::Point ||
             light.type == GameEngine::Components::LightType::Spot) &&
            light.castsLight != 0u &&
            light.intensity > 0.0f &&
            light.range > 0.0f)
        {
            return true;
        }
    }
    return false;
}

void ResolveFogAmbient(RenderServices& services, const VolumetricFogSettings& settings, float outAmbient[3])
{
    outAmbient[0] = settings.ambientScatteringTint[0];
    outAmbient[1] = settings.ambientScatteringTint[1];
    outAmbient[2] = settings.ambientScatteringTint[2];

    auto* skyFeature = services.GetFeature<SkyRenderFeature>();
    if (!skyFeature || !skyFeature->HasActiveSettings())
        return;

    const auto& sky = skyFeature->GetSettings();
    const float day = Clamp01(sky.primarySunDir[1] * 0.5f + 0.5f);
    const float night = Clamp01(sky.nightSkyBlend);
    float horizon[3]{};
    for (int i = 0; i < 3; ++i)
    {
        const float dayHorizon = sky.groundHorizonColor[i];
        const float nightHorizon = std::max(sky.groundHorizonNightColor[i], sky.nightSkyHorizonColor[i] * 0.35f);
        horizon[i] = dayHorizon * (1.0f - night) + nightHorizon * night;
    }

    // The AMBIENT wash is the one place the source's hue legitimately enters the fog: it is mixed
    // with the horizon colour rather than multiplied onto a light that already carries it, so this
    // is a single application. White by day; the moon's cool grey at night, once.
    //
    // This is the one consumer of the sun colour that is NOT proportional to it: the source is
    // normalized by its own luminance and mixed in at 35 %, so an authored sun tint reaches the fog
    // ambient as HUE only, never as magnitude. That is the intent — this is a wash, not a light.
    const float sunLum = std::max(ColorUtils::LinearRec709Luminance(sky.primarySunColor), 0.001f);
    float skyAmbient[3]{};
    for (int i = 0; i < 3; ++i)
    {
        const float normalizedSun = sky.primarySunColor[i] / sunLum;
        const float dayAmbient = horizon[i] * 0.65f + normalizedSun * 0.35f;
        const float nightAmbient = horizon[i];
        skyAmbient[i] = nightAmbient * night + dayAmbient * (1.0f - night);
        skyAmbient[i] *= 0.45f + day * 0.55f;
        outAmbient[i] *= std::max(skyAmbient[i], 0.02f);
    }
}

bool SunLightingChangedEnough(
    const float historySunDirection[3],
    const float historySunColor[3],
    float historySunIntensity,
    const ResolvedFogSun& sun)
{
    const float dirDot =
        historySunDirection[0] * sun.direction[0] +
        historySunDirection[1] * sun.direction[1] +
        historySunDirection[2] * sun.direction[2];
    if (dirDot < 0.9848077f) // 10 degrees.
        return true;

    const float oldLum = ColorUtils::LinearRec709Luminance(historySunColor) * historySunIntensity;
    const float newLum = ColorUtils::LinearRec709Luminance(sun.color) * sun.intensity;
    if (std::abs(oldLum - newLum) > std::max(0.05f, oldLum * 0.25f))
        return true;

    const float oldColorScale = std::max(ColorUtils::LinearRec709Luminance(historySunColor), 0.001f);
    const float newColorScale = std::max(ColorUtils::LinearRec709Luminance(sun.color), 0.001f);
    for (int i = 0; i < 3; ++i)
    {
        const float oldC = historySunColor[i] / oldColorScale;
        const float newC = sun.color[i] / newColorScale;
        if (std::abs(oldC - newC) > 0.25f)
            return true;
    }
    return false;
}

// Writes a pass's descriptor bindings, skipping any binding the pass's layout
// does not declare. Each pass's layout is shaped from its own shader
// reflection, so the shared write lists in the pass lambdas are supersets; a
// binding the shader does not use has no slot in the layout and must not reach
// the device, which asserts on undeclared writes.
class LayoutGatedWriter
{
public:
    LayoutGatedWriter(IDevice& device, DescriptorSetHandle set, const DescriptorSetLayoutDesc& layout)
        : m_Device(device), m_Set(set), m_Layout(layout)
    {
    }

    void StorageImage(uint32_t binding, TextureHandle texture)
    {
        if (Declares(binding))
            m_Device.UpdateStorageImageBinding(m_Set, binding, texture);
    }

    void CombinedImageSampler(uint32_t binding, TextureHandle texture, SamplerHandle sampler)
    {
        if (Declares(binding))
            m_Device.UpdateCombinedImageSamplerBinding(m_Set, binding, texture, sampler);
    }

    void UniformBuffer(uint32_t binding, BufferHandle buffer, size_t offset, size_t size)
    {
        if (Declares(binding))
            m_Device.UpdateBufferBinding(m_Set, binding, buffer, offset, size);
    }

    void StorageBuffer(uint32_t binding, BufferHandle buffer, size_t offset, size_t size)
    {
        if (Declares(binding))
            m_Device.UpdateStorageBufferBinding(m_Set, binding, buffer, offset, size);
    }

private:
    bool Declares(uint32_t binding) const
    {
        return std::any_of(m_Layout.bindings.begin(), m_Layout.bindings.end(),
                           [&](const DescriptorBinding& b) { return b.binding == binding; });
    }

    IDevice& m_Device;
    DescriptorSetHandle m_Set;
    const DescriptorSetLayoutDesc& m_Layout;
};

// Interns `desc` on first use and caches the id in `id`. A pass whose program
// did not load keeps an empty shader and gets an invalid id, which declines it.
ComputePipelineId EnsureComputePipeline(IDevice& device, const PipelineDesc& desc,
                                        ComputePipelineId& id)
{
    if (desc.computeShader.empty())
        return {};
    if (!id.IsValid())
        id = PipelineDescTranslator::InternCompute(device, desc);
    return id;
}
} // namespace

struct alignas(16) VolumetricFogRenderer::FogGpuParams
{
    float invViewProj[16];
    float view[16];
    float cameraPos[4];
    float sunDirection[4];
    float sunColor[4];
    float ambientColor[4];
    float albedoDensity[4];
    float emissionAnisotropy[4];
    float heightParams[4];
    float localVolumeCenterShape[4];
    float localVolumeAxisXHalfExtent[4];
    float localVolumeAxisYHalfExtent[4];
    float localVolumeAxisZHalfExtent[4];
    float localVolumeParams[4];
    float noiseParams[4];
    float noiseVelocityTime[4];
    float noiseChannelWeights[4];
    float densityParams[4];
    float volumeParams[4];
    float gridParams[4];
    float jitterParams[4];
    float fogExtraParams[4];
    float prevViewProj[16];
    float prevCameraPos[4];
};

VolumetricFogRenderer::~VolumetricFogRenderer()
{
    DestroyResources();
}

bool VolumetricFogRenderer::Initialize(IDevice* device)
{
    if (m_Initialized)
        return true;
    // A failed attempt is terminal — missing cooked shaders or capabilities do
    // not appear mid-session, and retrying every frame is the retry-storm
    // class: full init cost plus a console flood at frame rate.
    if (m_InitAttempted)
        return false;
    m_InitAttempted = true;
    m_Device = device;
    if (!m_Device)
        return false;
    SamplerDesc sd{};
    sd.minFilter = 1;
    sd.magFilter = 1;
    sd.addressModeU = 2;
    sd.addressModeV = 2;
    sd.addressModeW = 2;
    m_LinearClampSampler = m_Device->CreateSampler(sd);
    m_JitterAtlasSampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearRepeat("VolumetricFog.JitterAtlasSampler"));
    m_DensityNoiseSampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearRepeat("VolumetricFog.DensityNoiseSampler"));

    m_ComputeLayout.debugName = "VolumetricFog.Compute.Set0";
    m_ComputeLayout.bindings = {
        {0, DescriptorType::StorageImage, 1, kComputeStage},
        {1, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {2, DescriptorType::UniformBuffer, 1, kComputeStage},
        {3, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {4, DescriptorType::UniformBuffer, 1, kComputeStage},
        {5, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {6, DescriptorType::StorageBuffer, 1, kComputeStage},
        {7, DescriptorType::StorageBuffer, 1, kComputeStage},
        {8, DescriptorType::StorageBuffer, 1, kComputeStage},
        {11, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {12, DescriptorType::StorageBuffer, 1, kComputeStage},
        {13, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        // Diffuse irradiance cube for IBL in-scatter (lighting pass only; other
        // passes leave it unbound, as with the shadow array at binding 5).
        {14, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {15, DescriptorType::StorageImage, 1, kComputeStage},
        {16, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {17, DescriptorType::UniformBuffer, 1, kComputeStage},
        {18, DescriptorType::CombinedImageSampler, 1, kComputeStage},
        {19, DescriptorType::StorageBuffer, 1, kComputeStage},
        {20, DescriptorType::CombinedImageSampler, 1, kComputeStage},
    };

    m_CompositeLayout.debugName = "VolumetricFog.Composite.Set0";
    m_CompositeLayout.bindings = {
        {0, DescriptorType::CombinedImageSampler, 1, kFragmentStage},
        {1, DescriptorType::CombinedImageSampler, 1, kFragmentStage},
        {2, DescriptorType::UniformBuffer, 1, kFragmentStage},
        // Irradiance cube for the analytic far-fog in-scatter color (gated by
        // iblEnabled; bound only when the IBL feature is live).
        {3, DescriptorType::CombinedImageSampler, 1, kFragmentStage},
    };
    m_Initialized = CreateJitterAtlasTexture() && CreateDensityNoiseAtlasTexture() && CreatePipelines();
    return m_Initialized;
}

void VolumetricFogRenderer::DestroyResources()
{
    if (m_Device)
    {
        for (auto& [_, state] : m_PerView)
        {
            for (auto& buf : state.ubo)
            {
                if (buf.IsValid())
                    m_Device->DestroyBuffer(buf);
                buf = {};
            }
            for (auto& buf : state.localVolumeBuffer)
            {
                if (buf.IsValid())
                    m_Device->DestroyBuffer(buf);
                buf = {};
            }
        }
        if (m_LinearClampSampler.IsValid())
            m_Device->DestroySampler(m_LinearClampSampler);
        if (m_JitterAtlasSampler.IsValid())
            m_Device->DestroySampler(m_JitterAtlasSampler);
        if (m_JitterAtlasTexture.IsValid())
            m_Device->DestroyTexture(m_JitterAtlasTexture);
        if (m_DensityNoiseSampler.IsValid())
            m_Device->DestroySampler(m_DensityNoiseSampler);
        if (m_DensityNoiseTexture.IsValid())
            m_Device->DestroyTexture(m_DensityNoiseTexture);
        if (m_ZeroShadowBuffer.IsValid())
            m_Device->DestroyBuffer(m_ZeroShadowBuffer);
        if (m_ZeroStorageBuffer.IsValid())
            m_Device->DestroyBuffer(m_ZeroStorageBuffer);
    }
    m_LinearClampSampler = {};
    m_JitterAtlasSampler = {};
    m_JitterAtlasTexture = {};
    m_DensityNoiseSampler = {};
    m_DensityNoiseTexture = {};
    m_ZeroShadowBuffer = {};
    m_ZeroStorageBuffer = {};
    m_PerView.clear();
    m_Device = nullptr;
    m_Initialized = false;
}

bool VolumetricFogRenderer::CreateJitterAtlasTexture()
{
    if (!m_Device)
        return false;
    if (m_JitterAtlasTexture.IsValid())
        return true;

    TextureDesc td{};
    td.width = kJitterAtlasSize;
    td.height = kJitterAtlasSize;
    td.depth = kJitterAtlasSize;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::R8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.debugName = "VolumetricFog.JitterAtlas";
    m_JitterAtlasTexture = m_Device->CreateTexture(td);
    if (!m_JitterAtlasTexture.IsValid())
        return false;

    const uint32_t n = kJitterAtlasSize;
    std::vector<float> base(n * n * n, 0.0f);
    auto indexOf = [n](uint32_t x, uint32_t y, uint32_t z) {
        return static_cast<size_t>(x + y * n + z * n * n);
    };

    for (uint32_t z = 0; z < n; ++z)
    {
        for (uint32_t y = 0; y < n; ++y)
        {
            for (uint32_t x = 0; x < n; ++x)
            {
                const uint32_t h =
                    PcgHash(x * 1973u ^ y * 9277u ^ z * 26699u ^ 0x9E3779B9u);
                const float whiteRank = static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777216.0f);
                const float stratified =
                    Fract((static_cast<float>(x * 13u + y * 17u + z * 19u) + 0.5f) / static_cast<float>(n) +
                          static_cast<float>(z) * 0.61803398875f);
                base[indexOf(x, y, z)] = Fract(whiteRank * 0.62f + stratified * 0.38f);
            }
        }
    }

    std::vector<uint8_t> voxels(n * n * n, 0u);
    for (uint32_t z = 0; z < n; ++z)
    {
        for (uint32_t y = 0; y < n; ++y)
        {
            for (uint32_t x = 0; x < n; ++x)
            {
                float neighborhood = 0.0f;
                for (int dz = -1; dz <= 1; ++dz)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const uint32_t sx = static_cast<uint32_t>((static_cast<int>(x) + dx + static_cast<int>(n)) % static_cast<int>(n));
                            const uint32_t sy = static_cast<uint32_t>((static_cast<int>(y) + dy + static_cast<int>(n)) % static_cast<int>(n));
                            const uint32_t sz = static_cast<uint32_t>((static_cast<int>(z) + dz + static_cast<int>(n)) % static_cast<int>(n));
                            neighborhood += base[indexOf(sx, sy, sz)];
                        }
                    }
                }
                neighborhood *= 1.0f / 27.0f;
                const float contrastAdapted = Clamp01((base[indexOf(x, y, z)] - neighborhood) * 0.72f + 0.5f);
                voxels[indexOf(x, y, z)] = static_cast<uint8_t>(std::round(contrastAdapted * 255.0f));
            }
        }
    }

    UploadTexture3D(
        m_Device,
        m_JitterAtlasTexture,
        voxels.data(),
        n,
        n,
        n,
        n,
        n * n,
        "VolumetricFog.JitterAtlasUpload");
    return true;
}

bool VolumetricFogRenderer::CreateDensityNoiseAtlasTexture()
{
    if (!m_Device)
        return false;
    if (m_DensityNoiseTexture.IsValid())
        return true;

    TextureDesc td{};
    td.width = kDensityNoiseAtlasSize;
    td.height = kDensityNoiseAtlasSize;
    td.depth = kDensityNoiseAtlasSize;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.debugName = "VolumetricFog.DensityNoiseAtlas";
    m_DensityNoiseTexture = m_Device->CreateTexture(td);
    if (!m_DensityNoiseTexture.IsValid())
        return false;

    const uint32_t n = kDensityNoiseAtlasSize;
    auto indexOf = [n](uint32_t x, uint32_t y, uint32_t z, uint32_t c) {
        return static_cast<size_t>((x + y * n + z * n * n) * 4u + c);
    };

    std::vector<uint8_t> voxels(static_cast<size_t>(n) * n * n * 4u, 0u);
    for (uint32_t z = 0; z < n; ++z)
    {
        for (uint32_t y = 0; y < n; ++y)
        {
            for (uint32_t x = 0; x < n; ++x)
            {
                for (uint32_t c = 0; c < 4; ++c)
                {
                    float sum = 0.0f;
                    float amp = 0.55f;
                    float norm = 0.0f;
                    uint32_t scale = 1u;
                    for (uint32_t octave = 0; octave < 4; ++octave)
                    {
                        const uint32_t h = PcgHash(
                            ((x * scale + c * 11u) * 1973u) ^
                            ((y * scale + octave * 17u) * 9277u) ^
                            ((z * scale + c * 29u) * 26699u) ^
                            (0xA511E9B3u + octave * 0x9E3779B9u));
                        sum += (static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777216.0f)) * amp;
                        norm += amp;
                        amp *= 0.5f;
                        scale *= 2u;
                    }
                    const float v = Clamp01(sum / std::max(norm, 0.0001f));
                    voxels[indexOf(x, y, z, c)] = static_cast<uint8_t>(std::round(v * 255.0f));
                }
            }
        }
    }

    UploadTexture3D(
        m_Device,
        m_DensityNoiseTexture,
        voxels.data(),
        n,
        n,
        n,
        n * 4u,
        n * n * 4u,
        "VolumetricFog.DensityNoiseAtlasUpload");
    return true;
}

bool VolumetricFogRenderer::CreatePipelines()
{
    // Each pass keeps the superset's bindings but takes its OWN image shapes:
    // binding 3 is a sampler3D in the temporal pass and a samplerCube in the
    // media pass, and a WebGPU layout has to say which. Reflection is the only
    // place that knows, so the meta rides back with the SPIR-V.
    // The device picks the shader source once; the loaders below carry it by value.
    const ShaderSourceKind sourceKind = m_Device->PreferredShaderSource();
    auto loadCompute = [sourceKind](const char* path, PipelineDesc& out, ShaderMeta& outMeta) -> bool {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg(path, sourceKind, pkg, &err))
            return false;
        auto it = pkg.stageBytes.find("cs");
        if (it == pkg.stageBytes.end() || it->second.empty())
            return false;
        out = {};
        out.type = PipelineType::Compute;
        out.computeShader = it->second;
        outMeta = std::move(pkg.meta);
        return true;
    };

    auto loadGraphics = [sourceKind](const char* path, PipelineDesc& out, bool blend,
                                     ShaderMeta& outMeta) -> bool {
        ShaderPackage pkg{};
        std::string err;
        if (!LoadShaderPkg(path, sourceKind, pkg, &err))
            return false;
        auto vs = pkg.stageBytes.find("vs");
        auto fs = pkg.stageBytes.find("fs");
        if (vs == pkg.stageBytes.end() || fs == pkg.stageBytes.end() ||
            vs->second.empty() || fs->second.empty())
            return false;
        out = {};
        out.type = PipelineType::Graphics;
        out.vertexShader = vs->second;
        out.pixelShader = fs->second;
        out.rasterizationSamples = 1;
        out.EnableDepthTest(false);
        out.SetCullingMode(CullModeFlagBits::None);
        if (blend)
        {
            out.EnableBlending(true, BlendFactor::One, BlendFactor::SrcAlpha);
            // RGB remains scattering + scene * transmittance. Alpha is an
            // internal transmittance mask consumed by the shared fog-glow
            // stage, so multiply the destination alpha by the same T.
            auto& attachment = out.colorBlendState.attachments[0];
            attachment.srcAlphaBlendFactor = BlendFactor::Zero;
            attachment.dstAlphaBlendFactor = BlendFactor::SrcAlpha;
        }
        out.AddDynamicState(DynamicState::Viewport);
        out.AddDynamicState(DynamicState::Scissor);
        outMeta = std::move(pkg.meta);
        return true;
    };

    ShaderMeta cullMeta{}, mediaMeta{}, lightMeta{}, filterMeta{}, integrateMeta{}, temporalMeta{}, compositeMeta{};
    // The brick light lists are an optimisation over scanning every light, and
    // a zero cluster header selects that scan. Keep the cull package out of the
    // required set so a platform that cannot cook it still renders fog.
    const bool cullOk = loadCompute("Shaders/volumetric_fog_cull.shaderpkg", m_CullPipeline, cullMeta);
    const bool ok =
        loadCompute("Shaders/volumetric_fog_media.shaderpkg", m_MediaPipeline, mediaMeta) &&
        loadCompute("Shaders/volumetric_fog_light.shaderpkg", m_LightingPipeline, lightMeta) &&
        loadCompute("Shaders/volumetric_fog_filter.shaderpkg", m_FilterPipeline, filterMeta) &&
        loadCompute("Shaders/volumetric_fog_integrate.shaderpkg", m_IntegratePipeline, integrateMeta) &&
        loadCompute("Shaders/volumetric_fog_temporal.shaderpkg", m_TemporalPipeline, temporalMeta) &&
        loadGraphics("Shaders/volumetric_fog_composite.shaderpkg", m_CompositePipeline, true,
                     compositeMeta);

    if (!ok && !m_WarnedShaderLoad)
    {
        LOG_WARNING("VolumetricFogRenderer: shader packages are missing; rebuild shader packages.");
        m_WarnedShaderLoad = true;
    }
    if (ok && !cullOk && !m_WarnedCullShaderLoad)
    {
        LOG_WARNING("VolumetricFogRenderer: volumetric_fog_cull.shaderpkg is missing; fog lighting "
                    "scans every light. Rebuild shader packages to restore the brick light lists.");
        m_WarnedCullShaderLoad = true;
    }

    if (ok)
    {
        // One layout per pass, holding exactly the bindings that pass's shader
        // declares, shaped from its own reflection. The superset cannot serve
        // all five: binding 3 is a samplerCube in the media pass and a
        // sampler3D in the temporal one, and binding 14 is a cube only in
        // lighting. The pass lambdas keep sharing one write list but gate each
        // write on the pass's own layout (LayoutGatedWriter), so a binding a
        // pass does not declare is never written — the device asserts on
        // undeclared writes rather than dropping them.
        auto shaped = [this](const ShaderMeta& meta) {
            DescriptorSetLayoutDesc layout = m_ComputeLayout;
            const DescriptorSetMeta* setMeta = nullptr;
            for (const auto& set : meta.Sets)
            {
                if (set.Set == 0)
                {
                    setMeta = &set;
                    break;
                }
            }
            if (setMeta)
            {
                std::vector<DescriptorBinding> declared;
                declared.reserve(setMeta->Bindings.size());
                for (const auto& binding : layout.bindings)
                {
                    const bool used = std::any_of(
                        setMeta->Bindings.begin(), setMeta->Bindings.end(),
                        [&](const DescriptorBindingMeta& bm) { return bm.Binding == binding.binding; });
                    if (used)
                        declared.push_back(binding);
                }
                layout.bindings = std::move(declared);
            }
            ApplyMetaImageShapeToLayout(meta, layout);
            return layout;
        };

        if (cullOk)
        {
            m_CullLayout = shaped(cullMeta);
            m_CullPipeline.descriptorSetLayouts.push_back(m_CullLayout);
            m_CullPipeline.debugName = "VolumetricFog.LightCull";
        }
        m_MediaLayout = shaped(mediaMeta);
        m_LightingLayout = shaped(lightMeta);
        m_FilterLayout = shaped(filterMeta);
        m_IntegrateLayout = shaped(integrateMeta);
        m_TemporalLayout = shaped(temporalMeta);

        m_MediaPipeline.descriptorSetLayouts.push_back(m_MediaLayout);
        m_MediaPipeline.debugName = "VolumetricFog.Media";
        m_LightingPipeline.descriptorSetLayouts.push_back(m_LightingLayout);
        m_LightingPipeline.debugName = "VolumetricFog.Lighting";
        m_FilterPipeline.descriptorSetLayouts.push_back(m_FilterLayout);
        m_FilterPipeline.debugName = "VolumetricFog.Filter";
        m_IntegratePipeline.descriptorSetLayouts.push_back(m_IntegrateLayout);
        m_IntegratePipeline.debugName = "VolumetricFog.Integrate";
        m_TemporalPipeline.descriptorSetLayouts.push_back(m_TemporalLayout);
        m_TemporalPipeline.debugName = "VolumetricFog.Temporal";
        // The composite reads the froxel volume as a 3D texture; its layout
        // needs the same shape treatment as the compute passes.
        ApplyMetaImageShapeToLayout(compositeMeta, m_CompositeLayout);
        m_CompositePipeline.descriptorSetLayouts.push_back(m_CompositeLayout);
        m_CompositePipeline.debugName = "VolumetricFog.Composite";
    }
    return ok;
}

GraphicsPipelineId VolumetricFogRenderer::EnsureCompositePipeline(IDevice& device)
{
    if (!m_CompositePipelineId.IsValid())
        m_CompositePipelineId = PipelineDescTranslator::InternGraphics(device, m_CompositePipeline);
    return m_CompositePipelineId;
}

void VolumetricFogRenderer::SetSettings(ViewId viewId, const VolumetricFogSettings& settings)
{
    auto& state = m_PerView[viewId];
    const bool wasActive = IsFogActive(state.settings);
    const bool nowActive = IsFogActive(settings);
    const bool canUseTemporal = nowActive && settings.temporalEnabled && settings.temporalBlend > 0.0f;
    const bool temporalReenabled = !state.settings.temporalEnabled && settings.temporalEnabled;

    if (!canUseTemporal || !wasActive || temporalReenabled || !IsTemporalCompatible(state.settings, settings))
        state.historyValid = false;

    state.settings = settings;
}

const VolumetricFogSettings& VolumetricFogRenderer::GetSettings(ViewId viewId) const
{
    auto it = m_PerView.find(viewId);
    return it != m_PerView.end() ? it->second.settings : m_DefaultSettings;
}

void VolumetricFogRenderer::SetEnabled(ViewId viewId, bool enabled)
{
    auto settings = GetSettings(viewId);
    settings.enabled = enabled;
    SetSettings(viewId, settings);
}

bool VolumetricFogRenderer::IsEnabled(ViewId viewId) const
{
    return GetSettings(viewId).enabled;
}

void VolumetricFogRenderer::ResetHistory(ViewId viewId)
{
    auto it = m_PerView.find(viewId);
    if (it == m_PerView.end())
        return;

    auto& state = it->second;
    state.historyValid = false;
    state.historyFrame = 0;
    MakeIdentity(state.previousViewProj);
    state.previousCameraPos[0] = 0.0f;
    state.previousCameraPos[1] = 0.0f;
    state.previousCameraPos[2] = 0.0f;
    state.previousCameraPos[3] = 1.0f;
}

BufferHandle VolumetricFogRenderer::GetOrCreateUBO(PerViewState& state, uint32_t slot)
{
    slot %= IDevice::kMaxSupportedFramesInFlight;
    if (!state.ubo[slot].IsValid())
    {
        BufferDesc bd{};
        bd.size = sizeof(FogGpuParams);
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "VolumetricFog.Params";
        state.ubo[slot] = m_Device->CreateBuffer(bd);
    }
    return state.ubo[slot];
}

BufferHandle VolumetricFogRenderer::GetOrCreateLocalVolumeBuffer(PerViewState& state, uint32_t slot)
{
    slot %= IDevice::kMaxSupportedFramesInFlight;
    if (!state.localVolumeBuffer[slot].IsValid())
    {
        BufferDesc bd{};
        bd.size = sizeof(FogLocalVolumeBufferGpu);
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "VolumetricFog.LocalVolumeBuffer";
        state.localVolumeBuffer[slot] = m_Device->CreateBuffer(bd);
    }
    return state.localVolumeBuffer[slot];
}

BufferHandle VolumetricFogRenderer::GetZeroShadowBuffer(IDevice* dev)
{
    if (!m_ZeroShadowBuffer.IsValid() && dev)
    {
        BufferDesc bd{};
        bd.size = sizeof(ShadowDataGPU);
        bd.usage = static_cast<uint32_t>(BufferUsage::Uniform);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "VolumetricFog.ZeroShadowData";
        m_ZeroShadowBuffer = dev->CreateBuffer(bd);
        ShadowDataGPU zero{};
        dev->UpdateBuffer(m_ZeroShadowBuffer, 0, sizeof(zero), &zero);
    }
    return m_ZeroShadowBuffer;
}

BufferHandle VolumetricFogRenderer::GetZeroStorageBuffer(IDevice* dev)
{
    if (!m_ZeroStorageBuffer.IsValid() && dev)
    {
        // Also serves as a single disabled point-shadow slot. A zero light
        // header must mean no lights; a count of one would read a nonexistent
        // packed light when the view has no LightBuffer.
        uint32_t zero[sizeof(PointShadowSlotGPU) / sizeof(uint32_t)]{};
        BufferDesc bd{};
        bd.size = sizeof(zero);
        bd.usage = static_cast<uint32_t>(BufferUsage::Storage);
        bd.memoryUsage = BufferMemoryUsage::Upload;
        bd.flags = BufferCreateFlags::PersistentlyMapped;
        bd.debugName = "VolumetricFog.ZeroStorage";
        m_ZeroStorageBuffer = dev->CreateBuffer(bd);
        dev->UpdateBuffer(m_ZeroStorageBuffer, 0, sizeof(zero), &zero);
    }
    return m_ZeroStorageBuffer;
}

void VolumetricFogRenderer::FillParams(
    RenderServices& services,
    ViewId viewId,
    const CameraData& camera,
    const VolumetricFogGrid& grid,
    float effectiveMaxDistance,
    uint32_t renderWidth,
    uint32_t renderHeight,
    FogGpuParams& out) const
{
    const auto& s = GetSettings(viewId);
    Mathematics::Matrix4x4 vp{};
    std::memcpy(vp.Data(), camera.viewProj, sizeof(float) * 16);
    Mathematics::Matrix4x4 inv = Mathematics::Inverse(vp);

    CopyMatrix(out.invViewProj, inv.Data());
    CopyMatrix(out.view, camera.view);
    out.cameraPos[0] = camera.cameraPos[0];
    out.cameraPos[1] = camera.cameraPos[1];
    out.cameraPos[2] = camera.cameraPos[2];
    out.cameraPos[3] = 1.0f;

    auto stateIt = m_PerView.find(viewId);
    const float* sunDir = stateIt != m_PerView.end() ? stateIt->second.currentSunDirection : nullptr;
    const float* sunColor = stateIt != m_PerView.end() ? stateIt->second.currentSunColor : nullptr;
    const float sunIntensity = stateIt != m_PerView.end() ? stateIt->second.currentSunIntensity : 1.0f;
    const float fallbackSunDir[3] = {0.35f, -0.65f, 0.68f};
    const float fallbackSunColor[3] = {1.0f, 0.86f, 0.62f};
    if (!sunDir)
        sunDir = fallbackSunDir;
    if (!sunColor)
        sunColor = fallbackSunColor;

    out.sunDirection[0] = sunDir[0];
    out.sunDirection[1] = sunDir[1];
    out.sunDirection[2] = sunDir[2];
    out.sunDirection[3] = 0.0f;
    out.sunColor[0] = sunColor[0] * s.sunScatteringTint[0] * sunIntensity;
    out.sunColor[1] = sunColor[1] * s.sunScatteringTint[1] * sunIntensity;
    out.sunColor[2] = sunColor[2] * s.sunScatteringTint[2] * sunIntensity;
    out.sunColor[3] = sunIntensity;
    float ambient[3]{};
    ResolveFogAmbient(services, s, ambient);
    out.ambientColor[0] = ambient[0];
    out.ambientColor[1] = ambient[1];
    out.ambientColor[2] = ambient[2];
    // When the engine-shared IBL cube is live, the lighting pass samples it for
    // indirect in-scatter and uses .w as the intensity scale; otherwise the
    // shader keeps the flat ambient fallback (see uDensityParams.w below).
    float iblIntensity = 0.0f;
    bool iblEnabled = false;
    if (auto* ibl = services.GetFeature<ImageBasedLightingFeature>(); ibl && ibl->IsInitialized())
    {
        iblEnabled = true;
        iblIntensity = std::max(ibl->GetIblIntensity(), 0.0f);
    }
    out.ambientColor[3] = iblIntensity;
    out.albedoDensity[0] = s.albedo[0];
    out.albedoDensity[1] = s.albedo[1];
    out.albedoDensity[2] = s.albedo[2];
    out.albedoDensity[3] = std::max(s.density, 0.0f);
    out.emissionAnisotropy[0] = s.emission[0];
    out.emissionAnisotropy[1] = s.emission[1];
    out.emissionAnisotropy[2] = s.emission[2];
    out.emissionAnisotropy[3] = std::clamp(s.anisotropy, -0.95f, 0.95f);
    out.heightParams[0] = s.baseHeight;
    out.heightParams[1] = std::max(s.heightFalloff, 0.01f);
    out.heightParams[2] = (stateIt != m_PerView.end() && stateIt->second.hasLocalFogLights) ? 1.0f : 0.0f;
    out.heightParams[3] = s.isGlobal ? 1.0f : 0.0f;
    out.localVolumeCenterShape[0] = s.localVolumeCenter[0];
    out.localVolumeCenterShape[1] = s.localVolumeCenter[1];
    out.localVolumeCenterShape[2] = s.localVolumeCenter[2];
    out.localVolumeCenterShape[3] = static_cast<float>(s.localVolumeShape);
    out.localVolumeAxisXHalfExtent[0] = s.localVolumeAxisX[0];
    out.localVolumeAxisXHalfExtent[1] = s.localVolumeAxisX[1];
    out.localVolumeAxisXHalfExtent[2] = s.localVolumeAxisX[2];
    out.localVolumeAxisXHalfExtent[3] = std::max(s.localVolumeHalfExtents[0], 0.001f);
    out.localVolumeAxisYHalfExtent[0] = s.localVolumeAxisY[0];
    out.localVolumeAxisYHalfExtent[1] = s.localVolumeAxisY[1];
    out.localVolumeAxisYHalfExtent[2] = s.localVolumeAxisY[2];
    out.localVolumeAxisYHalfExtent[3] = std::max(s.localVolumeHalfExtents[1], 0.001f);
    out.localVolumeAxisZHalfExtent[0] = s.localVolumeAxisZ[0];
    out.localVolumeAxisZHalfExtent[1] = s.localVolumeAxisZ[1];
    out.localVolumeAxisZHalfExtent[2] = s.localVolumeAxisZ[2];
    out.localVolumeAxisZHalfExtent[3] = std::max(s.localVolumeHalfExtents[2], 0.001f);
    out.localVolumeParams[0] = std::max(s.localVolumeBlendDistance, 0.0f);
    out.localVolumeParams[1] = (!s.isGlobal && s.localVolumeValid) ? 1.0f : 0.0f;
    out.localVolumeParams[2] = std::clamp(s.skyFade, 0.0f, 1.0f);
    out.localVolumeParams[3] = 0.0f;
    out.noiseParams[0] = s.noiseEnabled ? 1.0f : 0.0f;
    out.noiseParams[1] = std::max(s.noiseScale, 0.01f);
    out.noiseParams[2] = std::clamp(s.noiseStrength, 0.0f, 1.0f);
    out.noiseParams[3] = std::max(s.noiseContrast, 0.01f);
    out.noiseVelocityTime[0] = s.noiseVelocity[0];
    out.noiseVelocityTime[1] = s.noiseVelocity[1];
    out.noiseVelocityTime[2] = s.noiseVelocity[2];
    out.noiseVelocityTime[3] = services.GetShaderAnimationTimeSeconds();
    out.noiseChannelWeights[0] = std::max(s.noiseChannelWeights[0], 0.0f);
    out.noiseChannelWeights[1] = std::max(s.noiseChannelWeights[1], 0.0f);
    out.noiseChannelWeights[2] = std::max(s.noiseChannelWeights[2], 0.0f);
    out.noiseChannelWeights[3] = std::max(s.noiseChannelWeights[3], 0.0f);
    out.densityParams[0] = std::clamp(s.densityThreshold, 0.0f, 1.0f);
    out.densityParams[1] = std::max(s.densityThresholdSoftness, 0.0001f);
    out.densityParams[2] = IsOrthographicProjectionLH_ZO(camera.proj) ? 1.0f : 0.0f;
    out.densityParams[3] = iblEnabled ? 1.0f : 0.0f;
    out.volumeParams[0] = std::max(effectiveMaxDistance, 0.01f);
    out.volumeParams[1] = std::max(s.depthDistribution, 0.05f);
    out.volumeParams[2] = s.temporalEnabled ? std::clamp(s.temporalBlend, 0.0f, 0.99f) : 0.0f;
    out.volumeParams[3] = std::clamp(s.jitterStrength, 0.0f, 1.0f);
    out.gridParams[0] = static_cast<float>(grid.width);
    out.gridParams[1] = static_cast<float>(grid.height);
    out.gridParams[2] = static_cast<float>(grid.slices);
    out.gridParams[3] = 1.0f / static_cast<float>(std::max(grid.slices, 1u));
    const uint64_t fogFrameIndex =
        s.jitterMotion && stateIt != m_PerView.end() ? stateIt->second.frameCounter : 0ull;
    out.jitterParams[0] = static_cast<float>(fogFrameIndex & 1023ull);
    const bool historyValid = stateIt != m_PerView.end() && stateIt->second.historyValid;
    out.jitterParams[1] = historyValid ? 1.0f : 0.0f;
    out.jitterParams[2] = static_cast<float>(std::max(renderWidth, 1u));
    out.jitterParams[3] = static_cast<float>(std::max(renderHeight, 1u));
    out.fogExtraParams[0] = s.compositeDepthBias;
    out.fogExtraParams[1] = s.shadowBias;
    // .z reserved — glow runs as the shared post-fog multiscale pyramid (FogGlow*).
    // .w says whether the emission grid carries anything the uniform does not.
    out.fogExtraParams[2] = 0.0f;
    out.fogExtraParams[3] = VolumesModifyEmission(s.localVolumes) ? 1.0f : 0.0f;
    if (historyValid)
    {
        CopyMatrix(out.prevViewProj, stateIt->second.previousViewProj);
        out.prevCameraPos[0] = stateIt->second.previousCameraPos[0];
        out.prevCameraPos[1] = stateIt->second.previousCameraPos[1];
        out.prevCameraPos[2] = stateIt->second.previousCameraPos[2];
        out.prevCameraPos[3] = 1.0f;
    }
    else
    {
        MakeIdentity(out.prevViewProj);
        out.prevCameraPos[0] = camera.cameraPos[0];
        out.prevCameraPos[1] = camera.cameraPos[1];
        out.prevCameraPos[2] = camera.cameraPos[2];
        out.prevCameraPos[3] = 1.0f;
    }
}

void VolumetricFogRenderer::DeclareForView(Pipeline::ViewDeclare& d,
                                           RenderGraph::RGTexture sceneColor,
                                           RenderGraph::RGTexture depth)
{
    using namespace ::GameEngine::Rendering;
    auto& services = d.Services;
    const ViewId viewId = d.View.id;

    const CameraData* camera = services.Views().FindCameraData(d.View.cameraId);
    if (!camera || !sceneColor.IsValid() || !depth.IsValid())
        return;

    // THE extent for grid sizing — the same numbers the old path derived from
    // the color target desc.
    const uint32_t renderW = std::max(d.RenderWidth, 1u);
    const uint32_t renderH = std::max(d.RenderHeight, 1u);

    // ── FillParams sequencing, AT DECLARATION (old AddPassesForView head) ──
    auto& state = m_PerView[viewId];
    const ResolvedFogSun sun = ResolveFogSunLighting(services, viewId, state.settings);
    state.hasLocalFogLights = HasActiveLocalFogLights(services, viewId);
    if (state.historyValid &&
        SunLightingChangedEnough(state.historySunDirection, state.historySunColor,
                                 state.historySunIntensity, sun))
    {
        state.historyValid = false;
    }
    std::memcpy(state.currentSunDirection, sun.direction, sizeof(state.currentSunDirection));
    std::memcpy(state.currentSunColor, sun.color, sizeof(state.currentSunColor));
    state.currentSunIntensity = sun.intensity;

    state.effectiveMaxDistance = ComputeEffectiveMaxDistance(state.settings, *camera);
    if (state.historyValid &&
        std::abs(state.historyMaxDistance - state.effectiveMaxDistance) >
            std::max(1.0f, state.historyMaxDistance * 0.10f))
    {
        state.historyValid = false;
    }
    state.grid = ComputeVolumetricFogGrid(renderW, renderH, state.settings);
    const VolumetricFogGrid grid = state.grid;
    if (state.historyGrid.width != grid.width || state.historyGrid.height != grid.height ||
        state.historyGrid.slices != grid.slices)
    {
        state.historyValid = false;
        state.historyGrid = grid;
    }

    // ── Two upload-ring allocs, WRITTEN NOW (the FiF UBO/local-volume rings
    // and GetOrCreate* dissolve — host-coherent, no declared edges). ──
    const auto paramsUp = d.Frame.AllocUpload<FogGpuParams>();
    const auto volumeUp = d.Frame.AllocUpload<FogLocalVolumeBufferGpu>();
    if (!paramsUp.Valid() || !volumeUp.Valid())
        return;
    FillParams(services, viewId, *camera, state.grid, state.effectiveMaxDistance, renderW, renderH,
               *paramsUp.Ptr);

    FogLocalVolumeBufferGpu& volumeGpu = *volumeUp.Ptr;
    volumeGpu = {};
    const uint32_t volumeCount = std::min<uint32_t>(
        static_cast<uint32_t>(state.settings.localVolumes.size()), kMaxVolumetricFogLocalVolumes);
    volumeGpu.header[0] = volumeCount;
    for (uint32_t i = 0; i < volumeCount; ++i)
    {
        const auto& src = state.settings.localVolumes[i];
        auto& dst = volumeGpu.volumes[i];
        dst.centerShape[0] = src.center[0];
        dst.centerShape[1] = src.center[1];
        dst.centerShape[2] = src.center[2];
        dst.centerShape[3] = static_cast<float>(src.shape);
        for (int c = 0; c < 3; ++c)
        {
            dst.axisXHalfExtent[c] = src.axisX[c];
            dst.axisYHalfExtent[c] = src.axisY[c];
            dst.axisZHalfExtent[c] = src.axisZ[c];
            dst.albedo[c] = std::clamp(src.albedo[c], 0.0f, 8.0f);
            dst.emission[c] = std::max(src.emission[c], 0.0f);
            dst.gradientLow[c] = std::max(src.gradientLowTint[c], 0.0f);
            dst.gradientHigh[c] = std::max(src.gradientHighTint[c], 0.0f);
        }
        dst.axisXHalfExtent[3] = std::max(src.halfExtents[0], 0.001f);
        dst.axisYHalfExtent[3] = std::max(src.halfExtents[1], 0.001f);
        dst.axisZHalfExtent[3] = std::max(src.halfExtents[2], 0.001f);
        dst.params0[0] = std::max(src.blendDistance, 0.0f);
        dst.params0[1] = src.enabled ? std::max(src.weight, 0.0f) : 0.0f;
        dst.params0[2] = std::max(src.density, 0.0f);
        dst.params0[3] = static_cast<float>(src.densityMode);
        dst.params1[0] = std::clamp(src.densityThreshold, 0.0f, 1.0f);
        dst.params1[1] = std::max(src.densityThresholdSoftness, 0.0001f);
        dst.params1[2] = static_cast<float>(src.gradientMode);
        dst.params1[3] = std::clamp(src.gradientStrength, 0.0f, 1.0f);
    }

    const BufferHandle uboBuf = paramsUp.Buffer;
    const uint64_t uboOffset = paramsUp.Offset;
    const BufferHandle volumeBuf = volumeUp.Buffer;
    const uint64_t volumeOffset = volumeUp.Offset;

    // ── Froxel grids: frame transients (3D, RGBA16F), computed ONCE. ──
    TextureDesc gridDesc{};
    gridDesc.width = grid.width;
    gridDesc.height = grid.height;
    gridDesc.depth = grid.slices;
    gridDesc.arrayLayers = 1;
    gridDesc.mipLevels = 1;
    gridDesc.sampleCount = 1;
    gridDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    gridDesc.usage =
        static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::UnorderedAccess);

    const std::string base = "VolumetricFog.View" + std::to_string(static_cast<uint32_t>(viewId));
    gridDesc.debugName = "VolumetricFog.Media";
    const RenderGraph::RGTexture mediaTex = d.Frame.CreateTexture((base + ".Media").c_str(), gridDesc);
    // Without a volume that changes emission the grid would hold the view's own
    // emission in every texel, which the lighting pass already has as a uniform.
    // Allocating it anyway costs a full rgba16f froxel grid every frame.
    const bool emissionGridActive = VolumesModifyEmission(state.settings.localVolumes);
    TextureDesc emissionDesc = gridDesc;
    if (!emissionGridActive)
        emissionDesc.width = emissionDesc.height = emissionDesc.depth = 1;
    emissionDesc.debugName = "VolumetricFog.Emission";
    const auto emissionTex = d.Frame.CreateTexture((base + ".Emission").c_str(), emissionDesc);
    gridDesc.debugName = "VolumetricFog.Lighting";
    const RenderGraph::RGTexture lightingTex = d.Frame.CreateTexture((base + ".Lighting").c_str(), gridDesc);
    const bool filterEnabled = state.settings.filterEnabled;
    RenderGraph::RGTexture filteredTex{};
    if (filterEnabled)
    {
        gridDesc.debugName = "VolumetricFog.Filtered";
        filteredTex = d.Frame.CreateTexture((base + ".Filtered").c_str(), gridDesc);
    }
    gridDesc.debugName = "VolumetricFog.Integrated";
    const RenderGraph::RGTexture integratedTex =
        d.Frame.CreateTexture((base + ".Integrated").c_str(), gridDesc);

    // ── History: pool imports, parity = state.historyFrame & 1; temporal-off
    // frames import nothing (the pool ages out). No first-frame clear — the
    // CPU historyValid flag masks invalid history on every realloc path. ──
    const bool temporalEnabled =
        state.settings.temporalEnabled && state.settings.temporalBlend > 0.0f;
    RenderGraph::RGTexture historyWriteTex{};
    RenderGraph::RGTexture historyReadTex{};
    if (temporalEnabled)
    {
        TextureDesc historyDesc = gridDesc;
        historyDesc.persistent = true;
        historyDesc.debugName = "VolumetricFog.History";
        const uint32_t historyIndex = static_cast<uint32_t>(state.historyFrame & 1ull);
        const uint32_t previousHistoryIndex = historyIndex ^ 1u;
        const std::string writeName = base + ".History" + std::to_string(historyIndex);
        const std::string readName = base + ".History" + std::to_string(previousHistoryIndex);
        historyWriteTex = d.Frame.ImportPersistentTexture(writeName.c_str(), historyDesc);
        historyReadTex = d.Frame.ImportPersistentTexture(readName.c_str(), historyDesc);
    }

    // Upload-ring light data is frame-local. Fog's spatial lists cover the
    // full froxel grid, including empty space beyond opaque surface depth.
    const auto shadowDataB = d.ResolveBuffer(Pipeline::Names::Res::ShadowData);
    const auto lightB = d.ResolveBuffer(Pipeline::Names::Res::LightBuffer);

    // The cascade array via the consume-only accessor (valid only when a
    // cascade producer declared into this frame). NEVER import the feature's
    // shadow map texture as external, never pool-import the array name.
    const RenderGraph::RGTexture shadowArr = services.GetShadowMapArrayRG(d.Frame, viewId);

    // Engine-shared diffuse irradiance cube for IBL in-scatter. Import-by-handle
    // dedups onto the same RG resource the IBL bake wrote, so the Lighting pass's
    // Sampled read orders fog after any rebake this frame (no stale/hazard read).
    RenderGraph::RGTexture iblIrradianceTex{};
    SamplerHandle iblCubeSampler{};
    if (auto* ibl = services.GetFeature<ImageBasedLightingFeature>(); ibl && ibl->IsInitialized())
    {
        iblIrradianceTex = d.Frame.ImportExternalTexture(
            "IBL_Irradiance", ibl->GetIrradianceCube(), ResourceState::ShaderResource,
            TextureFormat::R16G16B16A16_FLOAT, 1, ImageBasedLightingFeature::kNumCaptureFaces);
        iblCubeSampler = ibl->GetCubeSampler();
    }

    const std::string passBase =
        "VolumetricFog[" + std::to_string(static_cast<uint32_t>(viewId)) + "]";

    const uint32_t gw = grid.width, gh = grid.height, gs = grid.slices;

    RenderGraph::RGBuffer fogLightIndices{}, fogLightClusters{};
    if (lightB.IsValid())
    {
        const uint32_t cx = DivCeil(gw, kComputeGroup), cy = DivCeil(gh, kComputeGroup);
        const uint32_t cz = DivCeil(gs, kComputeGroup);
        const size_t clusters = static_cast<size_t>(cx) * cy * cz;
        BufferDesc indexDesc{};
        indexDesc.size = clusters * kFogLightListCapacity * sizeof(uint32_t);
        indexDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
        indexDesc.memoryUsage = BufferMemoryUsage::DeviceLocal;
        indexDesc.debugName = "VolumetricFog.LightIndices";
        fogLightIndices = d.Frame.CreateBuffer((base + ".LightIndices").c_str(), indexDesc);
        BufferDesc clusterDesc = indexDesc;
        clusterDesc.size = 4u * sizeof(uint32_t) + clusters * 2u * sizeof(uint32_t);
        // AddBufferZeroInit fills this through a transfer, so the desc must say so.
        clusterDesc.usage |= static_cast<uint32_t>(BufferUsage::TransferDst);
        clusterDesc.debugName = "VolumetricFog.LightClusters";
        fogLightClusters = d.Frame.CreateBuffer((base + ".LightClusters").c_str(), clusterDesc);
        // A zero header selects the full-light scan if the cull dispatch fails.
        d.Frame.AddBufferZeroInit(fogLightClusters, (passBase + ".ClearLightClusters").c_str());
        d.Frame.AddPass((passBase + ".LightCull").c_str(), Rendering::PassPhase::kDefault,
            [&](RenderGraph::RGPassBuilder& p)
            {
                if (lightB.Graph.IsValid())
                    p.Read(lightB.Graph, RenderGraph::RGBufferRead::Storage);
                p.Write(fogLightIndices, RenderGraph::RGBufferWrite::Storage);
                p.Write(fogLightClusters, RenderGraph::RGBufferWrite::Storage);
            },
            [this, fogLightIndices, fogLightClusters, lightB, uboBuf, uboOffset, cx, cy, cz]
            (RenderGraph::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                if (!dev || !ctx.Cmd)
                    return;
                const auto pipe = ctx.GetOrCreatePipelineVariant(
                    EnsureComputePipeline(*dev, m_CullPipeline, m_CullPipelineId));
                if (!pipe.IsValid())
                    return;
                DescriptorSetDesc desc{};
                desc.layout = m_CullLayout;
                desc.debugName = "VolumetricFog.LightCull.DS";
                desc.transient = true;
                auto ds = dev->CreateDescriptorSet(desc);
                dev->UpdateBufferBinding(ds, 2, uboBuf, uboOffset, sizeof(FogGpuParams));
                dev->UpdateStorageBufferBinding(ds, 6, ctx.GetBuffer(fogLightIndices), 0, 0);
                dev->UpdateStorageBufferBinding(ds, 7, ctx.GetBuffer(fogLightClusters), 0, 0);
                dev->UpdateStorageBufferBinding(ds, 8, lightB.Buffer, static_cast<size_t>(lightB.Offset), 0);
                ctx.Cmd->SetPipeline(pipe);
                ctx.Cmd->BindDescriptorSet(0, ds, pipe);
                ctx.Cmd->Dispatch(cx, cy, cz);
            });
    }

    // ── Media ──
    d.Frame.AddPass(
        (passBase + ".Media").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Write(mediaTex, RenderGraph::RGTextureWrite::Storage);
            p.Write(emissionTex, RenderGraph::RGTextureWrite::Storage);
        },
        [this, mediaTex, emissionTex, uboBuf, uboOffset, volumeBuf, volumeOffset, gw, gh, gs](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto mediaPhysical = ctx.GetTexture(mediaTex);
            const auto emissionPhysical = ctx.GetTexture(emissionTex);
            if (!mediaPhysical.IsValid() || !emissionPhysical.IsValid())
                return;
            const auto pipe = ctx.GetOrCreatePipelineVariant(
                EnsureComputePipeline(*dev, m_MediaPipeline, m_MediaPipelineId));
            if (!pipe.IsValid())
                return;
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_MediaLayout;
            dsDesc.debugName = "VolumetricFog.Media.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            LayoutGatedWriter w(*dev, ds, m_MediaLayout);
            w.StorageImage(0, mediaPhysical);
            w.StorageImage(15, emissionPhysical);
            w.CombinedImageSampler(1, mediaPhysical, m_LinearClampSampler);
            w.UniformBuffer(2, uboBuf, uboOffset, sizeof(FogGpuParams));
            w.CombinedImageSampler(11, m_JitterAtlasTexture, m_JitterAtlasSampler);
            w.StorageBuffer(12, volumeBuf, volumeOffset, sizeof(FogLocalVolumeBufferGpu));
            w.CombinedImageSampler(13, m_DensityNoiseTexture, m_DensityNoiseSampler);
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(DivCeil(gw, kComputeGroup), DivCeil(gh, kComputeGroup),
                         DivCeil(gs, kComputeGroup));
        });

    // ── Lighting ──
    SamplerHandle shadowSampler{};
    if (auto* shadowFeature = services.GetFeature<ShadowMapRenderFeature>())
        shadowSampler = shadowFeature->GetShadowSampler();
    // volumetric_fog_light.comp declares `sampler2DArrayShadow ge_shadowMapArray`
    // at set 0 binding 5 statically, so binding 5 must be written on EVERY
    // dispatch. On a frame with no cascade producer shadowArr is invalid, and a
    // skipped write would leave the slot holding the recycled set's previous
    // occupant. The engine's typed fallback reads fully lit (reverse-Z far), the
    // same no-op answer the world pass binds.
    const TextureHandle shadowFallback = services.GetCascadeShadowFallbackTexture();
    const auto localShadows = services.GetLocalShadowInputsRG(d.Frame, viewId);
    const auto spotShadowSampler = services.GetSpotShadowSampler();
    const auto pointShadowSampler = services.GetPointShadowSampler();
    const auto spotShadowFallback = services.GetAreaShadowFallbackTexture();
    const auto pointShadowFallback = services.GetPointShadowFallbackTexture();
    d.Frame.AddPass(
        (passBase + ".Lighting").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(mediaTex, RenderGraph::RGTextureRead::SampledCompute);
            p.Read(emissionTex, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(lightingTex, RenderGraph::RGTextureWrite::Storage);
            if (fogLightIndices.IsValid())
                p.Read(fogLightIndices, RenderGraph::RGBufferRead::Storage);
            if (fogLightClusters.IsValid())
                p.Read(fogLightClusters, RenderGraph::RGBufferRead::Storage);
            if (lightB.Graph.IsValid())
                p.Read(lightB.Graph, RenderGraph::RGBufferRead::Storage);
            // The cascade array reads RAW from this frame's cascade writes, and
            // the consumer is this pass's DISPATCH: plain Sampled would scope the
            // barrier out of the cascade depth pass to the fragment stage, and
            // volumetric_fog_light.comp would sample depth no barrier made
            // visible to compute.
            if (shadowArr.IsValid())
                p.Read(shadowArr, RenderGraph::RGTextureRead::SampledCompute);
            if (localShadows.SpotMap.IsValid())
                p.Read(localShadows.SpotMap, RenderGraph::RGTextureRead::SampledCompute);
            if (localShadows.PointMap.IsValid())
                p.Read(localShadows.PointMap, RenderGraph::RGTextureRead::SampledCompute);
            // Order fog after the IBL bake (RAW on the shared irradiance cube).
            if (iblIrradianceTex.IsValid())
                p.Read(iblIrradianceTex, RenderGraph::RGTextureRead::SampledCompute);
        },
        [this, lightingTex, mediaTex, emissionTex, shadowArr, shadowSampler, shadowFallback, iblIrradianceTex,
         iblCubeSampler, uboBuf, uboOffset, volumeBuf, volumeOffset, shadowDataB, fogLightIndices,
         fogLightClusters, lightB, localShadows, spotShadowSampler,
         pointShadowSampler, spotShadowFallback, pointShadowFallback, gw, gh, gs](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto lightingPhysical = ctx.GetTexture(lightingTex);
            const auto mediaPhysical = ctx.GetTexture(mediaTex);
            const auto emissionPhysical = ctx.GetTexture(emissionTex);
            if (!lightingPhysical.IsValid() || !mediaPhysical.IsValid() || !emissionPhysical.IsValid())
                return;
            const auto pipe = ctx.GetOrCreatePipelineVariant(
                EnsureComputePipeline(*dev, m_LightingPipeline, m_LightingPipelineId));
            if (!pipe.IsValid())
                return;
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_LightingLayout;
            dsDesc.debugName = "VolumetricFog.Light.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            LayoutGatedWriter w(*dev, ds, m_LightingLayout);
            w.StorageImage(0, lightingPhysical);
            w.CombinedImageSampler(16, emissionPhysical, m_LinearClampSampler);
            w.CombinedImageSampler(1, mediaPhysical, m_LinearClampSampler);
            w.UniformBuffer(2, uboBuf, uboOffset, sizeof(FogGpuParams));

            if (shadowDataB.IsValid())
                w.UniformBuffer(4, shadowDataB.Buffer, static_cast<size_t>(shadowDataB.Offset),
                                sizeof(ShadowDataGPU));
            else
                w.UniformBuffer(4, GetZeroShadowBuffer(dev), 0, sizeof(ShadowDataGPU));

            auto shadowTex = shadowArr.IsValid() ? ctx.GetTexture(shadowArr) : TextureHandle{};
            if (!shadowTex.IsValid())
                shadowTex = shadowFallback;
            if (shadowTex.IsValid() && shadowSampler.IsValid())
                w.CombinedImageSampler(5, shadowTex, shadowSampler);

            if (iblIrradianceTex.IsValid() && iblCubeSampler.IsValid())
            {
                const auto iblTex = ctx.GetTexture(iblIrradianceTex);
                if (iblTex.IsValid())
                    w.CombinedImageSampler(14, iblTex, iblCubeSampler);
            }

            const auto spotMap = localShadows.SpotMap.IsValid()
                ? ctx.GetTexture(localShadows.SpotMap) : spotShadowFallback;
            const auto pointMap = localShadows.PointMap.IsValid()
                ? ctx.GetTexture(localShadows.PointMap) : pointShadowFallback;
            w.UniformBuffer(17,
                localShadows.SpotData.IsValid() ? localShadows.SpotData : GetZeroShadowBuffer(dev),
                static_cast<size_t>(localShadows.SpotOffset), sizeof(SpotShadowDataGPU));
            w.CombinedImageSampler(18, spotMap, spotShadowSampler);
            w.StorageBuffer(19,
                localShadows.PointData.IsValid() ? localShadows.PointData : GetZeroStorageBuffer(dev),
                static_cast<size_t>(localShadows.PointOffset),
                localShadows.PointData.IsValid() ? static_cast<size_t>(localShadows.PointBytes)
                                                 : sizeof(PointShadowSlotGPU));
            w.CombinedImageSampler(20, pointMap, pointShadowSampler);

            w.StorageBuffer(6,
                fogLightIndices.IsValid() ? ctx.GetBuffer(fogLightIndices) : GetZeroStorageBuffer(dev), 0, 0);
            w.StorageBuffer(7,
                fogLightClusters.IsValid() ? ctx.GetBuffer(fogLightClusters) : GetZeroStorageBuffer(dev), 0, 0);
            w.StorageBuffer(8,
                lightB.IsValid() ? lightB.Buffer : GetZeroStorageBuffer(dev),
                lightB.IsValid() ? static_cast<size_t>(lightB.Offset) : 0, 0);
            w.CombinedImageSampler(11, m_JitterAtlasTexture, m_JitterAtlasSampler);
            w.StorageBuffer(12, volumeBuf, volumeOffset, sizeof(FogLocalVolumeBufferGpu));
            w.CombinedImageSampler(13, m_DensityNoiseTexture, m_DensityNoiseSampler);
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(DivCeil(gw, kComputeGroup), DivCeil(gh, kComputeGroup),
                         DivCeil(gs, kComputeGroup));
        });

    // ── Filter (edge-preserving spatial denoise of scattered light) ──
    if (filterEnabled && filteredTex.IsValid())
    {
        d.Frame.AddPass(
            (passBase + ".Filter").c_str(), Rendering::PassPhase::kDefault,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(lightingTex, RenderGraph::RGTextureRead::SampledCompute);
                p.Write(filteredTex, RenderGraph::RGTextureWrite::Storage);
            },
            [this, filteredTex, lightingTex, uboBuf, uboOffset, volumeBuf, volumeOffset, gw, gh,
             gs](RenderGraph::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl)
                    return;
                const auto filteredPhysical = ctx.GetTexture(filteredTex);
                const auto lightingPhysical = ctx.GetTexture(lightingTex);
                if (!filteredPhysical.IsValid() || !lightingPhysical.IsValid())
                    return;
                const auto pipe = ctx.GetOrCreatePipelineVariant(
                    EnsureComputePipeline(*dev, m_FilterPipeline, m_FilterPipelineId));
                if (!pipe.IsValid())
                    return;
                DescriptorSetDesc dsDesc{};
                dsDesc.layout = m_FilterLayout;
                dsDesc.debugName = "VolumetricFog.Filter.DS";
                dsDesc.transient = true;
                auto ds = dev->CreateDescriptorSet(dsDesc);
                LayoutGatedWriter w(*dev, ds, m_FilterLayout);
                w.StorageImage(0, filteredPhysical);
                w.CombinedImageSampler(1, lightingPhysical, m_LinearClampSampler);
                w.UniformBuffer(2, uboBuf, uboOffset, sizeof(FogGpuParams));
                w.CombinedImageSampler(11, m_JitterAtlasTexture, m_JitterAtlasSampler);
                w.StorageBuffer(12, volumeBuf, volumeOffset, sizeof(FogLocalVolumeBufferGpu));
                w.CombinedImageSampler(13, m_DensityNoiseTexture, m_DensityNoiseSampler);
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(DivCeil(gw, kComputeGroup), DivCeil(gh, kComputeGroup),
                             DivCeil(gs, kComputeGroup));
            });
    }

    // ── Integrate ── (reads the filtered grid when filtering ran, else lighting)
    const RenderGraph::RGTexture integrateInputTex =
        (filterEnabled && filteredTex.IsValid()) ? filteredTex : lightingTex;
    d.Frame.AddPass(
        (passBase + ".Integrate").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(integrateInputTex, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(integratedTex, RenderGraph::RGTextureWrite::Storage);
        },
        [this, integratedTex, integrateInputTex, uboBuf, uboOffset, volumeBuf, volumeOffset, gw,
         gh](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto integratedPhysical = ctx.GetTexture(integratedTex);
            const auto lightingPhysical = ctx.GetTexture(integrateInputTex);
            if (!integratedPhysical.IsValid() || !lightingPhysical.IsValid())
                return;
            const auto pipe = ctx.GetOrCreatePipelineVariant(
                EnsureComputePipeline(*dev, m_IntegratePipeline, m_IntegratePipelineId));
            if (!pipe.IsValid())
                return;
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_IntegrateLayout;
            dsDesc.debugName = "VolumetricFog.Integrate.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            LayoutGatedWriter w(*dev, ds, m_IntegrateLayout);
            w.StorageImage(0, integratedPhysical);
            w.CombinedImageSampler(1, lightingPhysical, m_LinearClampSampler);
            w.UniformBuffer(2, uboBuf, uboOffset, sizeof(FogGpuParams));
            w.CombinedImageSampler(11, m_JitterAtlasTexture, m_JitterAtlasSampler);
            w.StorageBuffer(12, volumeBuf, volumeOffset, sizeof(FogLocalVolumeBufferGpu));
            w.CombinedImageSampler(13, m_DensityNoiseTexture, m_DensityNoiseSampler);
            cl->SetPipeline(pipe);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Dispatch(DivCeil(gw, kComputeGroup), DivCeil(gh, kComputeGroup), 1);
        });

    // ── Temporal (only when enabled) ──
    RenderGraph::RGTexture finalFogTex = integratedTex;
    if (temporalEnabled && historyWriteTex.IsValid() && historyReadTex.IsValid())
    {
        finalFogTex = historyWriteTex;
        d.Frame.AddPass(
            (passBase + ".Temporal").c_str(), Rendering::PassPhase::kDefault,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(integratedTex, RenderGraph::RGTextureRead::SampledCompute);
                p.Read(historyReadTex, RenderGraph::RGTextureRead::SampledCompute);
                p.Write(historyWriteTex, RenderGraph::RGTextureWrite::Storage);
            },
            [this, integratedTex, historyReadTex, historyWriteTex, uboBuf, uboOffset, volumeBuf,
             volumeOffset, gw, gh, gs](RenderGraph::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl)
                    return;
                const auto currentPhysical = ctx.GetTexture(integratedTex);
                const auto previousPhysical = ctx.GetTexture(historyReadTex);
                const auto outputPhysical = ctx.GetTexture(historyWriteTex);
                if (!currentPhysical.IsValid() || !previousPhysical.IsValid() ||
                    !outputPhysical.IsValid())
                    return;
                const auto pipe = ctx.GetOrCreatePipelineVariant(
                    EnsureComputePipeline(*dev, m_TemporalPipeline, m_TemporalPipelineId));
                if (!pipe.IsValid())
                    return;
                DescriptorSetDesc dsDesc{};
                dsDesc.layout = m_TemporalLayout;
                dsDesc.debugName = "VolumetricFog.Temporal.DS";
                dsDesc.transient = true;
                auto ds = dev->CreateDescriptorSet(dsDesc);
                LayoutGatedWriter w(*dev, ds, m_TemporalLayout);
                w.StorageImage(0, outputPhysical);
                w.CombinedImageSampler(1, currentPhysical, m_LinearClampSampler);
                w.UniformBuffer(2, uboBuf, uboOffset, sizeof(FogGpuParams));
                w.CombinedImageSampler(3, previousPhysical, m_LinearClampSampler);
                w.CombinedImageSampler(11, m_JitterAtlasTexture, m_JitterAtlasSampler);
                w.StorageBuffer(12, volumeBuf, volumeOffset, sizeof(FogLocalVolumeBufferGpu));
                w.CombinedImageSampler(13, m_DensityNoiseTexture, m_DensityNoiseSampler);
                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->Dispatch(DivCeil(gw, kComputeGroup), DivCeil(gh, kComputeGroup),
                             DivCeil(gs, kComputeGroup));
            });
    }

    // ── Composite into the scene color. AttachColor(Load) — the Load-derived
    // read IS the RAW edge from the world's write (the WorldColorReady fence
    // dies). ──
    d.Frame.AddPass(
        (passBase + ".Composite").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(finalFogTex, RenderGraph::RGTextureRead::Sampled);
            p.Read(depth, RenderGraph::RGTextureRead::Sampled);
            if (iblIrradianceTex.IsValid())
                p.Read(iblIrradianceTex, RenderGraph::RGTextureRead::Sampled);
            RenderGraph::RGAttachmentOps ops{};
            ops.Load = RenderGraph::RGLoadOp::Load;
            ops.Store = RenderGraph::RGStoreOp::Store;
            p.AttachColor(0, sceneColor, ops);
        },
        [this, finalFogTex, depth, iblIrradianceTex, iblCubeSampler, uboBuf, uboOffset, renderW,
         renderH](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl)
                return;
            const auto fogPhysical = ctx.GetTexture(finalFogTex);
            const auto depthPhysical = ctx.GetTexture(depth);
            if (!fogPhysical.IsValid() || !depthPhysical.IsValid())
                return;
            const auto pipe = ctx.GetOrCreatePipelineVariant(EnsureCompositePipeline(*dev));
            if (!pipe.IsValid())
                return;
            DescriptorSetDesc dsDesc{};
            dsDesc.layout = m_CompositeLayout;
            dsDesc.debugName = "VolumetricFog.Composite.DS";
            dsDesc.transient = true;
            auto ds = dev->CreateDescriptorSet(dsDesc);
            LayoutGatedWriter w(*dev, ds, m_CompositeLayout);
            w.CombinedImageSampler(0, fogPhysical, m_LinearClampSampler);
            w.CombinedImageSampler(1, depthPhysical, m_LinearClampSampler);
            w.UniformBuffer(2, uboBuf, uboOffset, sizeof(FogGpuParams));
            if (iblIrradianceTex.IsValid() && iblCubeSampler.IsValid())
            {
                const auto iblTex = ctx.GetTexture(iblIrradianceTex);
                if (iblTex.IsValid())
                    w.CombinedImageSampler(3, iblTex, iblCubeSampler);
            }
            cl->SetPipeline(pipe);
            cl->SetViewport(0.0f, 0.0f, static_cast<float>(renderW), static_cast<float>(renderH));
            cl->SetScissor(0, 0, renderW, renderH);
            cl->BindDescriptorSet(0, ds, pipe);
            cl->Draw(3, 1);
        });

    // ── History/state commit, AT DECLARATION (declared == will run). Exec
    // lambdas stayed read-only above; the previous-VP / frame counter / history
    // validity move here. ──
    CopyMatrix(state.previousViewProj, camera->viewProj);
    state.previousCameraPos[0] = camera->cameraPos[0];
    state.previousCameraPos[1] = camera->cameraPos[1];
    state.previousCameraPos[2] = camera->cameraPos[2];
    state.previousCameraPos[3] = 1.0f;
    ++state.frameCounter;
    state.historyValid = temporalEnabled;
    if (temporalEnabled)
    {
        state.historyMaxDistance = state.effectiveMaxDistance;
        std::memcpy(state.historySunDirection, state.currentSunDirection,
                    sizeof(state.historySunDirection));
        std::memcpy(state.historySunColor, state.currentSunColor, sizeof(state.historySunColor));
        state.historySunIntensity = state.currentSunIntensity;
        ++state.historyFrame;
    }
}

} // namespace GameEngine::Engine::Renderer
