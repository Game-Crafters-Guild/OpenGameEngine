#include "Assets/HlodBenefit.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace GameEngine {
namespace Hlod {

namespace {

// benefit/ΔVB, with the zero-VB edge folded to a signed infinity so an empty
// (degenerate) cluster never divides by zero yet still sorts by benefit sign.
double AdmissionScore(int64 benefit, uint64 vbBytes) {
    if (vbBytes == 0)
        return benefit > 0 ? std::numeric_limits<double>::infinity()
                           : -std::numeric_limits<double>::infinity();
    return static_cast<double>(benefit) / static_cast<double>(vbBytes);
}

} // namespace

void EvaluateClusterBenefit(const ClusterTable& table,
                            std::span<const MemberInput> members,
                            const GridConfig& config,
                            std::vector<ClusterBenefit>& outBenefits) {
    outBenefits.assign(table.Clusters.size(), ClusterBenefit{});

    // First pass: per-cluster accounting + the two hard gates.
    for (size_t ci = 0; ci < table.Clusters.size(); ++ci) {
        const Cluster& cluster = table.Clusters[ci];
        ClusterBenefit& b = outBenefits[ci];

        std::unordered_set<uint64> distinctMeshes;
        std::unordered_set<GUID>   distinctMaterials;
        uint64 vbBytes = 0;
        for (uint32 mi : cluster.MemberIndices) {
            const MemberInput& m = members[mi];
            distinctMeshes.insert(m.MeshContentHash);
            distinctMaterials.insert(m.MaterialGuid);
            vbBytes += static_cast<uint64>(m.VertexCount) * kHlodCoreVertexStrideBytes;
            if (m.CastsShadow)
                ++b.CastingMembers;
        }

        b.MemberCount = static_cast<uint32>(cluster.MemberIndices.size());
        b.DistinctMeshes = static_cast<uint32>(distinctMeshes.size());
        b.SubmeshCount = static_cast<uint32>(distinctMaterials.size());
        b.EstimatedVbBytes = vbBytes;
        b.InstancingRatio = b.DistinctMeshes > 0
            ? static_cast<float>(b.MemberCount) / static_cast<float>(b.DistinctMeshes)
            : static_cast<float>(b.MemberCount);
        // recordsSaved = members that stop emitting (N) minus the proxy's own
        // submesh rows (M); castersSaved similarly over shadow-casting members.
        const int64 recordsSaved = static_cast<int64>(b.MemberCount) - static_cast<int64>(b.SubmeshCount);
        const int64 castersSaved = static_cast<int64>(b.CastingMembers) - static_cast<int64>(b.SubmeshCount);
        b.Benefit = recordsSaved + castersSaved;

        if (b.MemberCount < config.MinMembers) {
            b.Disposition = ClusterDisposition::TooFewMembers;
            continue;
        }
        if (b.InstancingRatio > config.MaxInstancingRatio) {
            b.Disposition = ClusterDisposition::HighInstancing;
            continue;
        }
        // Passed both gates; provisionally a budget candidate.
        b.Disposition = ClusterDisposition::OverBudget;
    }

    // Second pass: greedy VB-budget admission over the gate survivors, best
    // benefit/ΔVB first (ties by ascending cluster index for determinism).
    std::vector<size_t> candidates;
    for (size_t ci = 0; ci < outBenefits.size(); ++ci)
        if (outBenefits[ci].Disposition == ClusterDisposition::OverBudget)
            candidates.push_back(ci);

    std::stable_sort(candidates.begin(), candidates.end(), [&](size_t a, size_t c) {
        const double sa = AdmissionScore(outBenefits[a].Benefit, outBenefits[a].EstimatedVbBytes);
        const double sc = AdmissionScore(outBenefits[c].Benefit, outBenefits[c].EstimatedVbBytes);
        if (sa != sc)
            return sa > sc;
        return a < c;
    });

    uint64 spent = 0;
    for (size_t ci : candidates) {
        const uint64 cost = outBenefits[ci].EstimatedVbBytes;
        if (spent + cost <= config.VbBudgetBytes) {
            outBenefits[ci].Disposition = ClusterDisposition::Admitted;
            spent += cost;
        }
        // else: stays OverBudget. Continue scanning so a later, cheaper cluster
        // can still slot under the remaining budget (greedy, not first-fit-stop).
    }
}

} // namespace Hlod
} // namespace GameEngine
