#pragma once

namespace GameEngine {
namespace Rendering {
namespace Passes {

// Sub-pixel correction for the TAA resolve (taa_resolve.comp /
// taa_resolve_upscale.comp): reduces history trust as a pixel's motion phase
// moves away from texel alignment — the worst case for bilinear-alike history
// resampling — independent of the resolve's existing velocity-falloff term.
// Ported from the reference TRAA implementation's subpixelCorrection, which
// documents the trade this toggle exists to A/B: it reduces blur under slow
// sub-pixel motion, at the cost of a possible square-pattern artifact.
// Default ON, matching the reference. Process-wide and settable at runtime
// (the editor's Experimental settings page writes it) — same discipline as
// TemporalDither.h's two toggles: read on the render-declare path, written
// from the UI thread, relaxed atomics (a toggle landing one frame later is
// invisible).
bool IsTaaSubpixelCorrectionEnabled();
void SetTaaSubpixelCorrectionEnabled(bool enabled);

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
