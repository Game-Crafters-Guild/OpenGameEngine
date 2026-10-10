// RenderServicesDetail.cpp
// Definitions for the shared helpers in RenderServicesDetail.h, plus the
// mesh-draw diagnostics ring they feed.
#include "RenderServicesDetail.h"

#include "Engine/Rendering/TextureService.h"

#include "Core/DebugMetrics.h"
#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
std::mutex g_MeshDrawDiagnosticsMutex;

std::vector<RenderServices::MeshDrawDiagnostic> g_MeshDrawDiagnostics;

void PushRecentMeshDrawDiagnostic(const char* pass,
                                  const char* reason,
                                  Rendering::ViewId viewId,
                                  uint32_t materialIndex,
                                  uint32_t meshIndex,
                                  uint32_t maxDrawCount,
                                  size_t entityKeyCount,
                                  size_t commandCount,
                                  size_t slotCount)
{
    std::lock_guard lock(g_MeshDrawDiagnosticsMutex);
    if (g_MeshDrawDiagnostics.size() >= 256)
        g_MeshDrawDiagnostics.erase(g_MeshDrawDiagnostics.begin(),
                                    g_MeshDrawDiagnostics.begin() + 128);
    auto& d = g_MeshDrawDiagnostics.emplace_back();
    d.pass = pass ? pass : "unknown";
    d.reason = reason ? reason : "unknown";
    d.viewId = static_cast<uint32_t>(viewId);
    d.materialIndex = materialIndex;
    d.meshIndex = meshIndex;
    d.maxDrawCount = maxDrawCount;
    d.entityKeyCount = entityKeyCount;
    d.commandCount = commandCount;
    d.slotCount = slotCount;
}

float MeshDiagReasonCode(const char* reason)
{
    if (!reason)
        return 0.0f;
    if (std::strcmp(reason, "pass-no-draws") == 0) return 1.0f;
    if (std::strcmp(reason, "material-null") == 0) return 2.0f;
    if (std::strcmp(reason, "scope-opaque-filter") == 0) return 3.0f;
    if (std::strcmp(reason, "scope-transmissive-filter") == 0) return 4.0f;
    if (std::strcmp(reason, "mesh-entry-missing") == 0) return 5.0f;
    if (std::strcmp(reason, kMeshNotDrawableReason) == 0) return 6.0f;
    if (std::strcmp(reason, "index-count-zero") == 0) return 7.0f;
    if (std::strcmp(reason, "color-pipeline-invalid") == 0) return 8.0f;
    if (std::strcmp(reason, "drawstream-pipeline-invalid") == 0) return 9.0f;
    if (std::strcmp(reason, "device-null") == 0) return 10.0f;
    if (std::strcmp(reason, "draw-slot-invalid") == 0) return 11.0f;
    if (std::strcmp(reason, "indirection-bda-zero") == 0) return 12.0f;
    if (std::strcmp(reason, "instance-bda-zero") == 0) return 13.0f;
    if (std::strcmp(reason, "material-bind-invalid") == 0) return 14.0f;
    if (std::strcmp(reason, "depth-pipeline-invalid") == 0) return 15.0f;
    if (std::strcmp(reason, "alpha-blend-filter") == 0) return 16.0f;
    if (std::strcmp(reason, "transmission-depth-filter") == 0) return 17.0f;
    if (std::strcmp(reason, "pass-enter") == 0) return 18.0f;
    return 99.0f;
}

} // namespace

void LogMeshDrawSkipDiagnostic(const char* pass,
                               const char* reason,
                               Rendering::ViewId viewId,
                               uint32_t materialIndex,
                               uint32_t meshIndex,
                               bool drawStreamReady,
                               bool drawSlotValid,
                               uint64_t indirectionBda,
                               uint64_t instanceBda,
                               bool supportsBufferDeviceAddress,
                               size_t slotCount)
{
    PushRecentMeshDrawDiagnostic(pass, reason, viewId, materialIndex, meshIndex,
                                 0u, 0u, 0u, slotCount);

#if GE_ENABLE_METRICS
    static std::atomic<int> s_Count{0};
    const int sampleIndex = s_Count.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& metrics = GameEngine::Debug::DebugMetrics::Get();
    metrics.PushSample("Render/MeshDrawDiag/Count", static_cast<float>(sampleIndex));
    metrics.PushSample("Render/MeshDrawDiag/Reason", MeshDiagReasonCode(reason));
    metrics.PushSample("Render/MeshDrawDiag/View", static_cast<float>(viewId));
    metrics.PushSample("Render/MeshDrawDiag/Material", static_cast<float>(materialIndex));
    metrics.PushSample("Render/MeshDrawDiag/Mesh", static_cast<float>(meshIndex));
    metrics.PushSample("Render/MeshDrawDiag/DrawStreamReady", drawStreamReady ? 1.0f : 0.0f);
    metrics.PushSample("Render/MeshDrawDiag/SlotValid", drawSlotValid ? 1.0f : 0.0f);
    metrics.PushSample("Render/MeshDrawDiag/IndirectionBDAZero", indirectionBda == 0 ? 1.0f : 0.0f);
    metrics.PushSample("Render/MeshDrawDiag/InstanceBDAZero", instanceBda == 0 ? 1.0f : 0.0f);
    metrics.PushSample("Render/MeshDrawDiag/SupportsBDA", supportsBufferDeviceAddress ? 1.0f : 0.0f);
    metrics.PushSample("Render/MeshDrawDiag/SlotCount", static_cast<float>(slotCount));
#else
    (void)drawStreamReady;
    (void)drawSlotValid;
    (void)indirectionBda;
    (void)instanceBda;
    (void)supportsBufferDeviceAddress;
#endif
}

void LogMeshPassDiagnostic(const char* pass,
                           const char* reason,
                           Rendering::ViewId viewId,
                           size_t entityKeyCount,
                           size_t commandCount,
                           size_t slotCount)
{
    PushRecentMeshDrawDiagnostic(pass, reason, viewId, 0u, 0u, 0u,
                                 entityKeyCount, commandCount, slotCount);

#if GE_ENABLE_METRICS
    static std::atomic<int> s_Count{0};
    const int sampleIndex = s_Count.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& metrics = GameEngine::Debug::DebugMetrics::Get();
    metrics.PushSample("Render/MeshPassDiag/Count", static_cast<float>(sampleIndex));
    metrics.PushSample("Render/MeshPassDiag/Reason", MeshDiagReasonCode(reason));
    metrics.PushSample("Render/MeshPassDiag/View", static_cast<float>(viewId));
    metrics.PushSample("Render/MeshPassDiag/EntityKeys", static_cast<float>(entityKeyCount));
    metrics.PushSample("Render/MeshPassDiag/Commands", static_cast<float>(commandCount));
    metrics.PushSample("Render/MeshPassDiag/SlotCount", static_cast<float>(slotCount));
#endif
}

uint64_t UploadedCompatInstanceListBytes(const ResolvedPassResources& resources)
{
    const StringId name = HashStringId(CpuDrawStreamBuilder::kIndexListBindingName);
    for (const ResolvedPassResources::BufferEntry& entry : resources.Buffers)
    {
        if (entry.Name == name)
            return entry.Range;
    }
    return 0u;
}

bool DrawAttributionEnabled()
{
    static const bool s_Enabled = []
    {
        const char* env = std::getenv("GE_DRAW_ATTRIBUTION");
        return env != nullptr && env[0] != '0';
    }();
    return s_Enabled;
}

void LogDrawAttribution(const char* pass, Rendering::ViewId viewId, size_t entityKeyCount,
                        uint32_t drawsIssued, uint32_t pipelineBinds, uint32_t descriptorBinds,
                        bool consolidationActive, uint32_t liveGroupCount)
{
    // Prime interval: the per-frame call-site count is typically a divisor of
    // a round number, so a round modulus would sample the SAME site forever.
    // 127 rotates the sample across every (pass, view, family) site.
    static std::atomic<uint64_t> s_Tick{0};
    if ((s_Tick.fetch_add(1, std::memory_order_relaxed) % 127u) != 0u)
        return;
    Logger::Log::Info(
        "[DrawAttrib] pass={} view={} keys={} draws={} pipelineBinds={} descriptorBinds={} "
        "consolidation={} liveGroups={}",
        pass ? pass : "?", static_cast<uint32_t>(viewId), entityKeyCount, drawsIssued,
        pipelineBinds, descriptorBinds, consolidationActive ? "on" : "off", liveGroupCount);
}

void PushMeshDrawIssuedDiagnostic(const char* pass,
                                  Rendering::ViewId viewId,
                                  uint32_t materialIndex,
                                  uint32_t meshIndex,
                                  uint32_t maxDrawCount,
                                  size_t slotCount)
{
    PushRecentMeshDrawDiagnostic(pass, "issued", viewId, materialIndex, meshIndex,
                                 maxDrawCount, 0u, 0u, slotCount);

#if GE_ENABLE_METRICS
    static std::atomic<int> s_Count{0};
    const int sampleIndex = s_Count.fetch_add(1, std::memory_order_relaxed) + 1;
    auto& metrics = GameEngine::Debug::DebugMetrics::Get();
    metrics.PushSample("Render/MeshDrawIssued/Count", static_cast<float>(sampleIndex));
    metrics.PushSample("Render/MeshDrawIssued/Pass", (pass && std::strcmp(pass, "depth") == 0) ? 2.0f : 1.0f);
    metrics.PushSample("Render/MeshDrawIssued/View", static_cast<float>(viewId));
    metrics.PushSample("Render/MeshDrawIssued/Material", static_cast<float>(materialIndex));
    metrics.PushSample("Render/MeshDrawIssued/Mesh", static_cast<float>(meshIndex));
    metrics.PushSample("Render/MeshDrawIssued/MaxDrawCount", static_cast<float>(maxDrawCount));
    metrics.PushSample("Render/MeshDrawIssued/SlotCount", static_cast<float>(slotCount));
#else
    (void)pass;
    (void)viewId;
    (void)materialIndex;
    (void)meshIndex;
    (void)maxDrawCount;
    (void)slotCount;
#endif
}

Rendering::SamplerPreset SamplerPresetFromMaterialFilter(MaterialTextureFilter f)
{
    switch (f)
    {
    case MaterialTextureFilter::Point:    return Rendering::SamplerPreset::PointRepeat;
    case MaterialTextureFilter::Bilinear: return Rendering::SamplerPreset::BilinearRepeat;
    case MaterialTextureFilter::Trilinear:
    default:                              return Rendering::SamplerPreset::LinearRepeat;
    }
}

AreaShadowFrameInfo BuildAreaShadowFrameInfo(std::span<const ExtractedLight> lights)
{
    AreaShadowFrameInfo out{};

    const ExtractedLight* shadowLight = nullptr;
    uint32_t clusterableIndex = 0;
    for (const auto& light : lights)
    {
        if (light.type == Components::LightType::Directional ||
            light.type == Components::LightType::Ambient)
        {
            continue;
        }

        if (light.type == Components::LightType::Area &&
            light.castsLight != 0 &&
            light.castsShadows != 0)
        {
            shadowLight = &light;
            out.packedLightIndex = clusterableIndex;
            break;
        }
        ++clusterableIndex;
    }

    if (!shadowLight)
        return out;

    Mathematics::Vector3 pos{
        shadowLight->positionWS[0],
        shadowLight->positionWS[1],
        shadowLight->positionWS[2]};
    Mathematics::Vector3 dir{
        shadowLight->directionWS[0],
        shadowLight->directionWS[1],
        shadowLight->directionWS[2]};
    if (dir.Length() < 1e-4f)
        dir = Mathematics::Vector3{0.0f, -1.0f, 0.0f};
    else
        dir = dir.Normalize();

    Mathematics::Vector3 up{
        shadowLight->upWS[0],
        shadowLight->upWS[1],
        shadowLight->upWS[2]};
    if (up.Length() < 1e-4f || std::fabs(Mathematics::Vector3::Dot(up.Normalize(), dir)) > 0.98f)
        up = std::fabs(dir.y) < 0.95f ? Mathematics::Vector3{0.0f, 1.0f, 0.0f}
                                       : Mathematics::Vector3{1.0f, 0.0f, 0.0f};
    up = up.Normalize();

    const float width = std::max(shadowLight->areaWidth, 0.001f);
    const float height = std::max(shadowLight->areaHeight, 0.001f);
    const float radius = std::max(shadowLight->areaRadius, 0.001f);
    const float range = std::max(shadowLight->range, 0.001f);
    float extent = 0.5f * std::sqrt(width * width + height * height);
    float cullHalfWidth = range + 0.5f * width;
    float cullHalfHeight = range + 0.5f * height;
    float cullNear = kAreaShadowNearPlane;
    if (shadowLight->areaShape == Components::AreaLightShape::Disc ||
        shadowLight->areaShape == Components::AreaLightShape::Sphere)
    {
        extent = radius;
        cullHalfWidth = range + radius;
        cullHalfHeight = range + radius;
        if (shadowLight->areaShape == Components::AreaLightShape::Sphere)
            cullNear = -(range + radius);
    }
    else if (shadowLight->areaShape == Components::AreaLightShape::Cylinder)
    {
        extent = std::sqrt(radius * radius + 0.25f * height * height);
        cullHalfWidth = range + radius;
        cullHalfHeight = range + 0.5f * height;
        cullNear = -(range + extent);
    }

    out.valid = true;
    out.position = pos;
    out.direction = dir;
    out.lightSize = std::max(extent, 0.001f);
    out.nearPlane = cullNear;
    out.farPlane = std::max(range + extent, 0.1f);
    out.lightView = Mathematics::MakeLookAtLH(pos, pos + dir, up);
    out.lightProj = Mathematics::MakeOrthographicLH_ZO_ReverseZ(
        -std::max(cullHalfWidth, 0.001f), std::max(cullHalfWidth, 0.001f),
        -std::max(cullHalfHeight, 0.001f), std::max(cullHalfHeight, 0.001f),
        out.nearPlane, out.farPlane);
    out.lightVP = out.lightProj * out.lightView;
    return out;
}

SpotShadowFrameInfo BuildSpotShadowFrameInfo(std::span<const ExtractedLight> lights)
{
    SpotShadowFrameInfo out{};

    const ExtractedLight* shadowLight = nullptr;
    uint32_t clusterableIndex = 0;
    for (const auto& light : lights)
    {
        if (light.type == Components::LightType::Directional ||
            light.type == Components::LightType::Ambient)
        {
            continue;
        }

        if (light.type == Components::LightType::Spot &&
            light.castsLight != 0 &&
            light.castsShadows != 0)
        {
            shadowLight = &light;
            out.packedLightIndex = clusterableIndex;
            break;
        }
        ++clusterableIndex;
    }

    if (!shadowLight)
        return out;

    Mathematics::Vector3 pos{
        shadowLight->positionWS[0],
        shadowLight->positionWS[1],
        shadowLight->positionWS[2]};
    Mathematics::Vector3 dir{
        shadowLight->directionWS[0],
        shadowLight->directionWS[1],
        shadowLight->directionWS[2]};
    if (dir.Length() < 1e-4f)
        dir = Mathematics::Vector3{0.0f, -1.0f, 0.0f};
    else
        dir = dir.Normalize();

    Mathematics::Vector3 up{
        shadowLight->upWS[0],
        shadowLight->upWS[1],
        shadowLight->upWS[2]};
    if (up.Length() < 1e-4f || std::fabs(Mathematics::Vector3::Dot(up.Normalize(), dir)) > 0.98f)
        up = std::fabs(dir.y) < 0.95f ? Mathematics::Vector3{0.0f, 1.0f, 0.0f}
                                       : Mathematics::Vector3{1.0f, 0.0f, 0.0f};
    up = up.Normalize();

    const float range = std::max(shadowLight->range, kSpotShadowNearPlane + 0.1f);
    const float halfAngle = std::clamp(std::max(shadowLight->outerAngle, 0.01f),
                                       0.01f, 0.49f * kPi);
    const float fovY = std::clamp(2.0f * halfAngle, 0.02f, 0.98f * kPi);

    out.valid = true;
    out.position = pos;
    out.direction = dir;
    out.nearPlane = std::min(kSpotShadowNearPlane, range * 0.5f);
    out.farPlane = std::max(range, out.nearPlane + 0.1f);
    out.lightView = Mathematics::MakeLookAtLH(pos, pos + dir, up);
    out.lightProj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(
        fovY, 1.0f, out.nearPlane, out.farPlane);
    out.lightVP = out.lightProj * out.lightView;
    return out;
}

void PopulatePointShadowGeometry(PointShadowFrameInfo& out, const Mathematics::Vector3& positionWS,
                                 float rawRange, uint32_t tier, uint32_t packedLightIndex,
                                 const PointShadowCameraCull* cull)
{
    out.packedLightIndex = packedLightIndex;
    out.resolutionTier = tier;

    const Mathematics::Vector3 pos = positionWS;
    const float range = std::max(rawRange, kPointShadowNearPlane + 0.1f);

    // S1 camera culling (only on the declaration + GPU-cull paths, which pass
    // `cull`). Light-level reject: an off-screen range sphere declares no faces.
    // Per-face reject: out.faceMask keeps only faces whose cube-face frustum can
    // intersect the camera frustum. Caster culling stays against the LIGHT
    // frustum downstream — never tightened to the camera.
    if (cull)
    {
        if (!PointShadowLightVisible(pos, range, *cull))
            return; // out.valid == false: no passes, light stays lit/unshadowed
        out.faceMask = ComputePointShadowFaceMask(pos, range, *cull);
        if ((out.faceMask & kPointShadowAllFaces) == 0u)
            return; // every face culled -> no passes
    }

    static const std::array<Mathematics::Vector3, kPointShadowFaceCount> kFaceDirs = {{
        { 1.0f,  0.0f,  0.0f},
        {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f},
        { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f},
        { 0.0f,  0.0f, -1.0f},
    }};
    static const std::array<Mathematics::Vector3, kPointShadowFaceCount> kFaceUps = {{
        {0.0f, -1.0f,  0.0f},
        {0.0f, -1.0f,  0.0f},
        {0.0f,  0.0f,  1.0f},
        {0.0f,  0.0f, -1.0f},
        {0.0f, -1.0f,  0.0f},
        {0.0f, -1.0f,  0.0f},
    }};

    out.valid = true;
    out.position = pos;
    out.nearPlane = std::min(kPointShadowNearPlane, range * 0.5f);
    out.farPlane = std::max(range, out.nearPlane + 0.1f);
    out.lightProj = Mathematics::MakePerspectiveLH_ZO_ReverseZ(
        0.5f * kPi, 1.0f, out.nearPlane, out.farPlane);
    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
    {
        out.direction[face] = kFaceDirs[face];
        out.lightView[face] = Mathematics::MakeLookAtLH(pos, pos + kFaceDirs[face], kFaceUps[face]);
        out.lightVP[face] = out.lightProj * out.lightView[face];
    }
}

float PointShadowScreenCoverageRadiusPx(const ::GameEngine::Rendering::CameraData& camData,
                                        const Mathematics::Vector3& lightPositionWS, float range)
{
    // Project the sphere's angular radius (range / distance) to NDC via the
    // projection's vertical focal (proj m11 = cot(fovY/2) for perspective), then
    // scale to a reference screen half-height so the planner's pixel thresholds
    // are viewport-agnostic. Camera INSIDE the sphere => a very large coverage.
    constexpr float kReferenceScreenHalfHeightPx = 540.0f; // half a 1080p view
    const Mathematics::Vector3 camPos{camData.cameraPos[0], camData.cameraPos[1],
                                      camData.cameraPos[2]};
    const float dist = std::max((lightPositionWS - camPos).Length(), 1e-3f);
    const float focalY = std::fabs(camData.proj[5]);
    return (focalY * range / dist) * kReferenceScreenHalfHeightPx;
}

std::optional<PointShadowCameraCull> MakePointShadowCullInputs(
    const ::GameEngine::Rendering::CameraData& camData)
{
    // Degenerate camera (thumbnail before its first update): a zero projection
    // inverts to NaN corners. Skip culling — every face renders (safe fallback).
    if (camData.proj[0] == 0.0f && camData.proj[5] == 0.0f)
        return std::nullopt;

    const ::GameEngine::Rendering::CameraDerivedData cam =
        ::GameEngine::Rendering::DeriveCameraData(camData);

    PointShadowCameraCull out{};
    out.CameraPosition = cam.Position;
    ::GameEngine::Rendering::ExtractFrustumPlanes(cam.ViewProjMatrix, out.FrustumPlanes.data());

    // Reverse-Z NDC corners (z = 1 near, z = 0 far), back-projected to world.
    const Mathematics::Matrix4x4 invViewProj = Mathematics::Inverse(cam.ViewProjMatrix);
    static constexpr float kNdc[8][4] = {
        {-1, -1, 1, 1}, {1, -1, 1, 1}, {1, 1, 1, 1}, {-1, 1, 1, 1}, // near
        {-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}, // far
    };
    for (int i = 0; i < 8; ++i)
    {
        const Mathematics::Vector4 ws =
            invViewProj.Transform(Mathematics::Vector4{kNdc[i][0], kNdc[i][1], kNdc[i][2], kNdc[i][3]});
        const float invW = (ws.w != 0.0f) ? 1.0f / ws.w : 0.0f;
        out.FrustumCorners[i] = Mathematics::Vector3{ws.x * invW, ws.y * invW, ws.z * invW};
    }
    return out;
}

std::vector<RenderServices::MeshDrawDiagnostic> RenderServices::GetRecentMeshDrawDiagnostics() const
{
    std::lock_guard lock(g_MeshDrawDiagnosticsMutex);
    return g_MeshDrawDiagnostics;
}

Rendering::DescriptorSetLayoutDesc CreateMaterialTextureSetLayout(Rendering::IDevice* device)
{
    if (device && TextureService::SelectMaterialIndexingMode(device->GetCapabilities())
                      == TextureService::MaterialIndexingMode::Classic)
    {
        return MaterialBindingCache::GetSetLayout();
    }
    const uint32_t maxTextures = device
        ? std::max<uint32_t>(1u, device->GetCapabilities().maxBindlessTextures)
        : kMaxBindlessTextures;
    return TextureService::GetBindlessTextureSetLayout(maxTextures);
}

} // namespace Engine::Renderer
} // namespace GameEngine
