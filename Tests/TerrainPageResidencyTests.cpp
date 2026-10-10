// The paged height source of a planar terrain as pure oracles: the level rule against the design
// note's numbers, the request set's closure under parents, the page cache's residency, and the CPU
// mirror of the paged resolve (parity, the fallback walk, ring continuity, parked quiescence).

#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/PagedHeightSampler.h"
#include "TerrainECS/TerrainAtlas.h"

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStoreFormat.h"

#include <gtest/gtest.h>

#include "GlslShim.h"
#include "TerrainPagedField.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::PageStreaming::PageAddress;
using namespace GameEngine::TerrainECS::Test;

namespace
{

// The design note's measured viewport: 900 rows, 60 degree vertical field, TargetPixelError 11.
PageLevelView NoteView()
{
    return MakePageLevelView(900, std::numbers::pi_v<float32> / 3.0f, 11.0f);
}

// The user's 60 x 15 km terrain at 0.25 m, ground between 171.6 and 304.3 m.
PageRequestTerrain UserTerrain()
{
    PageRequestTerrain terrain;
    terrain.Level0TexelX = 0.25f;
    terrain.Level0TexelZ = 0.25f;
    terrain.SamplesX = 240000;
    terrain.SamplesZ = 60000;
    terrain.MinHeight = 171.6f;
    terrain.MaxHeight = 304.3f;
    return terrain;
}

std::tuple<uint32, uint32, uint32> Key(const PageAddress& a)
{
    return {a.Level, a.X, a.Z};
}

} // namespace

TEST(PageLevelRule, GeometryLevelsStartAtTwentyOneMetersAndDoubleAsTheNoteComputes)
{
    const PageLevelView view = NoteView();
    EXPECT_NEAR(view.FocalPixels, 779.4f, 0.1f);
    EXPECT_NEAR(view.TargetPixels, 9.17f, 0.01f);
    // Level 0 ends at about 21 m for a 0.25 m field, and each level's ring is twice the one before.
    EXPECT_NEAR(GeometryPageLevel(21.25f, 0.25f, view), 0.0f, 0.01f);
    for (uint32 level = 1; level < 11; ++level)
        EXPECT_NEAR(GeometryPageLevel(21.25f * static_cast<float32>(1u << level), 0.25f, view),
                    static_cast<float32>(level), 0.01f);
}

TEST(PageLevelRule, ParentBlendRampsOverTheLastQuarterOfEachRingOnly)
{
    for (float32 base : {0.0f, 3.0f, 7.0f})
    {
        EXPECT_EQ(PageParentBlend(base), 0.0f);
        EXPECT_EQ(PageParentBlend(base + 0.5f), 0.0f);
        EXPECT_EQ(PageParentBlend(base + 0.75f), 0.0f);
        EXPECT_FLOAT_EQ(PageParentBlend(base + 0.875f), 0.5f);
        EXPECT_GT(PageParentBlend(base + 0.999f), 0.99f);
    }
}

TEST(PageLevelRule, PinnedLevelsAreTheSixteenMeterTexelsAndUp)
{
    // 0.25 m: level 6 is the first 16 m texel; levels 6 to 11 are the note's 323 pinned pages.
    const PageRequestTerrain terrain = UserTerrain();
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(240000, 60000);
    const uint32 first = FirstPinnedPageLevel(terrain, levels);
    EXPECT_EQ(first, 6u);
    uint32 pinnedPages = 0;
    for (uint32 level = first; level < levels.size(); ++level)
        pinnedPages += levels[level].PagesX * levels[level].PagesZ;
    EXPECT_EQ(pinnedPages, 323u);

    // A small terrain with no 16 m level pins its coarsest, so a fallback walk still ends on a page.
    PageRequestTerrain small = terrain;
    small.SamplesX = small.SamplesZ = 300;
    EXPECT_EQ(FirstPinnedPageLevel(small, PageStreaming::BuildPageStoreLevels(300, 300)), 2u);
}

// The owner's card 93, option B: a small pyramid is pinned whole, the cap a quarter of the
// profile's height cache. A 1 km terrain at 1, 2 and 4 samples per meter has 85, 341 and 1,365 pages.
TEST(PageLevelRule, ASmallPyramidIsPinnedWholeWithinAQuarterOfTheCache)
{
    PageRequestTerrain terrain;
    terrain.MaxHeight = 100.0f;
    const auto pagesOf = [](uint32 samples) {
        const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(samples, samples);
        return levels.back().FirstEntry + levels.back().PagesX * levels.back().PagesZ;
    };
    EXPECT_EQ(pagesOf(1001), 85u);
    EXPECT_EQ(pagesOf(2001), 341u);
    EXPECT_EQ(pagesOf(4001), 1365u);
    struct Case
    {
        uint32 Samples;
        float32 Texel;
        uint32 Slots;
        bool Whole;
    };
    // Desktop 512 height slots, Steam Deck 256, web 128, high detail 2,048.
    for (const Case& c : {Case{1001, 1.0f, 512, true}, Case{1001, 1.0f, 256, false}, Case{1001, 1.0f, 128, false},
                          Case{2001, 0.5f, 512, false}, Case{2001, 0.5f, 2048, true}, Case{4001, 0.25f, 2048, false}})
    {
        terrain.SamplesX = terrain.SamplesZ = c.Samples;
        terrain.Level0TexelX = terrain.Level0TexelZ = c.Texel;
        terrain.CacheSlots = c.Slots;
        HeightPageRequester requester;
        requester.Configure(terrain);
        EXPECT_EQ(requester.FirstPinnedLevel() == 0u, c.Whole) << c.Samples << " samples, " << c.Slots << " slots";

        // Whole: every page is requested and pinned, with the camera far away. Not whole: only the
        // texel rule's levels are pinned.
        const Mathematics::Vector3 far{1.0e6f, 1.0e6f, 1.0e6f};
        std::vector<PageStreaming::PageWant> wants;
        requester.Build(std::span(&far, 1), NoteView(), wants);
        const bool everyPagePinned =
            wants.size() == pagesOf(c.Samples) &&
            std::all_of(wants.begin(), wants.end(), [](const PageStreaming::PageWant& w) { return w.Pinned; });
        EXPECT_EQ(everyPagePinned, c.Whole) << c.Samples << " samples, " << c.Slots << " slots";
        // Not whole: the texel rule's levels, or the coarsest when no level reaches 16 m texels.
        const uint32 top = uint32(PageStreaming::BuildPageStoreLevels(c.Samples, c.Samples).size()) - 1u;
        if (!c.Whole)
            for (const PageStreaming::PageWant& want : wants)
                EXPECT_TRUE(c.Texel * float32(1u << want.Address.Level) >= kPinnedPageTexelMeters ||
                            want.Address.Level == top);
    }
}

TEST(PageLevelRule, RequestsAreClosedUnderParentsAndFineOnlyNearTheCamera)
{
    const PageRequestTerrain terrain = UserTerrain();
    const PageLevelView view = NoteView();
    const Mathematics::Vector3 eye{30000.0f, 289.0f, 7500.0f}; // 1.7 m above the ridge
    std::vector<PageStreaming::PageWant> requests;
    HeightPageRequester requester;
    requester.Configure(terrain);
    requester.Build(std::span(&eye, 1), view, requests);

    std::set<std::tuple<uint32, uint32, uint32>> seen;
    std::map<std::tuple<uint32, uint32, uint32>, float32> distance;
    for (const PageStreaming::PageWant& request : requests)
    {
        EXPECT_TRUE(seen.insert(Key(request.Address)).second) << "a page requested twice";
        distance[Key(request.Address)] = request.Distance;
    }
    const uint32 top = static_cast<uint32>(PageStreaming::BuildPageStoreLevels(240000, 60000).size()) - 1u;
    uint32 level0 = 0;
    for (const PageStreaming::PageWant& request : requests)
    {
        if (request.Address.Level < top)
            EXPECT_TRUE(seen.count(Key(request.Address.Parent())))
                << "level " << int(request.Address.Level) << " page without its parent";
        if (request.Address.Level == 0)
        {
            ++level0;
            // Level-0 pages come as the four children of a level-1 page some point of which needs
            // finer than level 1.
            EXPECT_LT(GeometryPageLevel(distance[Key(request.Address.Parent())], 0.25f, view), 1.0f);
        }
    }
    // The eye pose needs a handful of level-0 pages around the camera, not the terrain's 879,375.
    EXPECT_GE(level0, 4u);
    EXPECT_LE(level0, 16u);
    EXPECT_LT(requests.size(), 2000u);
}

TEST(PageLevelRule, AParkedCameraRequestsTheSameSetEveryFrame)
{
    const PageRequestTerrain terrain = UserTerrain();
    const Mathematics::Vector3 eye{12000.0f, 800.0f, 3000.0f};
    std::vector<PageStreaming::PageWant> first;
    std::vector<PageStreaming::PageWant> second;
    HeightPageRequester requester;
    requester.Configure(terrain);
    requester.Build(std::span(&eye, 1), NoteView(), first);
    requester.Build(std::span(&eye, 1), NoteView(), second);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
        EXPECT_EQ(first[i].Address, second[i].Address);
}

// ---- The page cache and the paged resolve -------------------------------------------------------

namespace
{

// Level L's lattice sampled bilinearly at terrain UV, from the field function directly: what the
// paged resolve must reproduce at a resident, settled level.
float32 DirectLevelSample(uint32 level, float32 u, float32 v)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(kFieldX, kFieldZ);
    const float32 cx = u * float32(kFieldX - 1) / float32(1u << level);
    const float32 cz = v * float32(kFieldZ - 1) / float32(1u << level);
    const float32 x0 = std::floor(cx);
    const float32 z0 = std::floor(cz);
    const auto at = [&](float32 j, float32 k) {
        j = std::min(j, float32(levels[level].SamplesX - 1));
        k = std::min(k, float32(levels[level].SamplesZ - 1));
        return FieldHeight(j * float32(1u << level), k * float32(1u << level));
    };
    const float32 fx = cx - x0;
    const float32 fz = cz - z0;
    const float32 a = at(x0, z0) + (at(x0 + 1, z0) - at(x0, z0)) * fx;
    const float32 b = at(x0, z0 + 1) + (at(x0 + 1, z0 + 1) - at(x0, z0 + 1)) * fx;
    return a + (b - a) * fz;
}

} // namespace

TEST(PagedHeightResolve, AResidentSettledLevelIsItsLatticeBilinearly)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    ASSERT_EQ(field.Cache.ResidentCount(), all.size());
    const PagedHeightSampler sampler = field.Sampler();
    for (uint32 level = 0; level <= field.TopLevel(); ++level)
        for (float32 u : {0.0f, 0.13f, 0.2133f, 0.5f, 0.77f, 1.0f})
            for (float32 v : {0.0f, 0.31f, 0.64f, 0.999f, 1.0f})
                EXPECT_NEAR(sampler.Sample(u, v, float32(level)), DirectLevelSample(level, u, v), 2e-4f)
                    << "level " << level << " uv " << u << "," << v;
}

TEST(PagedHeightResolve, AMissingPageResolvesToItsFirstResidentAncestorAlone)
{
    PagedField field(64);
    std::vector<PageAddress> wanted;
    for (const PageAddress& page : field.AllPages())
    {
        const bool missingLevel0 = page.Level == 0 && page.X == 1 && page.Z == 1;
        const bool missingLevel1 = page.Level == 1 && page.X == 0 && page.Z == 0;
        if (!missingLevel0 && !missingLevel1)
            wanted.push_back(page);
    }
    for (int frame = 0; frame < 40; ++frame)
        field.Step(wanted);
    // Parent first: level-0 (0, 0) is wanted but its parent is not resident, so it stays out too.
    EXPECT_EQ(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 0, 0})), PageStreaming::kNoResidentSlot);
    const PagedHeightSampler sampler = field.Sampler();
    // Inside level-0 page (1, 1) and level-1 page (0, 0): level 2 answers alone.
    // Off every coarser lattice, so each level of the walk answers differently.
    const float32 u = 203.0f / float32(kFieldX - 1);
    const float32 v = 203.0f / float32(kFieldZ - 1);
    ASSERT_GT(std::fabs(DirectLevelSample(2, u, v) - DirectLevelSample(3, u, v)), 0.01f);
    uint32 resolved = 0;
    const float32 ancestor = sampler.SampleLevel(u, v, 2, resolved);
    EXPECT_EQ(resolved, 2u);
    EXPECT_EQ(sampler.Sample(u, v, 0.0f), ancestor);
    EXPECT_NEAR(ancestor, DirectLevelSample(2, u, v), 2e-4f);
    // Inside a resident level-0 page the level-0 data answers.
    const float32 u2 = 520.0f / float32(kFieldX - 1);
    EXPECT_NEAR(sampler.Sample(u2, v, 0.0f), DirectLevelSample(0, u2, v), 2e-4f);
}

TEST(PagedHeightResolve, HeightIsContinuousAcrossEveryLevelRing)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    const PagedHeightSampler sampler = field.Sampler();
    const float32 v = 0.61f;
    for (float32 u : {0.137f, 0.4f, 0.9f})
    {
        for (uint32 ring = 1; ring <= field.TopLevel(); ++ring)
        {
            // Just inside a ring's end and at the next level's start: the same height, the parent's.
            const float32 before = sampler.Sample(u, v, float32(ring) - 1e-4f);
            const float32 after = sampler.Sample(u, v, float32(ring));
            EXPECT_NEAR(before, after, 1e-2f) << "a pop at ring " << ring << " (u " << u << ")";
        }
    }
}

TEST(PagedHeightResolve, APageArrivingFadesInFromItsParentWithoutAPop)
{
    PagedField field(64);
    std::vector<PageAddress> withoutPage;
    for (const PageAddress& page : field.AllPages())
        if (!(page.Level == 0 && page.X == 2 && page.Z == 1))
            withoutPage.push_back(page);
    for (int frame = 0; frame < 40; ++frame)
        field.Step(withoutPage);
    const PagedHeightSampler sampler = field.Sampler();
    // An odd level-0 sample: not on the parent's lattice, so the two levels differ there.
    const float32 u = 301.0f / float32(kFieldX - 1);
    const float32 v = 191.0f / float32(kFieldZ - 1);
    ASSERT_GT(std::fabs(DirectLevelSample(0, u, v) - DirectLevelSample(1, u, v)), 0.01f);
    const float32 before = sampler.Sample(u, v, 0.0f);
    field.Step(field.AllPages()); // the page arrives
    EXPECT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 2, 1})), PageStreaming::kNoResidentSlot);
    EXPECT_EQ(sampler.Sample(u, v, 0.0f), before) << "the arrival frame must show the parent";
    for (int frame = 0; frame < 30; ++frame)
        field.Step(field.AllPages());
    EXPECT_NEAR(sampler.Sample(u, v, 0.0f), DirectLevelSample(0, u, v), 2e-4f);
}

TEST(PageCacheResidency, AParkedCameraSettlesToZeroWork)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    field.Step(all);
    EXPECT_GT(field.Cache.AssignsThisFrame(), 0u);
    for (int frame = 0; frame < 40; ++frame) // past the 0.4 s fades
        field.Step(all);
    for (int frame = 0; frame < 10; ++frame)
    {
        field.Step(all);
        EXPECT_EQ(field.Cache.AssignsThisFrame(), 0u);
        EXPECT_EQ(field.Cache.ReleasesThisFrame(), 0u);
        EXPECT_TRUE(field.Cache.Transitions().empty());
        EXPECT_EQ(field.Cache.ActiveFadesThisFrame(), 0u);
        EXPECT_EQ(field.LastTableWrites, 0u);
    }
}

TEST(PageCacheResidency, PagesArriveParentFirstAndAFullCacheDegradesByLevel)
{
    PagedField field(5); // the top two levels' 3 pages and two more
    const std::vector<PageAddress> all = field.AllPages();
    field.Step(all, 1.0f / 60.0f, 1); // one assign a frame: the top page first
    EXPECT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 3, 0, 0})), PageStreaming::kNoResidentSlot);
    for (int frame = 0; frame < 20; ++frame)
        field.Step(all);
    EXPECT_EQ(field.Cache.ResidentCount(), 5u);
    for (const PageStreaming::CachedPage& page : field.Cache.Resident())
        if (page.Address.Level < field.TopLevel())
            EXPECT_NE(field.Cache.SlotOf(page.Parent()), PageStreaming::kNoResidentSlot) << "a page without its parent";
    for (const PageAddress& page : all)
        if (page.Level >= 2)
            EXPECT_NE(field.Cache.SlotOf(PagedField::Page(page)), PageStreaming::kNoResidentSlot) << "a coarse page left out";
    EXPECT_GT(field.Cache.DeferredThisFrame(), 0u);
}

// ---- Review fold (PR #3072): parent first on release, hysteresis, pressure, page corners ---------------

TEST(PageCacheResidency, AParentIsNeverReleasedUnderAResidentChildPastTheHysteresis)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    std::vector<PageAddress> withoutParent;
    for (const PageAddress& page : all)
        if (!(page.Level == 1 && page.X == 0 && page.Z == 0))
            withoutParent.push_back(page);
    // Past the release hysteresis, so only the resident-child rule keeps the parent.
    for (int frame = 0; frame < 2 * int(PageStreaming::kPageReleaseHysteresisFrames); ++frame)
        field.Step(withoutParent);
    const bool childResident = field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 0, 0})) != PageStreaming::kNoResidentSlot;
    const bool parentResident = field.Cache.SlotOf(PagedField::Page(PageAddress{0, 1, 0, 0})) != PageStreaming::kNoResidentSlot;
    ASSERT_TRUE(childResident) << "the test needs the child resident";
    EXPECT_TRUE(parentResident) << "the cache released level-1 (0,0) under its resident child level-0 (0,0)";
}

TEST(PagedHeightResolve, PageCornersResolveToTheLattice)
{
    PagedField field(64);
    const std::vector<PageAddress> all = field.AllPages();
    for (int frame = 0; frame < 40; ++frame)
        field.Step(all);
    const PagedHeightSampler sampler = field.Sampler();
    for (uint32 level = 0; level < 2; ++level)
        for (float32 c : {127.0f, 127.5f, 127.99f, 128.0f, 128.5f, 255.5f, 256.0f, 256.5f})
        {
            const float32 u = c * float32(1u << level) / float32(kFieldX - 1);
            const float32 v = c * float32(1u << level) / float32(kFieldZ - 1);
            if (u > 1.0f || v > 1.0f)
                continue;
            EXPECT_NEAR(sampler.Sample(u, v, float32(level)), DirectLevelSample(level, u, v), 2e-4f)
                << "level " << level << " lattice corner " << c;
        }
}

TEST(PageCacheResidency, AFullCacheGivesANearerWantedPageASlotOverAFartherOne)
{
    PageStreaming::PageCache cache;
    cache.Configure(5, 2, 0.0f);
    constexpr uint32 kTop = 3;
    const auto want = [](uint8 level, uint32 x, uint32 z, float32 distance) {
        return PageStreaming::PageWant{PageAddress{0, level, x, z}, distance, false, 0u, level >= kTop};
    };
    std::vector<PageStreaming::PageWant> wants = {want(3, 0, 0, 0.0f), want(2, 0, 0, 0.0f), want(2, 1, 0, 0.0f),
                                                  want(1, 0, 0, 900.0f), want(1, 1, 0, 1000.0f)};
    uint64 frame = 0;
    for (int i = 0; i < 5; ++i)
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
    ASSERT_EQ(cache.ResidentCount(), 5u);
    wants.push_back(want(1, 2, 0, 1.0f)); // the camera arrives beside level-1 (2,0); its parent is resident
    for (int i = 0; i < 10; ++i)
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
    EXPECT_NE(cache.SlotOf(PageStreaming::CachedPage{0u, PageAddress{0, 1, 2, 0}}), PageStreaming::kNoResidentSlot)
        << "the page beside the camera stays without a slot while pages 900 and 1000 m away keep theirs";
    EXPECT_EQ(cache.SlotOf(PageStreaming::CachedPage{0u, PageAddress{0, 1, 1, 0}}), PageStreaming::kNoResidentSlot)
        << "the farthest settled page is the one that gives its slot up";
}

TEST(PageCacheResidency, ACameraHoveringAtARingBoundaryDoesNotChurnTheCache)
{
    const PageRequestTerrain terrain = UserTerrain();
    const PageLevelView view = NoteView();
    // Directly above the field's top at the height where level-0 pages stop being wanted.
    const float32 boundary = 2.0f * view.FocalPixels * terrain.Level0TexelX / view.TargetPixels;
    const uint32 top = static_cast<uint32>(PageStreaming::BuildPageStoreLevels(240000, 60000).size()) - 1u;
    PageStreaming::PageCache cache;
    cache.Configure(4096, 2, 0.4f);
    std::vector<PageStreaming::PageWant> wants;
    HeightPageRequester requester;
    requester.Configure(terrain);
    uint64 frame = 0;
    const auto step = [&](float32 height) {
        const Mathematics::Vector3 eye{30000.0f, terrain.MaxHeight + height, 7500.0f};
        requester.Build(std::span(&eye, 1), view, wants);
        for (PageStreaming::PageWant& want : wants)
            want.Top = want.Address.Level >= top;
        cache.Update(++frame, 1.0f / 60.0f, wants, 4096);
    };
    for (int i = 0; i < 60; ++i)
        step(boundary - 0.05f);
    uint32 releases = 0;
    uint32 assigns = 0;
    for (int i = 0; i < 20; ++i)
    {
        step(i % 2 == 0 ? boundary + 0.05f : boundary - 0.05f); // 10 cm of camera bob
        releases += cache.ReleasesThisFrame();
        assigns += cache.AssignsThisFrame();
    }
    EXPECT_EQ(releases, 0u) << "assigns " << assigns << " over 20 frames of a 10 cm bob at " << boundary << " m";

    // Leaving for good, the pages do go once the hysteresis has run out.
    for (int i = 0; i < 2 * int(PageStreaming::kPageReleaseHysteresisFrames); ++i)
        step(4.0f * boundary);
    EXPECT_EQ(cache.SlotOf(PageStreaming::CachedPage{0u, PageAddress{0, 0, 30000u * 4u / 128u, 7500u * 4u / 128u}}),
              PageStreaming::kNoResidentSlot);
}

TEST(PageCacheResidency, PressureNeverEvictsAPageWithAResidentChild)
{
    PageStreaming::PageCache cache;
    cache.Configure(4, 2, 0.0f);
    constexpr uint32 kTop = 3;
    const auto want = [](uint8 level, uint32 x, uint32 z, float32 distance) {
        return PageStreaming::PageWant{PageAddress{0, level, x, z}, distance, false, 0u, level >= kTop};
    };
    // A parent farther than its child (an unwanted parent kept by its child counts as infinitely far).
    std::vector<PageStreaming::PageWant> wants = {want(3, 0, 0, 0.0f), want(3, 1, 0, 0.0f), want(2, 0, 0, 6000.0f),
                                                  want(1, 0, 0, 5000.0f)};
    uint64 frame = 0;
    for (int i = 0; i < 5; ++i)
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
    ASSERT_EQ(cache.ResidentCount(), 4u);
    wants.push_back(want(2, 2, 0, 1.0f)); // parent level-3 (1,0) is resident
    for (int i = 0; i < 10; ++i)
    {
        cache.Update(++frame, 1.0f / 60.0f, wants, 64);
        for (const PageStreaming::CachedPage& page : cache.Resident())
            if (page.Address.Level < kTop)
                EXPECT_NE(cache.SlotOf(page.Parent()), PageStreaming::kNoResidentSlot)
                    << "level " << int(page.Address.Level) << " (" << page.Address.X << "," << page.Address.Z
                    << ") without its parent";
    }
}

// The shipped GLSL resolve (cbt_page.glsl, between its GE_SHARED_PAGE markers), compiled as C++
// over the same table and texels as the mirror: the taps the consumer defines read them directly.
namespace PageResolveGlsl
{
// Declarations, not a using-directive: they hide <cmath>'s global floor and friends.
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::floor;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::uint;

struct Bound
{
    const PageTable* Table = nullptr;
    const float32* Texels = nullptr;
    uint32 Dim = 0;
};
Bound s_Bound;

#include "PageResolveTypesExtracted.h"

uint CBT_PageTableEntry(uint index)
{
    return s_Bound.Table->Entries[index];
}

float CBT_PageSlotFade(uint slot)
{
    return s_Bound.Table->SlotFade[slot];
}

CBTPageLevel CBT_PageLevelShape(uint level)
{
    const PageStreaming::PageStoreLevel& shape = s_Bound.Table->Levels[level];
    return CBTPageLevel{shape.FirstEntry, shape.PagesX, shape.PagesZ, 0u};
}

float CBT_PageCacheBilinear(float texelX, float texelZ)
{
    return SampleGridBilinearTexel(s_Bound.Texels, s_Bound.Dim, s_Bound.Dim, texelX, texelZ);
}

#include "PageResolveExtracted.h"

float32 Sample(const PagedField& field, float32 u, float32 v, float32 level)
{
    s_Bound = Bound{&field.Table, field.Texels.data(), field.Geometry.CacheDim};
    CBTPageField shape{};
    shape.Level0SamplesX = field.Table.Levels.front().SamplesX;
    shape.Level0SamplesZ = field.Table.Levels.front().SamplesZ;
    shape.LevelCount = uint32(field.Table.Levels.size());
    shape.SlotsPerRow = field.Geometry.SlotsPerRow;
    return CBT_PageSample(u, v, level, shape);
}

} // namespace PageResolveGlsl

TEST(PagedHeightResolve, TheGlslResolveIsTheMirrorBitForBit)
{
    // Every state the walk can meet: a missing level-0 page and a missing level-1 page (the
    // fallback walk), and a page and its parent both mid-fade (the fade chain).
    PagedField field(64);
    std::vector<PageAddress> partial;
    for (const PageAddress& page : field.AllPages())
    {
        const bool held = (page.Level == 0 && page.X == 1 && page.Z == 1) ||
                          (page.Level == 1 && page.X == 0 && page.Z == 0) ||
                          (page.Level == 0 && page.X == 3 && page.Z == 2) ||
                          (page.Level == 1 && page.X == 1 && page.Z == 1);
        if (!held)
            partial.push_back(page);
    }
    for (int frame = 0; frame < 40; ++frame)
        field.Step(partial);
    std::vector<PageAddress> arriving = partial;
    arriving.push_back(PageAddress{0, 1, 1, 1});
    field.Step(arriving, 0.1f); // level-1 (1, 1) arrives and fades
    arriving.push_back(PageAddress{0, 0, 3, 2});
    field.Step(arriving, 0.1f); // its child arrives under it while it still fades
    ASSERT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 3, 2})), PageStreaming::kNoResidentSlot);
    ASSERT_LT(field.Cache.FadeOf(PagedField::Page(PageAddress{0, 1, 1, 1})), 1.0f);

    const PagedHeightSampler mirror = field.Sampler();
    uint32 compared = 0;
    for (float32 level : {0.0f, 0.3f, 0.8f, 0.9375f, 1.0f, 1.5f, 1.9f, 2.6f, 40.0f})
        for (uint32 i = 0; i <= 64; ++i)
            for (uint32 k = 0; k <= 48; ++k)
            {
                const float32 u = float32(i) / 64.0f;
                const float32 v = float32(k) / 48.0f;
                ASSERT_EQ(PageResolveGlsl::Sample(field, u, v, level), mirror.Sample(u, v, level))
                    << "uv " << u << "," << v << " level " << level;
                ++compared;
            }
    EXPECT_EQ(compared, 9u * 65u * 49u);
}
