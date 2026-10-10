#include "ECSModules/Rendering/Systems/DDGIVolumeSystem.h"

#include "Components/Rendering/DDGIVolume.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/Query.h"
#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"  // complete type for EnsureFeature<DDGIProbeFeature>'s make_unique
#include "Engine/Rendering/DDGIEligibleView.h"
#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>

namespace GameEngine { namespace Engine::Renderer {

namespace
{
// How far the camera may drift from a following volume's centre, as a fraction
// of the volume's half-extent, before the grid re-centres on it.
//
// The trade is between two costs that pull opposite ways: re-centring clears
// the whole field, and NOT re-centring lets the camera approach the volume's
// face, where probes are least reliable because half their trilinear
// neighbourhood is outside the grid. A quarter of the half-extent keeps the
// viewer inside the well-sampled interior while bounding re-centres to one per
// quarter-half-extent of travel.
constexpr float kRecentreFraction = 0.25f;
}  // namespace

DDGIVolumeDesc DDGIVolumeSystem::ResolveActiveVolume(ECS::World& world)
{
    Components::DDGIVolume comp{};
    Components::WorldTransform xform{};
    bool found = false;
    int count = 0;

    world.Query<ECS::Read<Components::DDGIVolume>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle /*e*/, const Components::DDGIVolume& v,
                 const Components::WorldTransform& wt)
        {
            ++count;
            if (found)
                return;
            comp = v;
            xform = wt;
            found = true;
        });

    if (count > 1)
    {
        static bool warnedMultiple = false;
        if (!warnedMultiple)
        {
            LOG_WARNING("Multiple enabled DDGIVolume components found ({}). Only the first will be used.",
                       count);
            warnedMultiple = true;
        }
    }

    DDGIVolumeDesc desc{};  // default: Enabled=false -> DDGIProbeFeature declares nothing
    if (!found)
        return desc;

    // Translation is the 4th column of the column-major WorldTransform matrix
    // (matches every other GPUInstance/RTShadowMaskService transform read in
    // this codebase). Size comes from the same transform's scale — the volume
    // is a unit cube scaled by it (DDGIVolume.h's convention, shared with
    // PostProcessVolume). v1 ignores rotation only: the box stays
    // world-axis-aligned. ComputeVolumeHalfExtents is the ONE derivation, also
    // used by the editor gizmo so the drawn box is the box that reaches the GPU.
    const float* m = xform.matrix;
    const float centerX = m[12];
    const float centerY = m[13];
    const float centerZ = m[14];

    float halfExtents[3];
    DDGIProbeFeature::ComputeVolumeHalfExtents(m, halfExtents);

    desc.Enabled = true;
    desc.WorldId = world.GetWorldId();
    desc.GridMinWS[0] = centerX - halfExtents[0];
    desc.GridMinWS[1] = centerY - halfExtents[1];
    desc.GridMinWS[2] = centerZ - halfExtents[2];
    desc.GridSizeWS[0] = halfExtents[0] * 2.0f;
    desc.GridSizeWS[1] = halfExtents[1] * 2.0f;
    desc.GridSizeWS[2] = halfExtents[2] * 2.0f;
    desc.Fit = comp.Fit;
    desc.FollowRange = comp.FollowRange;
    desc.FollowHeightFraction = comp.FollowHeightFraction;
    desc.ProbePlacement = comp.ProbePlacement;
    desc.ClassifyStrength = comp.ClassifyStrength;
    desc.DebugView = comp.DebugView;
    desc.ProbesLongAxis = comp.ProbesLongAxis;
    desc.RaysPerProbe = comp.RaysPerProbe;
    desc.Intensity = comp.Intensity;
    desc.BounceIntensity = comp.BounceIntensity;
    desc.SkyIntensity = comp.SkyIntensity;
    desc.RadianceClamp = comp.RadianceClamp;
    desc.JitterMode = comp.JitterMode;
    desc.Hysteresis = comp.Hysteresis;
    desc.FireflyClamp = comp.FireflyClamp;
    desc.ChangeThreshold = comp.ChangeThreshold;
    desc.SnapAmount = comp.SnapAmount;
    desc.NormalBiasScale = comp.NormalBiasScale;
    desc.ChebyshevStrength = comp.ChebyshevStrength;
    desc.DepthSharpness = comp.DepthSharpness;
    desc.DepthResolution = comp.DepthResolution;
    desc.FilterStrength = comp.FilterStrength;
    desc.FilterSmoothness = comp.FilterSmoothness;
    desc.ContinuousSolve = comp.ContinuousSolve;
    desc.ConvergedSolve = comp.ConvergedSolve;
    desc.EnableFineCascade = comp.Cascades == Components::DDGICascadeMode::Cascaded2;
    desc.FineCascadeExtentFraction = comp.FineCascadeExtentFraction;
    desc.EnableGlossy = comp.EnableGlossy;
    desc.ReflectionIntensity = comp.ReflectionIntensity;
    desc.GlossyResolveScale = comp.GlossyResolveScale;
    return desc;
}

void DDGIVolumeSystem::ApplyCameraFit(DDGIVolumeDesc& desc)
{
    if (!desc.Enabled || desc.Fit != Components::DDGIVolumeFit::FollowCamera)
    {
        // Dropping back to Manual must forget the followed centre, or
        // re-enabling FollowCamera later would resume from a stale one.
        m_HasFollowCentre = false;
        return;
    }
    if (!m_RenderServices)
        return;

    // The authored box, captured before it is replaced. It is the fallback
    // centre for the tick where no eligible view exists AND nothing has been
    // followed yet — leaving the transform's min against the synthesised size
    // would otherwise describe a box that is neither.
    const Mathematics::Vector3 authoredCentre(desc.GridMinWS[0] + desc.GridSizeWS[0] * 0.5f,
                                              desc.GridMinWS[1] + desc.GridSizeWS[1] * 0.5f,
                                              desc.GridMinWS[2] + desc.GridSizeWS[2] * 0.5f);

    // A followed volume is sized NUMERICALLY, not by the transform — see
    // Components::DDGIVolume::FollowRange. Resolved before the centre because
    // SnapCentreToProbeGrid quantises to this box's probe spacing.
    const float range = std::max(desc.FollowRange, 0.1f);
    const float heightFraction = std::clamp(desc.FollowHeightFraction, 0.05f, 4.0f);
    desc.GridSizeWS[0] = range * 2.0f;
    desc.GridSizeWS[1] = range * 2.0f * heightFraction;
    desc.GridSizeWS[2] = range * 2.0f;

    Mathematics::Vector3 cameraWS;
    if (m_FollowTracker.Update(m_RenderServices->Views(), desc.WorldId, cameraWS))
    {
        bool recentre = !m_HasFollowCentre;
        for (int axis = 0; axis < 3 && !recentre; ++axis)
        {
            const float drift = std::abs(cameraWS[axis] - m_FollowCentreWS[axis]);
            recentre = drift > (desc.GridSizeWS[axis] * 0.5f) * kRecentreFraction;
        }
        if (recentre)
        {
            // Snapping to whole probe cells lands the lattice on the same
            // world-space positions wherever it re-centres, so repeated
            // re-centres cannot accumulate a sub-cell drift in probe positions.
            m_FollowCentreWS = DDGIProbeFeature::SnapCentreToProbeGrid(cameraWS, desc.GridSizeWS,
                                                                       desc.ProbesLongAxis);
            m_HasFollowCentre = true;
        }
    }
    else if (!m_HasFollowCentre)
    {
        // No eligible view has ever been seen (nothing rendering yet, or every
        // view is a preview capture). Sit on the authored position until one
        // appears; once a centre HAS been followed, hold it instead — a view
        // blinking out for a frame must not move the grid, because moving it
        // clears the field.
        m_FollowCentreWS = authoredCentre;
    }

    for (int axis = 0; axis < 3; ++axis)
        desc.GridMinWS[axis] = m_FollowCentreWS[axis] - desc.GridSizeWS[axis] * 0.5f;
}

void DDGIVolumeSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    // EnsureFeature mirrors AmbientLightSystem/SkyEnvironmentSystem: the
    // feature object is default-constructed here on first touch (or by
    // DDGINode, whichever runs first) even though its GPU state is not ready
    // until DDGINode's frame-scope Declare calls Initialize() — pushing the
    // volume desc onto an as-yet-uninitialized feature is harmless
    // (DeclareProbePasses no-ops until initialized) and means the desc is
    // already waiting the moment Initialize() runs, rather than lost for a frame.
    auto& feature = rs->EnsureFeature<DDGIProbeFeature>();
    DDGIVolumeDesc desc = ResolveActiveVolume(world);
    ApplyCameraFit(desc);
    feature.SetActiveVolume(desc);
}

} } // namespace GameEngine::Engine::Renderer
