#include "ECSModules/Rendering/Systems/ReflectionProbeSystem.h"

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"
#include "Engine/Rendering/SkyEnvironmentSource.h"

#include "Logger/Logger.h"
#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

namespace GameEngine { namespace Engine::Renderer {

namespace
{
using Mathematics::Dot3;

constexpr float kReflectionProbeDegToRad = 0.01745329251994329577f;
constexpr float kMinProbeExtent = 0.0001f;

SceneReflectionProbeUpdateMode ToSceneUpdateMode(Components::ReflectionProbeUpdateMode mode)
{
    switch (mode)
    {
    case Components::ReflectionProbeUpdateMode::Once:
        return SceneReflectionProbeUpdateMode::Once;
    case Components::ReflectionProbeUpdateMode::Realtime:
    default:
        return SceneReflectionProbeUpdateMode::Realtime;
    }
}

struct ProbeCandidate
{
    Components::ReflectionProbe Probe{};
    uint32 Priority = 0;
    float NormalizedDistanceSq = 0.0f;
    float Weight = 1.0f;
    float InfluenceRadius = 0.0f;
    float Position[3] = {0.0f, 0.0f, 0.0f};
    float BoxCenter[3] = {0.0f, 0.0f, 0.0f};
    float BoxHalfExtents[3] = {0.0f, 0.0f, 0.0f};
    float BoxAxisX[3] = {1.0f, 0.0f, 0.0f};
    float BoxAxisY[3] = {0.0f, 1.0f, 0.0f};
    float BoxAxisZ[3] = {0.0f, 0.0f, 1.0f};
    bool HasBox = false;
    uint64_t SourceKey = 0;
};

struct ProbeVolume
{
    float Center[3] = {0.0f, 0.0f, 0.0f};
    float AxisX[3] = {1.0f, 0.0f, 0.0f};
    float AxisY[3] = {0.0f, 1.0f, 0.0f};
    float AxisZ[3] = {0.0f, 0.0f, 1.0f};
    float HalfExtents[3] = {0.0f, 0.0f, 0.0f};
    bool Valid = false;
};

// The probe the camera missed by the least, recorded while the selection loop runs so
// the no-probe-selected warning can name the closest fix. A rejection on its own is a
// routine outcome in a multi-probe scene (each probe covers its own room); only the
// selection OUTCOME — no probe accepted the camera — is a diagnostic.
struct NearestRejectedProbe
{
    bool Valid = false;
    uint32 EntityId = 0;
    // How far the camera overshoots the volume, in world units: worst-axis overshoot
    // for a box (exact when one axis rejects, a lower bound when several do), radial
    // overshoot past the surface for an ellipsoid.
    float OvershootWorld = 0.0f;
    int WorstAxis = -1; // 0/1/2 = box local axis; -1 = ellipsoid (radial) rejection
    float CameraLocal[3] = {0.0f, 0.0f, 0.0f};
    float HalfExtents[3] = {0.0f, 0.0f, 0.0f};
};

void RecordIfNearest(NearestRejectedProbe& nearest, uint32 entityId, float overshootWorld,
                     int worstAxis, const float cameraLocal[3], const float halfExtents[3])
{
    if (nearest.Valid && overshootWorld >= nearest.OvershootWorld)
        return;
    nearest.Valid = true;
    nearest.EntityId = entityId;
    nearest.OvershootWorld = overshootWorld;
    nearest.WorstAxis = worstAxis;
    for (int i = 0; i < 3; ++i)
    {
        nearest.CameraLocal[i] = cameraLocal[i];
        nearest.HalfExtents[i] = halfExtents[i];
    }
}

void ClearProbeSourceIfActive(ImageBasedLightingFeature* feature, uint64_t worldId)
{
    if (!feature)
        return;
    if (auto* probeSource =
            dynamic_cast<SceneReflectionProbeEnvironmentSource*>(feature->GetEnvironmentSource()))
    {
        // The IBL feature is engine-global but this system ticks per world: a
        // probe-less world (preview, play-mode isolation) must not tear down a
        // source another world's probe installed — that destroy/recreate churn
        // reallocated the six capture views every frame and no capture ever
        // completed. Only the owning world clears it.
        if (probeSource->GetProbe().WorldId == worldId)
            feature->ClearEnvironmentSource();
    }
    else if (auto* skySource = dynamic_cast<SkyEnvironmentSource*>(feature->GetEnvironmentSource()))
        skySource->ClearProbeOverrides();
}

bool IsBetterCandidate(const ProbeCandidate& candidate, const ProbeCandidate& best)
{
    if (candidate.Priority != best.Priority)
        return candidate.Priority > best.Priority;
    return candidate.NormalizedDistanceSq < best.NormalizedDistanceSq;
}

ProbeVolume ExtractProbeVolume(const Components::WorldTransform& xf)
{
    ProbeVolume volume{};
    const float* m = xf.matrix;
    volume.Center[0] = m[12];
    volume.Center[1] = m[13];
    volume.Center[2] = m[14];

    const float ax[3] = {m[0], m[1], m[2]};
    const float ay[3] = {m[4], m[5], m[6]};
    const float az[3] = {m[8], m[9], m[10]};
    const float sx = std::sqrt(Dot3(ax, ax));
    const float sy = std::sqrt(Dot3(ay, ay));
    const float sz = std::sqrt(Dot3(az, az));
    if (sx <= kMinProbeExtent || sy <= kMinProbeExtent || sz <= kMinProbeExtent)
        return volume;

    const float invSx = 1.0f / sx;
    const float invSy = 1.0f / sy;
    const float invSz = 1.0f / sz;
    for (int i = 0; i < 3; ++i)
    {
        volume.AxisX[i] = ax[i] * invSx;
        volume.AxisY[i] = ay[i] * invSy;
        volume.AxisZ[i] = az[i] * invSz;
    }
    volume.HalfExtents[0] = sx * 0.5f;
    volume.HalfExtents[1] = sy * 0.5f;
    volume.HalfExtents[2] = sz * 0.5f;
    volume.Valid = true;
    return volume;
}

void ProjectToVolumeLocal(const ProbeVolume& volume, const float worldPos[3], float outLocal[3])
{
    const float d[3] = {
        worldPos[0] - volume.Center[0],
        worldPos[1] - volume.Center[1],
        worldPos[2] - volume.Center[2]};
    outLocal[0] = Dot3(d, volume.AxisX);
    outLocal[1] = Dot3(d, volume.AxisY);
    outLocal[2] = Dot3(d, volume.AxisZ);
}

float ComputeBoxCaptureRadius(const ProbeVolume& volume, const float capturePosition[3])
{
    float captureLocal[3] = {0.0f, 0.0f, 0.0f};
    ProjectToVolumeLocal(volume, capturePosition, captureLocal);
    const float dx = volume.HalfExtents[0] + std::fabs(captureLocal[0]);
    const float dy = volume.HalfExtents[1] + std::fabs(captureLocal[1]);
    const float dz = volume.HalfExtents[2] + std::fabs(captureLocal[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float ComputeEllipsoidDistanceToEdge(const float local[3], const float half[3], float normalizedRadius)
{
    if (normalizedRadius <= kMinProbeExtent)
        return std::min({half[0], half[1], half[2]});

    const float localLen = std::sqrt(Dot3(local, local));
    if (localLen <= kMinProbeExtent)
        return std::min({half[0], half[1], half[2]});

    return std::max(0.0f, localLen * ((1.0f / normalizedRadius) - 1.0f));
}

void CopyVolumeToCandidate(const ProbeVolume& volume, ProbeCandidate& candidate)
{
    for (int i = 0; i < 3; ++i)
    {
        candidate.BoxCenter[i] = volume.Center[i];
        candidate.BoxHalfExtents[i] = volume.HalfExtents[i];
        candidate.BoxAxisX[i] = volume.AxisX[i];
        candidate.BoxAxisY[i] = volume.AxisY[i];
        candidate.BoxAxisZ[i] = volume.AxisZ[i];
    }
}
} // namespace

void ReflectionProbeSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    float cameraPos[3] = {0.0f, 0.0f, 0.0f};
    const std::optional<Camera> activeCamera = FindActiveCamera(world);
    const bool hasCamera = activeCamera.has_value();
    if (hasCamera)
    {
        const float* m = activeCamera->worldTransform.Data();
        cameraPos[0] = m[12];
        cameraPos[1] = m[13];
        cameraPos[2] = m[14];
    }

    ProbeCandidate best{};
    bool found = false;
    NearestRejectedProbe nearestRejected{};

    auto q = world.Query<
        ECS::Read<Components::ReflectionProbe>,
        ECS::Read<Components::WorldTransform>>();

    q
     .Each([&](ECS::EntityHandle e,
               const Components::ReflectionProbe& probe,
               const Components::WorldTransform& xf) {
        const ProbeVolume volume = ExtractProbeVolume(xf);
        if (!volume.Valid)
            return;

        float normalizedDistanceSq = 0.0f;
        float weight = 1.0f;
        const float capturePosition[3] = {
            volume.Center[0] + volume.AxisX[0] * probe.OriginOffsetX +
                               volume.AxisY[0] * probe.OriginOffsetY +
                               volume.AxisZ[0] * probe.OriginOffsetZ,
            volume.Center[1] + volume.AxisX[1] * probe.OriginOffsetX +
                               volume.AxisY[1] * probe.OriginOffsetY +
                               volume.AxisZ[1] * probe.OriginOffsetZ,
            volume.Center[2] + volume.AxisX[2] * probe.OriginOffsetX +
                               volume.AxisY[2] * probe.OriginOffsetY +
                               volume.AxisZ[2] * probe.OriginOffsetZ};
        const bool hasBox = probe.BoxProjection;
        const float originOffsetLocal[3] = {
            probe.OriginOffsetX,
            probe.OriginOffsetY,
            probe.OriginOffsetZ};
        const float influenceRadius = hasBox
                                          ? ComputeBoxCaptureRadius(volume, capturePosition)
                                          : std::max({volume.HalfExtents[0], volume.HalfExtents[1], volume.HalfExtents[2]}) +
                                                std::sqrt(Dot3(originOffsetLocal, originOffsetLocal));
        if (hasCamera && hasBox)
        {
            float local[3] = {0.0f, 0.0f, 0.0f};
            ProjectToVolumeLocal(volume, cameraPos, local);
            const float ax = std::fabs(local[0]);
            const float ay = std::fabs(local[1]);
            const float az = std::fabs(local[2]);
            if (ax > volume.HalfExtents[0] ||
                ay > volume.HalfExtents[1] ||
                az > volume.HalfExtents[2])
            {
                const float overshoot[3] = {ax - volume.HalfExtents[0],
                                            ay - volume.HalfExtents[1],
                                            az - volume.HalfExtents[2]};
                const int worst = overshoot[0] > overshoot[1]
                                      ? (overshoot[0] > overshoot[2] ? 0 : 2)
                                      : (overshoot[1] > overshoot[2] ? 1 : 2);
                RecordIfNearest(nearestRejected, e.id, overshoot[worst], worst, local,
                                volume.HalfExtents);
                return;
            }

            const float nx = local[0] / volume.HalfExtents[0];
            const float ny = local[1] / volume.HalfExtents[1];
            const float nz = local[2] / volume.HalfExtents[2];
            normalizedDistanceSq = (nx * nx + ny * ny + nz * nz) / 3.0f;

            const float distanceToEdge = std::min({
                volume.HalfExtents[0] - ax,
                volume.HalfExtents[1] - ay,
                volume.HalfExtents[2] - az});
            const float blend = std::clamp(probe.BlendDistance, 0.0f, std::min({
                volume.HalfExtents[0],
                volume.HalfExtents[1],
                volume.HalfExtents[2]}));
            if (blend > 0.0001f && distanceToEdge < blend)
            {
                weight = std::clamp(distanceToEdge / blend, 0.0f, 1.0f);
                if (weight <= 0.0f)
                    return;
            }
        }
        else if (hasCamera)
        {
            float local[3] = {0.0f, 0.0f, 0.0f};
            ProjectToVolumeLocal(volume, cameraPos, local);
            const float nx = local[0] / volume.HalfExtents[0];
            const float ny = local[1] / volume.HalfExtents[1];
            const float nz = local[2] / volume.HalfExtents[2];
            const float normalizedRadiusSq = nx * nx + ny * ny + nz * nz;
            if (normalizedRadiusSq > 1.0f)
            {
                // Radial overshoot past the ellipsoid surface, in world units: the camera
                // sits at localLen, the surface at localLen / normalizedRadius along the
                // same ray (normalizedRadius > 1 here).
                const float localLen = std::sqrt(Dot3(local, local));
                const float normalizedRadius = std::sqrt(normalizedRadiusSq);
                RecordIfNearest(nearestRejected, e.id,
                                localLen * (1.0f - 1.0f / normalizedRadius), -1, local,
                                volume.HalfExtents);
                return;
            }
            const float normalizedRadius = std::sqrt(std::max(0.0f, normalizedRadiusSq));
            normalizedDistanceSq = normalizedRadiusSq;

            const float blend = std::clamp(probe.BlendDistance, 0.0f, influenceRadius);
            if (blend > 0.0001f)
            {
                const float distanceToEdge = ComputeEllipsoidDistanceToEdge(
                    local, volume.HalfExtents, normalizedRadius);
                if (distanceToEdge < blend)
                    weight = std::clamp(distanceToEdge / blend, 0.0f, 1.0f);
                if (weight <= 0.0f)
                    return;
            }
        }

        ProbeCandidate candidate{};
        candidate.Probe = probe;
        candidate.Priority = probe.Priority;
        candidate.NormalizedDistanceSq = normalizedDistanceSq;
        candidate.Weight = weight;
        candidate.InfluenceRadius = influenceRadius;
        candidate.Position[0] = capturePosition[0];
        candidate.Position[1] = capturePosition[1];
        candidate.Position[2] = capturePosition[2];
        candidate.HasBox = hasBox;
        if (hasBox)
            CopyVolumeToCandidate(volume, candidate);
        candidate.SourceKey = (static_cast<uint64_t>(world.GetWorldId()) << 32u) ^
                              static_cast<uint64_t>(e.id);

        if (!found || IsBetterCandidate(candidate, best))
        {
            best = candidate;
            found = true;
        }
    });

    if (!found)
    {
        // Selection is camera-driven, so when every enabled probe rejects the camera the
        // IBL silently reverts to the sky and nothing on screen says so — every reflective
        // surface simply loses the probe content. Warn once per lost-reflections episode
        // (latched until a probe is accepted again), naming the probe the camera missed by
        // the least. A world with no probes at all records no rejection and stays silent.
        if (nearestRejected.Valid && !m_LoggedNoProbeSelected)
        {
            if (nearestRejected.WorstAxis >= 0)
                Logger::Log::Warning(
                    "No ReflectionProbe contributes: the active camera is outside every "
                    "enabled probe's influence volume, so reflections fall back to the sky. "
                    "Nearest is the box probe on entity {}, missed by {:.2f} on its local {} "
                    "axis (camera local ({:.2f}, {:.2f}, {:.2f}) against half-extents "
                    "({:.2f}, {:.2f}, {:.2f})). Grow that probe's Transform.scale on that "
                    "axis, or move the camera inside.",
                    nearestRejected.EntityId, nearestRejected.OvershootWorld,
                    "XYZ"[nearestRejected.WorstAxis], nearestRejected.CameraLocal[0],
                    nearestRejected.CameraLocal[1], nearestRejected.CameraLocal[2],
                    nearestRejected.HalfExtents[0], nearestRejected.HalfExtents[1],
                    nearestRejected.HalfExtents[2]);
            else
                Logger::Log::Warning(
                    "No ReflectionProbe contributes: the active camera is outside every "
                    "enabled probe's influence volume, so reflections fall back to the sky. "
                    "Nearest is the ellipsoid probe on entity {}, missed by {:.2f} radially "
                    "(camera local ({:.2f}, {:.2f}, {:.2f}) against half-extents ({:.2f}, "
                    "{:.2f}, {:.2f})). Grow that probe's Transform.scale, or move the camera "
                    "inside.",
                    nearestRejected.EntityId, nearestRejected.OvershootWorld,
                    nearestRejected.CameraLocal[0], nearestRejected.CameraLocal[1],
                    nearestRejected.CameraLocal[2], nearestRejected.HalfExtents[0],
                    nearestRejected.HalfExtents[1], nearestRejected.HalfExtents[2]);
            m_LoggedNoProbeSelected = true;
        }
        ClearProbeSourceIfActive(rs->GetFeature<ImageBasedLightingFeature>(), world.GetWorldId());
        return;
    }
    m_LoggedNoProbeSelected = false; // a probe contributes again — rearm the warning

    auto* device = rs->GetDevice();
    if (!device)
        return;

    auto& feature = rs->EnsureFeature<ImageBasedLightingFeature>();
    if (!feature.IsInitialized())
        feature.Initialize(device);
    if (!feature.IsInitialized())
        return;

    SceneReflectionProbeEnvironmentDesc desc{};
    desc.Position[0] = best.Position[0];
    desc.Position[1] = best.Position[1];
    desc.Position[2] = best.Position[2];
    desc.InfluenceRadius = std::max(0.0f, best.InfluenceRadius);
    desc.BoxProjection = best.HasBox;
    if (best.HasBox)
    {
        for (int i = 0; i < 3; ++i)
        {
            desc.BoxCenter[i] = best.BoxCenter[i];
            desc.BoxHalfExtents[i] = best.BoxHalfExtents[i];
            desc.BoxAxisX[i] = best.BoxAxisX[i];
            desc.BoxAxisY[i] = best.BoxAxisY[i];
            desc.BoxAxisZ[i] = best.BoxAxisZ[i];
        }
    }
    desc.MaxDistance = std::max(0.0f, best.Probe.MaxDistance);
    desc.CullMask = best.Probe.CullMask;
    desc.WorldId = world.GetWorldId();
    desc.SourceKey = best.SourceKey != 0 ? best.SourceKey : 1u;
    desc.Intensity = std::max(0.0f, best.Probe.Intensity) * best.Weight;
    desc.ExposureEV = best.Probe.ExposureEV;
    desc.RotationRadians = best.Probe.RotationDegrees * kReflectionProbeDegToRad;
    desc.LowerHemisphereDarkness = std::clamp(best.Probe.IblLowerHemisphereDarkness, 0.0f, 1.0f);
    desc.CaptureEnvironment = best.Probe.CaptureEnvironment;
    desc.CaptureResolution =
        ImageBasedLightingFeature::NormalizeCaptureResolution(best.Probe.CaptureResolution);
    desc.UpdateMode = ToSceneUpdateMode(best.Probe.UpdateMode);
    desc.RealtimeUpdateInterval = std::max(0.0f, best.Probe.RealtimeUpdateInterval);

    if (auto* sceneSource = dynamic_cast<SceneReflectionProbeEnvironmentSource*>(feature.GetEnvironmentSource()))
    {
        sceneSource->SetProbe(desc);
    }
    else
    {
        auto source = std::make_unique<SceneReflectionProbeEnvironmentSource>(*rs);
        source->SetProbe(desc);
        feature.SetEnvironmentSource(std::move(source));
    }
}

} } // namespace GameEngine::Engine::Renderer
