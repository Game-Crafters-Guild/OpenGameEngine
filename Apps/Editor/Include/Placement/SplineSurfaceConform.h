#pragma once

#include "Components/Spline/SplinePlacement.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Placement/TileLayout.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::TerrainECS { class TerrainModifierSystem; }

namespace GameEngine::Editor
{

// Shared surface-conform raycast for spline consumers: the placement, fence and
// extrude controllers, and the width-envelope gizmo. They share the ray, the
// ignore list and the conform-miss rule.
//
// They do NOT share the TARGET. Each recipe conforms to what its author chose
// (SplineConformTarget); the drape gizmo reads no recipe and always takes the
// whole scene. Over a prop the displayed centerline can therefore sit on the
// prop while the pieces stand on the terrain beneath it. Routing a recipe's
// target into the gizmo is the open half of that decision.

// All RuntimeOnlyEntity-tagged entities: the standing ignore list for conform
// rays, so placement output, HLOD proxies, and other engine-spawned dressing
// can never act as their own ground. The picking API's exclusion mechanism is
// an entity list (PickOptions::IgnoreEntities).
[[nodiscard]] std::vector<ECS::EntityHandle> CollectConformRayIgnoreList(ECS::World& world);

// Conform-ray misses accumulated over ONE placement rebuild.
//
// A miss is silent by construction: the sample keeps its authored altitude, and
// no consumer can tell that apart from an altitude the author meant — the piece
// simply hangs over ground the ray never found. A dense centerline probes
// thousands of samples, so the rebuild reports the class once, with a count and
// a witness position, instead of per-sample spam or (previously) nothing.
struct ConformMissTally
{
    uint32 Probes = 0;
    uint32 Misses = 0;
    Mathematics::Vector3 FirstMiss{};

    // The first missing sample is kept as the witness: it is the one a reader can
    // navigate to, and later misses in the same run are almost always its neighbours.
    void Record(bool hit, const Mathematics::Vector3& sample);
};

// Down-ray conform against the visible scene: the ray starts above the sample
// and probes straight down. Returns true and writes the hit when anything
// under the sample was struck. `outNormal` (optional) receives the surface
// normal at the hit — a real triangle normal for mesh ground and a
// heightfield-gradient normal for planar terrain — so one ray carries the
// full surface orientation (pitch AND roll), not just height.
// `tally` (optional) accumulates the miss diagnostics above.
//
// `target` chooses what counts as ground. Scene — the default, and what every
// recipe did before the knob existed — takes the nearest surface of any kind,
// so a sample under a cart lands on the cart. TerrainOnly makes meshes and
// primitives transparent to the ray. It is the AUTHOR's choice because the ray
// cannot tell scenery from ground: a bridge deck is ground and a cart is not,
// and both are just meshes to the picker.
bool ConformRayDown(ECS::World& world, const Mathematics::Vector3& sample,
                    std::span<const ECS::EntityHandle> ignore,
                    Mathematics::Vector3& outHit,
                    Mathematics::Vector3* outNormal = nullptr,
                    ConformMissTally* tally = nullptr,
                    Components::SplineConformTarget target =
                        Components::SplineConformTarget::Scene);

// Warn once for a rebuild whose conform rays missed, naming the count, the probe
// total, the witness position and the ray's reach. `lastReportedMisses` is the
// caller's per-entity dedupe state (updated in place), so a scene that rebuilds
// every frame reports on change rather than every frame. Silent when nothing missed.
void ReportConformMisses(const ConformMissTally& tally, uint32 entityId,
                         uint32& lastReportedMisses);

// Digest of the terrain ground the conform rays measure, folded into one value.
//
// A conformed piece is only as current as the surface it measured, and that
// surface moves without any spline recipe, transform or curve changing: a
// modifier bake, a sculpt stroke, a streamed tile, or the terrain entity itself
// being moved or resized. On scene load the move is GUARANTEED — the placement
// controllers tick from EditorApplication::Update (through RecipeControllers),
// which runs ahead of the ECS systems in the same frame (Application.cpp drives
// Update before EngineCore::Update), and the schedule runs TerrainModifiers BEFORE
// TerrainExtraction, so the bake first meets a field extraction created on the
// frame before it. A rebuild in that window measures base terrain the modifier
// volumes have not shaped yet. Consumers fold this into the state they re-place
// on, which is what turns "the ground moved" into a rebuild.
//
// Folding it is not enough for a recipe's FIRST build, which has no earlier
// state to re-place from — see ConformSurfaceState below for the readiness half.
//
// Covers exactly what TerrainPicking derives a planar hit from — the seven pick
// inputs (translation x/y/z, SizeX, SizeZ, HeightScale, Domain) plus the
// heightfield content and identity — because the content half alone is not
// enough: a MODIFIER-FREE terrain never bakes (the empty-modifier hash sentinel
// short-circuits TerrainModifierSystem::Update), so its version never moves
// while dragging its entity moves every hit the pick reports.
//
// What it does NOT cover, and so cannot schedule a rebuild for:
//   - mesh ground. Conform rays hit any pickable mesh, not just terrain, so a
//     rock or bridge moving under a path leaves the pieces where they were.
//   - the spherical domain's drawn surface. RaycastActivePlanet resolves that
//     through the CBT feature's own tuning, of which only the editable sculpt
//     layer's version is folded here.
// The Domain field is folded but not filtered on, so a terrain flipping to
// Spherical moves the digest without changing what the planar march sees. That
// costs one spurious rebuild and never a missed one, which is the safe side.
//
// Quiescent at rest: every bump comes from an edit, a bake or a streaming
// integration, and the modifier system's own change gate keeps idle frames from
// re-baking. Tile streaming DOES move it, so a placement over a tiled terrain
// re-places when tiles arrive — correct, and bounded by the consumers' settle
// window plus their count-stable transform-only update path. Callers with
// nothing conformed should skip it rather than pay the terrain scan.
//
// `modifierSystem` contributes the bake's own ground revision on a tiled terrain,
// and belongs to callers that read COMPOSED ground. It cannot be inferred from the
// tiles: the per-tile versions folded in describe only what is RESIDENT, so an
// edit whose footprint lies off-camera moves none of them and the digest would sit
// still while the composed bed moved — leaving water on stale ground until those
// tiles arrived and retired its chunks in front of the camera.
//
// Pass null when the caller conforms by RAYCAST instead. A ray reads the resident
// tiles and nothing else, so ground it cannot yet see cannot change its result;
// when those tiles do arrive their versions bump and the digest moves anyway.
[[nodiscard]] uint64 ConformSurfaceRevision(
    ECS::World& world, const TerrainECS::TerrainModifierSystem* modifierSystem);

// The surface term one recipe folds into its rebuild-settle observation: the
// world digest above when the recipe READS THE GROUND, and no term when it
// does not. A recipe that never probes the ground must contribute no term, or
// unrelated terrain movement — a modifier bake, a streamed tile — resets its
// settle clock every frame and starves its rebuilds while the terrain stays
// busy. The controllers likewise skip computing the world digest when no
// candidate reads the ground.
//
// The caller passes the PREDICATE rather than a conform mode, because
// conforming is not the only way to read the ground. An extrude recipe fitting
// its rings to the banks probes at every station whatever its ConformMode is,
// and reporting itself by conform mode alone would leave it observing no
// surface term and never re-fitting when the bed under it was carved. Each
// recipe kind owns its own answer: Components::ReadsSurface for the extrude,
// and "conforms at all" for the placement and fence recipes, which have no
// second ground reader.
[[nodiscard]] uint64 ObservedSurfaceRevision(bool readsSurface, uint64 worldDigest);

// Whether the ground a conforming recipe is about to measure EXISTS yet.
//
// ConformSurfaceRevision answers "has the ground moved", which cannot express
// "the ground is not here yet": an unprovisioned terrain folds no terms, so its
// digest is indistinguishable from a settled one. This is the missing third
// answer, and it is a property of the WORLD, never of a ray — a ray that misses
// is an authoring fact (the spline left the footprint), while an unprovisioned
// terrain is a load-order fact that fixes itself.
enum class ConformSurfaceReadiness : uint8
{
    // No enabled terrain entity in this world. Nothing is coming, so a
    // conforming recipe should place against whatever else is there rather than
    // wait for ground that does not exist.
    NoSurface,
    // A terrain entity is declared but neither of its data handles resolves:
    // the scene loader instantiates every entity up front and provisions
    // terrain data from the asset-resolve pump many seconds later, so this is
    // the window where a conform ray falls through to the props.
    //
    // Transient by construction — TerrainExtraction walks the same terrains
    // this does and re-creates a destroyed handle within its own tick, so no
    // frame sees "neither resolves" from a resize or a domain flip. It is
    // permanent in exactly one case: extraction refusing to run at all (no
    // render services, no terrain feature, no device), where every
    // ground-reading recipe spends the full budget once and then says so.
    Provisioning,
    // Every declared terrain has resolvable surface data.
    Ready
};

// Readiness of the terrain ground in this world, over the same query
// ConformSurfaceRevision digests — so the two agree on which terrains a conform
// ray can hit. Cheap: bounded by terrain-entity count, and it resolves the same
// handles the digest already reads.
[[nodiscard]] ConformSurfaceReadiness ConformSurfaceState(ECS::World& world);

// Total time a recipe's FIRST build may be deferred waiting for its ground,
// for either reason below.
//
// SIZED FROM A MEASUREMENT WHOSE CAUSE IS NOT ESTABLISHED. On a large island scene
// (2859 entities, DebugFast) the approach recipes became eligible ~62 s before the
// terrain they conform to had resolvable surface data — 37 s of it after their
// own first build would otherwise have run. That gap is reproducible and is
// what this budget is scaled from (~3x). What produces it is NOT known: the
// scene loader's two-phase load is real (every entity instantiated up front,
// then a per-frame asset-resolve pump), but that pump resolves model meshes and
// materials, not terrain, and TerrainExtraction derives its config from
// component fields and ticks every frame — so the pump is not a demonstrated
// cause and neither is a one-frame schedule race, which is far too small.
//
// Treat this as an empirical bound with an unknown tail, not a derived one. A
// colder cache, a heavier scene or a slower machine could all move it, and
// without the mechanism there is no argument that they cannot move it past 3x.
// The asymmetry is what makes that acceptable: undershooting reinstates the
// defect on a merely slow load, while overshooting costs one late placement
// that reports itself.
//
// Seconds of DELTA TIME — wall-clock in an interactive session, but SIMULATED
// time under a fixed timestep (Application.cpp substitutes the fixed step for
// movie recording and UI replay). A frame count would be worse regardless: it
// would expire in well under a second against a 250 ms per-frame scene-build
// budget.
inline constexpr float32 kFirstBuildDeferBudgetSeconds = 120.0f;

// Whether a recipe's FIRST build must be deferred this frame because the ground
// it reads is not final.
//
// TWO reasons, ONE budget. The surface may not be provisioned yet, or it may be
// provisioned and still moving — a terrain fills its base heights on one frame
// and composes its modifiers on another, and a TILED one keeps bumping the
// digest for as long as tiles arrive. Both mean "not final", and a first build
// has no earlier state to fall back on either way.
//
// The budget covers both halves because a first build that never lands leaves
// NOTHING on the ground, where a starved later rebuild merely leaves the
// previous, already-plausible pieces. A streaming terrain can hold the digest
// moving indefinitely, so the settle half has to be bounded exactly as the
// provision half is — an unbounded settle would trade a visible defect for an
// invisible one.
//
// Only the first build: every later one already re-places through the settle
// window, which the arriving terrain arms. The first build skips that gate by
// design, so it is the only one that can commit a measurement of a surface that
// was never there.
//
// `surfaceSettled` is the caller's own settle verdict (its observation has held
// still for its settle window), so this function does not need to know the
// window or own a second clock. Advances `deferredSeconds` while it defers;
// once the budget is spent it warns once (via `warned`) and stops deferring, so
// ground that never settles costs a late, reported placement rather than a
// permanently empty one.
//
// KNOWN COST, measured rather than argued: the settle half binds every
// first build of a ground-reading recipe, including one AUTHORED by hand onto a
// terrain that is already there. On a tiled terrain streaming under a moving
// camera the digest never holds still, so drawing a spline could show nothing
// until the budget expires. It is not gated on "this recipe came up during a
// load" because that cannot be told apart here: the terrain's handles resolve
// BEFORE the recipes become eligible, so a scene-load recipe never observes
// Provisioning either. A latch on "saw Provisioning" was implemented and
// falsified at runtime — it restored the full 37 s defect on that scene, because
// the recipes it was supposed to catch never see that state. Telling the two
// apart may be answerable with the scene-build signal the editor already owns
// (SceneEditorController::IsSceneBuildInProgress) — a candidate, not the fix:
// m_BuildActive spans the resolve pump, which does not resolve terrain, so
// terrain can provision after the flag clears — the same failure shape as the
// latch above. Whatever lands must be runtime-verified against the scene-load
// case; until then the budget is the only bound, and a streaming tiled
// terrain is the case to watch.
[[nodiscard]] bool DeferFirstBuild(ConformSurfaceReadiness readiness, bool readsSurface,
                                   bool appliedOnce, bool surfaceSettled, float32 deltaSeconds,
                                   uint32 entityId, float32& deferredSeconds, bool& warned);

// Warn once for a rebuild whose ground leaned past the recipe's tilt ceiling,
// naming how many stations were held and the steepest slope seen. Authoring a
// path up a near-cliff is legal and still places pieces — the author is simply
// told, because held tiles cut into the face instead of lying on it, and
// nothing else in the scene says so. Same per-entity dedupe contract as
// ReportConformMisses. Silent when nothing was held.
void ReportTiltClamp(const TiltClampReport& report, uint32 entityId, float32 maxTiltDegrees,
                     uint32& lastReportedClamps);

// Warn once for a rebuild whose spans stood too near vertical to carry a roll
// and borrowed their opening station's frame. Such a span stops joining its two
// posts, so the run reads as broken rather than steep, and the author needs to
// know it is the ground and not the fence that put it there. Same per-entity
// dedupe contract as ReportConformMisses. Silent when none borrowed.
void ReportSpanRollBorrowed(uint32 borrowedSpans, uint32 entityId, uint32& lastReportedBorrows);

} // namespace GameEngine::Editor
