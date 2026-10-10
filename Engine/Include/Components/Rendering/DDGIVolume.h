#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

/// How probes are positioned inside the volume. Grid is the plain uniform
/// lattice: every probe sits exactly at its cell centre and every probe
/// contributes. Adaptive keeps the SAME lattice topology — DDGI's lookup is
/// grid-trilinear by construction (Includes/ddgi_probes.glsl), so genuinely
/// scattered probes would need a different interpolation scheme entirely —
/// but per probe it runs the industry-standard relocation + classification
/// pass: a probe embedded in geometry is nudged out of the solid within its
/// own cell, and one that stays buried is deactivated so it stops polluting
/// the trilinear gather with light it can never see.
enum class DDGIProbePlacement : int32 {
    Grid = 0,
    Adaptive = 1,
};

/// How many probe grids the volume runs. SingleGrid is the coarse lattice
/// alone; Cascaded2 adds the finer C1 sub-volume. Matches the reference
/// demo's cascades control (`single grid` / `cascaded (2)`).
enum class DDGICascadeMode : int32 {
    SingleGrid = 0,
    Cascaded2 = 1,
};

/// Where the probe grid's CENTRE comes from. Manual is the authored
/// transform: GI exists exactly where the volume was placed, and nothing
/// moves it. FollowCamera re-centres the grid on the active camera each
/// frame, so a scene gets GI without anyone placing a volume at all. A
/// followed grid is sized by FollowRange and FollowHeightFraction, not by
/// the transform's scale.
///
/// FollowCamera snaps the centre to whole probe cells (see
/// DDGIProbeFeature::SnapCentreToProbeGrid). Sub-cell camera motion therefore
/// moves no probe at all, which is what keeps a following grid from
/// re-converging every frame.
enum class DDGIVolumeFit : int32 {
    Manual = 0,
    FollowCamera = 1,
};

/// Resolution the glossy-reflection resolve pass runs at, as a fraction of
/// the view's render extent. The probe reflection gather is evaluated once
/// per resolve pixel into two screen textures (one per baked lobe) that the
/// forward pass bilinear-taps, instead of once per forward fragment. Full
/// skips the resolve pass entirely and keeps the inline per-fragment gather
/// — byte-identical to the pre-resolve path.
enum class DDGIGlossyResolveScale : int32 {
    Quarter = 0,
    Half = 1,
    ThreeQuarter = 2,
    Full = 3,
};

/// What the DDGI consumer draws instead of lit shading. Diagnostic only: the
/// probe field is a 3D structure sampled per pixel, so the fastest way to see
/// why it looks wrong somewhere is to draw what it actually resolved there
/// rather than infer it from the shaded result.
enum class DDGIDebugView : int32 {
    None = 0,
    /// Fraction of the trilinear cell backed by ACTIVE probes, white = 1.
    /// Anything not white is a point the field only partly answers for.
    Coverage = 1,
    /// Green where the nearest probe is active, red where it is classified
    /// buried, brightness = how far relocation has pushed it from its cell.
    ProbeState = 2,
    /// Cascade ownership: red = coarse only, green = fine, yellow = crossfade.
    CascadeWeight = 3,
    /// The NEAREST coarse probe's stored irradiance toward the surface normal,
    /// with no trilinear blend, visibility or classify weighting: what one
    /// probe actually holds, shown at scene exposure like lit shading.
    NearestProbeIrradiance = 4,
    /// The same two views against the FINE cascade's own grid and atlas.
    ProbeStateFine = 5,
    NearestProbeIrradianceFine = 6,
    /// Grid coordinate and storage slot of the nearest FINE probe, as
    /// xyz / resolution. Storage is toroidal (Includes/ddgi_common.glsl), so a
    /// defect tied to a slot shows here as a plane fixed in the world while
    /// the coordinate view stays continuous.
    FineProbeCoord = 7,
    FineProbeSlot = 8,
};

/// Resolution of the depth-moment tile each probe stores for the Chebyshev
/// visibility test. Shared keeps the moments in the 6x6 irradiance tile: one
/// fused blend and upload. Fine gives the moments their own 14x14 tile
/// (RTXGI's ratio), so an occluder's direction is resolved about 2.3x more
/// finely and the test leaks less at a given strength. Fine adds one
/// 256-thread depth blend and one depth upload dispatch per cascade per tick,
/// and its atlas is 4x the shared one's bytes (RGBA16F at 16x16 tiles against
/// 8x8).
enum class DDGIDepthResolution : int32 {
    Shared = 0,
    Fine = 1,
};

/// What the solve does once the field has converged. Continuous keeps the
/// full adaptive ray budget running every tick (the reference library's
/// behaviour). Throttle reads the per-texel steady-tick counts back each
/// tick and, once nearly every texel has sat at its history cap, drops the
/// budget to a small patrol so the GPU time goes back to the frame; a
/// lighting or geometry change resets the counts of the probes it reaches,
/// the readback sees the mean fall, and the full budget returns within a
/// few ticks. The patrol is what bounds that latency: a smaller sweep would
/// take longer to notice a change.
enum class DDGIConvergedSolve : int32 {
    Continuous = 0,
    Throttle = 1,
};

/// Which sampling regime the probe solve runs — the reference library's
/// jitterMode pair. Gated HOLDS the spherical-Fibonacci ray basis: every
/// solve traces the same directions, so a converged field is a fixed point
/// and shows zero temporal noise, moving camera or not. MonteCarlo rotates
/// the basis every solve for temporal decorrelation — better long-run
/// angular coverage, but the field carries permanent low-level shimmer that
/// its higher paired Hysteresis only bounds, never removes.
enum class DDGIJitterMode : int32 {
    Gated = 0,
    MonteCarlo = 1,
};

// Dynamic diffuse global illumination probe volume. Placement is authored by
// default; DDGIVolumeFit::FollowCamera opts a volume into tracking the camera.
//
// SIZE ALWAYS COMES FROM THE TRANSFORM, in both fit modes, matching
// PostProcessVolume's convention:
// the volume is a unit cube (edge length 1, so half-extent 0.5) scaled and
// positioned by the entity's WorldTransform. Scale the entity to size the
// probe grid — the standard scale gizmo is the sizing tool, and there is no
// second source of truth to keep in sync. v1 ignores ROTATION only: the grid
// stays world-axis-aligned (a rotated probe grid would need oriented-box
// sampling in the consumer), so a rotated transform is read for its
// translation and scale magnitude alone.
//
// See Engine::Renderer::DDGIProbeFeature for the GPU-side probe field this
// drives, and its ComputeVolumeHalfExtents for the single derivation both
// the extraction system and the editor gizmo share.
struct DDGIVolume {
    // Diagnostic overlay (see DDGIDebugView). Costs nothing when None: the
    // debug evaluation lives in the resolve compute pass, never in the forward
    // shader, so switching it on cannot perturb the frame it is measuring.
    // @ge-tooltip Draws what the probe field resolved instead of lit shading. Coverage shows where the probes only partly answer for a surface, Probe State shows which probes were classified buried and how far they relocated, Cascade Weight shows which grid owns each pixel. Diagnostic only — costs nothing when off.
    DDGIDebugView DebugView = DDGIDebugView::None;

    // @ge-tooltip Draws the probe lattice in the Scene View, matching the reference demo's show-probes checkbox. Selecting the volume draws its bounds, not its probes, so this stays authoritative while you edit it.
    bool ShowProbes = false;

    // Manual keeps the authored transform's translation. FollowCamera
    // overrides it with the active camera's position, snapped to probe
    // spacing so sub-cell motion moves nothing.
    // @ge-tooltip Where the grid's centre comes from. Manual keeps it where you placed it. Follow Camera re-centres it on the camera every frame so the scene has GI everywhere the camera goes, snapped to whole probe cells so small movements cost nothing. In Manual the entity's scale sizes the grid; in Follow Camera, Follow Range and Follow Height Fraction size it.
    DDGIVolumeFit Fit = DDGIVolumeFit::Manual;

    // Horizontal reach of a FollowCamera volume, in world units: GI extends
    // this far from the camera on X and Z. Ignored in Manual mode.
    //
    // A followed volume sizes itself NUMERICALLY rather than from the
    // transform, because in that mode the transform describes nothing the user
    // can see — the box is not where the entity is. Each fit mode therefore has
    // exactly one source of truth for size: Manual reads the transform, and
    // FollowCamera reads this. Probe spacing follows from it and ProbesLongAxis
    // (range * 2 / (probes - 1)), which is the number that actually decides
    // whether small geometry is resolved.
    // @ge-tooltip How far GI reaches from the camera, in world units, when Fit is Follow Camera. Larger covers more of the level but spreads the same probe count thinner - probe spacing is range * 2 / (Divisions - 1). Ignored when Fit is Manual, where the entity's scale sizes the volume instead.
    float32 FollowRange = 16.0f;

    // Vertical reach as a fraction of FollowRange. Levels are wider than they
    // are tall, so a cube spends probes on empty air above and below the
    // playable space; 0.75 is the same default Godot's SDFGI y-scale ships.
    // Only a SYNTHESISED box needs this — an authored volume already gets its
    // aspect from the transform's per-axis scale.
    // @ge-tooltip Vertical reach of a Follow Camera volume, as a fraction of Range. Below 1 spends fewer probes on empty headroom, which is usually what a level wants. Ignored when Fit is Manual.
    float32 FollowHeightFraction = 0.75f;

    // Probe positioning inside the volume (see DDGIProbePlacement). Adaptive
    // is the default because relocation + classification is strictly better
    // wherever a probe can land inside geometry — which is every volume that
    // straddles a wall, floor or prop — and costs one short-ray pass per tick
    // over the round-robin probe window. Grid stays available as the cheaper,
    // fully predictable lattice, and as the A/B control for seeing exactly
    // what Adaptive changed: in Grid mode no classify pass is dispatched at
    // all and every probe keeps its clear-time state (zero relocation offset,
    // active).
    // @ge-tooltip How probes are positioned. Adaptive (default) keeps the uniform lattice but nudges each probe out of any geometry it is embedded in, and deactivates one that stays buried so it stops leaking light it cannot see. Grid is the plain lattice — every probe sits at its cell centre and contributes, no classify pass — cheaper and fully predictable, and the A/B control for judging what Adaptive changed.
    DDGIProbePlacement ProbePlacement = DDGIProbePlacement::Adaptive;

    // How much authority the classify pass has over the field, in [0,1]. It
    // scales BOTH of that pass's outputs at every point they are applied: the
    // per-probe relocation offset (consumer, recursive bounce gather, and the
    // trace kernels' ray origin) and the buried flag, which becomes a weight
    // mix(1, active, strength) rather than a hard reject. 0 makes Adaptive
    // placement behave exactly like Grid — no relocation, no deactivation —
    // and skips the classify dispatch entirely, so it is also the cheap way
    // to A/B what classification is doing without editing the placement mode.
    //
    // Default deviates from the upstream library, deliberately. There the
    // knob ships at 0 (gi_settings.js:14 giSolid) because its classification
    // is opt-in for solid scenes — a backface test misreads thin two-sided
    // walls. This engine already shipped Adaptive placement ON by default,
    // so adopting 0 would silently switch that feature off; 1 is the value
    // that preserves it, and Grid remains the explicit way to turn it off.
    // @ge-tooltip How much authority probe classification has, from 0 (off - probes are never relocated or deactivated, exactly like Grid placement, and the classify pass is skipped) to 1 (full). Lower it if thin or two-sided walls are being misread as solid and probes are wrongly deactivated. Does nothing while Probe Placement is Grid. Applied live, no grid rebuild.
    float32 ClassifyStrength = 1.0f;

    // Probes along the grid's longest axis; other axes scale by aspect
    // ratio. Clamped 2..32 (DDGIProbeFeature re-clamps defensively).
    //
    // Deliberately NOT the upstream library's 16: measured on the Cornell
    // reference scene, 16 cost ~2.4x the trace/blend work for an 18% WORSE
    // colour-bleed separation and visibly blotchier back-wall irradiance than
    // 12 (not a convergence artifact — a 150 s settle reproduced it exactly).
    // Their knob is also a spacing divisor rather than a probe count, so 16
    // there is ~17 probes here; the number was never directly transferable.
    // @ge-tooltip Probe count along the grid's longest axis; the other two axes scale by aspect ratio. Higher = finer indirect detail at a roughly cubic cost in trace/blend/upload work. Clamped 2-32; a resolution change rebuilds the probe grid after a short idle delay, not every frame while dragging.
    int32 ProbesLongAxis = 12;

    // Rays traced per probe per tick. Clamped 32..256 by the feature. The
    // trace kernels stride a 64-thread workgroup over the ray set, so a probe
    // with more rays than threads loops; the adaptive per-tick budget is in
    // rays, so raising this visits fewer probes per tick rather than costing
    // more per tick. 64 is the port's measured default; RTXGI ships 256.
    // @ge-tooltip Rays traced per probe per tick — more rays reduce noise per solve at a linear trace cost. Clamped 32-256. The per-tick budget is counted in rays, so a higher value visits fewer probes each tick and the whole grid takes longer to sweep; 256 is what RTXGI ships, 64 is this port's measured default.
    int32 RaysPerProbe = 64;

    // Multiplier on the final blended irradiance before it reaches the
    // forward pass's ambient term.
    // @ge-tooltip Multiplier on the final blended irradiance before it reaches the forward pass's ambient term. Applied live, no grid rebuild.
    float32 Intensity = 1.0f;

    // Strength of surface-to-surface bounce: it scales the recursive term the
    // trace kernels feed back into the field, where Intensity scales the
    // finished field at consume time.
    //
    // It SATURATES, and the range where it does anything is narrow — measured,
    // not assumed. Two independent bounds pin it: the convergence gain bound
    // rescales albedo*BounceIntensity so its largest channel lands on
    // GE_DDGI_MAX_BOUNCE_ALBEDO (a UNIFORM rescale, so above roughly
    // 1/albedo the knob is renormalized straight back out), and RadianceClamp
    // then pins the term's luminance. On the Cornell reference scene 1 and 10
    // measured identical to three decimal places, while 0 vs 1 moved mean
    // luminance 26 -> 62. Treat it as "how much multi-bounce, between none and
    // full", not as a colour-bleed depth control: it cannot deepen bleed,
    // because a uniform scale on the gain is exactly what the gain bound
    // undoes. (The earlier per-channel clamp did change hue as this rose —
    // by clipping the dominant channel and collapsing saturated bounce toward
    // grey, which was a defect, not the feature the knob advertised.)
    // @ge-tooltip How much multi-bounce light the field carries, scaling the recursive bounce term rather than the final output. Useful mainly between 0 (single bounce only) and about 1 (full multi-bounce); above that the convergence bound and the radiance clamp renormalize it away and it stops doing anything. To change how strongly colour moves between surfaces, use Radiance Clamp - not this. Applied live, no grid rebuild.
    float32 BounceIntensity = 1.0f;

    // Scale on the sky radiance a probe ray picks up when it hits nothing.
    // 0 makes the field ignore the sky entirely (miss = black, indoor-only
    // GI); 1 is the environment's own brightness. It multiplies with the
    // scene's global SkyEnvironment/Skybox IblIntensity rather than replacing
    // it, so the global sky slider and this per-volume trim compose the way
    // the raster path's ambient already composes them.
    // @ge-tooltip How much sky light the probes pick up on rays that hit nothing. 0 ignores the sky completely (indoor-only GI, miss rays are black), 1 is the environment's own brightness. Multiplies with the scene's global sky IBL intensity rather than replacing it. Applied live, no grid rebuild.
    float32 SkyIntensity = 1.0f;

    // Luminance ceiling on the recursive bounce term. A ceiling, not an
    // on/off: 0 admits no bounce at all. Applied as a uniform scale by
    // clamp/luminance once luminance exceeds it, so an outlier is dimmed
    // without its hue being clipped. 8 is the upstream library's own default.
    // @ge-tooltip Ceiling on how bright a single bounce sample may be, which is what stops one over-bright surface from smearing a firefly through the probe field. Dims an outlier without changing its colour. This is a ceiling, not a switch - 0 means no bounce light at all. Applied live, no grid rebuild.
    float32 RadianceClamp = 8.0f;

    // The ray-basis regime. Gated (the reference library's default) holds
    // the sampling basis, so a converged field is a fixed point — flicker-free
    // even mid-fly-through. MonteCarlo applies a fresh uniformly random 3D
    // rotation every solve (Includes/ddgi_common.glsl's GE_DDGIRayBasis), so
    // every ray covers the whole sphere over time rather than one elevation
    // band. The regime and Hysteresis pair (see Hysteresis); flipping this
    // without moving that slider trades a mismatch documented there.
    // @ge-tooltip Sampling regime for the probe solve. Gated (default, matching the reference library) traces the same ray directions every solve, so once the field converges it is perfectly stable - no shimmer, even while the camera moves. Monte Carlo rotates the ray set every solve for better long-run coverage of the sphere at the cost of permanent low-level temporal noise. Pair Gated with Hysteresis around 0.6 and Monte Carlo with around 0.9. Applied live, no grid rebuild.
    DDGIJitterMode JitterMode = DDGIJitterMode::Gated;

    // Temporal blend retention in [0,1). Higher = smoother/slower to
    // converge, lower = noisier/faster. The value is only meaningful paired
    // with JitterMode: 0.6 for a HELD (Gated) basis, 0.9 for the rotating
    // MonteCarlo basis — the reference library's own pairing
    // (JITTER_HYSTERESIS_DEFAULTS, gi_probes.js:147). Held-basis retention
    // under a rotating basis keeps too little history to average the rotation
    // out, and reads as large low-frequency blobs sliding over surfaces.
    // Retention this low is workable at all only because the blend kernel
    // measures each texel's own noise scale (see FireflyClamp /
    // ChangeThreshold / SnapAmount) rather than treating every change as
    // signal.
    // @ge-tooltip Temporal blend retention in [0,1), which is how much of the old estimate each texel keeps every solve. In frames: a value h takes about ln(0.05)/ln(h) solves to forget an old reading, so 0.9 is roughly 28 frames and 0.6 is roughly 6. Higher is smoother but slower to react to a light or geometry change; lower reacts faster but stays noisier. Pairs with Jitter Mode: 0.6 with Gated (the default pairing, matching the reference library) and 0.9 with Monte Carlo - a held-basis value under a rotating basis reads as large blobs drifting over surfaces. Applied live, no grid rebuild.
    float32 Hysteresis = 0.6f;

    // Firefly rejection band, in multiples of a texel's own measured
    // luminance standard deviation. An update further from history than this
    // is pulled back toward the band instead of accepted. Lower = steadier
    // but laggier.
    // @ge-tooltip How far a probe texel may jump in one update, measured in multiples of its own noise level. A bigger jump is treated as a firefly and pulled back rather than accepted. Lower is steadier but slower to react; raise it if real lighting changes arrive late.
    float32 FireflyClamp = 6.0f;

    // How many standard deviations of change count as a REAL lighting change
    // rather than this tick's Monte-Carlo noise. Lower = snappier.
    // @ge-tooltip How large a change must be, in multiples of a probe texel's own noise level, before it is treated as a real lighting change instead of sampling noise. Lower reacts sooner to real changes at the cost of also reacting to noise.
    float32 ChangeThreshold = 2.5f;

    // How much retention is dropped on a texel that saw a real change —
    // higher = harder snap toward the fresh estimate.
    // @ge-tooltip How hard the field snaps to new values where a real lighting change was detected. Higher converges faster after a light moves or a door opens; too high reintroduces noise on those texels.
    float32 SnapAmount = 0.30f;

    // Scale on the surface-normal sample bias (Includes/ddgi_common.glsl's
    // GE_DDGI_SURFACE_NORMAL_BIAS_CELL) — increase if thin geometry shows
    // self-shadowing artifacts in the indirect term, decrease if indirect
    // light visibly detaches from a surface. 1.75 is the upstream library's
    // canonical tuning, in identical units (both scale the same 3%-of-a-cell
    // offset).
    // @ge-tooltip Scale on the surface sample bias used when reading indirect irradiance. Increase if thin geometry shows self-shadowing in the indirect term; decrease if indirect light visibly detaches from a surface. Applied live, no grid rebuild.
    float32 NormalBiasScale = 1.75f;

    // How strongly the Chebyshev visibility test is allowed to reject a probe
    // that cannot see the shaded point — 0 is pure trilinear (maximum leak
    // through walls), 1 is the full test. Full strength over low-resolution
    // depth moments over-rejects on thin and two-sided geometry, so the
    // upstream library's canonical tuning admits a little leak back at 0.8
    // rather than raising probe density.
    // @ge-tooltip How aggressively light is stopped from leaking through walls. 1 is the full visibility test, 0 disables it (light bleeds through thin geometry). Backing off from 1 trades a little leak for less over-darkening on thin or two-sided geometry, which is why the default is not 1.
    float32 ChebyshevStrength = 0.8f;

    // Resolution of the per-probe depth-moment tile (see DDGIDepthResolution).
    // Shared is the historical field; Fine is the RTXGI-ratio tile, the
    // single change with the largest effect on light leaking through thin
    // walls, at the cost of one extra blend and upload dispatch per cascade
    // per tick. A change rebuilds the probe grid after the same idle delay a
    // resolution change does.
    // @ge-tooltip Resolution of the occluder-distance data each probe stores for the leak test. Shared keeps it in the 6x6 irradiance tile (the default, cheapest). Fine gives it its own 14x14 tile, the ratio RTXGI uses, which stops noticeably more light leaking through thin walls; costs one extra depth blend and upload dispatch per cascade per tick and four times the depth atlas memory. Rebuilds the grid after a short idle delay.
    DDGIDepthResolution DepthResolution = DDGIDepthResolution::Shared;

    // Cosine power the depth moments are gathered with, shaping how sharply a
    // probe texel's stored distance tracks the nearest occluder in exactly
    // that direction. The irradiance gather keeps its own plain cosine weight;
    // this raises that weight to this power for the DEPTH accumulation alone,
    // so 1 is the plain cosine-weighted mean distance, values below it average
    // the whole hemisphere almost uniformly (blurry moments, more leak), and
    // high values narrow the gather toward the texel's own direction (crisp
    // moments, harder occlusion, more sensitive to ray noise). Only matters
    // while ChebyshevStrength is above 0.
    //
    // Floored at 0.01 rather than reaching 0: pow(x, 0) is 1 for every ray
    // including the backfacing ones whose cosine weight is exactly 0, which
    // would admit distances from behind the texel. The upstream library floors
    // its own setter at the same value for the neighbouring reason (its
    // pow(0, 0) is indeterminate and poisons the depth history with NaN).
    // @ge-tooltip How sharply each probe direction's stored occluder distance tracks that exact direction. 1 is a plain cosine-weighted average; lower blurs the distance over the whole hemisphere, which leaks more light through walls; higher tracks the nearest occluder in that direction crisply at the cost of more noise. Does nothing while Chebyshev Strength is 0. Applied live, no grid rebuild.
    float32 DepthSharpness = 1.0f;

    // How much of the spatial denoise filter reaches the atlas. The filter is
    // a variance-adaptive bilateral pass over each probe's OWN octahedral tile,
    // applied where the blended field is copied into the atlas
    // (Shaders/ddgi_upload.comp) rather than into the temporal history, so it
    // cannot compound into an over-blurred field and 0 leaves the atlas exactly
    // as blended. Intra-tile only: no tap ever leaves a probe, so it cannot
    // move light between probes. 1 is the upstream library's default.
    // @ge-tooltip How strongly each probe's octahedral tile is denoised on its way into the atlas. 0 is off (the raw blended field, grainier at a given ray count), 1 is the full filter and matches the upstream library. It smooths only within one probe, never between probes, so it cannot bleed light through a wall. Applied live, no grid rebuild.
    float32 FilterStrength = 1.0f;

    // Width of that filter's edge-stop band: 0 keeps the baseline bandwidth
    // (directional detail preserved, least smoothing), 1 widens it about 7x
    // (more neighbouring directions trusted, strongest smoothing).
    // @ge-tooltip How large a difference between neighbouring probe directions the denoise filter still treats as noise rather than a real edge. Higher trusts more neighbours and smooths harder, at the cost of directional detail within a probe; lower keeps that detail and removes less grain. Does nothing while Filter Strength is 0.
    float32 FilterSmoothness = 0.5f;

    // Specular probe reflections: two Phong lobes (broad and sharp) baked
    // from the SAME rays the diffuse field already traces, crossfaded by
    // surface roughness and composited over the prefiltered environment cube
    // where the probes actually resolved geometry. Coarse cascade only.
    // @ge-tooltip Adds specular reflections baked from the probes, giving metals and smooth surfaces local reflections without a screen-space pass. Costs two extra blend/upload dispatches and one high-resolution atlas per volume; reuses the rays the diffuse field already traces, so it adds no ray cost.
    bool EnableGlossy = false;
    // @ge-tooltip How strongly probe reflections displace the environment reflection. At 0 the environment cube is used unchanged; at 1 the probes take full authority wherever they actually saw geometry.
    float32 ReflectionIntensity = 1.0f;

    // Screen-space resolution of the glossy-reflection resolve (see
    // DDGIGlossyResolveScale). The per-fragment gather is the dominant DDGI
    // consume cost — up to 2 cascades x 8 probes of atlas taps per fragment —
    // so resolving it at reduced resolution and tent-tapping the blurred
    // result recovers most of that cost. The resolve is the ONLY source of
    // probe reflections (Includes/ibl.glsl); Full runs it at view resolution
    // with no reduction. The lobe atlases are 6x6 and 16x16 texels per probe,
    // so a reflector that fills the view shows those texels at Full; the
    // reduced resolves' pre-blur and tent tap are what soften them. Default
    // Half for both the cost and that softening.
    // @ge-tooltip Resolution the probe reflections are computed at, as a fraction of the view. 50% (default) recovers most of the cost and its blur hides the probe lobes' coarse texels on large reflectors; 100% computes them per pixel and shows those texels on a close-up mirror. Applied live, no grid rebuild.
    DDGIGlossyResolveScale GlossyResolveScale = DDGIGlossyResolveScale::Half;

    // Whether the probe solve keeps running while a camera moves. On (the
    // reference library's own default) the bounded per-tick solve runs every
    // frame regardless of view motion. Off holds the entire solve — classify,
    // trace, blend and upload, both cascades and both reflection lobes —
    // until every scene/game camera has been at rest for a short quiet window,
    // trading a field frozen during a fly-through for the GPU time back. The
    // field is world-space, so holding it while the view moves changes nothing
    // that is not already lit; it only stops reacting to moved lights or
    // geometry until the view rests.
    // @ge-tooltip Keeps the probe solve running while the camera moves. On (default, matching the reference library) the field keeps converging during a fly-through. Off pauses the whole solve during camera motion and resumes shortly after the view comes to rest - the field is world-space, so it stays correct while paused, it just stops reacting to lighting or geometry changes until you stop moving. Costs nothing visually when the view is still; gives the GPU time back while it is not.
    bool ContinuousSolve = true;

    // What happens to the per-tick budget once the field has converged (see
    // DDGIConvergedSolve). Continuous is the reference's behaviour and the
    // default; Throttle is the RTXGI probe-variability idea, driven here by
    // the blend kernel's own steady-tick counts read back through a small
    // reduction pass each tick.
    // @ge-tooltip What the solve does once the probe field has settled. Continuous (default, matching the reference library) keeps spending the full per-tick ray budget. Throttle drops to a small patrol budget once nearly every probe texel has been steady for its full history, and restores the full budget within a few ticks of a lighting or geometry change. Costs one tiny reduction dispatch and readback per tick while on; gives most of the solve's GPU time back on a static scene.
    DDGIConvergedSolve ConvergedSolve = DDGIConvergedSolve::Continuous;

    // Second, finer probe grid (C1) covering a smaller sub-volume centered
    // on this volume — closer-range indirect detail than the coarse grid's
    // spacing alone gives. SingleGrid costs exactly what one lattice already
    // costs (C1 never allocates or dispatches). Default Cascaded2: the
    // reference library ships two cascades by default (setCascades, default
    // 2) — this is the reference-parity default, not a cheap one; it roughly
    // doubles trace/blend/upload cost. Switch to SingleGrid on a budget
    // where that doubling is not affordable.
    // @ge-tooltip How many probe grids to run. Single Grid is the coarse lattice alone. Cascaded (2) adds a second, finer grid covering a smaller region centered on this volume, for closer-range indirect detail — roughly double the trace/blend/upload cost. Cascaded is the default, matching the reference library.
    DDGICascadeMode Cascades = DDGICascadeMode::Cascaded2;
    // @ge-tooltip Fraction of this volume's extents the fine cascade covers (e.g. 0.35 = a sub-box 35% the size, centered the same). Clamped 0.05-0.9. Smaller = denser probes in a smaller region.
    float32 FineCascadeExtentFraction = 0.35f;
};

} // namespace Components
} // namespace GameEngine
