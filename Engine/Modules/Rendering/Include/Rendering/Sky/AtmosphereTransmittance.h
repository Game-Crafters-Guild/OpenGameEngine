#pragma once

#include "Rendering/Sky/SkyRenderer.h" // AtmosphereParametersGPU
#include "Rendering/Sky/SkySystem.h"   // SkySystemConfig, SkySystemState

namespace GameEngine {
namespace Rendering {

/// The atmosphere the sky LUTs bake, as the ground-level evaluators below take it. Its scattering
/// parameters are fixed (SkyRenderNode's FillAtmosphereFromSettings overrides only the ground,
/// horizon and below-horizon fields), so the extinction a ground-level surface sees can be
/// evaluated from the same model the GPU integrates.
const AtmosphereParametersGPU& ScatteringAtmosphere();

/// Colour of the primary celestial source as it reaches the GROUND: `colorAboveAtmosphere`
/// attenuated by the extra air mass its light travels through at elevation `sourceUpDot` (the
/// zenith cosine of the direction toward the source: 1 = overhead, 0 = on the horizon, < 0 = below
/// it, where the path runs through the planet and the result falls to ~0).
///
/// Relative to the ZENITH path, so an overhead source comes through unchanged. A directional
/// light's authored illuminance is already a ground value (see kSkyIrradianceScale, which lifts it
/// to top-of-atmosphere for the sky's source term), so applying the overhead extinction to it a
/// second time would dim every scene's key light at noon. What time of day changes is the extra
/// air mass, and that is what this returns.
///
/// The extinction is the optical-depth integral `sky_transmittance_lut.comp` bakes — same
/// Rayleigh/Mie densities, same 32-step midpoint quadrature — evaluated on the CPU at the planet
/// surface, because a directional light's colour is decided long before that LUT is sampled.
///
/// How closely it tracks that LUT is elevation-dependent, and only meaningful ABOVE the horizon.
/// Against an emulation of the LUT's own sampling (256x64, `u_mu = 0.5 + 0.5 mu`, bilinear) the two
/// agree to 0.2 % down to 7.5 degrees and 0.8 % at 2, then diverge fast at grazing — over 10 % in
/// blue below about 1 degree, where one mu texel spans ~0.45 degrees and the function changes by
/// more than that across it. Same model, not the same numbers.
///
/// BELOW the horizon the two are not even the same quantity, so no parity is claimed. This
/// evaluator sits on the planet surface and takes the chord straight through the planet: hundreds
/// of kilometres of full-density air, transmittance ~0. The LUT's bottom row sits ~6 m higher and
/// takes the short path down to the ground instead — a few hundred metres — and returns ~0.99. The
/// depth below the horizon is additionally floored at the grazing value here: the chord is SHORTER
/// than the grazing path for the first degree after a source sets, so without the floor a setting
/// sun brightens before it goes out.
///
/// (The dome does not use this. `sky_view_lut.comp` has no planet-shadow test at all — its only
/// occlusion term is that same LUT sample, which is ~1 near the surface — so its night comes from
/// the sun/moon intensity blend and the night composite, not from extinction.)
///
/// Ground consumers only. The sky's own passes attenuate for themselves — the sky-view LUT
/// multiplies the per-step transmittance into its in-scatter source term and the sun disc
/// multiplies the LUT along the view ray — so handing this colour to a sky shader would count the
/// extinction twice.
void EvaluateGroundLevelSunColor(const AtmosphereParametersGPU& atmosphere,
                                 const float colorAboveAtmosphere[3],
                                 float sourceUpDot,
                                 float outColor[3]);

/// The two celestial bodies' ground-level colours, each extinguished at its OWN elevation, and how
/// far the primary light has handed over from the sun to the moon. Evaluated once per frame (each
/// colour is a transmittance integral); the mixes below derive the colours the frame consumes.
///
/// Mixing the two DIRECTIONS first and extinguishing once is the trap this exists to close. With
/// the default antipodal moon the blended direction flips sign at blend 0.5, so a single evaluation
/// jumps from "sun three degrees under the western horizon" to "moon four degrees over the eastern
/// one" in one step, and the light snaps back on from the wrong side of the sky with a source that
/// is still half white. Extinguishing each body where it actually is keeps the handoff continuous.
struct SkyBodyGroundColors
{
    /// The sun's ground colour: the sun tint extinguished at the sun's elevation, scene-linear.
    float Sun[3] = {0.0f, 0.0f, 0.0f};
    /// The moon's ground colour: SkySystemConfig::moonColor extinguished at the moon's elevation.
    float Moon[3] = {0.0f, 0.0f, 0.0f};
    /// The handover weight w in [0, 1]: state.primaryMoonBlend clamped, 0 when the config keeps the
    /// sun primary.
    float MoonBlend = 0.0f;
};

/// `sunTint` is the SUN's above-atmosphere colour: white for the physical sky, or the authored
/// stylistic tint. The moon's source is `config.moonColor` ALONE, deliberately untinted, its
/// magnitude carrying the stylized moon-to-sun illuminance ratio. Keep that asymmetry: the moon
/// disc is drawn from the same untinted colour, so tinting moonlight here would light the ground
/// with a colour the visible moon does not have.
SkyBodyGroundColors EvaluateBodyGroundColors(const AtmosphereParametersGPU& atmosphere,
                                             const SkySystemConfig& config,
                                             const SkySystemState& state,
                                             const float sunTint[3]);

/// Ground-level colour of the PRIMARY light source across the sun/moon handoff (the sky's
/// primarySunGroundColor): the two bodies' ground colours mixed by colour, (1 - w) x sun + w x moon.
void MixPrimaryGroundColor(const SkyBodyGroundColors& bodies, float outColor[3]);

/// Colour of a directional light that carries both bodies, as the sky drives a linked light: the
/// two ground colours mixed by the energy each delivers rather than by colour,
/// (1 - w) x sun + w x moonLightScale x moon. Both terms sit on the light's day scale, so
/// `moonLightScale` is the moon's illuminance as a fraction of the sun's (LinkedLightMoonScale) and
/// the light's Intensity stays the author's day value. By day (w == 0) this is the sun's ground
/// colour exactly; at full night (w == 1) the moon's times `moonLightScale`; between, each body
/// fades by its own share and neither is dimmed twice.
void MixLinkedLightGroundColor(const SkyBodyGroundColors& bodies, float moonLightScale, float outColor[3]);

} // namespace Rendering
} // namespace GameEngine
