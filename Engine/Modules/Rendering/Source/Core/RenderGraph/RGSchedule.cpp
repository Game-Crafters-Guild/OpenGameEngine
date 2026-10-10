#include "Rendering/Core/RenderGraph/RGGraph.h"

#include "Logger/Logger.h"
#include "Rendering/Core/RenderGraph/RGScratch.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <tuple>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

void RGGraph::Schedule()
{
    const uint32_t passCount = static_cast<uint32_t>(m_Passes.size());
    const uint32_t resCount = static_cast<uint32_t>(m_Resources.size());

    m_ScheduledOrder.clear();
    m_ScheduledOrder.reserve(passCount);
    m_Level.assign(passCount, 0u);
    m_WorkingSetId.assign(passCount, kInvalidId);
    m_WorkingSetTransitions = 0;

    auto live = [&](RGPassId p) { return !m_Passes[p].Culled; };

    // ── Per-pass attachment working-set (sorted, unique). Empty == neutral. ──
    auto& attSet = m_ScratchAttSet;
    PrepareNested(attSet, passCount);
    for (const RGAccessRecord& a : m_Accesses)
    {
        if (live(a.Pass) && IsAttachment(a.Access))
            attSet[a.Pass].push_back(a.Resource);
    }
    for (RGPassId p = 0; p < passCount; ++p)
    {
        auto& s = attSet[p];
        std::sort(s.begin(), s.end());
        s.erase(std::unique(s.begin(), s.end()), s.end());
    }
    // Intern working-set ids (small N -> linear interning; logical count local,
    // member storage retains capacity).
    auto& sets = m_ScratchSets;
    uint32_t setCount = 0;
    auto acquireSet = [&](const std::vector<RGResourceId>& src) -> uint32_t
    {
        if (setCount == sets.size())
            sets.emplace_back();
        sets[setCount].assign(src.begin(), src.end());
        return setCount++;
    };
    for (RGPassId p = 0; p < passCount; ++p)
    {
        if (!live(p) || attSet[p].empty())
            continue;
        uint32_t id = kInvalidId;
        for (uint32_t i = 0; i < setCount; ++i)
        {
            if (sets[i] == attSet[p]) { id = i; break; }
        }
        if (id == kInvalidId)
            id = acquireSet(attSet[p]);
        m_WorkingSetId[p] = id;
    }

    // ── Hazard edges among live passes (RAW/WAR/WAW), derived from accesses in
    //    RECORDING order: writer-before-reader gives RAW, reader-before-writer
    //    gives WAR, writer-before-writer gives WAW. Within a frame the recording
    //    order is the declaration order, so the public AddPass builder must
    //    document that Read/Write call order is meaningful for same-resource
    //    redeclarations. Resource-granular (conservative for subresource ranges). ──
    auto& succ = m_ScratchSucc;
    PrepareNested(succ, passCount);
    auto& indeg = m_ScratchIndeg;
    indeg.assign(passCount, 0u);
    auto addEdge = [&](RGPassId from, RGPassId to)
    {
        if (from == to)
            return;
        for (RGPassId s : succ[from])
            if (s == to)
                return; // dedup
        succ[from].push_back(to);
        ++indeg[to];
    };
    {
        auto& lastWriter = m_ScratchLastWriter;
        lastWriter.assign(resCount, kInvalidId);
        auto& readersSince = m_ScratchReadersSince;
        PrepareNested(readersSince, resCount);
        for (const RGAccessRecord& a : m_Accesses)
        {
            if (!live(a.Pass))
                continue;
            const RGResourceId r = a.Resource;
            if (IsWrite(a.Access))
            {
                for (RGPassId rd : readersSince[r])
                    addEdge(rd, a.Pass); // WAR
                if (lastWriter[r] != kInvalidId)
                    addEdge(lastWriter[r], a.Pass); // WAW
                lastWriter[r] = a.Pass;
                readersSince[r].clear();
            }
            else
            {
                if (lastWriter[r] != kInvalidId)
                    addEdge(lastWriter[r], a.Pass); // RAW
                readersSince[r].push_back(a.Pass);
            }
        }
    }

    // ── Explicit ordering edges (AddOrderingEdge), replayed after the
    //    access-derived ones. Edges touching culled passes are dropped —
    //    ordering is a scheduling constraint, not a lifetime one. Dedup,
    //    levels, the transitive closure, component union, and cycle culling
    //    all treat them exactly like hazard edges. ──
    for (const auto& [before, after] : m_OrderingEdges)
    {
        if (before < passCount && after < passCount && live(before) && live(after))
            addEdge(before, after);
    }

    uint32_t liveCount = 0;
    for (RGPassId p = 0; p < passCount; ++p)
        if (live(p))
            ++liveCount;

    // ── Dependency levels (longest path) + a Kahn topological order ──
    auto& topo = m_ScratchTopo;
    topo.clear();
    topo.reserve(liveCount);
    auto& indegWork = m_ScratchIndegWork;
    indegWork = indeg;
    {
        for (RGPassId p = 0; p < passCount; ++p)
            if (live(p) && indegWork[p] == 0)
                topo.push_back(p);
        size_t head = 0;
        while (head < topo.size())
        {
            const RGPassId p = topo[head++];
            for (RGPassId s : succ[p])
            {
                m_Level[s] = std::max(m_Level[s], m_Level[p] + 1);
                if (--indegWork[s] == 0)
                    topo.push_back(s);
            }
        }
    }
    if (topo.size() != liveCount)
    {
        // Dependency cycle. Never silently truncate (Release included): name the
        // cyclic passes, cull them with an explicit reason, and continue with the
        // acyclic remainder. Cycle members are exactly the live passes whose
        // working in-degree never reached zero.
        for (RGPassId p = 0; p < passCount; ++p)
        {
            if (live(p) && indegWork[p] != 0)
            {
                Logger::Log::Error("[RenderGraph] dependency cycle: culling pass '{}' (declare the cycle away — "
                                   "split the pass or break the read/write loop)",
                                   m_Passes[p].Desc.Name ? m_Passes[p].Desc.Name : "<unnamed>");
                m_Passes[p].Culled = true;
                m_Passes[p].CullReason = RGCullReason::Cycle;
                --m_LivePassCount;
                --liveCount;
            }
        }
        // No assert: a cycle is a DATA error in the caller's declarations, not an
        // engine invariant — handled loudly (error log + CullReason::Cycle) and
        // survivably in every build, and covered by a unit test.
    }

    // ── Transitive closure: reach[p] = bitset of passes reachable FROM p ──
    const uint32_t words = (passCount + 63) / 64;
    auto& reach = m_ScratchReach;
    reach.assign(static_cast<size_t>(passCount) * words, 0);
    auto rowOf = [&](RGPassId p) { return &reach[static_cast<size_t>(p) * words]; };
    auto setBit = [](uint64_t* row, uint32_t b) { row[b >> 6] |= 1ull << (b & 63); };
    auto testBit = [](const uint64_t* row, uint32_t b) -> bool
    { return (row[b >> 6] >> (b & 63)) & 1ull; };
    for (size_t i = topo.size(); i-- > 0;)
    {
        const RGPassId p = topo[i];
        uint64_t* row = rowOf(p);
        for (RGPassId s : succ[p])
        {
            const uint64_t* srow = rowOf(s);
            for (uint32_t w = 0; w < words; ++w)
                row[w] |= srow[w];
            setBit(row, s);
        }
    }

    // ── Weakly-connected components over hazard edges (union-find) ──
    auto& uf = m_ScratchUnionFind;
    uf.resize(passCount);
    for (uint32_t i = 0; i < passCount; ++i)
        uf[i] = i;
    auto find = [&](uint32_t x)
    {
        while (uf[x] != x)
        {
            uf[x] = uf[uf[x]];
            x = uf[x];
        }
        return x;
    };
    for (RGPassId p = 0; p < passCount; ++p)
        for (RGPassId s : succ[p])
            uf[find(p)] = find(s);
    constexpr auto kMaxPhase = std::numeric_limits<int32_t>::max();
    auto& compRank = m_ScratchCompRank;
    compRank.assign(passCount, {kMaxPhase, std::numeric_limits<uint32_t>::max()});
    for (RGPassId p = 0; p < passCount; ++p)
    {
        if (!live(p))
            continue;
        const uint32_t root = find(p);
        const std::pair<int32_t, uint32_t> cand{m_Passes[p].Desc.Phase, p};
        if (cand < compRank[root])
            compRank[root] = cand;
    }

    // ── Working-set clusters with the contiguity split rule ──
    auto& clusterOf = m_ScratchClusterOf;
    clusterOf.assign(passCount, kInvalidId);
    auto& clusters = m_ScratchClusters;
    uint32_t clusterCount = 0;
    auto acquireCluster = [&]() -> uint32_t
    {
        if (clusterCount == clusters.size())
            clusters.emplace_back();
        clusters[clusterCount].clear();
        return clusterCount++;
    };
    {
        auto& wsMembers = m_ScratchWsMembers;
        PrepareNested(wsMembers, setCount);
        for (RGPassId p = 0; p < passCount; ++p)
            if (live(p) && m_WorkingSetId[p] != kInvalidId)
                wsMembers[m_WorkingSetId[p]].push_back(p);

        auto& unionReach = m_ScratchUnionReach;
        unionReach.assign(words, 0);
        auto& groupMask = m_ScratchGroupMask;
        groupMask.assign(words, 0);
        auto& cur = m_ScratchClusterCur;
        cur.clear();
        auto flush = [&]()
        {
            if (cur.empty())
                return;
            const uint32_t ci = acquireCluster();
            for (RGPassId m : cur)
                clusterOf[m] = ci;
            clusters[ci].assign(cur.begin(), cur.end());
            cur.clear();
            std::fill(unionReach.begin(), unionReach.end(), 0);
        };

        for (uint32_t setId = 0; setId < setCount; ++setId)
        {
            auto& members = wsMembers[setId];
            std::sort(members.begin(), members.end(),
                      [&](RGPassId a, RGPassId b)
                      {
                          if (m_Level[a] != m_Level[b])
                              return m_Level[a] < m_Level[b];
                          return a < b;
                      });
            std::fill(groupMask.begin(), groupMask.end(), 0);
            for (RGPassId m : members)
                setBit(groupMask.data(), m);

            for (RGPassId m : members)
            {
                bool violation = false;
                if (!cur.empty())
                {
                    if (find(m) != find(cur[0]))
                        violation = true; // never cluster across components
                    for (uint32_t z = 0; z < passCount && !violation; ++z)
                    {
                        if (testBit(unionReach.data(), z) && !testBit(groupMask.data(), z) &&
                            testBit(rowOf(z), m))
                            violation = true;
                    }
                }
                if (violation)
                    flush();
                cur.push_back(m);
                const uint64_t* mrow = rowOf(m);
                for (uint32_t w = 0; w < words; ++w)
                    unionReach[w] |= mrow[w];
            }
            flush();
        }
        // Neutral / unclustered passes become singleton clusters.
        for (RGPassId p = 0; p < passCount; ++p)
        {
            if (live(p) && clusterOf[p] == kInvalidId)
            {
                const uint32_t ci = acquireCluster();
                clusterOf[p] = ci;
                clusters[ci].push_back(p);
            }
        }
    }

    // ── Contracted cluster DAG + emission, with false-cycle refinement ──
    // A pass-level ACYCLIC graph can still contract to a cyclic cluster DAG when
    // two clusters mutually reach into each other (symmetric cross dependencies:
    // a1→b2 and b1→a2 make {a1,a2}⇄{b1,b2}). Contiguity is a preference and
    // correctness is not: when the contraction stalls, split every stalled
    // multi-pass cluster into singletons and retry. Terminates — each retry
    // strictly increases the cluster count toward all-singletons, which equals
    // the (acyclic) pass DAG itself.
    using CKey = Detail::ClusterKey;
    for (;;)
    {
        auto& csucc = m_ScratchCSucc;
        PrepareNested(csucc, clusterCount);
        auto& cindeg = m_ScratchCIndeg;
        cindeg.assign(clusterCount, 0);
        for (RGPassId p = 0; p < passCount; ++p)
        {
            if (!live(p))
                continue;
            const uint32_t cu = clusterOf[p];
            for (RGPassId s : succ[p])
            {
                // Hazard edges can point INTO cycle-culled passes (a live
                // predecessor of a declared cycle). Those passes have no
                // cluster — clusterOf[s] == kInvalidId would index the
                // contraction arrays out of bounds.
                if (!live(s))
                    continue;
                const uint32_t cv = clusterOf[s];
                if (cu == cv)
                    continue;
                bool seen = false;
                for (uint32_t e : csucc[cu])
                    if (e == cv)
                    {
                        seen = true;
                        break;
                    }
                if (!seen)
                {
                    csucc[cu].push_back(cv);
                    ++cindeg[cv];
                }
            }
        }

        uint32_t nonEmpty = 0;
        auto& ckey = m_ScratchCKey;
        ckey.assign(clusterCount, CKey{});
        for (uint32_t ci = 0; ci < clusterCount; ++ci)
        {
            if (clusters[ci].empty())
                continue; // husk left behind by an earlier split
            ++nonEmpty;
            int32_t minPhase = kMaxPhase;
            uint32_t minIns = std::numeric_limits<uint32_t>::max();
            for (RGPassId m : clusters[ci])
            {
                minPhase = std::min(minPhase, m_Passes[m].Desc.Phase);
                minIns = std::min(minIns, m);
            }
            const auto rank = compRank[find(clusters[ci][0])];
            ckey[ci] = CKey{rank.first, rank.second, minPhase, minIns};
            std::sort(clusters[ci].begin(), clusters[ci].end(),
                      [&](RGPassId a, RGPassId b)
                      {
                          if (m_Level[a] != m_Level[b])
                              return m_Level[a] < m_Level[b];
                          if (m_Passes[a].Desc.Phase != m_Passes[b].Desc.Phase)
                              return m_Passes[a].Desc.Phase < m_Passes[b].Desc.Phase;
                          return a < b;
                      });
        }

        auto& ready = m_ScratchReady;
        ready.clear();
        for (uint32_t ci = 0; ci < clusterCount; ++ci)
            if (!clusters[ci].empty() && cindeg[ci] == 0)
                ready.push_back(ci);

        m_ScheduledOrder.clear();
        uint32_t emittedClusters = 0;
        while (!ready.empty())
        {
            size_t best = 0;
            for (size_t i = 1; i < ready.size(); ++i)
                if (ckey[ready[i]] < ckey[ready[best]])
                    best = i;
            const uint32_t ci = ready[best];
            ready[best] = ready.back();
            ready.pop_back();

            for (RGPassId m : clusters[ci])
                m_ScheduledOrder.push_back(m);
            ++emittedClusters;

            for (uint32_t cv : csucc[ci])
                if (--cindeg[cv] == 0 && !clusters[cv].empty())
                    ready.push_back(cv);
        }
        if (emittedClusters == nonEmpty)
            break; // complete schedule

        // Stalled: break every stalled multi-pass cluster into singletons.
        bool split = false;
        const uint32_t snapshot = clusterCount;
        for (uint32_t ci = 0; ci < snapshot; ++ci)
        {
            if (clusters[ci].empty() || cindeg[ci] == 0 || clusters[ci].size() <= 1)
                continue;
            for (RGPassId m : clusters[ci])
            {
                const uint32_t ni = acquireCluster();
                clusterOf[m] = ni;
                clusters[ni].push_back(m);
            }
            clusters[ci].clear();
            split = true;
        }
        if (!split)
        {
            // All stalled clusters are singletons ⇒ a genuine pass-level cycle,
            // which the level phase already culled — defensive, never silent.
            for (uint32_t ci = 0; ci < snapshot; ++ci)
            {
                if (clusters[ci].empty() || cindeg[ci] == 0)
                    continue;
                for (RGPassId m : clusters[ci])
                {
                    Logger::Log::Error("[RenderGraph] cluster-DAG cycle: culling pass '{}'",
                                       m_Passes[m].Desc.Name ? m_Passes[m].Desc.Name : "<unnamed>");
                    m_Passes[m].Culled = true;
                    m_Passes[m].CullReason = RGCullReason::Cycle;
                    --m_LivePassCount;
                    --liveCount;
                }
            }
            assert(false && "RenderGraph cluster DAG stalled on singletons (pass cycle leaked through?)");
            break;
        }
    }
    assert(m_ScheduledOrder.size() == liveCount && "scheduled fewer than live passes");
    (void)liveCount;

    // ── Working-set transition metric (observability + tests) ──
    {
        const std::vector<RGResourceId>* last = nullptr;
        for (RGPassId p : m_ScheduledOrder)
        {
            if (attSet[p].empty())
                continue; // neutral passes neither count nor reset
            if (last && *last != attSet[p])
                ++m_WorkingSetTransitions;
            last = &attSet[p];
        }
    }
}

} // namespace GameEngine::Rendering::RenderGraph
