#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::SplineGeometry
{

// How a station's ring meets the ring before it.
enum class SplineStationJoin : uint8
{
    // The ring is stitched to the previous one, and its Distance must advance
    // past it.
    Advance = 0,
    // A crease across the run: the ring stands at the previous ring's Distance
    // and is NOT stitched to it, so the faces either side keep their own
    // normals up to the shared edge. A mitred corner is such a pair: the same
    // positions, shaded once as the incoming leg and once as the outgoing one.
    Crease,
    // Another ring of a corner that turns about one point: the previous ring's
    // Distance, its own shape, stitched to it. A round corner's fan.
    Pivot,
};

// Sides of travel a station's MitreRight applies to (bit flags).
inline constexpr uint8 kSplineMitreLeftSide = 1u;
inline constexpr uint8 kSplineMitreRightSide = 2u;
inline constexpr uint8 kSplineMitreBothSides = kSplineMitreLeftSide | kSplineMitreRightSide;

// One station of the draped path: the frame a cross-section is placed into,
// plus the per-station channels that shape it there.
//
// Stations are the SHARED SEAM between every consumer of a spline run -- the
// swept strip, rigid tile placement, and the water region fill all walk the
// same draped, gap-held, width-carrying polyline by true 3-D length. Producing
// them is the caller's job precisely because that is where the conform raycast
// lives, and the raycast is the one editor-bound piece of the pipeline.
//
// The fill reads Position, Right, Forward and Distance and ignores the width
// channels: a region's extent comes from the ground, not from the station.
struct SplineStripStation
{
    Mathematics::Vector3 Position{};
    // Orthonormal and ROLL-FREE. The strip builder applies RollRadians about
    // Forward itself, so a basis that already carries roll would double it.
    // Frames straight out of Spline::Evaluate have roll applied already and must
    // be passed with RollRadians = 0. The rings of one corner share a single
    // Up, so where its two legs climb at different grades that Up is only
    // nearly orthogonal to each ring's Right.
    Mathematics::Vector3 Right{1.0f, 0.0f, 0.0f};
    Mathematics::Vector3 Up{0.0f, 1.0f, 0.0f};
    Mathematics::Vector3 Forward{0.0f, 0.0f, 1.0f};
    // Half-widths in metres, PER SIDE of travel: profile points with negative
    // lateral coordinates scale by HalfWidthLeft / profile.NominalHalfWidth,
    // and the rest by HalfWidthRight / NominalHalfWidth. A point at exactly
    // zero lateral offset sits on the centreline and scales to itself either
    // way.
    //
    // The two sides exist because a measured channel is rarely symmetric. A
    // caller driving the width from the spline's own channel sets both to the
    // same value, which is what makes the swept cross-section symmetric again
    // and reproduces the single-half-width sweep EXACTLY: the per-point scale
    // is then one value, and the arithmetic per vertex is unchanged.
    float32 HalfWidthLeft = 1.0f;
    float32 HalfWidthRight = 1.0f;
    // Bank angle about Forward, in radians, from the spline's roll channel.
    float32 RollRadians = 0.0f;
    // Distance along the run in metres, strictly ascending. It drives U
    // directly, so it must be the DRAPED 3-D length wherever the caller
    // conformed -- authored arc length under-counts distance on a slope, and a
    // U built from it would compress the texture downhill.
    float32 Distance = 0.0f;

    // How this station's ring meets the previous one. A Crease or Pivot ring
    // takes the previous ring's Distance, so a corner's rings share one U.
    SplineStationJoin Join = SplineStationJoin::Advance;
    // The lateral axis that PLACES the profile points on the sides named by
    // MitreSides, where it differs from Right (Right, Up and Forward still shade
    // them). A mitre ring passes the bisector's right scaled by 1 / cos(θ/2) on
    // both sides, which puts the faces of both legs on one edge; a round
    // corner's fan passes it on the inside of the turn only, where every ring
    // meets at the inner mitre point. The builder rolls it as it rolls Right.
    Mathematics::Vector3 MitreRight{};
    uint8 MitreSides = 0;
    // Metres along Up added to the profile points marked Grounded, and to no
    // other point: negative sinks them. A wall's base takes the fall of the
    // ground across it here, so its downhill edge stands in the ground rather
    // than above it.
    float32 GroundOffset = 0.0f;
    // Metres along Up added to every profile point NOT marked Grounded. A
    // stepped wall raises its top to its run's level here while its base stays
    // on the ground; where a Crease ring's top stands at another height than
    // the ring before it, the builder closes the step with a riser.
    float32 TopOffset = 0.0f;
    // Per side of travel, the length a side face has run beyond the centreline
    // up to this station, per metre the face stands from it: a side-face vertex
    // x metres to that side takes U from Distance + |x| · FaceTurn (x in the
    // metres of Distance, SplineStripParams::LateralMetresPerUnit). On a curve it
    // is the cumulative turn in radians, positive on the outside; a mitre adds
    // tan(θ/2) at its ring and again past it, the extra length of the outer face
    // (and the shortfall of the inner). Zero, the default, is the centreline U
    // on every face, which is what paths and roads take.
    float32 FaceTurnLeft = 0.0f;
    float32 FaceTurnRight = 0.0f;
    // How far the top's U has run ahead of the centreline around round
    // corners, in the profile's lateral units (SplineStripParams::
    // LateralMetresPerUnit makes it metres), uniform across the width: each
    // round corner adds its outside half-width times its turn, the arc the top's
    // outer edge sweeps, and a ring inside the fan carries the arc swept so far.
    // Zero, the default, is the centreline U; AccumulateFaceTurn sets it.
    float32 TopArcAllowance = 0.0f;
    // A corner ring's face turn relative to the leg before its corner, per
    // side: ApplyCorners writes it and AccumulateFaceTurn reads it to set
    // FaceTurnLeft/Right. The builder never reads it.
    float32 CornerTurnLeft = 0.0f;
    float32 CornerTurnRight = 0.0f;
};

} // namespace GameEngine::SplineGeometry
