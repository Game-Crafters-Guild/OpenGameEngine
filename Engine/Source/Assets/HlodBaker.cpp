#include "Assets/HlodBaker.h"

#include "Assets/HlodMeshMerge.h"
#include "Assets/ModelAsset.h"
#include "Logger/Logger.h"

#include <vector>

namespace GameEngine {
namespace Hlod {

namespace {

// Cluster-relative world matrix: subtract the cluster origin from the (column-
// major) translation column. Left-multiplication by a pure translation shifts
// only cols 12..14, preserving rotation/scale — the ComposeEffectiveWorldTransform
// identity. Keeps baked vertices small-magnitude around the proxy origin,
// preserving float precision + composing with camera-relative rendering.
void ClusterRelative(const float world[16], const float origin[3], float out[16]) {
    for (int i = 0; i < 16; ++i)
        out[i] = world[i];
    out[12] -= origin[0];
    out[13] -= origin[1];
    out[14] -= origin[2];
}

} // namespace

HlodBakedScene BakeScene(std::span<const MemberInput> members,
                         std::span<const Mesh* const> memberGeometry,
                         const GridConfig& config,
                         BakeStats* outStats) {
    HlodBakedScene scene;
    scene.CellSize = config.CellSize;
    scene.GridOrigin[0] = config.GridOrigin[0];
    scene.GridOrigin[1] = config.GridOrigin[1];
    scene.GridOrigin[2] = config.GridOrigin[2];

    const ClusterTable table = BuildClusters(members, config);
    std::vector<ClusterBenefit> benefits;
    EvaluateClusterBenefit(table, members, config, benefits);

    BakeStats stats;
    stats.ClusterCount = static_cast<uint32>(table.Clusters.size());
    stats.OversizedMembers = static_cast<uint32>(table.OversizedMembers.size());

    uint64 sceneSourceHash = 0;
    for (size_t ci = 0; ci < table.Clusters.size(); ++ci) {
        switch (benefits[ci].Disposition) {
            case ClusterDisposition::TooFewMembers:      ++stats.SkippedTooFew; continue;
            case ClusterDisposition::HighInstancing:     ++stats.SkippedHighInstancing; continue;
            case ClusterDisposition::OverBudget:         ++stats.SkippedOverBudget; continue;
            case ClusterDisposition::Admitted:           break;
        }

        const Cluster& cluster = table.Clusters[ci];

        BakedCluster baked;
        baked.Cell = cluster.Cell;
        for (int c = 0; c < 3; ++c) {
            baked.SphereCenter[c] = cluster.SphereCenter[c];
            baked.BoundsMin[c] = cluster.BoundsMin[c];
            baked.BoundsMax[c] = cluster.BoundsMax[c];
        }
        baked.SphereRadius = cluster.SphereRadius;

        // Member records (cluster-relative transforms — the C3 key inputs) +
        // the parallel merge inputs.
        std::vector<MergeMember> mergeMembers;
        mergeMembers.reserve(cluster.MemberIndices.size());
        baked.Members.reserve(cluster.MemberIndices.size());
        for (uint32 mi : cluster.MemberIndices) {
            const MemberInput& m = members[mi];

            BakedMemberRef ref;
            ref.StableId = m.StableId;
            ref.MeshContentHash = m.MeshContentHash;
            ref.MaterialGuid = m.MaterialGuid;
            ref.MeshHandleKey = m.MeshHandleKey;
            ClusterRelative(m.WorldMatrix, cluster.SphereCenter, ref.Transform);
            ref.ChosenLod = m.ChosenLod;
            ref.CastsShadow = m.CastsShadow ? 1u : 0u;
            baked.Members.push_back(ref);

            const Mesh* geometry = mi < memberGeometry.size() ? memberGeometry[mi] : nullptr;
            if (geometry != nullptr) {
                MergeMember mm;
                mm.Source = geometry;
                mm.MaterialGuid = m.MaterialGuid;
                mm.ChosenLod = m.ChosenLod;
                ClusterRelative(m.WorldMatrix, cluster.SphereCenter, mm.Transform);
                mergeMembers.push_back(mm);
            }
        }

        const MergedProxy merged = MergeClusterMembers(mergeMembers);
        if (merged.Submeshes.empty()) {
            // A refused merge must keep the originals resident. Publishing a
            // members-only cluster would let EnterProxy evict them with no
            // replacement geometry, and must not enter the admitted hash.
            Logger::Log::Warning("HLOD: cluster merge produced no proxy geometry; retaining original members.");
            ++stats.SkippedEmptyMerge;
            continue;
        }

        baked.ClusterSourceHash = ComputeClusterSourceHash(baked.Members);
        // Order-dependent combine over the admitted clusters (deterministic cell
        // order from BuildClusters), robust against the XOR-cancellation two
        // equal cluster hashes would cause.
        sceneSourceHash ^= baked.ClusterSourceHash + 0x9e3779b97f4a7c15ull +
                           (sceneSourceHash << 6) + (sceneSourceHash >> 2);

        baked.Submeshes.reserve(merged.Submeshes.size());
        for (size_t s = 0; s < merged.Submeshes.size(); ++s) {
            const Mesh& sm = merged.Submeshes[s];
            BakedSubmesh out;
            out.MaterialGuid = merged.SubmeshMaterials[s];
            out.Vertices = sm.Vertices;
            out.Indices = sm.Indices;
            if (sm.HasColor0())
                out.Color0 = sm.Color0;
            if (sm.HasTexCoords1())
                out.TexCoords1 = sm.TexCoords1;
            baked.Submeshes.push_back(std::move(out));
        }
        stats.ProxyVertexBytes += merged.CoreVertexBytes;

        scene.Clusters.push_back(std::move(baked));
        ++stats.AdmittedCount;
    }

    scene.Key.ConfigHash = ComputeHlodConfigHash(config);
    scene.Key.SourceHash = sceneSourceHash;

    if (outStats)
        *outStats = stats;
    return scene;
}

} // namespace Hlod
} // namespace GameEngine
