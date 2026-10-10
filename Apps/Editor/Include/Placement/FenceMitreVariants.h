#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Geometry.h"
#include "Placement/FenceEmission.h"
#include "SplineLayout/SpanMitre.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
struct Mesh;
}

namespace GameEngine::Editor
{

class SplineChunkCommit;

// Vertex and index bytes of cut variants one recipe may hold: the fence
// design's flip condition (section 4c). A castle wall variant is about 4 KB,
// so a castle wall reaches it past about 4 000 distinct joins; a 300 KB stone
// piece past about 50. Past it the remaining mitred pieces draw their pool
// piece, laid as their joins placed them, and the report says so.
inline constexpr uint64 kMaxMitreVariantBytes = 16ull * 1024ull * 1024ull;

// One pool piece a mitre variant can be cut from: the model it is drawn from,
// its first submesh (what the fence draws) and the bounds the layout measured
// it by.
struct FenceMitreSource
{
    GUID Model;
    // How the mesh is named to the author.
    std::string Name;
    const Mesh* SourceMesh = nullptr;
    // The registry's content hash of that mesh (MeshGPUEntry::contentHash), so
    // a reimported piece names new variants rather than keeping the old cut.
    uint64 Content = 0;
    SplineLayout::FencePieceBounds Bounds;
};

// A registered variant, ready to draw. The handle is invalid where the piece
// had no mesh to cut.
struct FenceMitreMesh
{
    Rendering::MeshGPUHandle Handle;
    Mathematics::BoundingBox Bounds;
};

// What the fence keeps of each variant it cut, by mesh key, so a rebuild that
// keeps a variant's mesh keeps its bounds and its report without cutting it
// again. Entries the plan no longer names are dropped with their meshes.
struct FenceMitreCut
{
    Mathematics::BoundingBox Bounds;
    uint32 OpenLoops = 0;
    // Vertex and index bytes the variant holds, counted against the budget.
    uint64 Bytes = 0;
};
using FenceMitreCuts = std::unordered_map<Rendering::MeshGPUKey, FenceMitreCut>;

struct FenceMitreVariants
{
    // Parallel to the plan's Variants.
    std::vector<FenceMitreMesh> Meshes;
    // Author-facing lines: a mesh the cut found open, the budget past its end.
    std::vector<std::string> Report;
    // Mitred pieces that draw uncut because the budget ran out before their
    // variant.
    uint32 PiecesPastBudget = 0;
};

// What each pool a variant can be cut from offers the cutter, indexed by its
// active slot: the SpanPool, the CrestPool and the GatePool (a gate is a span
// and is mitred like one).
struct FenceMitreSources
{
    std::span<const FenceMitreSource> Spans;
    std::span<const FenceMitreSource> Crests;
    std::span<const FenceMitreSource> Gates;
};

// Registers every variant the plan names through the recipe's chunk commit and
// releases the ones it no longer names (fence design section 4c). A variant is
// keyed on the recipe entity, its piece's model and content and its quantised
// shape, so
// the equal spans of a uniform arc share one mesh and a rebuild that keeps a
// shape keeps its mesh without cutting it again; a variant is a pure function
// of its key. Each variant is cut from the source its role names. Variants are
// kept in the plan's order until their bytes would pass `byteBudget`; every
// later one is left unregistered.
FenceMitreVariants CommitFenceMitreVariants(Rendering::MeshGPURegistry& registry,
                                            SplineChunkCommit& commit, FenceMitreCuts& cuts,
                                            uint32 entityId,
                                            const SplineLayout::MitreVariantPlan& plan,
                                            const FenceMitreSources& sources,
                                            uint64 byteBudget = kMaxMitreVariantBytes);

// The piece a mitred span or crest draws: its pool piece, drawing the variant's
// mesh within the variant's bounds. It keeps the pool piece's material and its
// model GUID: what reads the GUID (picking first, the navigation and ocean
// depth bakers only) meets the uncut model at the stretched pose, whose extra
// wedge lies inside the neighbouring piece, rather than losing the piece.
FencePoolPiece MitreVariantPiece(const FencePoolPiece& pool, const FenceMitreMesh& variant);

// One number for the registered content of the models a fence cuts variants
// from (their first submesh, as the fence draws it): it changes when any of
// them is reimported, which is what a fence must rebuild on, since nothing
// else it observes changes then. A model not registered yet counts as empty.
uint64 FencePoolContentDigest(const Rendering::MeshGPURegistry& registry,
                              std::span<const GUID> models);

} // namespace GameEngine::Editor
