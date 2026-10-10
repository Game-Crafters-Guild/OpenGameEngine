// CBTTreeSeed: when the CBT subdivision tree restarts from its roots. CBTRenderFeature re-seeds
// the instance whenever RestartReason names a reason, so these pin the reasons themselves: a
// retired terrain (replaced, re-provisioned, removed, or a scene opened), a lowered depth cap and
// a domain switch restart the tree at once; repeats within the window are held until they
// settle; nothing else restarts it.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrainECS/CBTTreeSeed.h"

namespace
{
using GameEngine::CBTTerrain::kDomainPlanar;
using GameEngine::CBTTerrain::kDomainSpherical;
using GameEngine::CBTTerrainECS::CBTTreeRestart;
using GameEngine::CBTTerrainECS::CBTTreeSeed;
constexpr uint32_t kSettle = CBTTreeSeed::kRestartSettleFrames;

// Asks once per frame from `firstFrame` while the request holds; returns the first frame the
// restart fires in, 0 if it does not fire within `frames`, or kWrongReason if it fires with a
// reason other than `expected`.
constexpr uint32_t kWrongReason = 0xFFFFFFFFu;
uint32_t FrameRestartFires(CBTTreeSeed& seed, uint32_t domain, uint32_t maxDepth,
                           uint32_t firstFrame, uint32_t frames, CBTTreeRestart expected)
{
    for (uint32_t frame = firstFrame; frame < firstFrame + frames; ++frame)
    {
        const CBTTreeRestart reason = seed.RestartReason(domain, maxDepth, frame);
        if (reason != CBTTreeRestart::None)
            return reason == expected ? frame : kWrongReason;
    }
    return 0u;
}
} // namespace

TEST(CBTTreeSeed, RetiredTerrainRestartsTheTreeAtOnce)
{
    CBTTreeSeed seed;
    seed.Seeded(kDomainPlanar, 23u);
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 23u, 1u, 40u, CBTTreeRestart::None), 0u)
        << "an unchanged terrain restarted the tree";

    seed.NoteTerrainRetired();
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 21u, 100u, 40u, CBTTreeRestart::TerrainRetired),
              100u)
        << "a scene open must restart the tree in the frame it lands in, inside its load hitch";

    seed.Seeded(kDomainPlanar, 21u);
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 21u, 101u, 40u, CBTTreeRestart::None), 0u)
        << "a re-seed consumes the retire";
}

TEST(CBTTreeSeed, LoweredDepthCapRestartsTheTree)
{
    CBTTreeSeed seed;
    seed.Seeded(kDomainPlanar, 21u);
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 25u, 1u, 40u, CBTTreeRestart::None), 0u)
        << "a raised cap lets the tree grow deeper without a restart";
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 21u, 100u, 40u, CBTTreeRestart::DepthCapLowered),
              100u)
        << "leaves may have grown to the raised cap; back at the seeded cap they sit below it";
}

TEST(CBTTreeSeed, DomainSwitchRestartsTheTree)
{
    CBTTreeSeed seed;
    seed.Seeded(kDomainPlanar, 21u);
    EXPECT_EQ(FrameRestartFires(seed, kDomainSpherical, 21u, 1u, 40u, CBTTreeRestart::DomainChanged),
              1u);

    seed.Seeded(kDomainSpherical, 30u);
    EXPECT_EQ(seed.Domain(), kDomainSpherical);
    EXPECT_EQ(FrameRestartFires(seed, kDomainSpherical, 30u, 2u, 40u, CBTTreeRestart::None), 0u);
}

// An inspector drag writes the component on most frames: a non-tiled resize retires a texture
// set per step and a downward Size, Samples Per Meter, radius or override drag lowers the cap step
// by step. The tree restarts at the first step, keeps its detail through the rest of the drag,
// and restarts once more after it settles.
TEST(CBTTreeSeed, DragRestartsAtItsFirstStepAndOnceAfterItSettles)
{
    CBTTreeSeed seed;
    seed.Seeded(kDomainPlanar, 23u);
    uint32_t frame = 1u;
    uint32_t restarts = 0u;
    for (uint32_t step = 0; step < 30u; ++step)
    {
        seed.NoteTerrainRetired();
        // A debug-port drag lands a step every 3 to 4 frames; two views ask in every frame.
        for (uint32_t i = 0; i < 4u; ++i, ++frame)
            for (uint32_t view = 0; view < 2u; ++view)
            {
                const CBTTreeRestart reason = seed.RestartReason(kDomainPlanar, 23u, frame);
                if (reason == CBTTreeRestart::None)
                    continue;
                ++restarts;
                EXPECT_EQ(frame, 1u) << "the drag restarted the tree mid-gesture at frame " << frame;
                seed.Seeded(kDomainPlanar, 23u);
            }
    }
    EXPECT_EQ(restarts, 1u) << "the first step restarts the tree at once";
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 23u, frame, 40u, CBTTreeRestart::TerrainRetired),
              frame - 4u + kSettle)
        << "the restart fires once the last step has held for the window";
    seed.Seeded(kDomainPlanar, 23u);
    EXPECT_EQ(FrameRestartFires(seed, kDomainPlanar, 23u, frame + kSettle, 40u, CBTTreeRestart::None),
              0u);
}
