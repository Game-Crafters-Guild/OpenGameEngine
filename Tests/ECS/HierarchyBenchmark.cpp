#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "ECS/ECSTemplates.h" // typed World operation definitions
#include "ECS/Entity.h"
#include "ECS/RelationIndex.h"
#include "ECS/World.h"
#include "TestComponents.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <thread>
#include <vector>

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;
using GameEngine::Components::ChildrenOf;
using GameEngine::Components::DescendantsOf;
using GameEngine::Components::ParentRelationIndex;
using GameEngine::Components::IsDescendantOf;
using GameEngine::Components::Parent;
using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Hierarchy Query Benchmarks
//
// Measures ChildrenOf / DescendantsOf / IsDescendantOf at three scene shapes:
//   - Wide-shallow   : 1000 children under a single root  (1 level)
//   - Deep-narrow    : chain of 1000 entities             (1000 levels)
//   - Realistic mix  : 100 roots x 10 mid x 5 leaf        (5000 entities)
//
// Methodology mirrors ComparisonBenchmark: interleaved runs, median + min
// reported. Warm the target code path before sampling so L1/L2/TLB state is
// settled. A single warmup-then-N-runs loop biases the first sample; warming
// all measured paths up front keeps comparisons honest.
//
// Release-only numbers are meaningful — Debug is noisy enough to mislead.
// ============================================================================

namespace {

struct Sample {
    double Median = 0.0;
    double Min = 0.0;
};

static constexpr int kWarmups = 3;
static constexpr int kRuns = 15;

static double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

static Sample MeasureMs(const std::function<void()>& fn) {
    for (int i = 0; i < kWarmups; ++i) fn();
    std::vector<double> samples;
    samples.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        auto t0 = Clock::now();
        fn();
        samples.push_back(ToMs(Clock::now() - t0));
    }
    std::sort(samples.begin(), samples.end());
    return Sample{samples[kRuns / 2], samples.front()};
}

static EntityHandle MakeNode(World& w, EntityHandle parent = {}) {
    auto e = w.Create();
    e.Set(Position{0, 0, 0});
    if (parent.IsValid()) e.Set(Parent{parent});
    return e.GetHandle();
}

struct WideScene {
    EntityHandle Root;
    std::vector<EntityHandle> Children;
};

static WideScene BuildWide(World& w, std::size_t fanout) {
    WideScene s;
    s.Root = MakeNode(w);
    s.Children.reserve(fanout);
    for (std::size_t i = 0; i < fanout; ++i) s.Children.push_back(MakeNode(w, s.Root));
    w.ProcessCommands();
    return s;
}

struct DeepScene {
    EntityHandle Root;
    EntityHandle Leaf;
    std::size_t Depth;
};

static DeepScene BuildDeep(World& w, std::size_t depth) {
    DeepScene s{{}, {}, depth};
    s.Root = MakeNode(w);
    EntityHandle cur = s.Root;
    for (std::size_t i = 1; i < depth; ++i) {
        cur = MakeNode(w, cur);
    }
    s.Leaf = cur;
    w.ProcessCommands();
    return s;
}

struct MixScene {
    std::vector<EntityHandle> Roots;
    std::vector<EntityHandle> Mids;
    std::vector<EntityHandle> Leaves;
};

static MixScene BuildRealisticMix(World& w, std::size_t rootCount, std::size_t midPerRoot, std::size_t leafPerMid) {
    MixScene s;
    s.Roots.reserve(rootCount);
    s.Mids.reserve(rootCount * midPerRoot);
    s.Leaves.reserve(rootCount * midPerRoot * leafPerMid);
    for (std::size_t r = 0; r < rootCount; ++r) {
        auto root = MakeNode(w);
        s.Roots.push_back(root);
        for (std::size_t m = 0; m < midPerRoot; ++m) {
            auto mid = MakeNode(w, root);
            s.Mids.push_back(mid);
            for (std::size_t l = 0; l < leafPerMid; ++l) {
                s.Leaves.push_back(MakeNode(w, mid));
            }
        }
    }
    w.ProcessCommands();
    return s;
}

static void Report(const char* label, std::size_t sceneSize, const Sample& s) {
    std::printf("  [BENCH] %-38s  scene=%6zu  median=%8.3f ms  min=%8.3f ms\n",
                label, sceneSize, s.Median, s.Min);
}

// Shield any returned handle vector from the optimizer. `out.size()` reads
// the vector after the call, preventing DCE.
template <class T>
static void Sink(const T& v) {
    static volatile std::size_t sink = 0;
    sink += v.size();
}

} // namespace

// ----------------------------------------------------------------------------
// Wide-shallow: 1000 children under a single root, 1 level deep.
// Exercises the linear scan in ChildrenOf / DescendantsOf when the result set
// is large. IsDescendantOf only walks one parent hop here (cheapest case).
// ----------------------------------------------------------------------------
TEST(HierarchyBenchmark, WideShallow_1000Children) {
    World w;
    auto scene = BuildWide(w, 1000);
    const std::size_t sceneSize = 1 + scene.Children.size();

    std::vector<EntityHandle> out;
    auto childrenSample = MeasureMs([&]() {
        ChildrenOf(w, scene.Root, out);
        Sink(out);
    });
    auto descendantsSample = MeasureMs([&]() {
        DescendantsOf(w, scene.Root, out);
        Sink(out);
    });
    // Sample at the leaf-ish midpoint so the call has to walk up.
    auto middleLeaf = scene.Children[scene.Children.size() / 2];
    auto isDescSample = MeasureMs([&]() {
        volatile bool b = IsDescendantOf(w, middleLeaf, scene.Root);
        (void)b;
    });

    std::printf("\n  [BENCH] WideShallow (1 root + 1000 children)\n");
    Report("ChildrenOf(root)", sceneSize, childrenSample);
    Report("DescendantsOf(root)", sceneSize, descendantsSample);
    Report("IsDescendantOf(leaf -> root)", sceneSize, isDescSample);
}

// ----------------------------------------------------------------------------
// Deep-narrow: chain of 1000. DescendantsOf processes a single entity per BFS
// level => 1000 levels. This is where the per-level fresh-vector allocation in
// the current implementation should hurt the most.
// IsDescendantOf walks 1000 Parent hops (each acquires worldMutex shared_lock).
// ----------------------------------------------------------------------------
TEST(HierarchyBenchmark, DeepNarrow_Chain1000) {
    World w;
    auto scene = BuildDeep(w, 1000);

    std::vector<EntityHandle> out;
    auto childrenSample = MeasureMs([&]() {
        ChildrenOf(w, scene.Root, out);
        Sink(out);
    });
    auto descendantsSample = MeasureMs([&]() {
        DescendantsOf(w, scene.Root, out);
        Sink(out);
    });
    auto isDescSample = MeasureMs([&]() {
        volatile bool b = IsDescendantOf(w, scene.Leaf, scene.Root);
        (void)b;
    });

    // Indexed DescendantsOf — primed, so measurement reflects hot-path walk.
    ParentRelationIndex idx;
    (void)idx.GetSources(w, scene.Root);
    auto descendantsIndexedSample = MeasureMs([&]() {
        DescendantsOf(w, scene.Root, idx, out);
        Sink(out);
    });

    std::printf("\n  [BENCH] DeepNarrow (chain of 1000)\n");
    Report("ChildrenOf(root)", scene.Depth, childrenSample);
    Report("DescendantsOf(root)", scene.Depth, descendantsSample);
    Report("DescendantsOf(root) indexed (hot)", scene.Depth, descendantsIndexedSample);
    Report("IsDescendantOf(leaf -> root)", scene.Depth, isDescSample);
}

// ----------------------------------------------------------------------------
// Realistic mix: 100 roots x 10 mids x 5 leaves = 5000 entities.
// This is the shape users actually ship — shallow roots with moderate fanout.
// ----------------------------------------------------------------------------
TEST(HierarchyBenchmark, RealisticMix_100x10x5) {
    World w;
    auto scene = BuildRealisticMix(w, 100, 10, 5);
    const std::size_t sceneSize = scene.Roots.size() + scene.Mids.size() + scene.Leaves.size();

    // Pick deterministic mid-range probes so runs are comparable.
    auto probeRoot = scene.Roots[scene.Roots.size() / 2];
    auto probeLeaf = scene.Leaves[scene.Leaves.size() / 2];

    std::vector<EntityHandle> out;
    auto childrenSample = MeasureMs([&]() {
        ChildrenOf(w, probeRoot, out);
        Sink(out);
    });
    auto descendantsSample = MeasureMs([&]() {
        DescendantsOf(w, probeRoot, out);
        Sink(out);
    });
    auto isDescSample = MeasureMs([&]() {
        volatile bool b = IsDescendantOf(w, probeLeaf, probeRoot);
        (void)b;
    });

    // Indexed DescendantsOf — primed, hot-path walk.
    ParentRelationIndex idx;
    (void)idx.GetSources(w, probeRoot);
    auto descendantsIndexedSample = MeasureMs([&]() {
        DescendantsOf(w, probeRoot, idx, out);
        Sink(out);
    });

    std::printf("\n  [BENCH] RealisticMix (100 roots x 10 mid x 5 leaf)\n");
    Report("ChildrenOf(mid-root)", sceneSize, childrenSample);
    Report("DescendantsOf(mid-root)", sceneSize, descendantsSample);
    Report("DescendantsOf(mid-root) indexed (hot)", sceneSize, descendantsIndexedSample);
    Report("IsDescendantOf(leaf -> mid-root)", sceneSize, isDescSample);
}

// ----------------------------------------------------------------------------
// Query-construction overhead: `ChildrenOf` builds a fresh
// `Query<Read<Parent>>` on every call (signature construction + first-use
// cache build). Compare against a pre-constructed Query the caller reuses
// across frames. Informs whether we need a CachedQuery helper or just a doc
// note recommending manual reuse.
// ----------------------------------------------------------------------------
TEST(HierarchyBenchmark, QueryReuse_ChildrenOf) {
    World w;
    auto scene = BuildRealisticMix(w, 100, 10, 5);
    auto probeRoot = scene.Roots[scene.Roots.size() / 2];

    // (a) Fresh Query per call — current helper behavior.
    std::vector<EntityHandle> out;
    auto freshSample = MeasureMs([&]() {
        ChildrenOf(w, probeRoot, out);
        Sink(out);
    });

    // (b) Pre-constructed Query reused across iterations, manual filter walk.
    auto q = w.Query<Read<GameEngine::Components::Parent>>();
    auto reuseSample = MeasureMs([&]() {
        out.clear();
        q.Each([&](EntityHandle e, const GameEngine::Components::Parent& p) {
            if (p.parent == probeRoot) out.push_back(e);
        });
        Sink(out);
    });

    std::printf("\n  [BENCH] Query reuse comparison (6100-entity mix)\n");
    Report("ChildrenOf  (fresh Query)", 6100, freshSample);
    Report("Reused Query (manual walk)", 6100, reuseSample);
    const double delta = freshSample.Median - reuseSample.Median;
    std::printf("  [BENCH] Per-call Query construction overhead: %.4f ms\n", delta);
}

// ----------------------------------------------------------------------------
// RelationIndex vs naive scan. Headline Phase-1 measurement: how much do we
// save on a large flat scene where the naive path must scan all parented
// entities to answer "children of X".
// Scene: 100 roots, 100000 children round-robin under them. Probing one root
// yields ~1000 children. Naive scan walks 100000, index walks ~1000.
// ----------------------------------------------------------------------------
namespace RelationBench {
struct ParentOf {};
using ParentIdx = GameEngine::ECS::RelationIndex<
    ParentOf, GameEngine::Components::Parent, &GameEngine::Components::Parent::parent>;
}

TEST(HierarchyBenchmark, RelationIndex_vs_Scan) {
    constexpr std::size_t kRoots = 100;
    constexpr std::size_t kChildren = 100'000;

    World w;
    std::vector<EntityHandle> roots;
    roots.reserve(kRoots);
    for (std::size_t i = 0; i < kRoots; ++i) roots.push_back(MakeNode(w));
    for (std::size_t i = 0; i < kChildren; ++i) {
        MakeNode(w, roots[i % kRoots]);
    }
    w.ProcessCommands();

    const auto probe = roots[kRoots / 2];
    std::vector<EntityHandle> out;

    // Naive: fresh Query<Read<Parent>>().Each() scanning 100K entities.
    auto scanSample = MeasureMs([&]() {
        ChildrenOf(w, probe, out);
        Sink(out);
    });

    // Index path: hot cache (first call rebuilds, subsequent calls are O(1)).
    RelationBench::ParentIdx idx;
    (void)idx.GetSources(w, probe); // prime the cache outside the measured path
    auto indexSample = MeasureMs([&]() {
        const auto* kids = idx.GetSources(w, probe);
        if (kids) {
            static volatile std::size_t sink = 0;
            sink += kids->size();
        }
    });

    // Cold-rebuild cost: invalidate before each sample to simulate one
    // structural change between every query — worst-case workload.
    auto rebuildSample = MeasureMs([&]() {
        idx.Invalidate();
        const auto* kids = idx.GetSources(w, probe);
        if (kids) {
            static volatile std::size_t sink = 0;
            sink += kids->size();
        }
    });

    // Many-query scenario: amortize one rebuild across N queries (e.g., a
    // UI system asking ChildrenOf for every panel in a single frame).
    constexpr int kQueriesPerFrame = 50;
    auto manyQueriesScan = MeasureMs([&]() {
        for (int i = 0; i < kQueriesPerFrame; ++i) {
            ChildrenOf(w, roots[i % kRoots], out);
            Sink(out);
        }
    });
    RelationBench::ParentIdx idx2;
    auto manyQueriesIndex = MeasureMs([&]() {
        idx2.Invalidate(); // simulate one structural change per frame
        for (int i = 0; i < kQueriesPerFrame; ++i) {
            const auto* kids = idx2.GetSources(w, roots[i % kRoots]);
            if (kids) {
                static volatile std::size_t sink = 0;
                sink += kids->size();
            }
        }
    });

    std::printf("\n  [BENCH] RelationIndex vs scan (100 roots, 100K children)\n");
    Report("ChildrenOf (naive scan)", kChildren, scanSample);
    Report("RelationIndex (hot, no mutation)", kChildren, indexSample);
    Report("RelationIndex (full rebuild each)", kChildren, rebuildSample);
    std::printf("  [BENCH] %d queries/frame (1 structural change per frame):\n", kQueriesPerFrame);
    Report("  naive scan x N", kChildren, manyQueriesScan);
    Report("  index rebuild + N lookups", kChildren, manyQueriesIndex);

    // Report hot-path lookup in nanoseconds (median*1e6 / sample count is
    // meaningless — the fn ran once per iteration; report the raw median).
    std::printf("  [BENCH] Hot lookup ~= %.3f us (single GetSources call)\n",
                indexSample.Median * 1000.0);
    std::printf("  [BENCH] Rebuild cost ~= %.3f ms (one Query<Read<Parent>>.Each() + map inserts)\n",
                rebuildSample.Median);
    std::printf("  [BENCH] Crossover: index wins when queries-per-rebuild > %.1f\n",
                rebuildSample.Median / std::max(scanSample.Median, 1e-6));
}
