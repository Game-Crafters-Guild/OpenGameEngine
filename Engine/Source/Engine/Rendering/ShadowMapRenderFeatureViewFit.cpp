// ShadowMapRenderFeatureViewFit.cpp
//
// A view's cascade fit for the frame: the inputs it is computed from
// (RenderServices' lights, shadow settings and scene bounds, the camera, the
// newest SDSM readback), and the sharing between the caster cull, which runs
// first (OnScheduleCulling), and the ShadowMap node, which declares the cascade
// passes from the same fit.

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"

#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{

using Mathematics::Vector3;

namespace
{

bool SameFloats(const float* a, const float* b, size_t count)
{
    return std::memcmp(a, b, count * sizeof(float)) == 0;
}

bool SameVector(const Vector3& a, const Vector3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// View depth (world units) of the far side of the sphere around `boundsRel`,
// the scene's box relative to the render origin (ox, oy, oz). A sphere rather
// than the box's corners so the reach does not change while the camera orbits
// the scene, which keeps the fitted range, and with it the cascade splits, still.
float SceneReachAlongView(const Rendering::CameraData& camera,
                          const ShadowMapRenderFeature::SceneBoundsRel& boundsRel, float ox,
                          float oy, float oz)
{
    // Row 2 of the column-major view matrix: the camera's forward axis in world space.
    const Vector3 forward = Vector3{camera.view[2], camera.view[6], camera.view[10]}.Normalize();
    const Vector3 center = (boundsRel.Min + boundsRel.Max) * 0.5f;
    const float radius = (boundsRel.Max - boundsRel.Min).Length() * 0.5f;
    const Vector3 cameraRel{camera.cameraPos[0] - ox, camera.cameraPos[1] - oy,
                            camera.cameraPos[2] - oz};
    return Vector3::Dot(center - cameraRel, forward) + radius;
}

} // namespace

float ShadowMapRenderFeature::FitShadowDistanceToScene(float previousDistance, float sceneReach,
                                                       float nearPlane, float fadeFraction)
{
    // Negated so a NaN reach keeps the previous range too.
    if (!(sceneReach > nearPlane))
        return previousDistance;
    const float fullStrength =
        1.0f - std::clamp(fadeFraction, 0.0f, Components::ShadowSettingsEffect::kDistanceFadeFractionMax);
    if (previousDistance > 0.0f)
    {
        const float fadeStart = previousDistance * fullStrength;
        if (sceneReach <= fadeStart && sceneReach * kSceneFitShrinkRatio >= fadeStart)
            return previousDistance;
    }
    return sceneReach * kSceneFitHeadroom / fullStrength;
}

void ShadowMapRenderFeature::ApplySceneDistanceFit(ViewCascadeFit& record, ViewCascadeFitInputs& inputs)
{
    if (!inputs.Settings.FitDistanceToScene)
    {
        record.FittedDistance = 0.0f;
        return;
    }
    if (inputs.HasSceneBounds)
    {
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        ExtractNearFarLH_ZO(inputs.Camera.proj, nearPlane, farPlane);
        record.FittedDistance = FitShadowDistanceToScene(record.FittedDistance, inputs.SceneReach,
                                                         nearPlane, inputs.DistanceFadeFraction);
    }
    // Before the first fit (an empty scene, or nothing ahead of the camera yet)
    // the authored distance stands.
    if (record.FittedDistance > 0.0f)
        inputs.Settings.MaxShadowDistance = record.FittedDistance;
}

bool ShadowMapRenderFeature::SupportsSdsm(const Rendering::IDevice* device)
{
    return device && !device->GetCapabilities().prefersStableShadowFiltering;
}

bool ShadowMapRenderFeature::SameFitInputs(const ViewCascadeFitInputs& a,
                                           const ViewCascadeFitInputs& b)
{
    // Bitwise on the floats: a refit is owed whenever any input bit moved.
    const Rendering::CameraData& ca = a.Camera;
    const Rendering::CameraData& cb = b.Camera;
    if (!SameFloats(ca.view, cb.view, 16) || !SameFloats(ca.proj, cb.proj, 16) ||
        !SameFloats(ca.viewProj, cb.viewProj, 16) || !SameFloats(ca.cameraPos, cb.cameraPos, 4))
        return false;
    const float settingsA[4] = {a.Settings.MaxShadowDistance, a.Settings.SplitLambda,
                                a.Settings.DepthBias, a.Settings.NormalBias};
    const float settingsB[4] = {b.Settings.MaxShadowDistance, b.Settings.SplitLambda,
                                b.Settings.DepthBias, b.Settings.NormalBias};
    const float configA[4] = {a.LightAngularDiameter, a.Cascade0TexelSize, a.CascadeTexelRatio,
                              a.PcssMaxPenumbra};
    const float configB[4] = {b.LightAngularDiameter, b.Cascade0TexelSize, b.CascadeTexelRatio,
                              b.PcssMaxPenumbra};
    if (!SameFloats(settingsA, settingsB, 4) || !SameFloats(configA, configB, 4))
        return false;
    if (!SameVector(a.LightDirection, b.LightDirection) ||
        a.LightCascadeCount != b.LightCascadeCount || a.NumCascades != b.NumCascades ||
        a.Projection != b.Projection || a.Resolution != b.Resolution ||
        a.HasSceneBounds != b.HasSceneBounds)
        return false;
    return !a.HasSceneBounds || (SameVector(a.SceneBounds.Min, b.SceneBounds.Min) &&
                                 SameVector(a.SceneBounds.Max, b.SceneBounds.Max));
}

ShadowMapRenderFeature::ViewCascadeFitInputs ShadowMapRenderFeature::GatherFitInputs(
    RenderServices& rs, uint64_t worldId, const Rendering::CameraData& camera,
    const ExtractedLight& light, const DirectionalShadowSettings& authored) const
{
    ViewCascadeFitInputs inputs{};
    inputs.Camera = camera;
    inputs.LightDirection = Vector3{light.directionWS[0], light.directionWS[1], light.directionWS[2]};
    inputs.LightCascadeCount = light.cascadeCount;
    inputs.LightAngularDiameter = light.shadowAngularDiameter;

    // A PostProcessVolume carrying a ShadowSettingsEffect drives the distance,
    // split lambda and biases for the whole world; without one the node's
    // authored values apply. Resolved every frame so removing the volume
    // reverts to the authored look with no residual state.
    const ResolvedShadowSettings& world = rs.GetWorldShadowSettings(worldId);
    inputs.Settings = world.HasOverride
                          ? DirectionalShadowSettings{world.MaxShadowDistance, world.SplitLambda,
                                                      world.DepthBias, world.NormalBias}
                          : authored;
    inputs.Settings.FitDistanceToScene = authored.FitDistanceToScene;
    inputs.DistanceFadeFraction = world.DistanceFadeFraction;
    inputs.NumCascades = m_Config.NumCascades;
    inputs.Projection = m_Config.Projection;
    inputs.Resolution = m_Config.Resolution;
    inputs.Cascade0TexelSize = m_Config.Cascade0TexelSize;
    inputs.CascadeTexelRatio = m_Config.CascadeTexelRatio;
    inputs.PcssMaxPenumbra = m_PcssMaxPenumbra;

    // Scene bounds for the fit clamp. GPUScene reports them in WORLD space; the
    // fit works render-origin-relative, so they are rebased with the origin the
    // fit derives from the camera position.
    if (const Rendering::GPUScene* scene = rs.GetGPUScene())
    {
        Vector3 sceneMin{};
        Vector3 sceneMax{};
        if (scene->GetInstancesWorldBounds(sceneMin, sceneMax))
        {
            const auto originSector =
                ComputeRenderOriginSector(camera.cameraPos[0], camera.cameraPos[1], camera.cameraPos[2]);
            float ox = 0.0f;
            float oy = 0.0f;
            float oz = 0.0f;
            SectorToWorld(originSector, ox, oy, oz);
            inputs.SceneBounds.Min = Vector3{sceneMin.x - ox, sceneMin.y - oy, sceneMin.z - oz};
            inputs.SceneBounds.Max = Vector3{sceneMax.x - ox, sceneMax.y - oy, sceneMax.z - oz};
            inputs.HasSceneBounds = true;
            if (inputs.Settings.FitDistanceToScene)
                inputs.SceneReach = SceneReachAlongView(camera, inputs.SceneBounds, ox, oy, oz);
        }
    }
    return inputs;
}

void ShadowMapRenderFeature::ResolveSdsmReadback(RenderServices& rs, Rendering::ViewId viewId)
{
    // Token-gated, newest signaled wins. The depth words linearize against the
    // projection the reduce measured with (the slot's context); a camera cut
    // adopts the next measurement without gradual convergence. Untouched
    // sentinels mean a degenerate frame (nothing rendered depth): the previous
    // results stay.
    ShadowReceiverReadback readback{};
    ShadowReceiverMeasurement measured{};
    if (!TryResolveSdsmRG(viewId, readback) || !DecodeShadowReceiverResult(readback, measured))
        return;
    // Reverse-Z: the MIN bit pattern is the FARTHEST sample, MAX the nearest.
    float minNdc = 0.0f;
    float maxNdc = 0.0f;
    std::memcpy(&minNdc, &readback.MinWords[0], sizeof(float));
    std::memcpy(&maxNdc, &readback.MaxWords[0], sizeof(float));
    const SDSMBounds resolved = ResolveSDSMBounds(
        minNdc, maxNdc, readback.Measured.NearPlane, readback.Measured.FarPlane,
        readback.Measured.Orthographic, rs.Views().FindViewAntiAliasing(viewId) != nullptr);
    if (resolved.valid)
        UpdateSDSMBounds(viewId, resolved.nearDepth, resolved.farDepth);
    UpdateShadowReceivers(viewId, measured);
}

const CascadeFrameData& ShadowMapRenderFeature::RefitView(RenderServices& rs, Rendering::ViewId viewId,
                                                          const ViewCascadeFitInputs& inputs,
                                                          ViewCascadeFit& record)
{
    ApplyRuntimeShadowSettings(inputs.Settings.MaxShadowDistance, inputs.Settings.SplitLambda,
                               inputs.Settings.DepthBias, inputs.Settings.NormalBias);
    const bool sdsm = SupportsSdsm(rs.GetDevice());
    if (sdsm)
        ResolveSdsmReadback(rs, viewId);

    const Rendering::CameraData& camera = inputs.Camera;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    ExtractNearFarLH_ZO(camera.proj, nearPlane, farPlane);
    // Bounds invalid until the first readback: the fit takes the pure lambda
    // splits from the near plane.
    CascadeFrameData frame = ComputeCascades(
        camera, nearPlane, farPlane, inputs.LightDirection, sdsm ? &GetSDSMBounds(viewId) : nullptr,
        viewId, inputs.HasSceneBounds ? &inputs.SceneBounds : nullptr,
        sdsm ? &GetShadowReceivers(viewId) : nullptr);
    frame.NumCascades = std::min(frame.NumCascades, inputs.LightCascadeCount);
    // Penumbra comes from the light's own angular size — the same light this
    // fit was built for, so the penumbra cannot disagree with the geometry it
    // filters.
    frame.ShadowTanHalfAngle = ResolveShadowTanHalfAngle(inputs.LightAngularDiameter);

    record.Inputs = inputs;
    record.Frame = frame;
    CacheFrameData(viewId, frame);
    return record.Frame;
}

const CascadeFrameData& ShadowMapRenderFeature::FitViewCascades(
    RenderServices& rs, Rendering::ViewId viewId, uint64_t worldId,
    const Rendering::CameraData& camera, const ExtractedLight& light,
    const DirectionalShadowSettings& authored)
{
    ViewCascadeFit& record = m_ViewFits[viewId];
    record.Authored = authored;
    record.HasAuthored = true;
    ViewCascadeFitInputs inputs = GatherFitInputs(rs, worldId, camera, light, authored);
    ApplySceneDistanceFit(record, inputs);
    const bool reuse = record.FittedForCull && SameFitInputs(record.Inputs, inputs);
    record.FittedForCull = false;
    if (!reuse)
        return RefitView(rs, viewId, inputs, record);
    // Another view's fit may have run since: the rest of this node's declaration
    // of this view reads the config. Nodes declared after this one run after every
    // view has passed through it, so they read a view's range from
    // GetCachedFrameData, never from the config.
    ApplyRuntimeShadowSettings(inputs.Settings.MaxShadowDistance, inputs.Settings.SplitLambda,
                               inputs.Settings.DepthBias, inputs.Settings.NormalBias);
    CacheFrameData(viewId, record.Frame);
    return record.Frame;
}

const CascadeFrameData* ShadowMapRenderFeature::FitViewCascadesForCulling(
    RenderServices& rs, const Rendering::ViewDesc& view, const Rendering::CameraData& camera)
{
    // Only views the ShadowMap node declares get cascades; the node's last
    // authored settings stand in for this frame's.
    const auto it = m_ViewFits.find(view.id);
    if (it == m_ViewFits.end() || !it->second.HasAuthored)
        return nullptr;
    const ExtractedLight* primary = SelectPrimaryDirectional(rs.GetWorldLights(view.worldId));
    if (!primary || primary->castsShadows == 0)
        return nullptr;
    // A camera before its first update has a zero projection, and inverting a
    // zero view-projection poisons every fit value with NaN.
    if (camera.proj[0] == 0.0f && camera.proj[5] == 0.0f)
        return nullptr;
    ViewCascadeFit& record = it->second;
    ViewCascadeFitInputs inputs = GatherFitInputs(rs, view.worldId, camera, *primary, record.Authored);
    ApplySceneDistanceFit(record, inputs);
    const CascadeFrameData& frame = RefitView(rs, view.id, inputs, record);
    record.FittedForCull = true;
    return &frame;
}

} // namespace Engine::Renderer
} // namespace GameEngine
