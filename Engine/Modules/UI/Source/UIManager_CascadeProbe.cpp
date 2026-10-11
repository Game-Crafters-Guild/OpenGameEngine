// Level-wave cascade probe orchestrator (UIStyleTests / bench ONLY).
//
// This is the third, previously-untested MT design for the UI style cascade.
// The first two — MT-4 subtree fork-join and MT-4.3 work-first claim — were
// both implemented and killed on measurement. Their kill mechanism was
// per-run round-trips (fork/join or publish/park) whose latency dwarfed the
// few-µs compute of a wide-shallow tree's tiny sibling runs.
//
// LEVEL-SYNCHRONOUS WAVES sidestep per-run round-trips: elements at depth d are
// independent given resolved depth d-1, so the cascade is driven one level at a
// time — collect the level's work, ONE ParallelFor over it, barrier, next level.
// Barriers = tree depth (~5-15), not runs (~hundreds). The open question this
// probe answers is the crossover N at which that trade wins.
//
// The parallel unit is the sibling-share GROUP, not the raw element: same-shape
// siblings share one donor snapshot (P4), and letting two same-key elements
// compute concurrently would race the shared donor cache. So per level the work
// items are grouped by share key; each group's donor computes and its sharees
// copy, all inside one group task. Groups touch disjoint elements and (for
// multi-member groups) a lane-private donor cache, so ParallelFor over groups is
// race-free. This reuses the REAL per-element cascade (ResolveCascadeForElement)
// verbatim — only the traversal/parallelism differs between the two modes.
//
// NOT a product path: never called by Update(). Compiled into the UI library so
// it can reach the private cascade internals, exposed through the single public
// RunLevelWaveCascadeProbe entry declared in UIManager.h.

#include "UI/UIManager.h"
#include "UIManager_Internal.h"

#include "UI/Parsers/CSSParser.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"

#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA

namespace
{
using ProbeClock = std::chrono::high_resolution_clock;

inline double ElapsedMs(ProbeClock::time_point a, ProbeClock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// One level's parallel work item: the members that share a donor snapshot, plus
// the parent style they inherit (identical for every member — same DfsParent).
struct ShareGroup
{
    std::vector<UIElement*> Members;
    const ResolvedStyle* ParentStyle = nullptr;
};
} // namespace

void UIManager::RunLevelWaveCascadeProbe(bool levelWave, bool coldCaches,
                                         CascadeProbeResult& out)
{
    out = CascadeProbeResult{};
    if (!m_Root)
        return;

    // --- Prep (untimed): shared by both modes -------------------------------
    // Uniform-scene sheet span: the probe tree attaches its stylesheet(s) at the
    // root and adds no per-element local sheets, so every element resolves
    // against the same global set. Rule indices are built HERE on the UI thread
    // (GetOrBuildRuleIndex mutates a manager cache) so the parallel region below
    // only ever reads them.
    std::vector<const Stylesheet*> sheetsVec;
    std::vector<const StylesheetRuleIndex*> indicesVec;
    sheetsVec.reserve(m_GlobalStylesheets.size());
    indicesVec.reserve(m_GlobalStylesheets.size());
    for (const StylesheetHandle& h : m_GlobalStylesheets)
    {
        const Stylesheet* s = h.get();
        if (!s)
            continue;
        sheetsVec.push_back(s);
        indicesVec.push_back(GetOrBuildRuleIndex(s));
    }
    const std::span<const Stylesheet* const> sheetsSpan{sheetsVec.data(), sheetsVec.size()};
    const std::span<const StylesheetRuleIndex* const> indicesSpan{indicesVec.data(), indicesVec.size()};

    // Headless probe: no hover target, so the rightmost-:hover share bit is off
    // for every element. One shared read-only set, pointed at by every context.
    const std::unordered_set<const UIElement*> emptyHover;

    // Cold restyle == full structural rematch. Invalidating the per-element rule
    // cache forces ComputeStyleInto to rebuild each element's matched-rule set,
    // the CSS-cold-reload equivalent. Warm leaves the cache populated. Done
    // before the timed region — the real system's invalidation is an epoch bump,
    // not part of the cascade wall.
    if (coldCaches)
        m_Root->InvalidateRuleCacheSubtree();

    // ------------------------------------------------------------------------
    if (!levelWave)
    {
        // SERIAL: DFS preorder through one shared donor cache — the product's
        // BuildYogaRecursive cascade order. Sibling sharing is live (first of a
        // key computes, the rest copy from the shared cache).
        CascadeShareCache serialCache;
        CascadeShareContext ctx;
        ctx.Cache = &serialCache;
        ctx.Hover = &emptyHover;
        ctx.Active = true;

        struct Frame { UIElement* El; const ResolvedStyle* Parent; };
        std::vector<Frame> stack;
        stack.reserve(1024);
        stack.push_back({m_Root.get(), nullptr});

        const auto t0 = ProbeClock::now();
        while (!stack.empty())
        {
            const Frame f = stack.back();
            stack.pop_back();

            TransitionRegistration tr = TransitionRegistration::None;
            const bool shared = ResolveCascadeForElement(f.El, sheetsSpan, indicesSpan,
                                                         f.Parent, &ctx, tr);
            ++out.ElementCount;
            if (shared) ++out.ShareeCopies; else ++out.DonorComputes;

            const ResolvedStyle* thisStyle = &f.El->GetResolvedStyle();
            const auto& children = f.El->GetChildren();
            // Push in reverse so pops visit siblings in natural order (donor is
            // the first sibling of each key, matching the product).
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                if (it->get())
                    stack.push_back({it->get(), thisStyle});
        }
        out.DriveMs = ElapsedMs(t0, ProbeClock::now());
        out.LevelWidths.clear();
        return;
    }

    // LEVEL-WAVE: process depth level by depth level.
    const size_t workerCount =
        m_JobSystem ? std::max<size_t>(1, m_JobSystem->GetWorkerCount()) : 1;

    double groupingMs = 0.0;
    const auto t0 = ProbeClock::now();

    // Level 0 — the root, alone (parentless: never shares). Singleton compute.
    {
        TransitionRegistration tr = TransitionRegistration::None;
        ResolveCascadeForElement(m_Root.get(), sheetsSpan, indicesSpan,
                                 /*parentStyle=*/nullptr, /*shareCtx=*/nullptr, tr);
        ++out.ElementCount;
        ++out.DonorComputes;
        out.LevelWidths.push_back(1);
        out.LevelGroupCounts.push_back(1);
    }

    std::vector<UIElement*> curLevel;
    for (const auto& ch : m_Root->GetChildren())
        if (ch.get())
            curLevel.push_back(ch.get());

    std::unordered_map<uint64_t, size_t> keyToGroup;
    std::vector<ShareGroup> groups;

    while (!curLevel.empty())
    {
        out.LevelWidths.push_back(static_cast<uint32_t>(curLevel.size()));
        out.ElementCount += static_cast<uint32_t>(curLevel.size());

        // --- Group the level by sibling-share key (timed as grouping cost) ---
        const auto g0 = ProbeClock::now();
        keyToGroup.clear();
        groups.clear();
        for (UIElement* el : curLevel)
        {
            // Parent is at level d-1: its style is final before this level runs.
            const ResolvedStyle* parentStyle =
                el->GetParent() ? &el->GetParent()->GetResolvedStyle() : nullptr;

            UIParsing::ElementState st = BuildCascadeElementState(el);
            uint64_t key = 0;
            uint16_t stateBits = 0;
            uint32_t sheetSetId = 0;
            const bool eligible = ComputeCascadeShareKey(el, indicesSpan, st, &emptyHover,
                                                         key, stateBits, sheetSetId);
            if (eligible)
            {
                auto it = keyToGroup.find(key);
                if (it == keyToGroup.end())
                {
                    keyToGroup.emplace(key, groups.size());
                    groups.push_back(ShareGroup{{el}, parentStyle});
                }
                else
                {
                    groups[it->second].Members.push_back(el);
                }
            }
            else
            {
                groups.push_back(ShareGroup{{el}, parentStyle});
            }
        }
        groupingMs += ElapsedMs(g0, ProbeClock::now());

        out.LevelGroupCounts.push_back(static_cast<uint32_t>(groups.size()));
        // Donor per group; the rest of a group copy the donor snapshot.
        out.DonorComputes += static_cast<uint32_t>(groups.size());

        // --- ONE ParallelFor over the level's groups, then a barrier --------
        // minBatch sized so the fan-out lands near workerCount balanced chunks
        // (ParallelFor clamps chunkCount to workerCount+1). This is the design's
        // best case: max parallel width, one dispatch + one barrier per level.
        const size_t groupCount = groups.size();
        const size_t minBatch = std::max<size_t>(1, groupCount / (workerCount + 1));

        ShareGroup* groupData = groups.data();
        JobSystem::ParallelFor(
            m_JobSystem, groupCount,
            [groupData, sheetsSpan, indicesSpan, &emptyHover, this](size_t b, size_t e)
            {
                // Lane-private donor cache: multi-member groups register their
                // donor here and their sharees copy it. Cleared per group. A
                // singleton group needs no cache (nullptr context = pure compute).
                CascadeShareCache lane;
                CascadeShareContext lctx;
                lctx.Cache = &lane;
                lctx.Hover = &emptyHover;
                lctx.Active = true;

                for (size_t gi = b; gi < e; ++gi)
                {
                    ShareGroup& g = groupData[gi];
                    TransitionRegistration tr = TransitionRegistration::None;
                    if (g.Members.size() == 1)
                    {
                        ResolveCascadeForElement(g.Members[0], sheetsSpan, indicesSpan,
                                                 g.ParentStyle, /*shareCtx=*/nullptr, tr);
                    }
                    else
                    {
                        lane.Entries.clear();
                        for (UIElement* el : g.Members)
                            ResolveCascadeForElement(el, sheetsSpan, indicesSpan,
                                                     g.ParentStyle, &lctx, tr);
                    }
                }
            },
            minBatch);

        // --- Descend to the next level --------------------------------------
        std::vector<UIElement*> next;
        next.reserve(curLevel.size());
        for (UIElement* el : curLevel)
            for (const auto& ch : el->GetChildren())
                if (ch.get())
                    next.push_back(ch.get());
        curLevel = std::move(next);
    }

    out.DriveMs = ElapsedMs(t0, ProbeClock::now());
    out.GroupingMs = groupingMs;
    if (out.ElementCount >= out.DonorComputes)
        out.ShareeCopies = out.ElementCount - out.DonorComputes;
    out.LevelCount = static_cast<uint32_t>(out.LevelWidths.size());
    for (uint32_t w : out.LevelWidths)
        out.MaxLevelWidth = std::max(out.MaxLevelWidth, w);
    for (uint32_t gc : out.LevelGroupCounts)
        out.MaxLevelGroups = std::max(out.MaxLevelGroups, gc);
}

#else // !GE_HAVE_YOGA

void UIManager::RunLevelWaveCascadeProbe(bool, bool, CascadeProbeResult& out)
{
    out = CascadeProbeResult{};
}

#endif // GE_HAVE_YOGA

} // namespace GameEngine
