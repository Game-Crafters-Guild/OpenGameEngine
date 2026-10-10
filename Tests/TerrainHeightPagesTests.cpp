// The height source of a tiled terrain (TerrainHeightIsPaged) and the main-thread half of its height
// pages (TerrainHeightPages): the predicate's arms, the GPU page-table words, the dirty rects that
// make the CBT re-evaluate the bisectors whose height changed source, a moved terrain's requests and
// the table versions; and the GPU side's latch (TerrainHeightPageFeature): a table is readable only
// with the upload pass of its pages, and the cache texture only from the latch after its clear.

#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainHeightSource.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/PageStoreFormat.h"
#include "PageStreaming/PageTableEntry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <gtest/gtest.h>

#include <bit>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{

// A terrain whose modifiers are baked into its page overlay, whose page source exists and whose
// table fits.
TerrainHeightSourceInputs BakedPageable()
{
    TerrainHeightSourceInputs inputs;
    inputs.ModifiersPending = false;
    inputs.GeneratedBase = false;
    inputs.CookedStoreOpen = true;
    inputs.PageTableFits = true;
    inputs.GpuReadsPages = true;
    return inputs;
}

// One flat tile of 257 samples: a pyramid of 3 x 3, 2 x 2 and 1 pages, all pinned in 64 slots.
PagedTerrainRequest FlatTileRequest()
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Flat;
    lattice.TileWorldSize = 256.0f;
    lattice.TileSamples = 257;
    lattice.TilesX = 1;
    lattice.TilesZ = 1;
    PagedTerrainRequest request;
    request.Terrain = 3;
    request.TerrainGeneration = 7;
    request.SourceIdentity = 11;
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.Level0TexelX = 1.0f;
    request.Shape.Level0TexelZ = 1.0f;
    request.Shape.SamplesX = 257;
    request.Shape.SamplesZ = 257;
    request.Shape.MaxHeight = 1.0f;
    return request;
}

bool CoversWholeTerrain(const HeightPageDirtyRect& rect)
{
    return rect.MinU == 0.0f && rect.MinV == 0.0f && rect.MaxU == 1.0f && rect.MaxV == 1.0f;
}

} // namespace

TEST(TerrainHeightSource, ATerrainWhoseModifiersAreNotBakedIntoItsPagesYetKeepsItsTexture)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.ModifiersPending = true;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs));
    inputs.GeneratedBase = true;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs)) << "pending modifiers keep even a generated base on its texture";
}

TEST(TerrainHeightSource, AHeightmapWithoutAnOpenStoreKeepsItsTexture)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.CookedStoreOpen = false;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs));
    inputs.CookedStoreOpen = true;
    inputs.PageTableFits = false;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs)) << "a table past the GPU ring slot keeps the texture";
}

TEST(TerrainHeightSource, TheNarrowCbtArmKeepsTheTexture)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    inputs.GpuReadsPages = false;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs));
    inputs.CookedStoreOpen = false;
    inputs.GeneratedBase = true;
    EXPECT_FALSE(TerrainHeightIsPaged(inputs)) << "nothing streams pages the narrow arm never reads";
}

TEST(TerrainHeightSource, AnOpenStoreOrAGeneratedBasePages)
{
    TerrainHeightSourceInputs inputs = BakedPageable();
    EXPECT_TRUE(TerrainHeightIsPaged(inputs));
    inputs.CookedStoreOpen = false;
    inputs.GeneratedBase = true;
    EXPECT_TRUE(TerrainHeightIsPaged(inputs));
}

TEST(TerrainHeightPages, PackedWordsCarryTheShapeFadesAndEntriesWhereTheResolveReadsThem)
{
    PageTable table;
    table.Reset(257, 257, 4);
    table.SetEntry(0, 1, 2, PageStreaming::PackPageEntry(2, 0));
    table.SlotFade[2] = 0.25f;
    const PageCacheGeometry geometry = MakePageCacheGeometry(4);

    std::vector<uint32> words;
    PackPageTableWords(table, geometry, words);
    ASSERT_EQ(words.size(), PageTableWordCount(257, 257, 4));
    EXPECT_EQ(words[0], 257u);
    EXPECT_EQ(words[1], 257u);
    EXPECT_EQ(words[2], table.Levels.size());
    EXPECT_EQ(words[3], geometry.SlotsPerRow);
    EXPECT_EQ(words[4], 4u);
    const uint32 levelsAt = 8u;
    const uint32 fadesAt = levelsAt + 4u * 32u;
    for (uint32 level = 0; level < table.Levels.size(); ++level)
    {
        EXPECT_EQ(words[levelsAt + 4u * level], table.Levels[level].FirstEntry);
        EXPECT_EQ(words[levelsAt + 4u * level + 1u], table.Levels[level].PagesX);
        EXPECT_EQ(words[levelsAt + 4u * level + 2u], table.Levels[level].PagesZ);
    }
    EXPECT_EQ(std::bit_cast<float32>(words[fadesAt + 2u]), 0.25f);
    const uint32 entriesAt = fadesAt + 4u;
    const uint32 pageAt = entriesAt + 2u * table.Levels[0].PagesX + 1u;
    EXPECT_EQ(words[pageAt], PageStreaming::PackPageEntry(2, 0)) << "level 0, page (1, 2), row-major";
    EXPECT_EQ(words[entriesAt], PageStreaming::kNoPage);
}

TEST(TerrainHeightPages, ATerrainDirtiesItsRectWhenItsSourceSwitchesOrItsPagesChangeAndNothingOnceSettled)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap});
    const PagedTerrainRequest request = FlatTileRequest();
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(257, 257);
    const uint32 pyramidPages = levels.back().FirstEntry + levels.back().PagesX * levels.back().PagesZ;

    pages.Update(1, 1.0f / 60.0f, false, 2); // configures the field's cache
    pages.DirtyRects().clear();

    // The terrain turns paged: its whole rect re-evaluates at once, then again while its pages
    // arrive and fade in (0.4 s); a settled parked terrain publishes nothing.
    uint32 uploads = 0;
    uint64 lastDirtyFrame = 0;
    bool wholeOnSwitch = false;
    for (uint64 frame = 2; frame < 62; ++frame)
    {
        ASSERT_EQ(pages.NeedsSource(request.Terrain, request.SourceIdentity), frame == 2);
        pages.Request(request, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        uploads += static_cast<uint32>(pages.Uploads().size());
        for (const HeightPageDirtyRect& rect : pages.DirtyRects())
        {
            EXPECT_EQ(rect.Terrain, request.Terrain);
            EXPECT_EQ(rect.TerrainGeneration, request.TerrainGeneration);
            wholeOnSwitch = wholeOnSwitch || (frame == 2 && CoversWholeTerrain(rect));
            lastDirtyFrame = frame;
        }
        pages.DirtyRects().clear();
        pages.Uploads().clear();
    }
    EXPECT_TRUE(wholeOnSwitch);
    EXPECT_EQ(uploads, pyramidPages) << "every page of the pinned pyramid uploads once";
    EXPECT_GT(lastDirtyFrame, 2u + 20u) << "the arrival fades publish their pages for 0.4 s";
    EXPECT_LT(lastDirtyFrame, 2u + 40u) << "a settled parked terrain publishes nothing";
    uint64 version = 0;
    ASSERT_NE(pages.TableWords(request.Terrain, version), nullptr);

    // Not requested for a frame: it goes back to its texture, its whole rect re-evaluated.
    pages.Update(62, 1.0f / 60.0f, false, 2);
    ASSERT_EQ(pages.DirtyRects().size(), 1u);
    EXPECT_TRUE(CoversWholeTerrain(pages.DirtyRects().front()));
    EXPECT_EQ(pages.TableWords(request.Terrain, version), nullptr);
}

TEST(TerrainHeightPages, AnOverlayOfRebuiltModifierDataRewritesEveryPageTheOldOverlayReached)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap});
    const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(257, 257);
    const PageStreaming::PageSampleRect raised{240, 240, 256, 256}; // the far corner: nine pages read it
    PagedTerrainRequest modified = FlatTileRequest();
    modified.Overlay = std::make_shared<const PageStreaming::HeightPageOverlay>(
        levels, raised, std::vector<float32>(raised.Width() * raised.Height(), 5.0f));
    modified.OverlayVersion = 3;
    modified.OverlayChanged = raised;

    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    for (; frame < 62; ++frame)
    {
        pages.Request(modified, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }

    // A scene load rebuilt the terrain's data with no modifier: its first overlay version (none
    // built) is not derived from the one the resident pages hold.
    const PagedTerrainRequest rebuilt = FlatTileRequest();
    uint32 rewritten = 0;
    for (; frame < 72; ++frame)
    {
        pages.Request(rebuilt, {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        for (const HeightPageUpload& upload : pages.Uploads())
        {
            ++rewritten;
            for (float32 sample : upload.Samples)
                ASSERT_LT(sample, 1.0f) << "a rewritten page carries no overlay";
        }
    }
    uint32 reached = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].PagesZ; ++z)
            for (uint32 x = 0; x < levels[level].PagesX; ++x)
                reached += PageStreaming::PageReadsLevel0Rect(
                               PageStreaming::PageAddress{0, static_cast<uint8>(level), x, z}, raised)
                               ? 1u
                               : 0u;
    EXPECT_EQ(rewritten, reached) << "every page of the pinned pyramid that read the old overlay, and no other";
}

namespace
{

// A flat 4097 x 4097 field (16 x 16 tiles of 257) in 64 slots: too many pages to pin whole, so its
// level 0 streams around the camera.
PagedTerrainRequest StreamedFieldRequest(float32 originX, uint64 identity)
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = PageStreaming::GeneratedHeightSource::Flat;
    lattice.TileWorldSize = 256.0f;
    lattice.TileSamples = 257;
    lattice.TilesX = 16;
    lattice.TilesZ = 16;
    PagedTerrainRequest request;
    request.Terrain = 0;
    request.TerrainGeneration = 1;
    request.SourceIdentity = identity;
    request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
    request.Shape.OriginX = originX;
    request.Shape.Level0TexelX = 1.0f;
    request.Shape.Level0TexelZ = 1.0f;
    request.Shape.SamplesX = 4097;
    request.Shape.SamplesZ = 4097;
    request.Shape.MaxHeight = 1.0f;
    return request;
}

// The published table entry of level-0 page (x, z) (the binding-22 layout the packing test pins).
uint32 Level0Entry(const TerrainHeightPages& pages, uint32 terrain, uint32 x, uint32 z)
{
    uint64 version = 0;
    const std::vector<uint32>* words = pages.TableWords(terrain, version);
    if (words == nullptr)
        return PageStreaming::kNoPage;
    const uint32 entriesAt = 8u + 4u * 32u + (*words)[4];
    const uint32 pagesX = (*words)[8u + 1u];
    return (*words)[entriesAt + z * pagesX + x];
}

} // namespace

TEST(TerrainHeightPages, AMovedTerrainStreamsThePagesUnderTheCameraAtItsNewPlacement)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap});
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    uint64 frame = 2;
    const PagedTerrainRequest before = StreamedFieldRequest(0.0f, 77);
    const Mathematics::Vector3 cameraBefore{100.0f, 10.0f, 100.0f};
    for (; frame < 40; ++frame)
    {
        pages.Request(before, {&cameraBefore, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    ASSERT_NE(Level0Entry(pages, 0, 0, 0), PageStreaming::kNoPage) << "page (0, 0) streams in under the camera";
    pages.DirtyRects().clear();

    // The terrain moves 3 km east with the same source; the camera moves with it.
    const PagedTerrainRequest after = StreamedFieldRequest(3000.0f, 77);
    const Mathematics::Vector3 cameraAfter{3100.0f, 10.0f, 100.0f};
    pages.Request(after, {&cameraAfter, 1}, view);
    pages.Update(frame++, 1.0f / 60.0f, false, 2);
    ASSERT_FALSE(pages.DirtyRects().empty());
    EXPECT_TRUE(CoversWholeTerrain(pages.DirtyRects().front())) << "every corner moved with the terrain";
    for (; frame < 200; ++frame)
    {
        pages.Request(after, {&cameraAfter, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
    }
    EXPECT_NE(Level0Entry(pages, 0, 0, 0), PageStreaming::kNoPage) << "the level-0 page under the camera";
    EXPECT_EQ(Level0Entry(pages, 0, 23, 0), PageStreaming::kNoPage)
        << "the page 2.9 km away, under the camera's place at the old placement, is released";
}

TEST(TerrainHeightPages, ATerrainPagedAgainAtTheSameIndexNeverRepeatsATableVersion)
{
    TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap});
    const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
    pages.Update(1, 1.0f / 60.0f, false, 2);
    const Mathematics::Vector3 camera{100.0f, 10.0f, 100.0f};
    std::vector<uint64> firstVersions;
    uint64 frame = 2;
    for (; frame < 6; ++frame)
    {
        pages.Request(StreamedFieldRequest(0.0f, 77), {&camera, 1}, view);
        pages.Update(frame, 1.0f / 60.0f, false, 2);
        uint64 version = 0;
        ASSERT_NE(pages.TableWords(0, version), nullptr);
        firstVersions.push_back(version);
    }
    pages.Update(frame++, 1.0f / 60.0f, false, 2); // not requested: forgotten
    pages.Request(StreamedFieldRequest(0.0f, 78), {&camera, 1}, view);
    pages.Update(frame, 1.0f / 60.0f, false, 2);
    uint64 version = 0;
    ASSERT_NE(pages.TableWords(0, version), nullptr);
    for (uint64 seen : firstVersions)
        EXPECT_NE(version, seen) << "the CBT's per-ring-slot upload check would keep the old terrain's table";
}

TEST(TerrainHeightPages, The60By15KmTerrainAtAQuarterMeterPagesOnDesktopAndTheDeckRefusesItWithTheFix)
{
    // 60 x 15 km at 0.25 m is 240,001 x 60,001 samples: 1,174,877 words with the desktop's 512 slots,
    // under its cap of 2^21; 1,174,621 with the Deck's 256, over its cap of 2^19.
    EXPECT_EQ(PageTableWordCount(240001, 60001, kDesktopHeightPages.Slots), 1174877u);
    EXPECT_EQ(PageTableRefusal("Outer", 240001, 60001, kDesktopHeightPages), "");
    const std::string refusal = PageTableRefusal("Outer", 240001, 60001, kSteamDeckHeightPages);
    EXPECT_NE(refusal.find("Outer (240001 x 60001 samples)"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("needs 1174621 words"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("cap of 524288 words"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Lower its samples per meter or its size"), std::string::npos) << refusal;
    // The DEM project's terrains (8193 x 2049 samples) page on every platform.
    EXPECT_EQ(PageTableRefusal("Near", 8193, 2049, kDesktopHeightPages), "");
    EXPECT_EQ(PageTableRefusal("Near", 8193, 2049, kSteamDeckHeightPages), "");
}

TEST(TerrainHeightPages, A1KmSquareModifierOnAQuarterMeterTerrainFitsEveryOverlayCapAndATerrainWideOneIsRefused)
{
    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(240001, 60001);
    // 1 km x 1 km at 0.25 m: 4001 x 4001 samples and the bake's one-sample margin on each side.
    const uint64 local = PageStreaming::HeightPageOverlay::Bytes(levels, {100000, 20000, 104002, 24002});
    EXPECT_GT(local, 4003ull * 4003ull * sizeof(float32)) << "the coarser levels hold samples too";
    EXPECT_EQ(OverlayRefusal("Outer", local, kDesktopHeightPages), "");
    EXPECT_EQ(OverlayRefusal("Outer", local, kSteamDeckHeightPages), "");
    // A modifier over the whole 60 x 15 km terrain reaches all of its 14.4 G samples.
    const uint64 wide = PageStreaming::HeightPageOverlay::Bytes(levels, {0, 0, 240000, 60000});
    const std::string refusal = OverlayRefusal("Outer", wide, kDesktopHeightPages);
    EXPECT_NE(refusal.find("Outer does not stream its height"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find(std::format("needs {} bytes", wide)), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("cap of 536870912 bytes"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Shrink the modifiers' bounds"), std::string::npos) << refusal;
}

// A power-of-two raw heightmap (the 0.25 m canyon, 16384 x 4096) is off its 16 x 4 tile lattice of
// 1025-sample tiles (16385 x 4097): it pages, but takes no modifier overlay, and the refusal names
// the lattice to import at.
TEST(TerrainHeightPages, AStoreOffTheTileLatticeTakesNoModifierOverlayAndTheRefusalNamesTheLattice)
{
    const HeightPageOverlayAllowance off = PageOverlayAllowance(true, 16384, 4096, 16385, 4097, kDesktopHeightPages);
    EXPECT_EQ(off.ByteCap, kDesktopHeightPages.OverlayByteCap);
    EXPECT_TRUE(off.OffGrid);
    const HeightPageOverlayAllowance on = PageOverlayAllowance(true, 8193, 2049, 8193, 2049, kDesktopHeightPages);
    EXPECT_EQ(on.ByteCap, kDesktopHeightPages.OverlayByteCap);
    EXPECT_FALSE(on.OffGrid);
    const HeightPageOverlayAllowance texture = PageOverlayAllowance(false, 16384, 4096, 16385, 4097, kDesktopHeightPages);
    EXPECT_EQ(texture.ByteCap, 0u) << "a terrain that keeps its texture holds no overlay";

    const std::string refusal = OverlayGridRefusal("Canyon", 16384, 4096, 16385, 4097);
    EXPECT_NE(refusal.find("Canyon does not stream its height while height modifiers reach it"), std::string::npos)
        << refusal;
    EXPECT_NE(refusal.find("(16384 x 4096 samples)"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("Import the heightmap at 16385 x 4097 samples"), std::string::npos) << refusal;
}

TEST(TerrainHeightPageFeature, ATableBecomesReadableOnlyWithTheUploadPassThatRecordsItsPages)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap});
        const PagedTerrainRequest request = FlatTileRequest();
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        pages.Update(1, 1.0f / 60.0f, false, 2);
        for (uint64 frame = 2; frame < 20 && pages.Uploads().empty(); ++frame)
        {
            pages.Request(request, {&camera, 1}, view);
            pages.Update(frame, 1.0f / 60.0f, false, 2);
        }
        ASSERT_FALSE(pages.Uploads().empty());
        uint64 published = 0;
        ASSERT_NE(pages.TableWords(request.Terrain, published), nullptr);

        TerrainHeightPageFeature feature;
        feature.Publish(pages);
        feature.Latch(*device, 1);
        ASSERT_TRUE(feature.HasStagedUploads());
        std::span<const uint32> words;
        uint64 version = 0;
        EXPECT_FALSE(feature.LatchedTable(request.Terrain, words, version))
            << "the table names slots whose upload pass is not declared yet";
        feature.Latch(*device, 2); // a frame whose graph declared no upload pass
        EXPECT_FALSE(feature.LatchedTable(request.Terrain, words, version));

        feature.CommitLatch(); // the upload pass that flushes the staged pages is declared
        ASSERT_TRUE(feature.LatchedTable(request.Terrain, words, version));
        EXPECT_EQ(version, published);
        auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        list->Begin();
        feature.FlushUploads(*list);
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();
        EXPECT_FALSE(feature.HasStagedUploads());
    }
    device->Shutdown();
}

TEST(TerrainHeightPageFeature, TheCacheTextureIsReadableOnlyFromTheLatchAfterItsClear)
{
    Rendering::DeviceDesc desc{};
    desc.preferredAPI = Rendering::GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    std::unique_ptr<Rendering::IDevice> device = Rendering::DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";
    {
        TerrainHeightPages pages(nullptr, nullptr, HeightPageProfile{64, kDesktopHeightPages.TableWordCap});
        const Mathematics::Vector3 camera{128.0f, 10.0f, 128.0f};
        const PageLevelView view = MakePageLevelView(kPageLevelReferenceRows, 1.0f, 11.0f);
        pages.Update(1, 1.0f / 60.0f, false, 2);
        for (uint64 frame = 2; frame < 20 && pages.Uploads().empty(); ++frame)
        {
            pages.Request(FlatTileRequest(), {&camera, 1}, view);
            pages.Update(frame, 1.0f / 60.0f, false, 2);
        }
        ASSERT_FALSE(pages.Uploads().empty());

        TerrainHeightPageFeature feature;
        feature.Publish(pages);
        feature.Latch(*device, 1); // creates the cache and stages its first pages
        ASSERT_TRUE(feature.HasStagedUploads());
        EXPECT_FALSE(feature.CacheTexture().IsValid())
            << "a consumer of this frame could run before the upload pass clears the new cache";
        feature.Latch(*device, 2); // a frame whose upload pass never ran
        EXPECT_FALSE(feature.CacheTexture().IsValid()) << "the cache was never cleared";
        feature.CommitLatch();
        auto list = device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
        list->Begin();
        feature.FlushUploads(*list);
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();
        EXPECT_FALSE(feature.CacheTexture().IsValid()) << "the clear is recorded in this frame";
        feature.Latch(*device, 3);
        EXPECT_TRUE(feature.CacheTexture().IsValid());
    }
    device->Shutdown();
}
