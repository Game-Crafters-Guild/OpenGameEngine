#include "Assets/HlodReconcile.h"

namespace GameEngine {
namespace Hlod {

namespace {

// Cluster-relative transform, matching HlodBaker::ClusterRelative: subtract the
// cluster origin from the (column-major) translation column so the live source
// hash is computed over the same values the bake stored.
BakedMemberRef ClusterRelativeRef(const LiveMemberState& live, const float origin[3]) {
    BakedMemberRef ref = live.Resolved;
    ref.Transform[12] -= origin[0];
    ref.Transform[13] -= origin[1];
    ref.Transform[14] -= origin[2];
    return ref;
}

} // namespace

ReconcileResult ReconcileBakedScene(const HlodBakedScene& baked, const LiveMemberIndex& live) {
    ReconcileResult result;
    result.Clusters.reserve(baked.Clusters.size());

    for (size_t ci = 0; ci < baked.Clusters.size(); ++ci) {
        const BakedCluster& bc = baked.Clusters[ci];

        ReconciledCluster rc;
        rc.ClusterId = static_cast<uint32>(ci);
        for (int c = 0; c < 3; ++c)
            rc.SphereCenter[c] = bc.SphereCenter[c];
        rc.SphereRadius = bc.SphereRadius;

        // Resolve members in the BAKED order so the recomputed source hash lines
        // up field-for-field with ComputeClusterSourceHash(bc.Members).
        std::vector<BakedMemberRef> liveRefs;
        liveRefs.reserve(bc.Members.size());
        for (const BakedMemberRef& baked_m : bc.Members) {
            auto it = live.find(baked_m.StableId);
            if (it == live.end()) {
                ++rc.MissingMembers;
                continue;
            }
            rc.Members.push_back(it->second.Entity);
            liveRefs.push_back(ClusterRelativeRef(it->second, bc.SphereCenter));
        }

        // Stale when a member vanished (proxy no longer covers the live scene) or
        // when the live-recomputed cluster source hash disagrees with the baked
        // one (C3: a material/mesh reassignment or a member move perturbs it).
        bool hashMismatch = false;
        if (rc.MissingMembers == 0)
            hashMismatch = ComputeClusterSourceHash(liveRefs) != bc.ClusterSourceHash;
        rc.Stale = rc.MissingMembers > 0 || hashMismatch;

        result.TotalMissingMembers += rc.MissingMembers;
        if (rc.Stale)
            ++result.StaleClusters;
        result.Clusters.push_back(std::move(rc));
    }

    return result;
}

} // namespace Hlod
} // namespace GameEngine
