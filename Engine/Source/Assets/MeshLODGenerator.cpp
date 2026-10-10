#include "Assets/MeshLODGenerator.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/ModelAsset.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>

#if defined(GE_HAVE_MESHOPTIMIZER)
#include <meshoptimizer.h>
#endif

namespace GameEngine {

namespace {

// Round a triangle-index count down to a whole number of triangles.
size_t RoundToTriangles(size_t indexCount) { return (indexCount / 3) * 3; }

// Grid cell for the pre-simplify position weld, as a fraction of the mesh's
// largest bounding-box extent. It must sit well below any real geometric
// feature (so distinct vertices are never merged) yet far above the trig
// round-off that separates a procedural seam's two columns (~1e-7 relative).
constexpr float kWeldPositionRelativeEpsilon = 1e-5f;

// --- Attribute-honest sloppy shells -----------------------------------------
//
// meshopt_simplifySloppy is position-only and reuses original vertex indices,
// so a collapsed triangle's corners can come from different UV-atlas islands:
// the rasterizer then interpolates UVs ACROSS islands (sweeping the whole
// atlas — rainbow smears on palette-atlas content) and the corners carry
// whatever normals the source surface had there (black facets on recombined
// connectivity). Instead of shipping those indices, each accepted sloppy level
// is rebuilt into its own per-face vertex block: flat face normals (fine for a
// sub-salience far shell) and island-snapped UVs — every triangle's corner UVs
// are guaranteed to lie in ONE island, so interpolation can never cross the
// atlas. A shell with no non-degenerate face is refused; a refused shell falls
// back to the topology-limited non-sloppy result, or the level is skipped
// entirely (skip-a-level), never emitted corrupt.

// Squared-length floor (on extent-normalized edge cross products) below which
// a shell face is positionally degenerate and dropped outright.
//
// Deliberately NO UV-deviation metric gates emission beyond degeneracy. Both
// candidate proxies were implemented and measured on the ElvenRealm gate
// corpus, and both were removed with cause: a post-snap UV-spread-vs-island
// bound is provably inert (snapping keeps every corner UV inside the dominant
// island's chart, so a triangle's UV box can never exceed the island's), and a
// donor-distance bound (donor farther than a fraction of the mesh extent)
// rejected 70% of shells at 0.2 and 31% at 0.5 — forfeiting exactly the
// heavyweight hero props' far savings — while guarding an artifact that
// area-dominant island snapping already bounds and that is sub-salient at the
// 0.03 engagement cap by that cap's definition. Chromatic honesty is
// guaranteed by construction (island-snapped UVs, flat face normals), not by
// post-hoc metrics; only shells with no non-degenerate face are refused.
constexpr float kShellDegenerateFaceEps = 1e-12f;

constexpr uint32 kNoIsland = 0xFFFFFFFFu;

// UV islands = connected components of the ORIGINAL index buffer's vertex
// graph. A vertex carries exactly one UV, so triangles connected through
// shared vertex indices form one contiguous UV chart; a UV seam duplicates
// positions under different indices and therefore splits charts — exactly the
// island boundary we must not interpolate across. Built on the original
// (pre-weld) indices: the tolerant position weld deliberately merges across
// UV seams, which would fuse the very islands this map exists to separate.
struct UvIslandMap {
    Vector<uint32> VertexIsland;        // per source vertex; kNoIsland if unreferenced
    Vector<Vector<uint32>> IslandVerts; // dense island id -> member vertex indices
    Vector<double> IslandArea;          // summed source-triangle 3D area per island
};

UvIslandMap BuildUvIslandMap(const Mesh& mesh) {
    const size_t vertexCount = mesh.Vertices.size();
    UvIslandMap map;
    map.VertexIsland.assign(vertexCount, kNoIsland);

    Vector<uint32> parent(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v)
        parent[v] = static_cast<uint32>(v);
    // Iterative find with path halving — kit meshes can chain long components.
    const auto find = [&parent](uint32 v) {
        while (parent[v] != v) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    };
    const auto unite = [&](uint32 a, uint32 b) {
        a = find(a); b = find(b);
        if (a != b) parent[b] = a;
    };

    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        unite(mesh.Indices[i], mesh.Indices[i + 1]);
        unite(mesh.Indices[i], mesh.Indices[i + 2]);
    }

    // Dense island ids for referenced vertices only.
    for (uint32 idx : mesh.Indices) {
        const uint32 root = find(idx);
        if (map.VertexIsland[root] == kNoIsland) {
            map.VertexIsland[root] = static_cast<uint32>(map.IslandVerts.size());
            map.IslandVerts.emplace_back();
            map.IslandArea.push_back(0.0);
        }
    }
    for (size_t v = 0; v < vertexCount; ++v) {
        const uint32 root = find(static_cast<uint32>(v));
        const uint32 island = map.VertexIsland[root];
        map.VertexIsland[v] = island;
        if (island == kNoIsland)
            continue; // vertex referenced by no triangle
        map.IslandVerts[island].push_back(static_cast<uint32>(v));
    }
    // Island surface area — the dominant-island vote weight. Corner-majority
    // voting mis-paints thin two-sided props whose vertex-dense trim/emblem
    // charts outnumber the perceptually dominant cloth chart (gate corpus:
    // banner shells rendered white instead of blue); the chart that carries
    // the surface AREA is the statistically safe color at shell distances.
    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
        const uint32 island = map.VertexIsland[mesh.Indices[i]];
        if (island == kNoIsland)
            continue;
        const float* p0 = mesh.Vertices[mesh.Indices[i]].Position;
        const float* p1 = mesh.Vertices[mesh.Indices[i + 1]].Position;
        const float* p2 = mesh.Vertices[mesh.Indices[i + 2]].Position;
        const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const double cx = e1[1] * e2[2] - e1[2] * e2[1];
        const double cy = e1[2] * e2[0] - e1[0] * e2[2];
        const double cz = e1[0] * e2[1] - e1[1] * e2[0];
        map.IslandArea[island] += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    return map;
}

float DistanceSq3(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// Nearest member of `island` to `pos` (brute force — islands are charts of a
// single submesh and queries only fire for the minority cross-island corners).
uint32 NearestIslandVertex(const Mesh& mesh, const UvIslandMap& map, uint32 island,
                           const float* pos) {
    uint32 best = kNoIsland;
    float bestSq = std::numeric_limits<float>::max();
    for (uint32 v : map.IslandVerts[island]) {
        const float d = DistanceSq3(pos, mesh.Vertices[v].Position);
        if (d < bestSq) {
            bestSq = d;
            best = v;
        }
    }
    return best;
}

struct ShellBuildOutcome {
    bool Accepted = false;
    Vector<uint32> LocalIndices; // 0..N-1 into Vertices (per-face, no sharing)
    Vector<Vertex> Vertices;
};

// Rebuild a sloppy level's triangles into an attribute-honest per-face vertex
// block. `shellIndices` reference source vertices (meshopt reuses indices);
// positions are kept, normals become flat face normals, and UVs are
// island-snapped: the triangle's dominant island keeps its corners' own UVs,
// foreign corners take the UV of the nearest (3D) vertex of the dominant
// island — interpolation then never crosses an island. Per-face vertices (3
// per triangle, no sharing) make the snap and the flat shading exact per face;
// shells are <= 10% of source triangles, so the duplication is noise next to
// LOD0. Refused only when no non-degenerate face survives.
ShellBuildOutcome BuildAttributeHonestShell(const Mesh& mesh, const UvIslandMap& map,
                                            const uint32* shellIndices,
                                            size_t shellIndexCount, float meshExtent) {
    ShellBuildOutcome out;
    const size_t triCount = shellIndexCount / 3;
    out.Vertices.reserve(triCount * 3);
    out.LocalIndices.reserve(triCount * 3);

    const float invExtent = meshExtent > 0.0f ? 1.0f / meshExtent : 1.0f;

    for (size_t t = 0; t < triCount; ++t) {
        const uint32 ia = shellIndices[t * 3 + 0];
        const uint32 ib = shellIndices[t * 3 + 1];
        const uint32 ic = shellIndices[t * 3 + 2];
        const Vertex& va = mesh.Vertices[ia];
        const Vertex& vb = mesh.Vertices[ib];
        const Vertex& vc = mesh.Vertices[ic];

        // Face normal (engine winding: cross(p1-p0, p2-p0)), extent-normalized
        // for a scale-independent degeneracy test.
        const float e1[3] = {(vb.Position[0] - va.Position[0]) * invExtent,
                             (vb.Position[1] - va.Position[1]) * invExtent,
                             (vb.Position[2] - va.Position[2]) * invExtent};
        const float e2[3] = {(vc.Position[0] - va.Position[0]) * invExtent,
                             (vc.Position[1] - va.Position[1]) * invExtent,
                             (vc.Position[2] - va.Position[2]) * invExtent};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                      e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float nLenSq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (nLenSq < kShellDegenerateFaceEps)
            continue; // positionally collapsed face — contributes nothing
        const float invNLen = 1.0f / std::sqrt(nLenSq);
        n[0] *= invNLen; n[1] *= invNLen; n[2] *= invNLen;

        // Dominant island = the corner island carrying the greatest SOURCE
        // surface area (ties break to the smaller id — first-referenced, so
        // deterministic). Not corner-majority: sloppy collapse samples charts
        // by vertex density, not by area, so majority voting painted thin
        // two-sided props with their vertex-dense trim chart (gate corpus:
        // banner shells came out white instead of cloth-blue). The largest
        // chart among the corners is the statistically safe color for a face
        // at shell engagement distances. Corners with no island (unreferenced
        // representative) never dominate and keep their own UV untouched.
        const uint32 corners[3] = {ia, ib, ic};
        const uint32 islands[3] = {map.VertexIsland[ia], map.VertexIsland[ib],
                                   map.VertexIsland[ic]};
        uint32 dominant = kNoIsland;
        for (int cand = 0; cand < 3; ++cand) {
            const uint32 island = islands[cand];
            if (island == kNoIsland)
                continue;
            if (dominant == kNoIsland ||
                map.IslandArea[island] > map.IslandArea[dominant] ||
                (map.IslandArea[island] == map.IslandArea[dominant] && island < dominant))
                dominant = island;
        }

        // Snap corner UVs into the dominant island.
        float uv[3][2] = {{va.TexCoords[0], va.TexCoords[1]},
                          {vb.TexCoords[0], vb.TexCoords[1]},
                          {vc.TexCoords[0], vc.TexCoords[1]}};
        if (dominant != kNoIsland) {
            for (int c = 0; c < 3; ++c) {
                if (islands[c] == dominant || islands[c] == kNoIsland)
                    continue;
                const uint32 donor = NearestIslandVertex(
                    mesh, map, dominant, mesh.Vertices[corners[c]].Position);
                if (donor != kNoIsland) {
                    uv[c][0] = mesh.Vertices[donor].TexCoords[0];
                    uv[c][1] = mesh.Vertices[donor].TexCoords[1];
                }
            }
        }

        // Per-face tangent from the snapped UVs; degenerate UV area falls back
        // to an arbitrary axis orthogonal to the face normal. Orthonormalized
        // against N; handedness from the computed bitangent orientation.
        const float duv1[2] = {uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]};
        const float duv2[2] = {uv[2][0] - uv[0][0], uv[2][1] - uv[0][1]};
        const float det = duv1[0] * duv2[1] - duv2[0] * duv1[1];
        float tangent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (std::abs(det) > 1e-12f) {
            const float invDet = 1.0f / det;
            float tx = (e1[0] * duv2[1] - e2[0] * duv1[1]) * invDet;
            float ty = (e1[1] * duv2[1] - e2[1] * duv1[1]) * invDet;
            float tz = (e1[2] * duv2[1] - e2[2] * duv1[1]) * invDet;
            // Gram-Schmidt against the face normal.
            const float ndt = n[0] * tx + n[1] * ty + n[2] * tz;
            tx -= n[0] * ndt; ty -= n[1] * ndt; tz -= n[2] * ndt;
            const float tLenSq = tx * tx + ty * ty + tz * tz;
            if (tLenSq > 1e-12f) {
                const float invT = 1.0f / std::sqrt(tLenSq);
                tangent[0] = tx * invT; tangent[1] = ty * invT; tangent[2] = tz * invT;
                const float bx = (e2[0] * duv1[0] - e1[0] * duv2[0]) * invDet;
                const float by = (e2[1] * duv1[0] - e1[1] * duv2[0]) * invDet;
                const float bz = (e2[2] * duv1[0] - e1[2] * duv2[0]) * invDet;
                const float cx = n[1] * tangent[2] - n[2] * tangent[1];
                const float cy = n[2] * tangent[0] - n[0] * tangent[2];
                const float cz = n[0] * tangent[1] - n[1] * tangent[0];
                tangent[3] = (cx * bx + cy * by + cz * bz) < 0.0f ? -1.0f : 1.0f;
            }
        }
        if (tangent[0] == 0.0f && tangent[1] == 0.0f && tangent[2] == 0.0f) {
            // Axis least aligned with N, made orthogonal.
            const float ax = std::abs(n[0]), ay = std::abs(n[1]), az = std::abs(n[2]);
            float axis[3] = {0.0f, 0.0f, 0.0f};
            axis[(ax <= ay && ax <= az) ? 0 : (ay <= az ? 1 : 2)] = 1.0f;
            float tx = axis[1] * n[2] - axis[2] * n[1];
            float ty = axis[2] * n[0] - axis[0] * n[2];
            float tz = axis[0] * n[1] - axis[1] * n[0];
            const float invT = 1.0f / std::sqrt(tx * tx + ty * ty + tz * tz);
            tangent[0] = tx * invT; tangent[1] = ty * invT; tangent[2] = tz * invT;
        }

        const Vertex* src[3] = {&va, &vb, &vc};
        for (int c = 0; c < 3; ++c) {
            Vertex v{};
            v.Position[0] = src[c]->Position[0];
            v.Position[1] = src[c]->Position[1];
            v.Position[2] = src[c]->Position[2];
            v.Normal[0] = n[0]; v.Normal[1] = n[1]; v.Normal[2] = n[2];
            v.TexCoords[0] = uv[c][0]; v.TexCoords[1] = uv[c][1];
            v.Tangent[0] = tangent[0]; v.Tangent[1] = tangent[1];
            v.Tangent[2] = tangent[2]; v.Tangent[3] = tangent[3];
            out.LocalIndices.push_back(static_cast<uint32>(out.Vertices.size()));
            out.Vertices.push_back(v);
        }
    }

    if (out.LocalIndices.size() < 3)
        return out; // everything degenerate — nothing honest to emit
    out.Accepted = true;
    return out;
}

// Mesh AABB + largest extent — the scale reference for the weld epsilon, the
// shell degeneracy test, and the seam-plane border classification.
struct MeshBoundsInfo {
    float Min[3] = {0.0f, 0.0f, 0.0f};
    float Max[3] = {0.0f, 0.0f, 0.0f};
    float Extent = 0.0f; // largest axis extent
};

MeshBoundsInfo ComputeMeshBounds(const Mesh& mesh) {
    MeshBoundsInfo info;
    if (mesh.Vertices.empty())
        return info;
    for (int c = 0; c < 3; ++c)
        info.Min[c] = info.Max[c] = mesh.Vertices[0].Position[c];
    for (const Vertex& v : mesh.Vertices) {
        for (int c = 0; c < 3; ++c) {
            info.Min[c] = std::min(info.Min[c], v.Position[c]);
            info.Max[c] = std::max(info.Max[c], v.Position[c]);
        }
    }
    info.Extent = std::max({info.Max[0] - info.Min[0], info.Max[1] - info.Min[1],
                            info.Max[2] - info.Min[2]});
    return info;
}

#if defined(GE_HAVE_MESHOPTIMIZER)
// Optional vertex streams are parallel to Mesh::Vertices. The generator emits
// no per-level copy of any of them, so a shell built on such a mesh would leave
// the level's parallel data addressing LOD0's block. Vertex color has a per-level
// block on the authored path (Mesh::ExtraLODColor0) that this generator does not
// produce, so it is refused here too. Refuse to build shells — the level then
// simply skips, never regresses to LOD0-only.
bool CanBuildOwnVertexShell(const Mesh& mesh) {
    return !mesh.HasColor0() && !mesh.HasTexCoords1() && !mesh.IsSkinned() &&
           mesh.ExtraTexCoords.empty();
}

// A border vertex is "on a seam plane" when it sits within this fraction of
// THAT AXIS's AABB extent from either of the axis's two face planes. Kit
// pieces tile at their bounding extremes exactly, so the tolerance only has to
// absorb export round-off; it must stay far below real feature sizes or
// interior borders near a face would be over-locked. Per-axis (not largest-
// extent) because thin pieces break otherwise: a 10 m x 0.1 m rail under a
// largest-extent epsilon gets a 1 cm band = 10% of its thin axis, so profile
// borders wrongly classify as seams and pin the whole chain. A degenerate axis
// (flat plane: min == max, epsilon floored) puts EVERY vertex on that axis's
// planes, so a flat piece's whole perimeter locks — which is exactly right for
// ground/wall tiles that tile in their own plane.
constexpr float kSeamPlaneRelativeEpsilon = 1e-3f;

// A mid level must earn its chain slot: emit only when it reduced to at most
// this fraction of the previous level's indices. A barely-reduced level wastes
// one of the four LOD slots for ~no triangle savings; skipping it lets the
// coarser budget try instead (levels are independent, so nothing compounds).
constexpr float kMinLevelReduction = 0.9f;

// Levels at or below this target ratio simplify over the PRUNED index source
// (floaters and micro-detail components removed); near levels keep every
// component. See kPruneErrorBudget for why pruning has its own budget.
constexpr float kPruneRatioCeiling = 0.25f;

// Dedicated error budget for component pruning (meshopt_simplifyPrune),
// deliberately DECOUPLED from the per-level simplify budgets. When pruning
// rode the level budget (v6, meshopt_SimplifyPrune under TargetError 0.35),
// the generous far budgets licensed it to delete components spanning a third
// of the mesh — window-decoration garlands, hanging vine cascades — which is
// "missing geometry", the one thing a mid tier must never produce. 0.05 sheds
// true floaters (sub-5%-of-extent debris) at coarse levels while anything
// structurally visible survives every quality level.
constexpr float kPruneErrorBudget = 0.05f;

// Co-located wedge groups whose UVs sit farther apart than this (atlas
// distance) are chromatic chart boundaries: the two sides sample DIFFERENT
// palette regions (emblem plate on cloth, awning stripes), and a collapse
// across them remaps whole surface regions to the wrong palette cell — the
// banner-to-plaster amalgamation class, invisible to both the position and
// the attribute error metric (measured: the moon emblem costs ~nothing to
// destroy in either). Such groups are tagged meshopt_SimplifyVertex_Protect,
// which Permissive honours by refusing collapses across the discontinuity
// while still simplifying freely on both sides. Palette-local seams (rock and
// cliff charts: co-located UV spread ~0 by construction, measured corpus-wide)
// tag nothing and keep full v6 reduction.
constexpr float kChartProtectUvSpread = 0.05f;

// Residual-amalgamation honesty tripwire: the area fraction of a level's
// triangles whose corners span multiple UV islands, weighted by their corner
// UV spread, approximates how much of the surface interpolates texels across
// atlas regions. Folded into the achieved error (max), so a level that still
// amalgamates despite the Protect flags self-delays its engagement through
// the standard error->coverage mapping instead of shipping wrong colors at
// mid distances. Scale calibrated on the gate corpus: damaged families (flags
// ~0.08-0.10 spread-area) fold to ~0.4-0.5 error (sub-salience engagement);
// clean structural content (rocks/trees/statues <= 0.02) stays below the
// per-slot anchor floors and is untouched.
constexpr float kChartAmalgamationErrorScale = 5.0f;

// Per-vertex simplification flag array (meshopt_SimplifyVertex_* bits):
//
// - meshopt_SimplifyVertex_Lock on open-border vertices (an edge referenced by
//   exactly one triangle in the position-welded topology) that lie on the mesh
//   AABB's face planes, under MeshLODBorderRule::SeamPlanes. Cross-piece kit
//   seams sit there by construction and must never move; all other borders
//   drift under the error budget.
// - meshopt_SimplifyVertex_Protect on every wedge of a co-located group whose
//   UV spread exceeds kChartProtectUvSpread (chromatic chart boundaries; see
//   above). Applied under EVERY border rule — chromatic protection is
//   orthogonal to border policy.
//
// Topology is viewed through the snapped position stream (co-located wedges =
// one logical vertex, matching meshopt's internal weld), and a flag on a
// logical vertex propagates to EVERY wedge sharing its position — meshopt
// reads flags per index, so an unflagged twin would defeat the flag. Returns
// an empty vector when nothing needs flagging (then pass no array at all).
Vector<uint8> BuildVertexSimplifyFlags(const Mesh& mesh, const Vector<float>& snappedPositions,
                                       const MeshBoundsInfo& bounds,
                                       MeshLODBorderRule borderRule) {
    const size_t vertexCount = mesh.Vertices.size();

    // Canonical logical vertex per bit-exact snapped position.
    std::map<std::array<uint32, 3>, uint32> firstAtPosition;
    Vector<uint32> canonical(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v) {
        std::array<uint32, 3> key;
        std::memcpy(key.data(), &snappedPositions[v * 3], sizeof(key));
        canonical[v] =
            firstAtPosition.emplace(key, static_cast<uint32>(v)).first->second;
    }

    Vector<uint8> flaggedCanonical(vertexCount, 0u);
    bool any = false;

    if (borderRule == MeshLODBorderRule::SeamPlanes) {
        // Undirected edge occurrence counts over canonical ids via a sorted
        // packed-edge list (cook path — cache-friendly, no hashing).
        Vector<uint64> edges;
        edges.reserve(mesh.Indices.size());
        for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
            const uint32 tri[3] = {canonical[mesh.Indices[i]], canonical[mesh.Indices[i + 1]],
                                   canonical[mesh.Indices[i + 2]]};
            for (int e = 0; e < 3; ++e) {
                uint32 a = tri[e], b = tri[(e + 1) % 3];
                if (a == b)
                    continue;
                if (a > b)
                    std::swap(a, b);
                edges.push_back((static_cast<uint64>(a) << 32) | b);
            }
        }
        std::sort(edges.begin(), edges.end());

        float eps[3];
        for (int c = 0; c < 3; ++c)
            eps[c] = std::max((bounds.Max[c] - bounds.Min[c]) * kSeamPlaneRelativeEpsilon,
                              1e-7f);
        const auto onSeamPlane = [&](uint32 v) {
            const float* p = mesh.Vertices[v].Position;
            for (int c = 0; c < 3; ++c) {
                if (p[c] - bounds.Min[c] <= eps[c] || bounds.Max[c] - p[c] <= eps[c])
                    return true;
            }
            return false;
        };

        for (size_t i = 0; i < edges.size();) {
            size_t j = i + 1;
            while (j < edges.size() && edges[j] == edges[i])
                ++j;
            if (j - i == 1) { // border edge: lock endpoints on a seam plane
                const uint32 a = static_cast<uint32>(edges[i] >> 32);
                const uint32 b = static_cast<uint32>(edges[i] & 0xFFFFFFFFu);
                if (onSeamPlane(a)) { flaggedCanonical[a] |= meshopt_SimplifyVertex_Lock; any = true; }
                if (onSeamPlane(b)) { flaggedCanonical[b] |= meshopt_SimplifyVertex_Lock; any = true; }
            }
            i = j;
        }
    }

    // Chromatic chart boundaries: max pairwise UV spread per co-located group.
    // Groups are tiny (a handful of wedges), so the pairwise scan is noise.
    {
        const float thresholdSq = kChartProtectUvSpread * kChartProtectUvSpread;
        std::map<std::array<uint32, 3>, Vector<uint32>> members;
        for (size_t v = 0; v < vertexCount; ++v) {
            std::array<uint32, 3> key;
            std::memcpy(key.data(), &snappedPositions[v * 3], sizeof(key));
            members[key].push_back(static_cast<uint32>(v));
        }
        for (const auto& [key, verts] : members) {
            if (verts.size() < 2)
                continue;
            float spreadSq = 0.0f;
            for (size_t i = 0; i < verts.size(); ++i) {
                for (size_t j = i + 1; j < verts.size(); ++j) {
                    const float du = mesh.Vertices[verts[i]].TexCoords[0] -
                                     mesh.Vertices[verts[j]].TexCoords[0];
                    const float dv = mesh.Vertices[verts[i]].TexCoords[1] -
                                     mesh.Vertices[verts[j]].TexCoords[1];
                    spreadSq = std::max(spreadSq, du * du + dv * dv);
                }
            }
            if (spreadSq > thresholdSq) {
                flaggedCanonical[canonical[verts[0]]] |= meshopt_SimplifyVertex_Protect;
                any = true;
            }
        }
    }

    if (!any)
        return {};

    // Propagate canonical flags to every wedge of the position.
    Vector<uint8> flags(vertexCount, 0u);
    for (size_t v = 0; v < vertexCount; ++v)
        flags[v] = flaggedCanonical[canonical[v]];
    return flags;
}

// Weld-snapped copy of the position stream. meshopt groups vertices by EXACT
// position bytes for its internal topology weld, so near-equal seam twins
// (procedural seams computed independently per side) must be snapped to a
// coarse grid — 1e-5 of extent, well below any real feature — to weld shut.
// The same stream keys the co-located wedge groups the re-point pass uses.
Vector<float> BuildSnappedPositions(const Mesh& mesh, float meshExtent) {
    const size_t vertexCount = mesh.Vertices.size();
    Vector<float> positions(vertexCount * 3);
    for (size_t v = 0; v < vertexCount; ++v) {
        const float* p = &mesh.Vertices[v].Position[0];
        for (int c = 0; c < 3; ++c)
            positions[v * 3 + static_cast<size_t>(c)] = p[c];
    }
    if (meshExtent > 0.0f) {
        const float weldEps = meshExtent * kWeldPositionRelativeEpsilon;
        for (size_t i = 0; i < positions.size(); ++i) {
            float snapped = std::round(positions[i] / weldEps) * weldEps;
            if (snapped == 0.0f)
                snapped = 0.0f; // normalize -0 so its bytes match +0
            positions[i] = snapped;
        }
    }
    return positions;
}

// --- Wedge-normal re-point --------------------------------------------------

// Co-located wedge groups over the snapped position stream: every wedge that
// shares a logical vertex, in one flat CSR-style block. Built once per mesh
// and consumed by the re-point pass below.
struct ColocatedWedges {
    Vector<uint32> GroupOf; // vertex -> group index
    Vector<uint32> Offsets; // group -> [Offsets[g], Offsets[g+1]) into Members
    Vector<uint32> Members; // wedge indices, grouped by position
};

ColocatedWedges BuildColocatedWedges(const Vector<float>& snappedPositions, size_t vertexCount) {
    std::map<std::array<uint32, 3>, Vector<uint32>> byPosition;
    for (size_t v = 0; v < vertexCount; ++v) {
        std::array<uint32, 3> key;
        std::memcpy(key.data(), &snappedPositions[v * 3], sizeof(key));
        byPosition[key].push_back(static_cast<uint32>(v));
    }

    ColocatedWedges groups;
    groups.GroupOf.resize(vertexCount, 0u);
    groups.Offsets.reserve(byPosition.size() + 1);
    groups.Members.reserve(vertexCount);
    groups.Offsets.push_back(0u);
    for (const auto& [key, verts] : byPosition) {
        const uint32 groupIndex = static_cast<uint32>(groups.Offsets.size() - 1);
        for (uint32 v : verts) {
            groups.GroupOf[v] = groupIndex;
            groups.Members.push_back(v);
        }
        groups.Offsets.push_back(static_cast<uint32>(groups.Members.size()));
    }
    return groups;
}

// Flat / hard-edge authoring stores one normal per FACE, as separate co-located
// wedges. A collapse leaves the surviving triangle's three corners holding the
// normals of three DIFFERENT source facets, so the interpolated shading normal
// no longer matches the triangle's own plane — large flat faces pick up a
// gradient and their highlights break (measured on the gate corpus: LOD0 keeps
// 0.0-0.5% of surface area more than 25 degrees off its face, LOD1 7-29%; some
// corners even face backwards, 87 of 3630 triangles on SM_Prop_Tent_01 LOD1).
//
// Every OTHER wedge at that same position is a free alternative: same position,
// same vertex buffer, no new data. Each corner therefore re-points to the
// co-located wedge whose normal best matches the emitted triangle's geometric
// normal. Signed cosine (candidate normals normalized, zero-length excluded),
// so a two-sided sheet's back triangle picks back-facing wedges instead of the
// front sheet's normals. Candidate gates:
//   - UV: within kChartProtectUvSpread of the corner's current UV, so
//     re-pointing stays inside the chart tolerance and cannot undo the
//     chromatic protection; smooth surfaces are untouched (one wedge per
//     position => nothing to choose).
//   - Tangent: same hemisphere and same handedness sign as the current wedge.
//     The whole Vertex rides the swap, so Tangent[4] moves with the normal;
//     same-chart wedges share a continuous tangent field up to the facet
//     rotation this repair exists for (< 90 deg), and the hemisphere +
//     handedness bound blocks the class that breaks normal mapping outright
//     (reversed tangents, mirrored-UV twins near a seam fold) without blocking
//     the repair. Unauthored zero tangents dot to 0 and pass.
//   - Degeneracy: a candidate equal to another corner of the same triangle is
//     never adopted — two within-weld-epsilon corners share a position group,
//     and fusing them would emit an index-degenerate triangle the level
//     silently loses.
// Index-only: no added vertices, no added triangles, positions and therefore
// the position-quadric error unchanged (the amalgamation/silhouette tripwires
// re-measure the re-pointed indices downstream).

// A shading normal shorter than this has no usable direction (authoring or
// import artifact): never adopted, and never able to hold its corner against a
// real candidate.
constexpr float kMinShadingNormalLengthSq = 1e-12f;
// Sorts strictly below any real cosine (>= -1), so a zero-length normal loses
// even to a backwards-facing wedge.
constexpr float kNoShadingNormalMatch = -2.0f;

void RepointCornersToBestMatchingWedge(const Mesh& mesh, const ColocatedWedges& groups,
                                       Vector<uint32>& indices) {
    const float uvThresholdSq = kChartProtectUvSpread * kChartProtectUvSpread;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const float* p0 = mesh.Vertices[indices[i]].Position;
        const float* p1 = mesh.Vertices[indices[i + 1]].Position;
        const float* p2 = mesh.Vertices[indices[i + 2]].Position;
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float faceNormal[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                               e1[0] * e2[1] - e1[1] * e2[0]};
        const float length = std::sqrt(faceNormal[0] * faceNormal[0] +
                                       faceNormal[1] * faceNormal[1] +
                                       faceNormal[2] * faceNormal[2]);
        if (length <= 0.0f)
            continue; // degenerate triangle: no plane to match
        for (int c = 0; c < 3; ++c)
            faceNormal[c] /= length;

        for (int corner = 0; corner < 3; ++corner) {
            const uint32 current = indices[i + static_cast<size_t>(corner)];
            const uint32 group = groups.GroupOf[current];
            const uint32 begin = groups.Offsets[group];
            const uint32 end = groups.Offsets[group + 1u];
            if (end - begin < 2u)
                continue;
            const uint32 otherA = indices[i + static_cast<size_t>((corner + 1) % 3)];
            const uint32 otherB = indices[i + static_cast<size_t>((corner + 2) % 3)];
            const Vertex& from = mesh.Vertices[current];
            // Cosine against the triangle plane. Normalized: the raw dot let a
            // longer normal outscore a better-aligned one, and a zero-length
            // normal (raw score 0) beat exactly the backwards corner this pass
            // repairs.
            const auto matchOf = [&](const Vertex& v) {
                const float lenSq = v.Normal[0] * v.Normal[0] + v.Normal[1] * v.Normal[1] +
                                    v.Normal[2] * v.Normal[2];
                if (lenSq < kMinShadingNormalLengthSq)
                    return kNoShadingNormalMatch;
                return (v.Normal[0] * faceNormal[0] + v.Normal[1] * faceNormal[1] +
                        v.Normal[2] * faceNormal[2]) /
                       std::sqrt(lenSq);
            };
            uint32 best = current;
            float bestMatch = matchOf(from);
            for (uint32 m = begin; m < end; ++m) {
                const uint32 candidate = groups.Members[m];
                if (candidate == current)
                    continue;
                if (candidate == otherA || candidate == otherB)
                    continue; // never fuse two corners of one triangle
                const Vertex& to = mesh.Vertices[candidate];
                const float du = to.TexCoords[0] - from.TexCoords[0];
                const float dv = to.TexCoords[1] - from.TexCoords[1];
                if (du * du + dv * dv > uvThresholdSq)
                    continue; // different chart: re-pointing would smear palette cells
                if (to.Tangent[0] * from.Tangent[0] + to.Tangent[1] * from.Tangent[1] +
                            to.Tangent[2] * from.Tangent[2] <
                        0.0f ||
                    to.Tangent[3] * from.Tangent[3] < 0.0f)
                    continue; // reversed tangent / mirrored handedness: would break normal mapping
                const float match = matchOf(to);
                if (match > bestMatch) {
                    bestMatch = match;
                    best = candidate;
                }
            }
            indices[i + static_cast<size_t>(corner)] = best;
        }
    }
}

// --- Silhouette-honesty tripwire --------------------------------------------
//
// The quadric error is blind to two damage classes the gate review surfaced on
// small high-saliency props: in-plane shrinkage of open-bordered sheets (leaf
// cards vanish at ~zero reported error — vertices slide within their own
// card's plane, which costs the position quadric nothing) and shape-identity
// destruction (a crescent finial collapsing to a chevron blob spends error
// well under what its ICONIC silhouette is worth on screen). Both are exactly
// "the level no longer draws the same object", so the honest measure is the
// silhouette itself: rasterize LOD0 and the level from the three axis-aligned
// orthographic views over the mesh AABB and count coverage that appears or
// disappears. The worst view's symmetric mismatch — pixels of either mask
// farther than one texel from the other mask, normalized by LOD0's coverage —
// folds into the achieved error (kSilhouetteErrorScale, max), so a
// shape-destroying level self-delays engagement through the standard mapping
// instead of showing a wrong object at mid distances. Like the amalgamation
// tripwire this changes WHEN a level engages, never what it contains.
//
// Calibration (gate corpus, 128 grid, 1-texel tolerance): honest structural
// coarsening measures well under the damage classes (rock mids .003/.041,
// tree .001/.031, statue .004/.024/.097, tent_01 .009/.067, awning .006/.041)
// and at scale 2.0 mostly folds below the per-slot error floors or the
// level's own achieved error — floor-clamped anchor slots are bit-identical
// v7->v8; above-floor switch points drift <= 1.5% on the structural controls
// (statue t0 .362->.357). The damage classes measure 0.23-0.99 (crescent
// finial mids .23/.49, vine/leaf card presets .25-.79 after protection took
// their worst collapses) and self-delay to sub-salient coverages (vane LOD2
// engagement .100->.021). Axis-aligned views are an
// empirical choice matching kit content's axis-aligned authoring — a
// diagonally-authored thin prop would be under-measured (same class of tuned
// approximation as the extent/radius note in MeshLODThresholds.h).
constexpr float kSilhouetteErrorScale = 2.0f;
constexpr int32 kSilhouetteRasterRes = 128;

// One bit per grid cell, three axis views. Words per row = res / 64.
struct SilhouetteMasks {
    static constexpr int32 kWordsPerRow = kSilhouetteRasterRes / 64;
    // [view][row * kWordsPerRow + word]
    std::array<Vector<uint64>, 3> Views;
};

void RasterizeSilhouette(const Mesh& mesh, const Vector<uint32>& indices,
                         const MeshBoundsInfo& bounds, SilhouetteMasks& out) {
    constexpr int32 res = kSilhouetteRasterRes;
    for (int view = 0; view < 3; ++view) {
        Vector<uint64>& mask = out.Views[view];
        mask.assign(static_cast<size_t>(res) * SilhouetteMasks::kWordsPerRow, 0u);
        const int axisU = view == 0 ? 1 : 0;
        const int axisV = view == 2 ? 1 : 2;
        const float extU = bounds.Max[axisU] - bounds.Min[axisU];
        const float extV = bounds.Max[axisV] - bounds.Min[axisV];
        // Uniform scale (largest of the two view extents) so thin meshes keep
        // their aspect; one-cell margin so the dilation tolerance never clips.
        const float scale =
            static_cast<float>(res - 2) / std::max({extU, extV, 1e-9f});
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            float px[3], py[3];
            for (int c = 0; c < 3; ++c) {
                const float* p = mesh.Vertices[indices[i + static_cast<size_t>(c)]].Position;
                px[c] = (p[axisU] - bounds.Min[axisU]) * scale + 1.0f;
                py[c] = (p[axisV] - bounds.Min[axisV]) * scale + 1.0f;
            }
            const int32 minX = std::max<int32>(static_cast<int32>(std::floor(std::min({px[0], px[1], px[2]}))), 0);
            const int32 maxX = std::min<int32>(static_cast<int32>(std::ceil(std::max({px[0], px[1], px[2]}))), res - 1);
            const int32 minY = std::max<int32>(static_cast<int32>(std::floor(std::min({py[0], py[1], py[2]}))), 0);
            const int32 maxY = std::min<int32>(static_cast<int32>(std::ceil(std::max({py[0], py[1], py[2]}))), res - 1);
            for (int32 y = minY; y <= maxY; ++y) {
                const float cy = static_cast<float>(y) + 0.5f;
                for (int32 x = minX; x <= maxX; ++x) {
                    const float cx = static_cast<float>(x) + 0.5f;
                    const float d0 = (px[1] - px[0]) * (cy - py[0]) - (py[1] - py[0]) * (cx - px[0]);
                    const float d1 = (px[2] - px[1]) * (cy - py[1]) - (py[2] - py[1]) * (cx - px[1]);
                    const float d2 = (px[0] - px[2]) * (cy - py[2]) - (py[0] - py[2]) * (cx - px[2]);
                    const bool inside = (d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f) ||
                                        (d0 <= 0.0f && d1 <= 0.0f && d2 <= 0.0f);
                    if (inside)
                        mask[static_cast<size_t>(y) * SilhouetteMasks::kWordsPerRow + static_cast<size_t>(x >> 6)] |=
                            1ull << (x & 63);
                }
            }
        }
    }
}

// 4-neighbourhood dilation by one cell (the tolerance band that keeps raster
// wobble on thin diagonals out of the mismatch count).
Vector<uint64> DilateMask(const Vector<uint64>& mask) {
    constexpr int32 res = kSilhouetteRasterRes;
    constexpr int32 words = SilhouetteMasks::kWordsPerRow;
    Vector<uint64> out(mask);
    for (int32 y = 0; y < res; ++y) {
        const size_t row = static_cast<size_t>(y) * words;
        for (int32 w = 0; w < words; ++w) {
            uint64 v = mask[row + static_cast<size_t>(w)];
            uint64 h = (v << 1) | (v >> 1);
            if (w > 0)
                h |= mask[row + static_cast<size_t>(w) - 1] >> 63;
            if (w + 1 < words)
                h |= mask[row + static_cast<size_t>(w) + 1] << 63;
            out[row + static_cast<size_t>(w)] |= h;
            if (y > 0)
                out[row - words + static_cast<size_t>(w)] |= v;
            if (y + 1 < res)
                out[row + words + static_cast<size_t>(w)] |= v;
        }
    }
    return out;
}

uint32 PopcountMask(const Vector<uint64>& mask) {
    uint32 n = 0;
    for (uint64 w : mask)
        n += static_cast<uint32>(std::popcount(w));
    return n;
}

// Worst-view symmetric silhouette mismatch of `levelIndices` against the
// prebuilt LOD0 masks, normalized by LOD0's view coverage.
float ComputeSilhouetteLoss(const Mesh& mesh, const Vector<uint32>& levelIndices,
                            const MeshBoundsInfo& bounds, const SilhouetteMasks& lod0) {
    SilhouetteMasks level;
    RasterizeSilhouette(mesh, levelIndices, bounds, level);
    float worst = 0.0f;
    for (int view = 0; view < 3; ++view) {
        const Vector<uint64>& a = lod0.Views[view];
        const Vector<uint64>& b = level.Views[view];
        const Vector<uint64> aDil = DilateMask(a);
        const Vector<uint64> bDil = DilateMask(b);
        uint32 mismatch = 0;
        uint32 covered = 0;
        for (size_t w = 0; w < a.size(); ++w) {
            mismatch += static_cast<uint32>(std::popcount(a[w] & ~bDil[w]));
            mismatch += static_cast<uint32>(std::popcount(b[w] & ~aDil[w]));
            covered += static_cast<uint32>(std::popcount(a[w]));
        }
        worst = std::max(worst, static_cast<float>(mismatch) /
                                    static_cast<float>(std::max(covered, 1u)));
    }
    return worst;
}

// Area fraction of a level's triangles whose corners span more than one UV
// island, weighted by the corners' atlas spread — the residual chart-
// amalgamation measure folded into the achieved error (see
// kChartAmalgamationErrorScale). Islands per BuildUvIslandMap (original
// unwelded indices).
float ComputeChartAmalgamationError(const Mesh& mesh, const UvIslandMap& islands,
                                    const Vector<uint32>& lodIndices) {
    double totalArea = 0.0;
    double weighted = 0.0;
    for (size_t i = 0; i + 2 < lodIndices.size(); i += 3) {
        const uint32 ia = lodIndices[i], ib = lodIndices[i + 1], ic = lodIndices[i + 2];
        const float* p0 = mesh.Vertices[ia].Position;
        const float* p1 = mesh.Vertices[ib].Position;
        const float* p2 = mesh.Vertices[ic].Position;
        const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const double cx = e1[1] * e2[2] - e1[2] * e2[1];
        const double cy = e1[2] * e2[0] - e1[0] * e2[2];
        const double cz = e1[0] * e2[1] - e1[1] * e2[0];
        const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        totalArea += area;
        const uint32 isA = islands.VertexIsland[ia];
        const uint32 isB = islands.VertexIsland[ib];
        const uint32 isC = islands.VertexIsland[ic];
        if (isA == isB && isA == isC)
            continue;
        const auto uvDist = [&mesh](uint32 a, uint32 b) {
            const float du = mesh.Vertices[a].TexCoords[0] - mesh.Vertices[b].TexCoords[0];
            const float dv = mesh.Vertices[a].TexCoords[1] - mesh.Vertices[b].TexCoords[1];
            return std::sqrt(du * du + dv * dv);
        };
        const float spread =
            std::max({uvDist(ia, ib), uvDist(ia, ic), uvDist(ib, ic)});
        weighted += area * spread;
    }
    return totalArea > 0.0 ? static_cast<float>(weighted / totalArea) : 0.0f;
}
#endif // GE_HAVE_MESHOPTIMIZER

} // namespace

float ComputeChartAmalgamationErrorForTest(const Mesh& mesh,
                                           const Vector<uint32>& levelIndices) {
#if defined(GE_HAVE_MESHOPTIMIZER)
    const UvIslandMap islands = BuildUvIslandMap(mesh);
    return ComputeChartAmalgamationError(mesh, islands, levelIndices);
#else
    (void)mesh;
    (void)levelIndices;
    return 0.0f;
#endif
}

float ComputeSilhouetteLossForTest(const Mesh& mesh, const Vector<uint32>& levelIndices) {
#if defined(GE_HAVE_MESHOPTIMIZER)
    const MeshBoundsInfo bounds = ComputeMeshBounds(mesh);
    SilhouetteMasks lod0;
    RasterizeSilhouette(mesh, mesh.Indices, bounds, lod0);
    return ComputeSilhouetteLoss(mesh, levelIndices, bounds, lod0);
#else
    (void)mesh;
    (void)levelIndices;
    return 0.0f;
#endif
}

void RepointCornersForTest(const Mesh& mesh, Vector<uint32>& indices) {
#if defined(GE_HAVE_MESHOPTIMIZER)
    const MeshBoundsInfo bounds = ComputeMeshBounds(mesh);
    const Vector<float> snapped = BuildSnappedPositions(mesh, bounds.Extent);
    const ColocatedWedges groups = BuildColocatedWedges(snapped, mesh.Vertices.size());
    RepointCornersToBestMatchingWedge(mesh, groups, indices);
#else
    (void)mesh;
    (void)indices;
#endif
}

bool BuildSloppyShellForTest(const Mesh& mesh, const Vector<uint32>& sloppyIndices,
                             Vector<uint32>& outLocalIndices, Vector<Vertex>& outVertices) {
    const UvIslandMap islands = BuildUvIslandMap(mesh);
    ShellBuildOutcome shell = BuildAttributeHonestShell(
        mesh, islands, sloppyIndices.data(), sloppyIndices.size(),
        ComputeMeshBounds(mesh).Extent);
    if (!shell.Accepted)
        return false;
    outLocalIndices = std::move(shell.LocalIndices);
    outVertices = std::move(shell.Vertices);
    return true;
}

MeshLODs GenerateMeshLODs(const Mesh& mesh, const MeshLODConfig& config) {
    MeshLODs result;

    // LOD0 is always the source index buffer (and always shares its vertices).
    result.LodIndices.push_back(mesh.Indices);
    result.LodErrors.push_back(0.0f);
    result.LodSloppy.push_back(0u);
    result.LodVertices.emplace_back();

    const size_t srcIndexCount = mesh.Indices.size();
    const size_t vertexCount   = mesh.Vertices.size();

    // Bail to LOD0-only for cases meshopt_simplify can't handle.
    const bool generatable =
        mesh.PrimitiveTopology == MeshPrimitiveTopology::Triangles &&
        srcIndexCount >= 3 && vertexCount > 0 && config.LodCount > 1;

#if defined(GE_HAVE_MESHOPTIMIZER)
    if (!generatable) {
        return result;
    }

    // Vertex-cache optimize LOD0 in place (same vertices, better fetch order).
    meshopt_optimizeVertexCache(result.LodIndices[0].data(), result.LodIndices[0].data(),
                                srcIndexCount, vertexCount);

    // Position-snap the stream the simplifier collapses over. meshopt groups
    // vertices by EXACT position bytes for its internal topology weld, so UV/
    // normal seam duplicates (one position, split attributes) act as one
    // logical vertex — no seam tear — while the OUTPUT still references the
    // original per-wedge indices, so split normals and palette-atlas UV seams
    // survive every quality level.
    //
    // Procedural content (spheres, cylinders, lathe surfaces) computes the two
    // seam columns independently, so their positions are NEAR-equal, not
    // bit-equal (e.g. a UV sphere's seam z is 0 on one side, ~1e-8 on the
    // other) and meshopt's exact-byte weld misses them. Snapping to a coarse
    // grid (well below any real feature, far above the trig round-off) makes
    // the twins bit-exact so the internal weld closes them. Deliberately NOT
    // the old meshopt_generateShadowIndexBuffer pre-weld: rewriting indices to
    // position representatives flattened every co-located seam's attributes —
    // one normal per hard edge, one palette UV per seam — which is exactly the
    // damage class the attribute-aware mid tier exists to avoid. The error
    // metric runs over snapped positions (grid is 1e-5 of extent — noise).
    // Genuinely-open borders (a plane's outer edge) have no position twin to
    // snap onto, so they stay borders and the BorderRule classification still
    // sees them.
    const MeshBoundsInfo meshBounds = ComputeMeshBounds(mesh);
    const float meshExtent = meshBounds.Extent;
    const Vector<float> simplifyPositions = BuildSnappedPositions(mesh, meshExtent);

    const uint32 lodCount = std::min<uint32>(config.LodCount, MeshLODConfig::kMaxLODs);

    // Per-vertex flags: seam-plane border locks (SeamPlanes rule) + chromatic
    // chart-boundary Protect (every rule). LockAll adds the global option bit.
    const Vector<uint8> vertexFlags =
        BuildVertexSimplifyFlags(mesh, simplifyPositions, meshBounds, config.BorderRule);
    const unsigned char* vertexLocks =
        vertexFlags.empty() ? nullptr
                            : reinterpret_cast<const unsigned char*>(vertexFlags.data());
    const unsigned int borderOption =
        config.BorderRule == MeshLODBorderRule::LockAll ? meshopt_SimplifyLockBorder : 0u;
    const bool anyHardLock =
        borderOption != 0u ||
        std::any_of(vertexFlags.begin(), vertexFlags.end(),
                    [](uint8 f) { return (f & meshopt_SimplifyVertex_Lock) != 0u; });

    // Attribute stream for the attribute-aware simplifier: shading normal +
    // UV0 per vertex, weighted per config. The simplifier reuses existing
    // vertices (it never rewrites attributes), so the weights only steer WHICH
    // collapses are chosen — retained vertices keep their exact source
    // attributes, and the attribute deviation folds into the achieved error.
    Vector<float> attributes(vertexCount * 5);
    for (size_t v = 0; v < vertexCount; ++v) {
        const Vertex& vert = mesh.Vertices[v];
        float* dst = &attributes[v * 5];
        dst[0] = vert.Normal[0];
        dst[1] = vert.Normal[1];
        dst[2] = vert.Normal[2];
        dst[3] = vert.TexCoords[0];
        dst[4] = vert.TexCoords[1];
    }
    const float attributeWeights[5] = {config.NormalWeight, config.NormalWeight,
                                       config.NormalWeight, config.UvWeight,
                                       config.UvWeight};

    // Scratch buffers reused across levels; meshopt writes at most srcIndexCount.
    // The sloppy fallback gets its OWN scratch: writing it over `simplified`
    // clobbered the non-sloppy result, so a rejected sloppy attempt (worse than
    // the topology-limited result, or < 1 triangle) emitted a level whose first
    // indices were sloppy output and whose tail was stale simplify output —
    // valid indices, garbage triangles.
    Vector<uint32> simplified(srcIndexCount);
    Vector<uint32> sloppyScratch;
    size_t prevIndexCount = srcIndexCount;

    // Attribute-honest shells need per-vertex UV/attribute context; a mesh with
    // optional parallel streams cannot carry them per-LOD yet (C3), so its far
    // levels simply skip instead of shipping a corrupt or LOD0-only shell.
    const bool canBuildShell = CanBuildOwnVertexShell(mesh);
    std::optional<UvIslandMap> islands;            // built lazily on first consumer
    std::optional<SilhouetteMasks> lod0Silhouette; // built lazily on first quality level
    std::optional<ColocatedWedges> colocated;      // built lazily on first quality level

    // Coarse levels (ratio <= kPruneRatioCeiling) simplify over a pruned copy
    // of the source: one standalone meshopt_simplifyPrune pass at the tight
    // kPruneErrorBudget, built lazily. Decoupled from the level budgets so a
    // generous far TargetError can never widen pruning's reach (v6 deleted
    // 0.34-extent garlands that way). Near levels always keep every component.
    Vector<uint32> prunedIndices;
    bool prunedBuilt = false;

    // A cancelled load must stop simplifying instead of running to completion.
    // One meshopt_simplify* call is opaque (no progress or cancellation callback,
    // exactly like DirectX::Compress in the texture cook), so a level boundary is
    // the granularity available: levels simplify independently from the source,
    // so stopping between them leaves the levels already built intact.
    //
    // The result is then SHORT, not wrong — which only stays safe because every
    // consumer that persists a chain re-reads the flag before adopting it (see
    // ModelAsset::LoadOrGenerateLODs, which skips the cache write).
    const std::shared_ptr<std::atomic<bool>>& cancelRequested = CurrentAssetDecodeCancellation();

    for (uint32 lod = 1; lod < lodCount; ++lod) {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire))
            break;

        const float ratio = std::clamp(config.TargetRatios[lod], 0.0f, 1.0f);
        size_t targetIndexCount = RoundToTriangles(static_cast<size_t>(srcIndexCount * ratio));
        targetIndexCount = std::max<size_t>(targetIndexCount, 3);

        // Nothing to gain if the previous level already met this budget — but a
        // coarser level's smaller budget may still be attainable, so skip, not
        // stop (levels are simplified independently from the source).
        if (targetIndexCount >= prevIndexCount) {
            continue;
        }

        const uint32* levelSource = mesh.Indices.data();
        size_t levelSourceCount = srcIndexCount;
        if (ratio <= kPruneRatioCeiling) {
            if (!prunedBuilt) {
                prunedBuilt = true;
                prunedIndices.resize(srcIndexCount);
                const size_t prunedCount = meshopt_simplifyPrune(
                    prunedIndices.data(), mesh.Indices.data(), srcIndexCount,
                    simplifyPositions.data(), vertexCount, sizeof(float) * 3,
                    kPruneErrorBudget);
                // A degenerate prune (everything was floater-sized) keeps the
                // full source — pruning must reduce, never erase the mesh.
                if (prunedCount >= 3 && prunedCount < srcIndexCount)
                    prunedIndices.resize(prunedCount);
                else
                    prunedIndices.clear();
            }
            if (!prunedIndices.empty()) {
                levelSource = prunedIndices.data();
                levelSourceCount = prunedIndices.size();
            }
        }

        // The budget above is a fraction of the ORIGINAL mesh, but this level's
        // simplify INPUT is whatever the prune left. On floater-dominated
        // content (cloud rings, shelf clutter) the shed micro components carry
        // most of the triangles, so the pruned source can be smaller than the
        // budget — meshopt asserts target_index_count <= index_count and aborts
        // the cook. Such a level has already MET its budget: ask for no more
        // than its own source supplies.
        //
        // Clamping down is only safe while the level source itself holds a
        // triangle: a source below 3 would drag the budget under the floor set
        // above and hand meshopt the same out-of-range target from the other
        // direction. Both entry paths guarantee it (the generatable gate for
        // the full source, the degenerate-prune guard for the pruned one), so
        // this asserts rather than silently re-floors.
        assert(levelSourceCount >= 3 && "level simplify source must hold a triangle");
        targetIndexCount = std::min(targetIndexCount, levelSourceCount);

        // Permissive is LOAD-BEARING for kit content: flat-shaded meshes
        // (rocks, trees, cliffs — per-face normals) give every vertex 3+
        // attribute wedges, which classic meshopt classifies complex and
        // locks outright — the whole mesh then refuses to simplify (measured
        // on the gate corpus: zero quality levels on any faceted mesh while
        // smooth-shaded props reduced fine). Permissive lets collapses cross
        // attribute discontinuities; the attribute quadrics still charge for
        // the damage, that cost lands in the achieved error (delaying
        // engagement through the threshold mapping), and chromatic chart
        // boundaries are exempted outright via the Vertex_Protect flags.
        const unsigned int options = borderOption | meshopt_SimplifyPermissive;

        float lodError = 0.0f;
        size_t newCount = meshopt_simplifyWithAttributes(
            simplified.data(), levelSource, levelSourceCount,
            simplifyPositions.data(), vertexCount, sizeof(float) * 3,
            attributes.data(), sizeof(float) * 5, attributeWeights, 5,
            vertexLocks, targetIndexCount, config.TargetError[lod], options, &lodError);

        // Topology can stop meshopt_simplify well short of the budget. For the
        // most aggressive levels, fall back to the sloppy simplifier, which
        // ignores topology and reliably hits the target triangle count. Sloppy
        // output is position-only over original indices — attribute-corrupt by
        // construction — so an accepted sloppy level is ALWAYS rebuilt into an
        // attribute-honest own-vertex shell; only an all-degenerate shell is
        // refused. A refused shell falls back to the topology-limited result
        // below; a level that still cannot reduce skips.
        if (config.AllowSloppy && ratio <= config.SloppyRatioThreshold &&
            newCount > targetIndexCount + targetIndexCount / 2) {
            if (!canBuildShell) {
                // C3 gap: optional streams cannot ride an own-vertex shell yet;
                // the level skips, counted so the cook log surfaces the
                // forfeited far savings.
                ++result.SloppyLevelsSkippedForStreams;
            } else {
                float sloppyError = 0.0f;
                sloppyScratch.resize(levelSourceCount);
                size_t sloppyCount = meshopt_simplifySloppy(
                    sloppyScratch.data(), levelSource, levelSourceCount,
                    simplifyPositions.data(), vertexCount, sizeof(float) * 3,
                    targetIndexCount, /*target_error*/ 1.0f, &sloppyError);
                if (sloppyCount >= 3 && sloppyCount < newCount &&
                    sloppyCount < prevIndexCount) {
                    if (!islands)
                        islands = BuildUvIslandMap(mesh);
                    ShellBuildOutcome shell = BuildAttributeHonestShell(
                        mesh, *islands, sloppyScratch.data(), sloppyCount, meshExtent);
                    if (shell.Accepted &&
                        static_cast<float>(shell.LocalIndices.size()) <=
                            static_cast<float>(prevIndexCount) * kMinLevelReduction) {
                        prevIndexCount = shell.LocalIndices.size();
                        result.LodIndices.push_back(std::move(shell.LocalIndices));
                        result.LodErrors.push_back(sloppyError);
                        result.LodSloppy.push_back(1u);
                        result.LodVertices.push_back(std::move(shell.Vertices));
                        continue;
                    }
                    ++result.SloppyShellsRejected;
                }
            }
        }

        // A level that could not meaningfully reduce is not emitted — it must
        // earn its chain slot (kMinLevelReduction). Deliberately NO lock-
        // dropping fallback here: a level that cannot reduce under its border
        // rule simply skips — locked seam-plane borders never move at any
        // non-sloppy level, so cross-piece kit seams stay watertight whatever
        // LOD each neighbour selected. Skip to the coarser levels instead of
        // truncating the chain: they carry progressively larger error budgets,
        // and the far level keeps the sloppy fallback above, so stubborn
        // shells retain their distance savings without ever emitting a torn
        // seam.
        if (newCount == 0 ||
            static_cast<float>(newCount) >
                static_cast<float>(prevIndexCount) * kMinLevelReduction) {
            if (anyHardLock)
                ++result.LevelsSkippedWithLocks;
            continue;
        }

        Vector<uint32> lodIndices(simplified.begin(), simplified.begin() + newCount);

        // Re-point every corner to the co-located wedge whose normal matches
        // the emitted triangle's own plane, before anything measures the level:
        // it changes which wedge each corner references, so both tripwires must
        // see the indices that actually ship.
        if (!colocated)
            colocated = BuildColocatedWedges(simplifyPositions, vertexCount);
        RepointCornersToBestMatchingWedge(mesh, *colocated, lodIndices);

        // Residual chart amalgamation folds into the achieved error so a level
        // that still interpolates across distant atlas regions (whatever the
        // Protect flags could not prevent) engages proportionally further away
        // instead of showing wrong palette colors at mid distances.
        if (!islands)
            islands = BuildUvIslandMap(mesh);
        const float amalgamation =
            ComputeChartAmalgamationError(mesh, *islands, lodIndices);
        lodError = std::max(lodError, kChartAmalgamationErrorScale * amalgamation);

        // Silhouette honesty: shape damage the quadric under-priced (vanished
        // sheets, destroyed icon shapes) self-delays the level's engagement.
        if (!lod0Silhouette) {
            lod0Silhouette.emplace();
            RasterizeSilhouette(mesh, result.LodIndices[0], meshBounds, *lod0Silhouette);
        }
        const float silhouetteLoss =
            ComputeSilhouetteLoss(mesh, lodIndices, meshBounds, *lod0Silhouette);
        lodError = std::max(lodError, kSilhouetteErrorScale * silhouetteLoss);

        meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(), newCount, vertexCount);

        result.LodIndices.push_back(std::move(lodIndices));
        result.LodErrors.push_back(lodError);
        result.LodSloppy.push_back(0u);
        result.LodVertices.emplace_back();
        prevIndexCount = newCount;
    }
#else
    (void)config;
    (void)generatable;
#endif

    return result;
}

uint32 GenerateMeshLODsInto(Mesh& mesh, const MeshLODConfig& config,
                            MeshLODGenStats* outStats) {
    MeshLODs lods = GenerateMeshLODs(mesh, config);
    mesh.ExtraLODs.clear();
    mesh.ExtraLODErrors.clear();
    mesh.ExtraLODSloppy.clear();
    mesh.ExtraLODVertices.clear();
    mesh.ExtraLODColor0.clear();
    // ExtraLODs[j] = LOD(j+1); its error/sloppy/vertex-block live at slot j+1 of
    // the MeshLODs arrays (LOD0 occupies slot 0). Keep the four arrays the same
    // size.
    bool anyOwnVertexLevel = false;
    for (uint32 lod = 1; lod < lods.LodCount(); ++lod) {
        mesh.ExtraLODs.push_back(std::move(lods.LodIndices[lod]));
        mesh.ExtraLODErrors.push_back(lods.LodErrors[lod]);
        mesh.ExtraLODSloppy.push_back(lods.LodSloppy[lod]);
        anyOwnVertexLevel |= !lods.LodVertices[lod].empty();
        mesh.ExtraLODVertices.push_back(std::move(lods.LodVertices[lod]));
    }
    // Normalize an all-index-only chain to an empty block array: "no blocks" and
    // "N empty blocks" are semantically identical, and the canonical form keeps
    // the GPU content hash and the cook bytes stable across the generate and
    // cache-hit paths.
    if (!anyOwnVertexLevel)
        mesh.ExtraLODVertices.clear();
    if (outStats) {
        outStats->ShellsRejected += lods.SloppyShellsRejected;
        outStats->ShellsSkippedForStreams += lods.SloppyLevelsSkippedForStreams;
        outStats->LevelsSkippedWithLocks += lods.LevelsSkippedWithLocks;
    }
    return mesh.LODCount();
}

bool ShouldGenerateLODsForMesh(const Mesh& mesh, bool generateSkinned) {
    return generateSkinned || !mesh.IsSkinned();
}

bool IsMeshLODGenerationAvailable() {
#if defined(GE_HAVE_MESHOPTIMIZER)
    return true;
#else
    return false;
#endif
}

namespace {
LODImportSettings& MutableLODImportSettings() {
    static LODImportSettings s_Settings;
    return s_Settings;
}
} // namespace

void SetLODImportSettings(const LODImportSettings& settings) {
    MutableLODImportSettings() = settings;
}

const LODImportSettings& GetLODImportSettings() {
    return MutableLODImportSettings();
}

} // namespace GameEngine
