#pragma once

#include "Components/Spline/SplinePlacement.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Mathematics { struct Matrix4x4; }
namespace GameEngine::Spline { class SplineData; }

namespace GameEngine::Editor::SceneTools
{

// Draped display polylines for a spline: the centerline and (optionally) the
// ±width envelope edges, sampled uniformly along the arc, transformed by the
// entity's world matrix, then conformed to the visible scene through the
// shared ConformRayDown. Display and tile placement share the
// transform-then-conform order, the conform ray, the conform-miss rule
// (HoldSurfaceAcrossGaps), and the local-to-world scaling of arc length and
// width.
//
// The caller names the conform TARGET (SplineConformTarget). The spline drape
// gizmo reads no recipe and passes Scene, so over a prop its drawn line can sit
// on the prop while a TerrainOnly recipe's pieces stand on the terrain beneath
// it; a mark-up region passes TerrainOnly, so its walls stand on the ground
// under a house or a tree.
//
// Tiles additionally re-probe the surface under each station
// (TileLayout's per-station probe), so within one display step a sharp
// feature — a kerb, a berm — can still separate the drawn line from a tile:
// the line is a preview at display density, not the placement measurement.

// Drape sampling density along the world-space arc, and its safety cap.
// Deliberately coarser than placement's kCenterlineStepMetres: the budget
// here is per-frame display cost, not tile-station accuracy.
inline constexpr float kDrapeSampleStepMetres = 1.0f;
inline constexpr uint32_t kDrapeMaxSamples = 256;
// Ride slightly above the conform hit so the depth-tested centerline and
// envelope edges don't z-fight the ground: under the overlay's depth split,
// z-fighting scatters a line's pixels between the dimmed and full-strength
// passes and it shimmers. Constant world units: the drape tracks the surface,
// so a camera-scaled lift would detach the line from the ground it describes.
// It is NOT what keeps the width band visible over tiles — the band opts into
// the always-on-top pass instead of trying to out-climb them.
inline constexpr float kDrapeSurfaceLiftMetres = 0.15f;
inline constexpr float kDrapeMinHalfWidth = 0.01f;

struct SplineDrapedPolylines
{
    std::vector<Mathematics::Vector3> Center;
    std::vector<Mathematics::Vector3> Left;
    std::vector<Mathematics::Vector3> Right;
};

// Builds the draped centerline and, when wantEdges is set, the two ±width
// offset polylines. Density and width are world metres: the entity-local arc
// length and width channel scale by the transform's largest axis (exact under
// uniform scale). Each sample drapes at its own XZ; conform misses hold the
// nearest measured altitude through the shared HoldSurfaceAcrossGaps, and a
// polyline the conform ray never reached keeps its authored altitudes (and
// takes no lift), which reads as "no ground here" instead of inventing one.
// `target` is what the conform rays may land on (ConformRayDown).
// Output vectors are cleared first; an invalid or zero-length spline leaves
// them all empty, and wantEdges=false leaves Left/Right empty (and casts no
// edge rays).
void BuildSplineDrapedPolylines(ECS::World& world,
                                const Spline::SplineData& data,
                                const Mathematics::Matrix4x4& worldM,
                                std::span<const ECS::EntityHandle> ignore,
                                bool wantEdges,
                                Components::SplineConformTarget target,
                                SplineDrapedPolylines& out);

} // namespace GameEngine::Editor::SceneTools
