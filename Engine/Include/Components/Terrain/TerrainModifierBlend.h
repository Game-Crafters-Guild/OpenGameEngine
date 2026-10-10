#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{

// How a modifier's value combines with the terrain height already there.
// Shared by the modifier volume's effect components and the paintable zones.
//
// Every mode is weight-aware in the same form — h' = lerp(h, op(h, v), weight) — so a
// volume's falloff skirt feathers the operator's result instead of biting a step out of
// the terrain at the shape boundary.
//
// VALUE 3 IS A PERMANENT TOMBSTONE. It was the retired Smooth mode; scenes on disk still
// carry it and the loader migrates 3 -> Add (ParseBlend in TerrainSceneSchemas.cpp).
// Reusing 3 for a new mode would make an old scene's 3 and a new scene's 3
// indistinguishable on disk, so new enumerators start at 4.
enum class TerrainModifierBlend : uint8
{
    Set = 0,        // Replace height within shape
    Add = 1,        // Add to existing height
    Subtract = 2,   // Subtract from existing height

    // Union operators. Their operand is the effect's value in the same space as the
    // terrain height — a candidate height, exactly as Set treats it, not a delta.
    Min = 4,        // Keep the lower of current and target — the CUT operator
    Max = 5,        // Keep the higher of current and target — the UNION operator

    // The same two through the polynomial smooth-min, so two masses meeting produce a
    // rounded saddle instead of a crease. The blend radius k comes from the effect's
    // BlendSmoothing field and is in METRES of height; k <= 0 degenerates to Min / Max.
    SmoothMin = 6,
    SmoothMax = 7,

    // POOLING, not a per-texel operator. An effect whose blend is Average joins a
    // POOL: every member accumulates (weight * target) and (weight) over the UNION
    // of the members' footprints, and the pool applies ONCE — at its highest-
    // priority member's slot — with the weighted-average target. Two overlapping
    // routes land between their grades instead of the later one overwriting the
    // earlier and leaving a step where its blend band ends.
    //
    // Unlike Min / Max, this is NOT a binary function of (current, target) at one
    // texel, and a pooled effect does NOT apply at its own StackOrder position:
    // the pool's slot is the only place it writes.
    Average = 8,
};

// Default blend radius for SmoothMin / SmoothMax, in metres of height: wide enough that a
// saddle between two overlapping masses reads as rounded at terrain scale, narrow enough
// that the operator still tracks the sharp result away from the seam.
inline constexpr float32 kDefaultBlendSmoothingM = 5.0f;

// True for the two modes that read BlendSmoothing. The others ignore it, so an inspector
// or a serializer can key the field's relevance off the mode.
constexpr bool BlendUsesSmoothing(TerrainModifierBlend blend)
{
    return blend == TerrainModifierBlend::SmoothMin || blend == TerrainModifierBlend::SmoothMax;
}

} // namespace GameEngine::Components
