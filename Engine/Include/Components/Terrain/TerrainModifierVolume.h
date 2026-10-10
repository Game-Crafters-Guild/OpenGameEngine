#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Shape of a modifier volume's region of effect.
//
// Spline shapes take their geometry from the SplineComponent on the SAME entity
// (the binding Shape::Spline already uses). The two spline shapes differ only
// for a CLOSED spline: SplinePath is the swept band around the curve — a ring
// road keeps its hole — while SplineArea additionally fills the enclosed region.
// On an open spline they are identical (there is nothing to enclose).
//
// Global has no footprint at all: weight 1 at every texel of every terrain, with
// no edge and therefore no falloff. It is what makes a world-scale procedural
// layer — continents, mountain belts, a detail pass — an ordinary modifier that
// sorts by Priority against the local ones instead of a terrain-sized rectangle
// somebody has to keep resized.
//
// The values are SERIALIZED (scenes store the shape as a number), so entries are
// appended, never reordered.
enum class TerrainVolumeShape : uint8
{
    Rectangle = 0,  // Axis-aligned rectangle, rotated by the entity's Y rotation
    Circle = 1,     // XZ circle centred on the entity position
    SplinePath = 2, // Swept band along the spline; a closed spline's interior stays untouched
    SplineArea = 3, // The region a closed spline encloses, plus its swept band
    Global = 4,     // The whole world: weight 1 everywhere, no edge, no falloff
};

// The comment above is a rule, so it is enforced rather than trusted: scenes
// store the shape as an integer (TerrainSceneSchemas.cpp parses `shape` as a
// uint32 and range-checks it against Global), so renumbering does not fail a
// build or a symbolic test — it silently reinterprets every saved volume, and a
// circle loads as a rectangle. Append only.
static_assert(static_cast<uint8>(TerrainVolumeShape::Rectangle) == 0
              && static_cast<uint8>(TerrainVolumeShape::Circle) == 1
              && static_cast<uint8>(TerrainVolumeShape::SplinePath) == 2
              && static_cast<uint8>(TerrainVolumeShape::SplineArea) == 3
              && static_cast<uint8>(TerrainVolumeShape::Global) == 4,
              "TerrainVolumeShape is serialized numerically — append new shapes, never reorder.");

// One authored terrain region. The volume owns the SHAPE, the falloff, the
// master weight and the cross-volume priority; the effect components on the
// same entity (TerrainFlattenEffect, TerrainNoiseEffect, ...) own what happens
// inside it and never see the shape — the bake hands each of them a scalar
// weight per terrain texel, in the stack order they declare.
//
// One volume per entity: an entity holds at most one component of a type, and
// two regions are two entities. Priority orders volumes against each other
// (higher = applied later); StackOrder orders the effects within one volume.
struct TerrainModifierVolume
{
    TerrainVolumeShape Shape = TerrainVolumeShape::Circle;
    uint8 _ShapePad[3] = {};

    // Radius for Circle; half-extents for Rectangle. Spline shapes take their
    // extent from the spline's per-control-point swept radius instead, and
    // Shape::Global reads none of them — it has no extent at all, which is why
    // a Stamp effect (whose mask needs a projection domain) is refused inside
    // one rather than silently borrowing these.
    float32 Radius = 50.0f;
    float32 RectHalfX = 50.0f;
    float32 RectHalfZ = 50.0f;

    // The two halves of ONE weight ramp across the shape edge, in metres. The
    // weight leaves 0 at Falloff metres OUTSIDE the edge, rises monotonically
    // through the edge, and reaches full strength at FalloffInward metres INSIDE
    // it — one continuous ramp when both are set, never two bands meeting at a
    // step. FalloffInward widens a filled region's rim inward, the plateau
    // shoulder a purely outward falloff cannot express.
    //
    // Either may be 0, which puts that end of the ramp on the edge itself:
    // Falloff = 0 is a hard outer edge, FalloffInward = 0 is the flat interior
    // every pre-volume modifier has, and both 0 is a binary in/out mask.
    //
    // Shape::Global reads neither: it has no edge for a ramp to cross.
    float32 Falloff = 10.0f;
    float32 FalloffInward = 0.0f;

    // Arc-length spacing the SplinePath route is resampled to; the other shapes
    // ignore it. The resampled route carries only a pooled flatten's
    // per-station reference heights — the volume's footprint (shape weight,
    // bounds) stays on the analytic curve for both spline shapes.
    //
    // An UPPER BOUND, not the exact step: the route is divided into
    // ceil(length / spacing) equal intervals, so the last station lands exactly
    // on the route's end and every interval is the same length.
    float32 StationSpacing = 0.5f;

    // Master strength, multiplied into every effect's per-texel weight. 1 = the
    // shape's own falloff unmodified; 0 = the volume contributes nothing.
    float32 Weight = 1.0f;

    // Order against OTHER volumes (higher = applied later).
    float32 Priority = 0.0f;
};

static_assert(std::is_trivially_copyable_v<TerrainModifierVolume>);
static_assert(std::is_standard_layout_v<TerrainModifierVolume>);

} // namespace GameEngine::Components
