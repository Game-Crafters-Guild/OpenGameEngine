#pragma once

#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// Cross-section presets. Each is a GENERATOR over a handful of dimensions
// rather than a baked point list, so widening a road is one float instead of
// re-authoring its points.
enum class SplineProfileShape : uint8
{
    Rectangle = 0, // wall:  Width, Height              (closed loop => caps)
    Bevel,         // path:  Width, EdgeDrop, EdgeInset (open strip => no caps)
    Crown,         // road:  Width, CrownRise, ShoulderWidth, ShoulderDrop
    Custom,        // authored point list
};

// One cross-section vertex, in the station frame's plane:
// X = lateral (frame Right), Y = vertical (frame Up), both in metres.
//
// The evaluator's (Right, Up, Forward) is exactly the engine's (+X, +Y, +Z) LH
// basis, so a profile authored as if looking down -Z lands the right way up
// with no sign fixes and no handedness conversion.
struct SplineProfilePoint
{
    Mathematics::Vector2 Position{};
    // A crease. The vertex is emitted TWICE per ring with split normals, so a
    // wall's top edge stays sharp instead of smoothing into a pillow. Without
    // it every profile reads as extruded rubber.
    uint8 Hard = 0;
    // A point that stands on the ground: the station's GroundOffset moves it
    // and no other point. A wall's base is grounded, so a cross slope sinks the
    // base without shortening the wall above it.
    uint8 Grounded = 0;
};

// POINT ORDER IS LOAD-BEARING, and one rule covers both open and closed
// profiles: the outward normal of an edge is its direction rotated +90 degrees
// in the (lateral, vertical) plane, i.e. edge (dx, dy) => normal (-dy, dx).
//
//   - An OPEN strip is ordered LEFT to RIGHT (-X to +X), which makes its
//     normals point up: a path's walkable surface faces the sky.
//   - A CLOSED loop is ordered CLOCKWISE in that plane (up the left face,
//     right across the top, down the right face, left across the bottom),
//     which makes every face point away from the solid.
//
// A profile wound the other way generates inward-facing geometry, which reads
// as invisible from outside and is also the wrong winding for Jolt.
struct SplineProfile
{
    std::vector<SplineProfilePoint> Points;
    // A closed polygon caps its ends; an open strip would cap to a degenerate
    // sliver, so cap policy is DERIVED from the profile rather than authored.
    bool Closed = false;
    // The half-width the point list is authored at: the spline's width channel
    // scales lateral coordinates by W(s) / NominalHalfWidth. Derived as the
    // widest |x| in the list so the generated geometry lands inside the width
    // band the editor gizmo already draws, for every preset and for Custom.
    // A profile with no lateral extent at all (a zero-thickness sheet) reports
    // 1.0 here, which makes lateral scaling a no-op instead of a divide by zero.
    float32 NominalHalfWidth = 1.0f;

    [[nodiscard]] bool IsValid() const { return Points.size() >= 2u; }
};

// Preset dimensions. Trivially copyable and standard layout so a component can
// embed it whole (the sibling spline recipes' storage contract).
struct SplineProfileParams
{
    SplineProfileShape Shape = SplineProfileShape::Bevel;
    // Total lateral extent of the carriageway, in metres. Rectangle reads it as
    // wall thickness; Bevel and Crown as path/road width.
    float32 Width = 2.0f;
    float32 Height = 1.0f; // Rectangle: wall height above the station.
    // Bevel: how far the outer edge drops below the walkable surface, and how
    // far in from the outer edge the surface stays flat. Both zero collapses
    // the profile to a flat two-point ribbon -- the water/decal case.
    float32 EdgeDrop = 0.1f;
    float32 EdgeInset = 0.15f;
    // Crown: camber rise at the centreline, plus the shoulder that runs beyond
    // Width on each side and drops away from the carriageway.
    float32 CrownRise = 0.12f;
    float32 ShoulderWidth = 0.5f;
    float32 ShoulderDrop = 0.2f;

    bool operator==(const SplineProfileParams&) const = default;
};

// Resolve a preset into the point list a sweep consumes. Consecutive
// coincident points are dropped, so a degenerate preset (Bevel with no drop
// and no inset) collapses to a valid smaller profile rather than emitting
// zero-length edges that would divide by zero in the normal and V-coordinate
// derivations.
//
// Custom returns an empty profile: it has no dimensions to generate from.
// Callers holding an authored point list use BuildCustomProfile.
[[nodiscard]] SplineProfile BuildProfile(const SplineProfileParams& params);

// Resolve an authored point list, applying the same coincident-point cleanup
// and the same derived nominal half-width. The caller owns the winding rule
// documented on SplineProfile.
[[nodiscard]] SplineProfile BuildCustomProfile(std::span<const SplineProfilePoint> points,
                                               bool closed);

} // namespace GameEngine::SplineGeometry
