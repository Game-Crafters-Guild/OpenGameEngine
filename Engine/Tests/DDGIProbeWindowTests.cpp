#include "Engine/Rendering/DDGIProbeWindow.h"

#include <gtest/gtest.h>

#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{
// A 32 x 24 x 32 grid, the leak-room fine cascade at 32 probes on the long axis.
constexpr uint32_t kProbeTotal = 24576;

// Runs windows until the cursor returns to 0 and reports how often each probe
// was covered.
std::vector<uint32_t> VisitsPerProbeOverOneCycle(uint32_t probesPerTick)
{
    std::vector<uint32_t> visits(kProbeTotal, 0);
    uint32_t cursor = 0;
    for (int tick = 0; tick < 100000; ++tick)
    {
        const DDGIProbeWindow w = ComputeDDGIProbeWindow(cursor, probesPerTick, kProbeTotal);
        EXPECT_LE(w.Base + w.Count, kProbeTotal) << "a window must never straddle the end of the grid";
        for (uint32_t i = 0; i < w.Count; ++i)
            ++visits[w.Base + i];
        cursor = w.NextCursor;
        if (cursor == 0)
            break;
    }
    return visits;
}
}  // namespace

TEST(DDGIProbeWindow, BudgetThatDividesTheGridVisitsEveryProbeOncePerCycle)
{
    const std::vector<uint32_t> visits = VisitsPerProbeOverOneCycle(768);
    for (uint32_t p = 0; p < kProbeTotal; ++p)
        ASSERT_EQ(visits[p], 1u) << "probe " << p;
}

TEST(DDGIProbeWindow, BudgetThatDoesNotDivideTheGridStillVisitsEveryProbeOncePerCycle)
{
    // 1040 probes per tick: the adaptive budget grows in 16-probe steps, so
    // sizes like this are the common case, and 24576 is not a multiple of it.
    // A window that wrapped past the end would leave the first probes of the
    // grid unvisited on every cycle — a stale slab the field keeps reading.
    const std::vector<uint32_t> visits = VisitsPerProbeOverOneCycle(1040);
    for (uint32_t p = 0; p < kProbeTotal; ++p)
        ASSERT_EQ(visits[p], 1u) << "probe " << p;
}

TEST(DDGIProbeWindow, LastWindowOfACycleIsClampedToTheEnd)
{
    const DDGIProbeWindow w = ComputeDDGIProbeWindow(kProbeTotal - 100, 1040, kProbeTotal);
    EXPECT_EQ(w.Base, kProbeTotal - 100);
    EXPECT_EQ(w.Count, 100u);
    EXPECT_EQ(w.NextCursor, 0u);
}

TEST(DDGIProbeWindow, DegenerateInputsStaySafe)
{
    const DDGIProbeWindow empty = ComputeDDGIProbeWindow(5, 64, 0);
    EXPECT_EQ(empty.Count, 0u);
    const DDGIProbeWindow zeroBudget = ComputeDDGIProbeWindow(0, 0, 16);
    EXPECT_EQ(zeroBudget.Count, 1u) << "a zero budget still advances one probe, never stalls";
    const DDGIProbeWindow whole = ComputeDDGIProbeWindow(0, 1 << 20, 16);
    EXPECT_EQ(whole.Count, 16u);
    EXPECT_EQ(whole.NextCursor, 0u);
}
}  // namespace GameEngine::Engine::Renderer
