#pragma once

#include "Types/Types.h"

#include <string_view>

namespace GameEngine {

struct Mesh;
struct Vertex;

/**
 * @brief Border handling during simplification.
 *
 * Modular-kit content tiles separate meshes along shared seams; each piece's
 * seam is an open border, and the pre-simplify weld only closes co-located
 * seams WITHIN one mesh, never across two. If both neighbours collapse their
 * shared border independently, the seam pulls apart into a visible crack — so
 * cross-piece seams must never move. But kit pieces tile at their own bounding
 * extremes BY CONSTRUCTION, so those seams lie on the mesh AABB's face planes;
 * every other open border (window holes, trim edges, foliage-card rims) is
 * mesh-internal and free to drift within the level's error budget — drift
 * there is a self-contained silhouette morph, not a hole, and the achieved
 * error it costs delays the level's engagement proportionally (the error->
 * coverage threshold mapping in MeshLODThresholds.h).
 */
enum class MeshLODBorderRule : uint8 {
    // Lock only open-border vertices lying on the mesh AABB's face planes (the
    // kit-seam invariant above). The default: watertight cross-piece seams AND
    // reducible mid levels on open-border kit content.
    SeamPlanes = 0,
    // Lock every open-border vertex. Watertight everywhere, but mid levels
    // cannot emit on open-border-dominated content (the pre-2026-07 default,
    // which left such meshes with no LOD between LOD0 and the far shell).
    LockAll = 1,
    // No locks; every border drifts under the error budget. For standalone
    // organic content that never tiles against a neighbour.
    Free = 2,
};

/**
 * @brief Configuration for automatic LOD generation.
 *
 * Each LOD level targets a fraction of LOD0's triangle count. LOD0 is always
 * the source mesh unchanged. Levels are generated independently from the
 * source mesh (not chained), which yields higher quality than simplifying a
 * previously simplified level.
 */
struct MeshLODConfig {
    // Number of levels to produce, including LOD0. Clamped to [1, kMaxLODs].
    uint32 LodCount = 4;

    // Target triangle fraction relative to LOD0, per level. Index 0 is LOD0
    // and is always treated as 1.0. Subsequent entries should be descending.
    float TargetRatios[4] = {1.0f, 0.5f, 0.25f, 0.10f};

    // Maximum relative error allowed per level (meshoptimizer units, relative
    // to mesh extent; includes the weighted attribute deviation). The
    // simplifier stops reducing once it would exceed this, so a level may keep
    // more triangles than TargetRatios asks. Budgets are deliberately generous:
    // low-poly kit content pays several percent of extent for its very FIRST
    // collapse, and a tight budget (the old 0.02/0.05) emitted no mid levels at
    // all on that content. Engagement safety does not come from the budget —
    // it comes from the achieved error, which derives each level's screen-
    // coverage switch point (MeshLODThresholds.h): a level that spent a large
    // error engages proportionally further away, and beating the budget never
    // engages earlier than the per-slot anchor floors.
    float TargetError[4] = {0.0f, 0.06f, 0.16f, 0.35f};

    // Cross-piece seam handling; see MeshLODBorderRule.
    MeshLODBorderRule BorderRule = MeshLODBorderRule::SeamPlanes;

    // Attribute weights for meshopt_simplifyWithAttributes: the simplifier
    // penalizes collapses that bend shading normals (NormalWeight per axis) or
    // stretch UVs (UvWeight per channel), in error units where 1.0 = one mesh
    // extent of positional deviation. Keeps atlas-palette texturing and hard
    // shading breaks coherent on the retained vertices; the attribute cost
    // folds into the achieved error, so attribute-expensive levels also
    // self-delay their engagement.
    //
    // NormalWeight 2.0 is measured, not arbitrary: at 0.5 the simplifier ate
    // thin dressing (vine canopies, leaf cards, scallop trim) as its cheapest
    // collapses — geometry whose position error is tiny but whose normals are
    // wildly divergent. 2.0 makes that divergence expensive enough that the
    // gate-corpus dressing survives its mid levels while faceted structural
    // content (rocks, trees) still reaches its coarse ratios. UvWeight stays
    // low: palette-atlas UV distances are near-zero, so the UV term is inert
    // on kit content (measured) — chromatic protection comes from the seam
    // Protect flags instead (kChartProtectUvSpread in the generator).
    // kLodErrorFloor / kLodCoverageErrorScale (MeshLODThresholds.h) are
    // calibrated against errors produced under THESE weights — change them
    // together.
    float NormalWeight = 2.0f;
    float UvWeight = 0.5f;

    // When meshopt_simplify cannot reach the target (topology-constrained),
    // fall back to meshopt_simplifySloppy for the most aggressive levels.
    bool AllowSloppy = true;

    // Only use the sloppy fallback for levels whose target ratio is at or below
    // this value (sloppy ignores topology, so reserve it for distant LODs).
    float SloppyRatioThreshold = 0.10f;

    static constexpr uint32 kMaxLODs = 4;
};

/**
 * @brief Result of LOD generation for a single mesh.
 *
 * Non-sloppy LODs share the source mesh's vertex buffer: meshopt_simplify
 * reuses existing vertex indices (it never creates new vertices), so each such
 * LOD is just a reduced index buffer over the same vertices — all parallel
 * attribute streams (normals, UVs, tangents, joints, weights) remain valid
 * without remapping. Sloppy far levels instead carry their OWN vertex block
 * (LodVertices) with reconstructed attributes: position-only sloppy collapse
 * over original indices interpolated UVs across atlas islands (rainbow smears)
 * and inherited arbitrary normals (black facets), so honest shells rebuild
 * per-face flat normals and island-snapped UVs into a dedicated block.
 */
struct MeshLODs {
    // [lod][index]. LodIndices[0] is LOD0 (the source indices). Always has at
    // least one entry. Levels with an empty LodVertices block index into the
    // source mesh's Vertices; levels with a non-empty block are LOD-LOCAL
    // (0-based into their own block).
    Vector<Vector<uint32>> LodIndices;

    // Achieved relative geometric error per LOD (0 for LOD0). Same units as
    // MeshLODConfig::TargetError.
    Vector<float> LodErrors;

    // 1 when the matching level was produced by the sloppy simplifier fallback
    // (LodSloppy[0] is always 0 — LOD0 is the source). Parallel to LodIndices.
    // Sloppy errors are on a different scale, so downstream threshold derivation
    // excludes these levels.
    Vector<uint8> LodSloppy;

    // Per-level own vertex blocks, parallel to LodIndices. Empty for LOD0 and
    // every non-sloppy level (they share the source Vertices); non-empty for
    // attribute-honest sloppy shells, whose LodIndices entry is LOD-local.
    Vector<Vector<Vertex>> LodVertices;

    // Sloppy levels whose rebuilt shell had no non-degenerate face and was
    // refused. A refused shell falls back to the topology-limited non-sloppy
    // result when that reduces, else the level is skipped (the standard
    // skip-a-level rule).
    uint32 SloppyShellsRejected = 0;

    // Sloppy levels skipped because the mesh carries optional parallel vertex
    // streams (color0/uv1/extra-uv) this generator emits no per-level copy of.
    // Such meshes keep their non-sloppy levels and
    // simply have no far shell — surfaced in the cook log so the forfeited
    // far savings stay visible (gate corpus: 63 levels across ~47 meshes, incl. the 20k-vert
    // fountain and four statues).
    uint32 SloppyLevelsSkippedForStreams = 0;

    // Quality levels that failed the reduction gate while simplification locks
    // (seam-plane / LockAll borders, chromatic Protect flags) were active. A
    // high count on a mesh family means its locks — not its budgets — are what
    // pins the chain, which is invisible in triangle counts alone.
    uint32 LevelsSkippedWithLocks = 0;

    uint32 LodCount() const { return static_cast<uint32>(LodIndices.size()); }
    uint32 TriangleCount(uint32 lod) const {
        return lod < LodIndices.size() ? static_cast<uint32>(LodIndices[lod].size() / 3) : 0u;
    }
};

/**
 * @brief Generate simplified LOD index buffers for a mesh.
 *
 * LOD0 is the source index buffer (vertex-cache optimized in place). LOD1..N
 * are produced via meshopt_simplify, falling back to meshopt_simplifySloppy
 * for aggressive levels when enabled.
 *
 * Returns only LOD0 if the mesh is non-triangle topology, has no indices,
 * meshoptimizer support was not compiled in (GE_HAVE_MESHOPTIMIZER), or the
 * decode running on this thread was already cancelled.
 *
 * Cancellation (CurrentAssetDecodeCancellation) is polled between levels, so a
 * cancel arriving mid-generation returns a SHORT chain — however many levels
 * were finished — never a wrong one. A caller that PERSISTS the result must
 * re-read the flag before adopting it, or it will key a truncated chain to the
 * full config; see ModelAsset::LoadOrGenerateLODs.
 */
MeshLODs GenerateMeshLODs(const Mesh& mesh, const MeshLODConfig& config = {});

// Per-mesh generation counters the per-model cook log aggregates (+=).
struct MeshLODGenStats {
    uint32 ShellsRejected = 0;          // all-degenerate shells refused
    uint32 ShellsSkippedForStreams = 0; // C3 gap: optional-stream meshes skip sloppy
    uint32 LevelsSkippedWithLocks = 0;  // reduction-gate skips with locks active
};

/**
 * @brief Generate LODs and store the simplified levels into mesh.ExtraLODs.
 *
 * LOD0 (mesh.Indices) is left untouched. Clears any existing ExtraLODs first.
 * Also fills mesh.ExtraLODErrors / mesh.ExtraLODSloppy / mesh.ExtraLODVertices
 * (parallel to ExtraLODs) so the GPU mesh row can derive per-mesh coverage
 * thresholds and resolve own-vertex sloppy shells. Returns the resulting total
 * LOD count — 1 if nothing could be simplified, and also 1 when the decode was
 * cancelled before the first level completed (see GenerateMeshLODs: a cancelled
 * chain is short, and must not be persisted). outStats, when non-null,
 * accumulates the shell counters for the per-model cook log.
 */
uint32 GenerateMeshLODsInto(Mesh& mesh, const MeshLODConfig& config = {},
                            MeshLODGenStats* outStats = nullptr);

/**
 * @brief Test seam for the attribute-honest shell rebuild.
 *
 * `sloppyIndices` plays the role of meshopt_simplifySloppy output: a triangle
 * list over mesh.Vertices. On acceptance returns true and fills the per-face
 * LOD-local index/vertex block (flat face normals, island-snapped UVs); returns
 * false when every face is positionally degenerate. Only the deterministic
 * rebuild is exercised — production callers go through GenerateMeshLODs, which
 * feeds real sloppy output through the same path.
 */
bool BuildSloppyShellForTest(const Mesh& mesh, const Vector<uint32>& sloppyIndices,
                             Vector<uint32>& outLocalIndices, Vector<Vertex>& outVertices);

/**
 * @brief Test seam for the wedge-normal re-point pass.
 *
 * Runs the production co-located-wedge grouping (weld-snapped positions) and
 * corner re-point over `indices` — a triangle list into mesh.Vertices —
 * mutating it in place exactly as an emitted quality level is processed.
 * No-op without meshoptimizer support.
 */
void RepointCornersForTest(const Mesh& mesh, Vector<uint32>& indices);

/**
 * @brief Test seam for the chart-amalgamation tripwire.
 *
 * Returns the area-weighted cross-island UV-spread of `levelIndices` over the
 * mesh's UV islands — the quantity the generator folds into a quality level's
 * achieved error (scaled by its internal calibration constant) so chromatically
 * amalgamated levels self-delay engagement. 0 when every triangle stays inside
 * one island, or without meshoptimizer support.
 */
float ComputeChartAmalgamationErrorForTest(const Mesh& mesh,
                                           const Vector<uint32>& levelIndices);

/**
 * @brief Test seam for the silhouette-honesty tripwire.
 *
 * Returns the worst-axis-view symmetric silhouette mismatch of `levelIndices`
 * against the mesh's own index buffer (LOD0), normalized by LOD0's view
 * coverage — the quantity the generator folds into a quality level's achieved
 * error so shape-destroying levels (vanished thin sheets, collapsed icon
 * silhouettes) self-delay engagement. ~0 for a level that draws the same
 * outline, or without meshoptimizer support.
 */
float ComputeSilhouetteLossForTest(const Mesh& mesh, const Vector<uint32>& levelIndices);

/**
 * @brief Whether LOD generation should run for a mesh given the skinned policy.
 *
 * Skinned meshes are skipped by default: position-only simplification ignores
 * joint-assignment seams (weight swim) and the attribute simplifier is both
 * costlier and only a partial mitigation. Opt in per asset via generateSkinned.
 */
bool ShouldGenerateLODsForMesh(const Mesh& mesh, bool generateSkinned);

// True if mesh simplification (meshoptimizer) was compiled in. When false,
// GenerateMeshLODs only ever returns LOD0.
bool IsMeshLODGenerationAvailable();

/**
 * @brief Process-wide LOD import policy.
 *
 * The Engine has no dependency on the Editor, so editor-side import preferences
 * are pushed down through this global: the Editor sets it from its saved
 * preferences (at startup and when the user toggles the setting), and the model
 * import path (ModelAsset::PostLoad) reads it. The Editor and packaged Player
 * default this ON for static meshes (via project settings / game config); this
 * engine-level default stays disabled so headless / non-editor / test hosts
 * never pay for generation unless a host explicitly opts in.
 */
struct LODImportSettings {
    bool AutoGenerateOnImport = false;
    MeshLODConfig Config{};
};

void SetLODImportSettings(const LODImportSettings& settings);
const LODImportSettings& GetLODImportSettings();

} // namespace GameEngine
