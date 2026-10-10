// P5 commit 1: unit tests for the shared virtualization engine, written
// BEFORE any view migrates onto it. These pin exactly the behaviors the
// review found untested across the three hand-rolled implementations:
// window shifts, ring slot identity, rebind counts, guard early-outs,
// subset application, safety-net repair, and shrink hysteresis.

#include <gtest/gtest.h>

#include "UI/Controls/VirtualWindowCore.h"

#include <map>
#include <vector>

using GameEngine::UI::VirtualWindowCore;
using Impact = GameEngine::UI::VirtualWindowCore::Impact;
using Guard = GameEngine::UI::VirtualWindowCore::Guard;
using ChangeSetView = GameEngine::UI::VirtualWindowCore::ChangeSetView;
using Reason = GameEngine::VirtualizationCoordinator::Reason;

namespace
{

struct FakeHost final : GameEngine::UI::VirtualWindowCore::Host
{
    std::vector<int> Bound;  // slot -> itemIndex (-1 = unbound)
    int RebindCalls = 0;
    int UnbindCalls = 0;
    int EnsureCalls = 0;
    int DestroyCalls = 0;
    Impact RebindResult = Impact::None;

    void EnsurePool(int slotCount) override
    {
        ++EnsureCalls;
        if (static_cast<int>(Bound.size()) < slotCount)
            Bound.resize(slotCount, -1);
    }
    int SlotCount() const override { return static_cast<int>(Bound.size()); }
    Impact Rebind(int slot, int itemIndex) override
    {
        ++RebindCalls;
        ASSERT_IN_RANGE(slot);
        Bound[slot] = itemIndex;
        return RebindResult;
    }
    void UnbindSlot(int slot) override
    {
        ++UnbindCalls;
        if (slot >= 0 && slot < static_cast<int>(Bound.size()))
            Bound[slot] = -1;
    }
    bool SlotBinds(int slot, int itemIndex) const override
    {
        return slot >= 0 && slot < static_cast<int>(Bound.size()) && Bound[slot] == itemIndex;
    }
    void DestroyTailSlot() override
    {
        ++DestroyCalls;
        if (!Bound.empty())
            Bound.pop_back();
    }

    void ResetCounters()
    {
        RebindCalls = 0;
        UnbindCalls = 0;
        EnsureCalls = 0;
        DestroyCalls = 0;
    }

    // Window invariant: every visible item is bound to exactly one slot.
    void ExpectWindowBound(const VirtualWindowCore& core, const Guard& g, int lanes = 1) const
    {
        for (int local = 0; local < g.Desired; ++local)
        {
            for (int lane = 0; lane < lanes; ++lane)
            {
                const int item = (g.First + local) * lanes + lane;
                if (item >= g.ItemCount)
                    continue;
                const int slot = core.SlotFor(local) * lanes + lane;
                EXPECT_EQ(Bound[slot], item)
                    << "local=" << local << " lane=" << lane << " slot=" << slot;
            }
        }
    }

  private:
    void ASSERT_IN_RANGE(int slot) const
    {
        ASSERT_GE(slot, 0);
        ASSERT_LT(slot, static_cast<int>(Bound.size()));
    }
};

Guard MakeGuard(int first, int desired, int count)
{
    Guard g;
    g.First = first;
    g.Desired = desired;
    g.ItemCount = count;
    g.ContentHPx = count * 20;
    g.ViewportWPx = 400;
    g.ContentWPx = 400;
    g.StructureGeneration = 1;
    return g;
}

const ChangeSetView kNoChanges{};

} // namespace

TEST(VirtualWindowCoreTests, FirstFromUniformClampsAndFloors)
{
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(0.0f, 20.0f, 100), 0);
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(19.9f, 20.0f, 100), 0);
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(20.0f, 20.0f, 100), 1);
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(1e9f, 20.0f, 100), 99);
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(-50.0f, 20.0f, 100), 0);
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(50.0f, 0.0f, 100), 0);
    EXPECT_EQ(VirtualWindowCore::FirstFromUniform(50.0f, 20.0f, 0), 0);
}

TEST(VirtualWindowCoreTests, FirstFromPrefixSumBinarySearches)
{
    // Items with heights 10, 30, 20, 40 → starts at 0, 10, 40, 60.
    const std::vector<int> cum{0, 10, 40, 60};
    EXPECT_EQ(VirtualWindowCore::FirstFromPrefixSum(cum, 0, 4), 0);
    EXPECT_EQ(VirtualWindowCore::FirstFromPrefixSum(cum, 9, 4), 0);
    EXPECT_EQ(VirtualWindowCore::FirstFromPrefixSum(cum, 10, 4), 1);
    EXPECT_EQ(VirtualWindowCore::FirstFromPrefixSum(cum, 39, 4), 1);
    EXPECT_EQ(VirtualWindowCore::FirstFromPrefixSum(cum, 40, 4), 2);
    EXPECT_EQ(VirtualWindowCore::FirstFromPrefixSum(cum, 1000, 4), 3);
}

TEST(VirtualWindowCoreTests, InitialUpdateBindsWholeWindowAndGrowsPool)
{
    VirtualWindowCore core;
    FakeHost host;
    const Guard g = MakeGuard(0, 8, 100);

    const Impact impact = core.Update(g, Reason::DataChanged, 1, kNoChanges, host);

    EXPECT_TRUE(Any(impact & Impact::Topology)) << "pool growth is a topology change";
    EXPECT_TRUE(Any(impact & Impact::Rebind));
    EXPECT_EQ(host.RebindCalls, 8);
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, GuardEqualNoChangesIsFreeAfterSettle)
{
    VirtualWindowCore core;
    FakeHost host;
    const Guard g = MakeGuard(3, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    const Impact impact = core.Update(g, Reason::ScrollChanged, 1, kNoChanges, host);

    EXPECT_EQ(impact, Impact::None);
    EXPECT_EQ(host.RebindCalls, 0) << "steady state must not rebind";
}

TEST(VirtualWindowCoreTests, RingShiftRebindsOnlyEnteringLines)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);

    // Remember which slot holds item 5 — after a +3 shift it must still
    // hold item 5 (slot identity: cells keep their content, no reshuffle).
    int slotOfItem5 = -1;
    for (int s = 0; s < 8; ++s)
        if (host.Bound[s] == 5)
            slotOfItem5 = s;
    ASSERT_NE(slotOfItem5, -1);

    host.ResetCounters();
    g.First = 3;
    const Impact impact = core.Update(g, Reason::ScrollChanged, 1, kNoChanges, host);

    EXPECT_TRUE(Any(impact & Impact::Rebind));
    EXPECT_EQ(host.RebindCalls, 3) << "+3 shift rebinds exactly the 3 entering lines";
    EXPECT_EQ(host.Bound[slotOfItem5], 5) << "surviving line kept its slot";
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, RingShiftBackwardsRebindsHead)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(10, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    g.First = 8;
    core.Update(g, Reason::ScrollChanged, 1, kNoChanges, host);

    EXPECT_EQ(host.RebindCalls, 2) << "-2 shift rebinds exactly the 2 entering head lines";
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, ShiftBeyondWindowFallsBackToFullRebind)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    g.First = 50; // jump > Desired
    core.Update(g, Reason::ScrollChanged, 1, kNoChanges, host);

    EXPECT_EQ(host.RebindCalls, 8);
    EXPECT_EQ(core.RingOffset(), 0) << "full rebind resets the ring";
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, StructureGenerationChangeForcesFullRebind)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(4, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    g.First = 5; // position moved AND structure changed → no ring shortcut
    g.StructureGeneration = 2;
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);

    EXPECT_EQ(host.RebindCalls, 8);
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, SubsetRebindsOnlyAffectedSlots)
{
    VirtualWindowCore core;
    FakeHost host;
    const Guard g = MakeGuard(0, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    const int slots[] = {2, 5};
    ChangeSetView changes;
    changes.ChangeKind = ChangeSetView::Kind::Subset;
    changes.AffectedSlots = slots;
    const Impact impact = core.Update(g, Reason::DataChanged, 1, changes, host);

    EXPECT_TRUE(Any(impact & Impact::Rebind));
    EXPECT_EQ(host.RebindCalls, 2) << "subset touches exactly the affected slots";
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, SubsetAfterRingShiftRebindsCorrectItems)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    g.First = 3;
    core.Update(g, Reason::ScrollChanged, 1, kNoChanges, host);
    ASSERT_NE(core.RingOffset(), 0);
    host.ResetCounters();

    // Poke every slot through the subset path: each must be rebound to the
    // item it CURRENTLY represents under the ring mapping.
    std::vector<int> all{0, 1, 2, 3, 4, 5, 6, 7};
    ChangeSetView changes;
    changes.ChangeKind = ChangeSetView::Kind::Subset;
    changes.AffectedSlots = all;
    core.Update(g, Reason::DataChanged, 1, changes, host);

    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, AllChangeRebindsWindow)
{
    VirtualWindowCore core;
    FakeHost host;
    const Guard g = MakeGuard(2, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    ChangeSetView changes;
    changes.ChangeKind = ChangeSetView::Kind::All;
    core.Update(g, Reason::DataChanged, 1, changes, host);

    EXPECT_EQ(host.RebindCalls, 8);
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, SafetyNetRepairsPokedHoleAndReportsImpact)
{
    VirtualWindowCore core;
    FakeHost host;
    const Guard g = MakeGuard(0, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);

    host.Bound[4] = -1; // simulate an out-of-band stale binding
    host.ResetCounters();

    const Impact impact = core.Update(g, Reason::ScrollChanged, 1, kNoChanges, host);

    EXPECT_TRUE(Any(impact & Impact::Rebind)) << "repairs must report (silent repair hides staleness)";
    EXPECT_EQ(host.RebindCalls, 1);
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, LanesMapGridSlots)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 4, 100); // 4 rows × 3 lanes
    core.Update(g, Reason::DataChanged, 3, kNoChanges, host);
    EXPECT_EQ(host.RebindCalls, 12);
    host.ExpectWindowBound(core, g, 3);

    host.ResetCounters();
    g.First = 1;
    core.Update(g, Reason::ScrollChanged, 3, kNoChanges, host);
    EXPECT_EQ(host.RebindCalls, 3) << "+1 row shift rebinds one row = lanes slots";
    host.ExpectWindowBound(core, g, 3);
}

TEST(VirtualWindowCoreTests, TailClampUnbindsPastEnd)
{
    VirtualWindowCore core;
    FakeHost host;
    // Window of 8 near the end of 10 items: local lines 6..7 are past the end.
    const Guard g = MakeGuard(4, 8, 10);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);

    EXPECT_EQ(host.RebindCalls, 6);
    EXPECT_EQ(host.UnbindCalls, 2);
    host.ExpectWindowBound(core, g);
}

TEST(VirtualWindowCoreTests, EmptyWindowUnbindsAll)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 8, 100);
    core.Update(g, Reason::DataChanged, 1, kNoChanges, host);
    host.ResetCounters();

    g.ItemCount = 0;
    g.Desired = 0;
    const Impact impact = core.Update(g, Reason::DataChanged, 1, kNoChanges, host);

    EXPECT_EQ(impact, Impact::None);
    EXPECT_EQ(host.UnbindCalls, 8);
}

TEST(VirtualWindowCoreTests, WindowShrinkUnbindsCellsBelowNewWindow)
{
    VirtualWindowCore core;
    FakeHost host;

    // Grow to a 40-line window, then shrink the window to 8 lines while the pool
    // is still 40 (pool shrink is gated by hysteresis). The 32 lines below the
    // new window must be unbound, not left showing their stale rows — otherwise
    // they reappear as duplicates once the window scrolls back over them.
    Guard big = MakeGuard(0, 40, 1000);
    core.Update(big, Reason::ViewportChanged, 1, kNoChanges, host);
    ASSERT_EQ(host.SlotCount(), 40);
    for (int s = 0; s < 40; ++s)
        ASSERT_EQ(host.Bound[s], s);

    Guard small = MakeGuard(0, 8, 1000);
    core.Update(small, Reason::ViewportChanged, 1, kNoChanges, host);

    EXPECT_EQ(host.SlotCount(), 40) << "no immediate pool shrink";
    for (int s = 0; s < 8; ++s)
        EXPECT_EQ(host.Bound[s], s) << "window slot " << s;
    for (int s = 8; s < 40; ++s)
        EXPECT_EQ(host.Bound[s], -1) << "slot below the window must be unbound, slot " << s;
    host.ExpectWindowBound(core, small);
}

TEST(VirtualWindowCoreTests, LaneGrowthWithConstantLinesRebindsFullWindow)
{
    // GridView splitter drag: columns (lanes) change while the row count
    // stays constant. Pool accounting must be slot-based — a lines-cached
    // model never grows the pool and the new columns render blank.
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 10, 1000);
    core.Update(g, Reason::ViewportChanged, 4, kNoChanges, host);
    ASSERT_EQ(host.SlotCount(), 40);

    g.ViewportWPx = 600; // guard breaks with the width change
    const Impact impact = core.Update(g, Reason::ViewportChanged, 6, kNoChanges, host);

    EXPECT_TRUE(Any(impact & Impact::Topology)) << "pool must grow for the new lanes";
    EXPECT_GE(host.SlotCount(), 60);
    host.ExpectWindowBound(core, g, 6);
}

TEST(VirtualWindowCoreTests, LaneShrinkWithConstantLinesUnbindsGhostSlots)
{
    // Narrowing: lanes 6 -> 4 with constant lines. Slots beyond the new
    // window must be unbound or they linger as ghost cells at stale rects.
    VirtualWindowCore core;
    FakeHost host;
    Guard g = MakeGuard(0, 10, 1000);
    core.Update(g, Reason::ViewportChanged, 6, kNoChanges, host);
    ASSERT_EQ(host.SlotCount(), 60);

    g.ViewportWPx = 300;
    core.Update(g, Reason::ViewportChanged, 4, kNoChanges, host);

    host.ExpectWindowBound(core, g, 4);
    for (int slot = 40; slot < 60; ++slot)
        EXPECT_EQ(host.Bound[slot], -1) << "ghost slot " << slot << " still bound";
}

TEST(VirtualWindowCoreTests, ShrinkBlockedWhileInDispatchFiresOnNextSafeUpdate)
{
    VirtualWindowCore core;
    FakeHost host;
    Guard big = MakeGuard(0, 40, 1000);
    core.Update(big, Reason::ViewportChanged, 1, kNoChanges, host);
    Guard small = MakeGuard(0, 8, 1000);
    core.Update(small, Reason::ViewportChanged, 1, kNoChanges, host);

    // Reach the threshold entirely on dispatch-blocked updates: the counter
    // must still advance, but no element may be destroyed mid-dispatch.
    // (Prewarm reason: ScrollChanged is counter-neutral by design.)
    for (int i = 0; i < VirtualWindowCore::kShrinkStableUpdates + 5; ++i)
        core.Update(small, Reason::Prewarm, 1, kNoChanges, host, /*allowShrink=*/false);
    EXPECT_EQ(host.DestroyCalls, 0) << "no destruction while dispatch-blocked";

    // First safe update fires the deferred shrink.
    const Impact impact = core.Update(small, Reason::Prewarm, 1, kNoChanges, host,
                                      /*allowShrink=*/true);
    EXPECT_TRUE(Any(impact & Impact::Topology));
    EXPECT_EQ(host.SlotCount(), 8 + VirtualWindowCore::kShrinkSlackLines);
}

TEST(VirtualWindowCoreTests, ShrinkOnlyAfterStableHysteresis)
{
    VirtualWindowCore core;
    FakeHost host;

    // Grow the pool to 40 lines, then shrink the window to 8.
    Guard big = MakeGuard(0, 40, 1000);
    core.Update(big, Reason::ViewportChanged, 1, kNoChanges, host);
    ASSERT_EQ(host.SlotCount(), 40);

    Guard small = MakeGuard(0, 8, 1000);
    core.Update(small, Reason::ViewportChanged, 1, kNoChanges, host);
    EXPECT_EQ(host.SlotCount(), 40) << "no immediate shrink";

    // Stable updates below the threshold: still no shrink. (Prewarm — a
    // ScrollChanged update is mid-gesture and counter-neutral by design.)
    for (int i = 0; i < VirtualWindowCore::kShrinkStableUpdates - 1; ++i)
        core.Update(small, Reason::Prewarm, 1, kNoChanges, host);
    EXPECT_EQ(host.DestroyCalls, 0);

    // Crossing the threshold shrinks to Desired + slack and reports topology.
    const Impact impact = core.Update(small, Reason::Prewarm, 1, kNoChanges, host);
    EXPECT_TRUE(Any(impact & Impact::Topology));
    EXPECT_EQ(host.SlotCount(), 8 + VirtualWindowCore::kShrinkSlackLines);
    EXPECT_EQ(host.DestroyCalls, 40 - (8 + VirtualWindowCore::kShrinkSlackLines));

    // ScrollChanged updates never advance the counter — a slow scrollbar
    // drag can't accumulate into a mid-gesture shrink.
    host.ResetCounters();
    for (int i = 0; i < VirtualWindowCore::kShrinkStableUpdates + 5; ++i)
        core.Update(small, Reason::ScrollChanged, 1, kNoChanges, host);
    EXPECT_EQ(host.DestroyCalls, 0);
}
