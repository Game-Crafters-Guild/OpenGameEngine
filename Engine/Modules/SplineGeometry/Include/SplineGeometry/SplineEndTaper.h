#pragma once

#include "SplineGeometry/SplineStripBuilder.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::SplineGeometry
{

// How a run's OPEN ends stop.
//
// A swept strip has no piece ends to bury, but it still has two of its own: the
// first ring and the last. Left alone each is a square cut across the whole
// cross-section, which reads as a wall of surface standing in the open wherever
// a run stops short of something to meet — the one discontinuity the welded
// interior does not have.
//
// The taper operates on the FINAL per-station half-widths, whatever produced
// them. An authored width channel and a measured bank fit both arrive here as
// the same two numbers per station, so both modes end the same way and neither
// needs to know about this.
struct SplineEndTaperParams
{
    // Length of the pinch at each open end, measured on
    // SplineStripStation::Distance. Zero (the default) is off, and off means
    // UNTOUCHED: the stations come back bit-identical, so a run that asks for no
    // taper cannot be reshaped or re-textured by one.
    float32 TaperMetres = 0.0f;
    // How far the pinched ring sinks below the run's own surface, in the units
    // of SplineStripStation::Position. The point the strip narrows to is buried
    // that far under the surface it came from, so the end tucks in instead of
    // stopping flush with it. Zero pinches in-plane.
    float32 HeightDrop = 0.0f;
    // A closed loop has no end: its first and last stations are the same place,
    // and pinching them would cut the loop open at its seam.
    bool ClosedLoop = false;
};

// Pinch `stations` to nothing over the last TaperMetres of run at each open end.
//
// The pinch is SMOOTH (smoothstep, not linear): a linear ramp meets the
// untapered run at a crease, and on a water surface that crease reads as a hard
// line straight across the strip. Both half-widths scale by the same factor, so
// an asymmetric channel carries its asymmetry all the way into the point.
//
// A run shorter than two tapers is not a special case: every station takes the
// nearer end, so the two pinches meet in the middle instead of fighting over it.
void ApplyEndTaper(std::span<SplineStripStation> stations, const SplineEndTaperParams& params);

} // namespace GameEngine::SplineGeometry
