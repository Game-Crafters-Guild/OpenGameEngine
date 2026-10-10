#pragma once

// Cooked LOD cache (.gelod): the simplified LOD index buffers plus their achieved
// errors for every submesh of a model, serialized so repeat editor loads and
// shipped Player builds skip meshopt entirely. LOD0 and its vertex streams are
// re-derived from the source parse; non-sloppy levels share those vertices
// (meshopt reuses indices) and store indices only, while attribute-honest sloppy
// shells additionally store their OWN per-level vertex block (v3) — LOD-local
// indices, reconstructed flat normals + island-snapped UVs. See
// mesh-lod-pipeline-design v0.2 §3. Format mirrors AssetSourceSnapshot:
// little-endian, magic + version, atomic temp+rename write, fully bounds-checked
// read (a truncated blob never reaches GPU upload — amendment A4).

#include "Types/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace GameEngine {

struct Mesh;
struct MeshLODConfig;

// Cache identity. Both halves must match for an editor cache hit. A packaged
// Player cannot recompute a source fingerprint, so it trusts the build-baked
// file and validates format + geometry structure + index bounds instead.
struct LodCacheKey {
    uint64 SourceHash = 0;   // full FNV-1a of the raw source bytes (portable)
    uint64 ConfigHash = 0;   // resolved config folded with meshopt + generator versions
};

// Outcome of a cache read. Any value other than Hit means "regenerate or fall
// back to LOD0-only" and leaves the caller's meshes untouched.
enum class LodCacheStatus : uint8 {
    Hit,               // valid, key/structure agreed, LODs applied to every mesh
    Missing,           // file absent or unreadable
    FormatMismatch,    // wrong magic or unsupported format version
    KeyMismatch,       // sourceHash / configHash disagree with expectKey (editor)
    StructureMismatch, // meshCount / submeshIndex / vertexCount disagree with the model
    Corrupt,           // truncated, or an index out of range (A4 rejection)
};

// Bump when the .gelod byte layout changes; gates format compatibility only, not
// cook output (that is what the config hash version fold is for).
//
// v2 (Phase C1): each submesh gains an `authored` byte. Authored submeshes carry
// artist-made per-LOD vertex blocks the parse produced; the cook cannot round-
// trip them and must not clobber them on read, so it writes them as zero-LOD
// authored entries and the reader leaves their parse-supplied chain intact.
//
// v3: each level gains a vertexCount field followed by that many raw Vertex
// records. 0 => index-only level sharing the submesh's parsed vertices (indices
// bounds-checked against the submesh, as before); non-zero => the level owns its
// vertex block (generated attribute-honest sloppy shell) and its indices are
// LOD-LOCAL, bounds-checked against the block. Authored entries remain zero-LOD
// placeholders — their blocks still come from the source parse. An old file
// fails the exact version check below and misses cleanly — a one-time full
// re-cook on first load after the bump (dev-only; packaged titles co-version the
// importer + baked cache).
inline constexpr uint32 kLodCacheFormatVersion = 3u;

// Authored admission affects the parsed submesh partition before generated
// cache application. Fold it independently of the generator algorithm version.
// v1 admits aligned per-level RGBA; authored payloads still come from source.
inline constexpr uint32 kAuthoredLodImportVersion = 1u;

// Bump when the generator algorithm changes in a way that alters produced indices
// for an unchanged config + input. Folded into the config hash so a generator
// change makes every prior cook a miss (amendment A3/F7).
//
// v3: border-locked levels that cannot hit their budget watertight are skipped
// (emit-no-level) instead of retried with unlocked borders — the retry produced
// cracked LODs (visible holes). Failed / budget-met levels no longer truncate
// the chain; coarser levels are still attempted.
//
// v4: sloppy far levels are rebuilt into attribute-honest own-vertex shells
// (per-face flat normals, area-dominant island-snapped UVs, LOD-local
// indices); an all-degenerate shell is refused and falls back to the
// topology-limited result or skips. Position-only sloppy output over original
// indices is never emitted again (it smeared UVs across atlas islands and
// inherited arbitrary normals).
//
// v5 (mid-tier arc): quality levels move to meshopt_simplifyWithAttributes
// (normal+UV0 weights, achieved error includes attribute deviation), the
// binary border lock becomes MeshLODBorderRule (default SeamPlanes: lock only
// open-border vertices on the mesh AABB's face planes — cross-piece kit seams
// — so mid levels can emit on open-border kit content), coarse levels may
// prune small disconnected components, and a level must reduce to <= 90% of
// its predecessor to be emitted. The shadow-index-buffer pre-weld is replaced
// by position-stream snapping: near-duplicate seams still weld (bit-exact
// after the snap, closed by meshopt's internal remap) but the output keeps
// per-wedge indices, so split normals and palette-atlas UV seams survive the
// emitted levels instead of flattening to one representative per position.
//
// v6: quality levels add meshopt_SimplifyPermissive — flat-shaded kit content
// (per-face normals) makes every vertex a 3+-wedge attribute corner, which
// non-permissive meshopt classifies complex and locks outright; the gate
// corpus's rocks/trees/cliffs emitted zero quality levels without it.
//
// v7: chromatic-honesty iteration. Component pruning decouples from the level
// budgets (standalone meshopt_simplifyPrune at kPruneErrorBudget feeds the
// coarse levels; the generous far TargetError can no longer delete garland-
// sized components). Co-located wedge groups whose UVs span distant atlas
// regions tag meshopt_SimplifyVertex_Protect, so Permissive keeps chromatic
// chart boundaries (moon emblems, awning stripes) intact. Residual cross-
// island interpolation folds into the achieved error as an engagement delay.
// Default NormalWeight rises 0.5 -> 2.0 (thin-dressing preservation) with the
// threshold mapping recalibrated in lockstep (MeshLODThresholds.h).
//
// v8: shape-honesty iteration, entirely at the index level — every level's
// triangle count is identical to v7. Quality levels fold a silhouette-honesty
// tripwire into the achieved error (LOD0 vs level axis-view raster mismatch),
// so a level that no longer draws the same object — a crescent finial collapsed
// to a blob, a leaf cascade shrunk within its own plane at ~zero position cost —
// self-delays engagement through the standard error->coverage mapping instead of
// showing the wrong shape at mid distances. Emitted levels additionally re-point
// each corner to the co-located wedge whose normal matches the triangle's own
// plane, repairing the mixed-facet shading normals a collapse leaves behind on
// flat-shaded kit content (broken highlights on large flat faces).
//
// v9: re-point hardening. Candidate normals are normalized before matching
// (zero-length normals are never adopted and never hold a corner), candidates
// must keep the current wedge's tangent hemisphere and handedness sign
// (Tangent[4] rides the wedge swap; a reversed or mirrored tangent breaks
// normal mapping), and a re-point never fuses two corners of one triangle
// into the same wedge index (within-weld-epsilon slivers share a position
// group and would otherwise emit an index-degenerate triangle).
//
// Bump this on EVERY generator-behavior change, including intra-iteration
// commits: v8 covered two behaviorally different generators (v8a silhouette
// fold, v8b corner re-point), so cooks measured across those commits aliased
// in .Cache/Lod and cross-commit comparisons silently reused the older cook.
// If a change is meant to be measured, it needs its own version — otherwise
// purge .Cache/Lod between measurement runs.
inline constexpr uint32 kLodGeneratorVersion = 9u;

// Full FNV-1a-64 content hash of a model's source bytes. Portable across machines
// and fresh git checkouts (unlike the registry's mtime+size fingerprint), at the
// cost of one read+hash — paid once at cook time, never at Player runtime (the
// key is baked into the header and validated by structure + bounds instead).
uint64 ComputeLodSourceHash(const uint8* data, std::size_t size);

// Hash the resolved generation inputs: the MeshLODConfig fields that affect
// produced indices, the skinned opt-in, meshopt + generator + authored-import revisions, and
// the format-specific parse-options hash (FBX / glTF / Blend loader options;
// 0 for formats with none). A non-zero parseOptionsHash is folded in, so a
// parse-option change is a miss; a 0 leaves the hash byte-identical to a
// config with no parse options (OBJ caches are not perturbed). Editor
// and packaging must fold identical inputs so their keys agree. The authored-
// import revision is a defaulted parameter so callers bind the current salt
// while tests can perturb it.
uint64 ComputeLodConfigHash(const MeshLODConfig& config, bool generateSkinned,
                            uint64 parseOptionsHash,
                            uint32 authoredImportVersion = kAuthoredLodImportVersion);

// Provenance fingerprint of the produced LOD output (index buffers + errors),
// stored in the .gelod header as generatedHash. Not part of the cache key —
// {sourceHash, configHash} decide validity — this just records what was cooked
// (A5: derived provenance lives in the header, never in the authoritative kv).
uint64 ComputeGeneratedLodHash(const Vector<Mesh>& meshes);

// Serialize every mesh's ExtraLODs / ExtraLODErrors / ExtraLODSloppy — plus the
// per-level ExtraLODVertices block for generated own-vertex shells — to `file`,
// written atomically via a temp file + rename so a crash mid-write can never
// leave a half-blob that passes magic/version with a truncated tail. Meshes with
// no ExtraLODs serialize as zero-LOD entries so the submesh array stays aligned
// with the model. Returns false on any I/O failure (the target is left as-is).
bool WriteLodCache(const std::filesystem::path& file,
                   const LodCacheKey& key,
                   uint64 generatedHash,
                   const Vector<Mesh>& meshes);

// Read + validate a .gelod and, only on a full-file Hit, apply its LODs into
// `meshes`. `meshes` must already hold the parsed LOD0 geometry (Vertices sized)
// so every deserialized index can be bounds-checked against its submesh vertex
// count (A4). When `expectKey` is non-null the header key must match it exactly
// (editor path); a packaged Player passes null and relies on format + structure +
// bounds validation. On any non-Hit status `meshes` is left untouched. Never
// throws and never applies out-of-bounds indices.
LodCacheStatus ReadLodCacheInto(const std::filesystem::path& file,
                                const LodCacheKey* expectKey,
                                Vector<Mesh>& meshes);

} // namespace GameEngine
