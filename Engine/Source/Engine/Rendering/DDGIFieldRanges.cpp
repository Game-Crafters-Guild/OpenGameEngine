#include "Engine/Rendering/DDGIFieldRanges.h"

#include "Components/ComponentRegistration.h"
#include "Components/Rendering/DDGIVolume.h"

// Inspector slider bounds for DDGIVolume's float fields, mirroring the C++-side
// clamps DDGIProbeFeature::SetActiveVolume already enforces (Engine/Source/Engine/
// Rendering/DDGIProbeFeature.cpp) so the editor visually reflects the same limits
// instead of only silently re-clamping after the fact. ProbesLongAxis and
// RaysPerProbe are int32 — the inspector's integer row renderer does not yet
// consume SetFieldRange (only the float row does), so setting a range on them is
// forward-compatible metadata, not a visible clamp, until that renderer gains
// range support; the tooltips on those fields (DDGIVolume.h's "// @ge-tooltip"
// markers) already state the 2..32 / 32..256 bounds in the meantime.
#define GE_DDGI_RANGE(Field, Lo, Hi)                                                             \
    ::GameEngine::Components::SetReflectedFieldRange<::GameEngine::Components::DDGIVolume>(#Field, \
                                                                                            (Lo), (Hi))

namespace GameEngine::Engine::Renderer
{

void RegisterDDGIFieldRanges()
{
    using namespace ::GameEngine::Components;

    // No Extents rows: the volume is sized by its entity's transform scale
    // (DDGIVolume.h), so the scale gizmo and the Transform inspector are the
    // sizing UI — there is no component field left to give a slider range.
    // Intensity has no inspector range — the reference demo's intensity slider
    // is `.min(0)` with no max, and hosted Sponza runs at 20.
    // RadianceClamp has none either: it is a ceiling in the field's own
    // radiance units, so a photometric scene (a 400 kLux sun) needs values
    // around 3e5 where a unitless one sits near the 8.0 default. The feature
    // only floors it at 0 (DDGIProbeFeature::SetActiveVolume).
    GE_DDGI_RANGE(BounceIntensity, 0.0f, 4.0f);
    GE_DDGI_RANGE(SkyIntensity, 0.0f, 2.0f);
    GE_DDGI_RANGE(Hysteresis, 0.0f, 0.99f);
    GE_DDGI_RANGE(FireflyClamp, 1.0f, 20.0f);
    GE_DDGI_RANGE(ChangeThreshold, 0.5f, 8.0f);
    GE_DDGI_RANGE(SnapAmount, 0.0f, 0.9f);
    GE_DDGI_RANGE(NormalBiasScale, 0.0f, 8.0f);
    GE_DDGI_RANGE(ChebyshevStrength, 0.0f, 1.0f);
    GE_DDGI_RANGE(ClassifyStrength, 0.0f, 1.0f);
    GE_DDGI_RANGE(DepthSharpness, 0.01f, 200.0f);
    GE_DDGI_RANGE(FilterStrength, 0.0f, 1.0f);
    GE_DDGI_RANGE(FilterSmoothness, 0.0f, 1.0f);
    GE_DDGI_RANGE(ReflectionIntensity, 0.0f, 1.0f);
    GE_DDGI_RANGE(FineCascadeExtentFraction, 0.05f, 0.9f);
}

}  // namespace GameEngine::Engine::Renderer

#undef GE_DDGI_RANGE
