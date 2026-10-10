#pragma once

#include "Mathematics/Matrix4x4.h"
#include "SplineGeometry/SplineFillField.h"
#include "SplineGeometry/SplineFillMesher.h"
#include "SplineGeometry/SplineStation.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Components { struct SplineExtrude; }
namespace GameEngine::TerrainECS { class TerrainModifierSystem; }

namespace GameEngine::Editor
{

// Ground samples one run may read in a single window. The fill allocates the
// whole window before it knows where the water goes -- the field, the stamp's
// distance scratch and the drain mask are all one entry per corner -- so this
// bounds the ALLOCATION, not the water.
//
// It is the run's own XZ extent that moves it. MaxHalfWidth enters only through
// the window's margin, so on a run of any length reducing the reach barely
// changes this number: a refusal here is answered by a shorter run, never by a
// narrower one.
inline constexpr uint32 kMaxFillWindowCorners = 300000u;

// BuildWaterFillRun leaves the water budget at the field's own default, and that
// default has to stay strictly below the window budget to be reachable at all:
// wet corners are a SUBSET of the window, so a water budget at the window's
// bound is a guard no input can trip. This pins the two constants; that the
// fill does not then override the default is pinned by
// SplineFillRebuildTests.TheWaterBudgetTripsInsideALegalWindow, whose flood sits
// deliberately between the two.
static_assert(SplineGeometry::SplineFillParams{}.MaxWetCorners < kMaxFillWindowCorners,
              "the water budget must be reachable inside the window budget, or the runaway-fill "
              "guard is dead code");

// Why a run produced no water. Refusal is a first-class outcome for a fill:
// building a truncated river is worse than building one against ground that is
// not there.
enum class WaterFillOutcome : uint8
{
    Built = 0,
    // No planar terrain, no lattice to align to, or a tiled terrain with no
    // modifier system to compose its ground from. Nothing to fill against.
    NoTerrain,
    // The window of ground the run would have to read is past
    // kMaxFillWindowCorners. Refused before any ground is sampled, so it says
    // nothing about whether the run holds water.
    SampleWindowTooLarge,
    // The WATER spread past the field's budget. Builds nothing on purpose rather
    // than coarsening. Distinct from SampleWindowTooLarge: this one is a flood
    // that ran away inside a window the fill was willing to read, and the reach
    // and the bed are what answer it.
    OverBudget,
    // The bed is nowhere below the waterline, so there is no region.
    NoRegion,
};

struct WaterFillRun
{
    // One mesh per lattice chunk, in the PLACER'S LOCAL SPACE — the space the
    // controller's chunk entities carry an identity transform in. Entries can be
    // empty: that stretch of the lattice holds no wet cell, which is a hole in
    // the chunk plan rather than a failure.
    std::vector<SplineGeometry::SplineFillMesh> Chunks;
    SplineGeometry::SplineFillDiagnostics Diagnostics;
    WaterFillOutcome Outcome = WaterFillOutcome::NoTerrain;

    // The ground window the run asked for: corner count, and the world metres it
    // spans. Carried because the number that refuses a run is the one the author
    // has to act on, and the field never sees it -- the window is decided here,
    // before the field is called. Zero until a window has been sized.
    uint64 WindowCorners = 0;
    float32 WindowSizeX = 0.0f;
    float32 WindowSizeZ = 0.0f;
};

// Flood the water region for one run and mesh it.
//
// `worldStations` are the draped, offset-applied centreline stations in WORLD
// space, because the terrain lattice exists in no other space; the meshes come
// back in the placer's local space via `placerWorldMatrix`.
//
// This is the editor's half of the split: it owns the terrain query, the region
// of interest and the ground sampling. The geometry itself — the field, the
// flood and the marching squares — is in SplineGeometry and needs no device.
//
// `modifierSystem` is the ground source for a TILED terrain and is required for
// one: tiles stream out behind the camera, so the tile store cannot answer for a
// corridor the camera is not near, and a run far from the camera would otherwise
// read a hole where its bed is. The system composes those heights instead, from
// the terrain's base and the modifier stack the tile bake applies — so the answer is
// camera-independent and equals what the tiles hold once they arrive. A single
// (untiled) terrain is always wholly resident and does not use it; null is legal
// only for that case.
[[nodiscard]] WaterFillRun BuildWaterFillRun(
    ECS::World& world, std::span<const SplineGeometry::SplineStripStation> worldStations,
    const Components::SplineExtrude& recipe, const float32 placerWorldMatrix[16],
    const TerrainECS::TerrainModifierSystem* modifierSystem);

// Log a run's diagnostics once per change, in the shape ReportConformMisses
// uses: silent while nothing moved, loud with the fix when it does.
void ReportWaterFillDiagnostics(const WaterFillRun& run, uint32 entityId,
                                SplineGeometry::SplineFillDiagnostics& lastReported,
                                WaterFillOutcome& lastOutcome);

} // namespace GameEngine::Editor
