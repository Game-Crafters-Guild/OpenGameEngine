#pragma once

#include "Components/AssetRef.h"
#include "Components/Spline/SplinePlacement.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// How a swept wall meets a slope. Both are right for different walls, so
// neither is a trap. Racked is the default because its top follows the ground
// with the base: nothing stands taller than Height. On either grade the base
// sinks into a cross slope, so its uphill edge is buried and nothing floats.
enum class SplineWallGrade : int32
{
    // The top runs parallel to the ground: a hedge, a palisade.
    Racked = 0,
    // Each run between two authored points keeps a level top at the wall's
    // height above its highest ground, and the top steps where two runs meet:
    // coursed masonry.
    Stepped = 1,
};

// What a swept wall builds where its spline turns at an authored point. It acts
// at corners only: between them a smooth spline is swept as the curve it is.
enum class SplineWallCorner : int32
{
    // The faces of both legs meet on one sharp edge.
    Mitre = 0,
    // The outside sweeps an arc of half the thickness about the point.
    Round = 1,
};

// Thinnest and lowest a wall is built, in metres: a cross-section with no extent
// has no normals. The sanitizer raises a smaller value to it and reports it.
inline constexpr float32 kSplineWallMinExtentMetres = 0.05f;
// Thickest and tallest a wall is built, in metres. At this thickness a 90-degree
// corner already reaches 50 m along each leg, a whole chunk; the sanitizer lowers
// a larger value to it and reports it.
inline constexpr float32 kSplineWallMaxExtentMetres = 100.0f;

// A wall generated along the sibling SplineComponent's curve as one continuous
// mesh: no kit pieces, no joins, no stretch.
//
// Pure authored data, like its sibling recipes: the editor-side
// SplineWallController samples the spline, conforms against the scene, builds
// the geometry through the shared sweep and spawns runtime-only chunk entities.
// The scene serializes this recipe, never the generated mesh.
//
// The spline's width channel is NOT read: a wall is as thick as it is built,
// and a fresh spline's 5 m radius would make it 10 m thick.
struct SplineWall
{
    // @ge-tooltip Metres across the wall. The spline's width does not change it.
    // @ge-range 0.05 100
    float32 Thickness = 0.6f;
    // @ge-tooltip Metres from the ground to the top of the wall. On a slope with Grade set to Stepped, each stretch between two points stands at least this tall everywhere.
    // @ge-range 0.05 100
    float32 Height = 3.0f;
    // @ge-tooltip How the wall meets a slope. Racked keeps the top parallel to the ground, as a hedge or a palisade does. Stepped keeps the top of each stretch between two points level, at Height above its highest ground, and steps up or down where two stretches meet, as coursed masonry does.
    SplineWallGrade Grade = SplineWallGrade::Racked;
    // Set from the spline's type when the component is added — straight
    // segments give Mitre, curves give Round — and authored after that. The
    // component factory derives it (Source/Components/SplineWallDefaults.cpp);
    // Reset restores the value the inspector first showed.
    // @ge-tooltip What the wall builds where its spline turns at a point: Mitre meets the two faces on one sharp edge, Round sweeps the outside in an arc. A turn sharper than 120 degrees is always built round. Adding the wall picks Mitre for a spline of straight segments and Round for a curved one.
    SplineWallCorner Corner = SplineWallCorner::Mitre;
    // Null keeps the built-in flat grey material so the wall renders on the
    // first try.
    // @ge-tooltip The wall's surface. Empty keeps a flat grey. Texture coordinates are in metres, along each face and up from the wall's lowest point, so the stone size is the material's own tiling.
    MaterialRef Material;
    // @ge-tooltip How the wall follows the ground under its spline. None keeps the spline's own heights; Height and Height And Slope both set the wall on the surface below the spline, since the wall takes its lean from the spline, not from the surface. The base always sinks into a cross slope so it never floats.
    SplinePlacementConform ConformMode = SplinePlacementConform::Height;
    // @ge-tooltip What the ground under the wall is. Scene takes the nearest surface of any kind, so a wall passing under a tree is built over its roof; TerrainOnly makes scenery transparent so the wall stands on the terrain beneath it.
    SplineConformTarget ConformTarget = SplineConformTarget::Scene;
    // @ge-tooltip Whether the wall casts shadows onto the scene.
    bool CastShadows = true;
    // @ge-tooltip Whether shadows fall on the wall.
    bool ReceiveShadows = true;

    // Memberwise: the rebuild scheduler compares whole recipes, so a
    // hand-written comparison that missed a field would silently stop rebuilds
    // for edits to that field. A NaN float would make a recipe unequal to itself
    // and re-arm the rebuild forever, so the controller compares sanitized
    // recipes only (SanitizeWallRecipe, Placement/SplineWallSanitize.h).
    bool operator==(const SplineWall&) const = default;
};

static_assert(std::is_trivially_copyable_v<SplineWall>,
              "SplineWall must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineWall>,
              "SplineWall must be standard layout for ECS storage");

} // namespace GameEngine::Components
