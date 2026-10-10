// RenderServicesViewState.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
//
// The camera/view registries and persistent per-view state moved to ViewRegistry
// (A1.2 S1). What stays here is world-scoped extraction state (lights, post-
// process, fog, content digests), the effective-settings blenders that combine a
// per-view override from the registry with a world fallback, and the device
// writer WriteViewLightBuffer.
#include "Engine/Rendering/RenderServices.h"
#include "Types/ColorUtils.h"
#include "Core/CpuProfiler.h"
#include "Core/Time.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAsset.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/FrustumCullingStrategy.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Rendering/Utils/CubeLutFileParser.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Types/StringId.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

void RenderServices::SubmitLight(uint64 worldId, const ExtractedLight& light)
{
    auto& lights = m_WorldLights[worldId];
    if (lights.empty())
        lights.reserve(100);
    lights.push_back(light);
}

float DeliveredLuminance(const ExtractedLight& l)
{
    const float luma = ColorUtils::LinearRec709Luminance(l.color);
    return std::max(luma, 0.0f) * std::max(l.intensity, 0.0f);
}

namespace
{
// View-independent light importance for contribution ordering. Higher = keep
// first when the light list overflows a cap. Luminance-weighted radiant
// intensity scaled by spatial reach (range^2 ~ the screen/cluster footprint a
// light touches — the view-independent analog of "bigger/nearer"). Camera
// distance is deliberately NOT used: the world light list is shared across
// every view of the world and is index-coupled to the shadow builders'
// packedLightIndex, so a per-view sort would either desync shadows or force
// per-view light lists. Non-emitting lights sink to the bottom so they
// truncate first.
// Known residual: directional lights ride the same range^2 metric. Range is
// meaningless for them (extraction passes the component value through, default
// 10), so a directional authored with Range=0 scores 0 and sorts with the
// non-emitting lights — which can hand the primary-directional pick (see
// SelectPrimaryDirectional) to a weaker light. Tracked; not fixed here because
// changing the metric reorders every packed light index mid-frame consumer.
float LightContribution(const ExtractedLight& l)
{
    if (l.castsLight == 0u)
        return 0.0f;
    const float reach = l.range * l.range;
    return DeliveredLuminance(l) * std::max(reach, 0.0f);
}
} // namespace

void RenderServices::FinalizeWorldLights(uint64 worldId)
{
    const auto it = m_WorldLights.find(worldId);
    if (it == m_WorldLights.end())
        return;
    auto& lights = it->second;
    const size_t count = lights.size();

    // Fail-visible: the engine lights up to kMaxDirectionalLights directionals
    // (strongest-first; cascaded shadows from the primary only). Directionals
    // past the cap contribute nothing, so say so once per episode instead of
    // silently ignoring them.
    size_t directionalCount = 0;
    for (const auto& l : lights)
    {
        if (l.type == GameEngine::Components::LightType::Directional)
            ++directionalCount;
    }
    if (directionalCount > kMaxDirectionalLights)
    {
        if (m_MultiDirectionalWarnedWorlds.insert(worldId).second)
        {
            Logger::Log::Warning(
                "RenderServices: world {} extracted {} directional lights; lighting the strongest "
                "{} (cascaded shadows from the primary only). The remaining {} do not contribute.",
                worldId, directionalCount, kMaxDirectionalLights,
                directionalCount - kMaxDirectionalLights);
        }
    }
    else
    {
        m_MultiDirectionalWarnedWorlds.erase(worldId);
    }

    if (count >= 2)
    {
        // Sort compact keys (contribution precomputed once, id tie-break) instead
        // of the ~130-byte ExtractedLight structs, then apply the permutation in
        // one pass. Keeps the per-frame cost low even at the 1024+ light cap: the
        // O(N log N) swaps move 12-byte keys, not whole light records.
        struct SortKey
        {
            float Contribution;
            uint32 SortId;
            uint32 Index;
        };
        std::vector<SortKey> keys(count);
        for (size_t i = 0; i < count; ++i)
            keys[i] = SortKey{LightContribution(lights[i]), lights[i].SortId, static_cast<uint32>(i)};

        std::sort(keys.begin(), keys.end(),
                  [](const SortKey& a, const SortKey& b)
                  {
                      if (a.Contribution != b.Contribution)
                          return a.Contribution > b.Contribution; // strongest first
                      return a.SortId < b.SortId;                  // deterministic tie-break
                  });

        std::vector<ExtractedLight> sorted;
        sorted.reserve(count);
        for (const auto& k : keys)
            sorted.push_back(std::move(lights[k.Index]));
        lights = std::move(sorted);
    }

    // Light-list version (idle recompute elision): exact bytes of the FINAL
    // (sorted, consumer-visible) list against last frame's snapshot — runs for
    // 0/1-light worlds too so an emptied list still bumps. All members are
    // 4-byte scalars; the static_assert pins the padding-free layout the
    // memcmp exactness depends on (a new member updates both together).
    static_assert(sizeof(ExtractedLight) == 144,
                  "ExtractedLight layout changed — re-verify it stays padding-free "
                  "(the light-list version memcmp requires it) and update this size");
    std::vector<ExtractedLight>& snapshot = m_WorldLightSnapshots[worldId];
    const bool lightsChanged =
        snapshot.size() != lights.size() ||
        (!lights.empty() &&
         std::memcmp(snapshot.data(), lights.data(), lights.size() * sizeof(ExtractedLight)) != 0);
    if (lightsChanged)
    {
        uint64_t& version = m_WorldLightVersions[worldId];
        ++version;
        // Churn diagnostic (rate-limited): a version advancing every frame in
        // a static scene blocks the light-keyed elision families — name the
        // first differing light + byte so the unstable field is identifiable
        // from a log alone.
        if (version % 600 == 1 && snapshot.size() == lights.size() && !lights.empty())
        {
            const auto* a = reinterpret_cast<const uint8_t*>(snapshot.data());
            const auto* b = reinterpret_cast<const uint8_t*>(lights.data());
            const size_t total = lights.size() * sizeof(ExtractedLight);
            size_t firstDiff = 0;
            while (firstDiff < total && a[firstDiff] == b[firstDiff])
                ++firstDiff;
            if (firstDiff < total)
            {
                const size_t lightIdx = firstDiff / sizeof(ExtractedLight);
                const size_t fieldOff = firstDiff % sizeof(ExtractedLight);
                uint32_t oldWord = 0, newWord = 0;
                const size_t wordOff = firstDiff & ~size_t{3};
                std::memcpy(&oldWord, a + wordOff, sizeof(uint32_t));
                std::memcpy(&newWord, b + wordOff, sizeof(uint32_t));
                Logger::Log::Info("[IdleElision] world {} light-list churn: version {} — light {} "
                                  "(SortId {}) byte offset {} changed {:#x} -> {:#x}",
                                  worldId, version, lightIdx, lights[lightIdx].SortId, fieldOff,
                                  oldWord, newWord);
            }
        }
        snapshot = lights;
    }
}

std::span<const ExtractedLight> RenderServices::GetWorldLights(uint64 worldId) const
{
    const auto it = m_WorldLights.find(worldId);
    if (it == m_WorldLights.end())
        return {};
    return it->second;
}

const ExtractedLight* SelectPrimaryDirectional(std::span<const ExtractedLight> sortedLights)
{
    for (const auto& l : sortedLights)
    {
        if (l.type == GameEngine::Components::LightType::Directional)
            return &l;
    }
    return nullptr;
}

const ExtractedLight* SelectLitPrimaryDirectional(std::span<const ExtractedLight> sortedLights)
{
    const ExtractedLight* primary = SelectPrimaryDirectional(sortedLights);
    if (!primary || primary->castsLight == 0u || !(DeliveredLuminance(*primary) > 0.0f))
        return nullptr;
    return primary;
}

void PackForwardLightDirectionals(std::span<const ExtractedLight> sortedLights, ForwardLightUBO& ubo)
{
    // Primary: THE directional every shadow/fog/sky consumer agrees on — its
    // direction, color, intensity AND castsShadows gate all come from the one
    // light SelectPrimaryDirectional picks (never a weaker fill's flags).
    const ExtractedLight* sun = SelectPrimaryDirectional(sortedLights);
    if (!sun)
        return; // no directional: leave the zeroed lanes (count 0, dark sun)
    ubo.uLightDirWorld[0] = sun->directionWS[0];
    ubo.uLightDirWorld[1] = sun->directionWS[1];
    ubo.uLightDirWorld[2] = sun->directionWS[2];
    ubo.uLightDirWorld[3] = sun->intensity;
    ubo.uLightColorWorld[0] = sun->color[0];
    ubo.uLightColorWorld[1] = sun->color[1];
    ubo.uLightColorWorld[2] = sun->color[2];
    ubo.uLightColorWorld[3] = (sun->castsShadows != 0) ? 1.0f : 0.0f;

    // Secondaries: the next-strongest emitting directionals, additive and
    // unshadowed (the v1 semantic — cascades exist for the primary only, so a
    // secondary can brighten but never darken). Premultiplying color*intensity
    // here keeps the shader loop to one MAD per channel.
    uint32 packed = 0;
    for (const auto& l : sortedLights)
    {
        if (packed >= kMaxSecondaryDirectionals)
            break;
        if (l.type != GameEngine::Components::LightType::Directional || &l == sun)
            continue;
        if (l.castsLight == 0u || DeliveredLuminance(l) <= 0.0f)
            continue; // contributes nothing — don't spend a per-pixel BRDF on it
        ubo.uSecondaryDirs[packed][0] = l.directionWS[0];
        ubo.uSecondaryDirs[packed][1] = l.directionWS[1];
        ubo.uSecondaryDirs[packed][2] = l.directionWS[2];
        ubo.uSecondaryDirs[packed][3] = 0.0f;
        ubo.uSecondaryColors[packed][0] = l.color[0] * l.intensity;
        ubo.uSecondaryColors[packed][1] = l.color[1] * l.intensity;
        ubo.uSecondaryColors[packed][2] = l.color[2] * l.intensity;
        ubo.uSecondaryColors[packed][3] = 0.0f;
        ++packed;
    }
    ubo.uSecondaryCount[0] = static_cast<float>(packed);
}

const PostProcessSettings RenderServices::kDefaultPostProcess{};
const ResolvedShadowSettings RenderServices::kDefaultShadowSettings{};

void RenderServices::SetWorldPostProcessSettings(uint64 worldId, const PostProcessSettings& settings)
{
    m_WorldPostProcess[worldId] = settings;
}

const PostProcessSettings& RenderServices::GetWorldPostProcessSettings(uint64 worldId) const
{
    const auto it = m_WorldPostProcess.find(worldId);
    if (it == m_WorldPostProcess.end())
        return kDefaultPostProcess;
    return it->second;
}

void RenderServices::SetWorldShadowSettings(uint64 worldId, const ResolvedShadowSettings& settings)
{
    m_WorldShadowSettings[worldId] = settings;
}

const ResolvedShadowSettings& RenderServices::GetWorldShadowSettings(uint64 worldId) const
{
    const auto it = m_WorldShadowSettings.find(worldId);
    if (it == m_WorldShadowSettings.end())
        return kDefaultShadowSettings;
    return it->second;
}

void RenderServices::SetWorldRenderContentDigest(uint64 worldId, uint64 digest)
{
    m_WorldRenderContentDigests[worldId] = digest;
}

uint64 RenderServices::GetWorldRenderContentDigest(uint64 worldId) const
{
    const auto it = m_WorldRenderContentDigests.find(worldId);
    return it != m_WorldRenderContentDigests.end() ? it->second : 0;
}

const PostProcessSettings& RenderServices::GetEffectivePostProcessSettings(Rendering::ViewId viewId, uint64 worldId) const
{
    // A registry override REPLACES the world settings rather than blending into
    // them: an override is already a complete PostProcessSettings, so a view
    // that registers one takes it whole and only a view without one falls back.
    if (const PostProcessSettings* over = m_ViewRegistry.FindViewPostProcessOverride(viewId))
        return *over;
    return GetWorldPostProcessSettings(worldId);
}

void RenderServices::SetWorldVolumetricFogVolumes(uint64 worldId, std::vector<VolumetricFogLocalVolume> volumes)
{
    m_WorldVolumetricFogVolumes[worldId] = std::move(volumes);
}

std::span<const VolumetricFogLocalVolume> RenderServices::GetWorldVolumetricFogVolumes(uint64 worldId) const
{
    const auto it = m_WorldVolumetricFogVolumes.find(worldId);
    if (it == m_WorldVolumetricFogVolumes.end())
        return {};
    return it->second;
}

std::span<const VolumetricFogLocalVolume> RenderServices::GetEffectiveVolumetricFogVolumes(Rendering::ViewId viewId, uint64 worldId) const
{
    if (const std::vector<VolumetricFogLocalVolume>* over =
            m_ViewRegistry.FindViewVolumetricFogVolumesOverride(viewId))
        return *over;
    return GetWorldVolumetricFogVolumes(worldId);
}

void RenderServices::WriteViewLightBuffer(ViewId viewId)
{
    const ViewDesc* view = m_ViewRegistry.FindViewDesc(viewId);
    if (!view || !m_Device)
        return;

    auto& lightBuffers = m_ViewRegistry.PerView(viewId).LightBuffers;
    const uint32_t frameSlot = m_Device->GetFrameIndex();
    auto& buf = lightBuffers[frameSlot];
    if (!buf.IsValid())
    {
        Rendering::BufferDesc bd{};
        bd.size = sizeof(ForwardLightUBO);
        bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::Uniform);
        bd.memoryUsage = Rendering::BufferMemoryUsage::Upload;
        // FrameSlotted: one slot of the per-view ring indexed above, so a host
        // write outside an acquired frame races whichever frame still holds it.
        bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped |
                   Rendering::BufferCreateFlags::FrameSlotted;
        bd.debugName = "RenderServices.ViewLightUBO";
        buf = m_Device->CreateBuffer(bd);
        if (!buf.IsValid())
            return;
    }

    ForwardLightUBO ubo{};
    // Identity light VP (these views have no shadow map).
    ubo.uLightVP[0] = ubo.uLightVP[5] = ubo.uLightVP[10] = ubo.uLightVP[15] = 1.0f;
    // No default light — caller must SubmitLight() before WriteViewLightBuffer().

    // Directionals: primary (shadowed) + unshadowed secondaries, packed from
    // the strongest-first finalized list by the shared packer — the same
    // primary the shadow node emits cascades for.
    const auto lights = GetWorldLights(view->worldId);
    PackForwardLightDirectionals(lights, ubo);
    for (const auto& l : lights)
    {
        if (l.type == GameEngine::Components::LightType::Ambient)
        {
            ubo.uAmbient[0] = l.color[0];
            ubo.uAmbient[1] = l.color[1];
            ubo.uAmbient[2] = l.color[2];
            ubo.uAmbient[3] = l.intensity;
        }
    }

    // xy: the process-wide globals. zw: the endpoint lanes a deforming vertex
    // stage stamps into its InstanceData — the deformation lane rebased against
    // the origin every view shares so two endpoints of one frame difference
    // exactly, the scroll lane on its existing bounded wrap.
    ubo.uTimeParams[0] = GetShaderAnimationTimeSeconds();
    ubo.uTimeParams[1] = GetScrollAnimationTimeSeconds();
    ubo.uTimeParams[2] =
        m_ViewTemporalHistory.ResolveDeformationClock(Time::GetCumulativeSeconds()).TimeSeconds;
    ubo.uTimeParams[3] = ubo.uTimeParams[1];

    void* mapped = m_Device->MapBuffer(buf);
    if (mapped)
    {
        std::memcpy(mapped, &ubo, sizeof(ubo));
        m_Device->UnmapBuffer(buf);
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
