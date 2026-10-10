// PageStoreTests: the terrain page store (Engine/Modules/PageStreaming) as pure oracles. The store
// round trip, the apron's bit-equality under the store-wide quantum, the sparse resolve, the
// incremental re-cook, the lattice-preserving tent, the generated provider's level 0 against the
// terrain's own tile fill, the cook key, the encoding rule, the terrain container, the PNG limit
// and the asynchronous loader.

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/HeightPageCodec.h"
#include "PageStreaming/HeightPageCooker.h"
#include "PageStreaming/HeightPageOverlay.h"
#include "PageStreaming/HeightStoreCache.h"
#include "PageStreaming/HeightStoreKey.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageResidency.h"
#include "PageStreaming/PageStoreFormat.h"
#include "PageStreaming/PageStoreReader.h"
#include "PageStreaming/PageStoreWriter.h"
#include "PageStreaming/PageTableEntry.h"
#include "PageStreaming/TerrainPageContainer.h"

#include "Assets/AssetIOService.h"
#include "CBTTerrain/CBTLayout.h"
#include "Components/Terrain/Terrain.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/PageLevelRule.h"
#include "TerrainECS/TerrainService.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PageStreaming;

namespace
{

constexpr auto kLoadTimeout = std::chrono::seconds(10);

using HeightFunction = std::function<float32(uint32 x, uint32 z)>;

void WriteR32(const std::filesystem::path& file, uint32 width, uint32 height, const HeightFunction& heightAt)
{
    std::vector<float32> samples(static_cast<std::size_t>(width) * height);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
            samples[static_cast<std::size_t>(z) * width + x] = heightAt(x, z);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4u));
}

std::vector<float32> ReadR32(const std::filesystem::path& file, uint32 width, uint32 height)
{
    std::vector<float32> samples(static_cast<std::size_t>(width) * height);
    std::ifstream in(file, std::ios::binary);
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4u));
    return samples;
}

// A smooth relief in [100, 400] m: a 300 m range, so the store is 16-bit.
float32 Relief(uint32 x, uint32 z)
{
    return 250.0f + 120.0f * std::sin(0.031f * static_cast<float32>(x)) * std::cos(0.017f * static_cast<float32>(z)) +
           30.0f * std::sin(0.11f * static_cast<float32>(x + 3u * z));
}

std::vector<uint8> FileBytes(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32 ClampSample(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

class PageStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Dir = TestUtils::MakeUniqueTempDirectory("page_store_test");
        std::filesystem::create_directories(m_Dir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    // Scans and cooks `source` into `output`, patching `previous` when it can.
    HeightCookResult Cook(const std::filesystem::path& source, uint32 width, uint32 height,
                          const std::filesystem::path& output, const std::filesystem::path& previous = {},
                          JobSystem::WorkStealingThreadPool* pool = nullptr, AssetIOService* io = nullptr)
    {
        HeightCookRequest request;
        EXPECT_EQ(ResolveHeightCookSource(source, width, height, request.Source), "");
        EXPECT_EQ(ScanHeightSource(request.Source, io, request.Identity), "");
        request.Key = ComputeHeightStoreKey(
            {request.Identity.ContentHash, request.Source.Format, request.Source.SamplesX, request.Source.SamplesZ});
        request.Output = output;
        request.Previous = previous;
        request.Pool = pool;
        request.Io = io;
        return CookHeightPageStore(request);
    }

    std::filesystem::path m_Dir;
};

std::vector<float32> PageSamples(const PageStoreReader& reader, const PageAddress& address)
{
    std::vector<float32> samples(kPageSampleCount);
    EXPECT_TRUE(reader.ReadHeightPage(address, samples));
    return samples;
}

// Every stored level-N sample of a store, as one row-major grid of the level (owned samples only).
std::vector<float32> LevelGrid(const PageStoreReader& reader, uint8 level)
{
    const PageStoreLevel& shape = reader.Layout().Levels[level];
    std::vector<float32> grid(static_cast<std::size_t>(shape.SamplesX) * shape.SamplesZ);
    for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
    {
        for (uint32 px = 0; px < shape.PagesX; ++px)
        {
            const std::vector<float32> page = PageSamples(reader, PageAddress{0, level, px, pz});
            for (uint32 r = 0; r < kPageOwnedSamples; ++r)
            {
                for (uint32 c = 0; c < kPageOwnedSamples; ++c)
                {
                    const uint32 x = px * kPageOwnedSamples + c;
                    const uint32 z = pz * kPageOwnedSamples + r;
                    if (x < shape.SamplesX && z < shape.SamplesZ)
                        grid[static_cast<std::size_t>(z) * shape.SamplesX + x] =
                            page[static_cast<std::size_t>(r + 1u) * kPageStrideSamples + c + 1u];
                }
            }
        }
    }
    return grid;
}

} // namespace

// ---- Format ----------------------------------------------------------------------------------------

TEST(PageStoreFormatTest, EntryBitPackAndSentinelMatchTheSculptPageTable)
{
    static_assert(kNoPage == CBTTerrain::kSculptNoPage);
    static_assert(kPageSlotMask == CBTTerrain::kSculptPageIdMask);
    static_assert(kPageLevelShift == CBTTerrain::kSculptPageLevelShift);
    const uint32 entry = PackPageEntry(0x00ABCDEFu, 7u);
    EXPECT_EQ(PageEntrySlot(entry), 0x00ABCDEFu);
    EXPECT_EQ(PageEntryLevel(entry), 7u);
    EXPECT_NE(PackPageEntry(kPageSlotMask, 254u), kNoPage);
}

TEST(PageStoreFormatTest, PyramidOfTheSixtyByFifteenKilometerDemHasTheNotesPageCounts)
{
    // 60 x 15 km at 0.25 m: the design note's 12 levels, 1,875 x 469 level-0 pages, 1,173,760 in all.
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(240000u, 60000u);
    ASSERT_EQ(levels.size(), 12u);
    EXPECT_EQ(levels[0].PagesX, 1875u);
    EXPECT_EQ(levels[0].PagesZ, 469u);
    EXPECT_EQ(levels[6].SamplesX, 3751u); // the first pinned level (16 m texels)
    EXPECT_EQ(levels.back().PagesX, 1u);
    EXPECT_EQ(levels.back().PagesZ, 1u);
    EXPECT_EQ(levels.back().FirstEntry + 1u, 1173760u);
}

TEST(PageStoreFormatTest, AbsoluteHeightsAreSixteenBitUpToSixHundredFiftyFiveMetersOfRange)
{
    const HeightEncoding narrow = ChooseAbsoluteHeightEncoding(171.6f, 304.3f); // the DEM's outer terrain
    EXPECT_EQ(narrow.Format, PageFieldFormat::HeightR16);
    EXPECT_LE(narrow.Quantum.Step, kHeightStepMax);
    EXPECT_NEAR(narrow.Quantum.Step, 0.00203f, 0.00001f);
    EXPECT_EQ(ChooseAbsoluteHeightEncoding(0.0f, 655.0f).Format, PageFieldFormat::HeightR16);
    EXPECT_EQ(ChooseAbsoluteHeightEncoding(0.0f, 656.0f).Format, PageFieldFormat::HeightR32F);
    // The boundary itself: 655.35 m is 65535 steps of 1 cm, 655.36 m is not.
    const HeightEncoding boundary = ChooseAbsoluteHeightEncoding(0.0f, 655.35f);
    EXPECT_EQ(boundary.Format, PageFieldFormat::HeightR16);
    EXPECT_LE(boundary.Quantum.Step, kHeightStepMax);
    EXPECT_EQ(ChooseAbsoluteHeightEncoding(0.0f, 655.36f).Format, PageFieldFormat::HeightR32F);
    EXPECT_EQ(ChooseAbsoluteHeightEncoding(-4000.0f, 4000.0f).Format, PageFieldFormat::HeightR32F);
    const HeightEncoding flat = ChooseAbsoluteHeightEncoding(12.0f, 12.0f);
    EXPECT_EQ(flat.Format, PageFieldFormat::HeightR16);
    EXPECT_GT(flat.Quantum.Step, 0.0f);
}

TEST(PageStoreFormatTest, NormalizedWordsDecodeToTodaysSixteenBitValues)
{
    const HeightQuantum quantum = NormalizedHeightEncoding().Quantum;
    constexpr float32 kInv65535 = 1.0f / 65535.0f; // Terrain/Heightfield.cpp's decode
    for (uint32 word = 0; word <= 65535u; word += 97u)
    {
        const float32 today = static_cast<float32>(word) * kInv65535;
        EXPECT_EQ(std::bit_cast<uint32>(quantum.Decode(static_cast<uint16>(word))), std::bit_cast<uint32>(today));
        EXPECT_EQ(quantum.Encode(today), word);
    }
}

TEST_F(PageStoreTest, StoreRoundTripsHeaderIndexAndPagesInMortonOrder)
{
    PageStoreHeader header;
    header.Format = PageFieldFormat::HeightR32F;
    header.SamplesX = 300;
    header.SamplesZ = 200;
    header.Key = 0x1234567890ABCDEFull;
    const std::filesystem::path file = m_Dir / "roundtrip.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, MakePageStoreLayout(header)), "");

    const PageStoreLayout& layout = writer.Layout();
    std::vector<std::vector<float32>> written;
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
    {
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
        {
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px)
            {
                std::vector<float32> samples(kPageSampleCount);
                for (uint32 i = 0; i < kPageSampleCount; ++i)
                    samples[i] = static_cast<float32>(level * 1000000u + pz * 10000u + px * 100u) + 0.5f * i;
                EncodedPage page;
                EncodeHeightPage(header, samples, samples.front(), samples.back(), page);
                ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
                written.push_back(std::move(samples));
            }
        }
    }
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file), "");
    EXPECT_EQ(reader.Layout().Header.Key, header.Key);
    EXPECT_EQ(reader.Layout().Header.SamplesX, 300u);
    ASSERT_EQ(reader.Layout().Levels.size(), 3u); // 300 x 200 -> 151 x 101 -> 76 x 51
    std::size_t next = 0;
    for (uint8 level = 0; level < reader.Layout().Levels.size(); ++level)
        for (uint32 pz = 0; pz < reader.Layout().Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < reader.Layout().Levels[level].PagesX; ++px)
                EXPECT_EQ(PageSamples(reader, PageAddress{0, level, px, pz}), written[next++]);
    EXPECT_EQ(std::filesystem::file_size(file), reader.Layout().TotalBytes());

    // Level 0 (3 x 2 pages) lies in Morton order: (0,0) (1,0) (0,1) (1,1) (2,0) (2,1).
    const auto offset = [&](uint32 x, uint32 z) { return reader.Layout().FindPresent(PageAddress{0, 0, x, z})->Offset; };
    EXPECT_LT(offset(0, 0), offset(1, 0));
    EXPECT_LT(offset(1, 0), offset(0, 1));
    EXPECT_LT(offset(0, 1), offset(1, 1));
    EXPECT_LT(offset(1, 1), offset(2, 0));
    EXPECT_LT(offset(2, 0), offset(2, 1));
}

TEST_F(PageStoreTest, TruncatedOrForeignFilesAreRefusedWithAReason)
{
    PageStoreHeader header;
    header.SamplesX = 200;
    header.SamplesZ = 200;
    const std::filesystem::path file = m_Dir / "short.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, MakePageStoreLayout(header)), "");
    std::vector<float32> samples(kPageSampleCount, 1.0f);
    EncodedPage page;
    EncodeHeightPage(header, samples, 1.0f, 1.0f, page);
    for (uint8 level = 0; level < writer.Layout().Levels.size(); ++level)
        for (uint32 pz = 0; pz < writer.Layout().Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < writer.Layout().Levels[level].PagesX; ++px)
                ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
    ASSERT_EQ(writer.Finish(), "");

    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 1u);
    PageStoreReader reader;
    EXPECT_NE(reader.Open(file).find("outside the store"), std::string::npos);
    EXPECT_FALSE(reader.IsOpen());

    const std::filesystem::path foreign = m_Dir / "foreign.gepage";
    std::ofstream(foreign, std::ios::binary) << std::string(64, 'x');
    EXPECT_NE(reader.Open(foreign).find("not a page store"), std::string::npos);
}

TEST_F(PageStoreTest, AbsentPagesResolveToTheirNearestPresentAncestor)
{
    PageStoreHeader header;
    header.SamplesX = 300;
    header.SamplesZ = 300;
    std::vector<uint8> present(MakePageStoreLayout(header).Entries.size(), 1u);
    // Level 0 is 3 x 3 pages; the near field covers all but the center page, and level 1 (2 x 2)
    // lacks the page above that center, so the center resolves two levels up.
    present[1u * 3u + 1u] = 0;
    present[9u + 0u] = 0;
    const PageStoreLayout layout = MakePageStoreLayout(header, present);

    EXPECT_EQ(layout.FindPresent(PageAddress{0, 0, 1, 1}), nullptr);
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 1, 1}), (PageAddress{0, 2, 0, 0}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 2, 2}), (PageAddress{0, 0, 2, 2}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 0, 0}), (PageAddress{0, 0, 0, 0}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 1, 0, 0}), (PageAddress{0, 2, 0, 0}));
    EXPECT_EQ(layout.ResolvePresent(PageAddress{0, 0, 3, 0}), std::nullopt);

    const std::filesystem::path file = m_Dir / "sparse.gepage";
    PageStoreWriter writer;
    ASSERT_EQ(writer.Create(file, layout), "");
    std::vector<float32> samples(kPageSampleCount, 7.0f);
    EncodedPage page;
    EncodeHeightPage(header, samples, 7.0f, 7.0f, page);
    EXPECT_FALSE(writer.WritePage(PageAddress{0, 0, 1, 1}, page));
    for (uint8 level = 0; level < layout.Levels.size(); ++level)
        for (uint32 pz = 0; pz < layout.Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < layout.Levels[level].PagesX; ++px)
                if (layout.FindPresent(PageAddress{0, level, px, pz}))
                    ASSERT_TRUE(writer.WritePage(PageAddress{0, level, px, pz}, page));
    ASSERT_EQ(writer.Finish(), "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(file), "");
    std::vector<float32> out(kPageSampleCount);
    EXPECT_FALSE(reader.ReadHeightPage(PageAddress{0, 0, 1, 1}, out));
    EXPECT_EQ(reader.Layout().ResolvePresent(PageAddress{0, 0, 1, 1}), (PageAddress{0, 2, 0, 0}));
    // Absent pages take no bytes: 9 + 4 + 1 pages less the two absent ones.
    EXPECT_EQ(reader.Layout().TotalBytes(),
              reader.Layout().DataOffset() + 12u * PageStoredBytes(PageFieldFormat::HeightR32F));
}

// ---- Cooking ---------------------------------------------------------------------------------------

TEST_F(PageStoreTest, CookedLevelZeroIsTheSourceInTheStoreWideQuantumWithEdgeClampedAprons)
{
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 260;
    const std::filesystem::path source = m_Dir / "relief.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const std::filesystem::path store = m_Dir / "relief.gepage";
    const HeightCookResult result = Cook(source, kWidth, kHeight, store);
    ASSERT_EQ(result.Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store), "");
    const PageStoreHeader& header = reader.Layout().Header;
    ASSERT_EQ(header.Format, PageFieldFormat::HeightR16);
    EXPECT_EQ(header.Units, PageHeightUnits::Absolute);
    EXPECT_EQ(result.PagesWritten, reader.Layout().Entries.size());

    const std::vector<float32> input = ReadR32(source, kWidth, kHeight);
    for (uint32 pz = 0; pz < 3u; ++pz)
    {
        for (uint32 px = 0; px < 3u; ++px)
        {
            const std::vector<float32> page = PageSamples(reader, PageAddress{0, 0, px, pz});
            for (uint32 r = 0; r < kPageStrideSamples; ++r)
            {
                for (uint32 c = 0; c < kPageStrideSamples; ++c)
                {
                    const uint32 x = ClampSample(static_cast<int64>(px) * kPageOwnedSamples + c - 1, kWidth);
                    const uint32 z = ClampSample(static_cast<int64>(pz) * kPageOwnedSamples + r - 1, kHeight);
                    const float32 expected =
                        header.Quantum.Decode(header.Quantum.Encode(input[static_cast<std::size_t>(z) * kWidth + x]));
                    ASSERT_EQ(page[static_cast<std::size_t>(r) * kPageStrideSamples + c], expected)
                        << "page " << px << "," << pz << " stride " << c << "," << r;
                }
            }
        }
    }
}

TEST_F(PageStoreTest, ApronSamplesAreBitEqualToTheNeighborsOwnedSamplesAtEveryLevel)
{
    // A relief whose pages each span a different height range: under a per-page quantum the same
    // sample would encode to different words in the two pages that store it.
    constexpr uint32 kWidth = 520;
    constexpr uint32 kHeight = 390;
    const std::filesystem::path source = m_Dir / "apron.r32";
    WriteR32(source, kWidth, kHeight, [](uint32 x, uint32 z) { return Relief(x, z) + 0.37f * static_cast<float32>(x); });
    const std::filesystem::path store = m_Dir / "apron.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store), "");
    ASSERT_EQ(reader.Layout().Header.Format, PageFieldFormat::HeightR16);
    std::size_t pairs = 0;
    for (uint8 level = 0; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        {
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                // Decoded values, compared bit for bit: what two pages' samplers read.
                const std::vector<float32> page = PageSamples(reader, PageAddress{0, level, px, pz});
                const auto word = [&](const std::vector<float32>& p, uint32 c, uint32 r) {
                    return std::bit_cast<uint32>(p[static_cast<std::size_t>(r) * kPageStrideSamples + c]);
                };
                if (px + 1u < shape.PagesX)
                {
                    const std::vector<float32> right = PageSamples(reader, PageAddress{0, level, px + 1u, pz});
                    for (uint32 r = 0; r < kPageStrideSamples; ++r)
                    {
                        ASSERT_EQ(word(page, kPageStrideSamples - 1u, r), word(right, 1u, r)) << int(level);
                        ASSERT_EQ(word(page, kPageStrideSamples - 2u, r), word(right, 0u, r)) << int(level);
                    }
                    ++pairs;
                }
                if (pz + 1u < shape.PagesZ)
                {
                    const std::vector<float32> above = PageSamples(reader, PageAddress{0, level, px, pz + 1u});
                    for (uint32 c = 0; c < kPageStrideSamples; ++c)
                    {
                        ASSERT_EQ(word(page, c, kPageStrideSamples - 1u), word(above, c, 1u)) << int(level);
                        ASSERT_EQ(word(page, c, kPageStrideSamples - 2u), word(above, c, 0u)) << int(level);
                    }
                    ++pairs;
                }
            }
        }
    }
    EXPECT_GE(pairs, 20u);
}

TEST_F(PageStoreTest, CoarserLevelsAreTheTentOfTheStoredLevelBelow)
{
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 260;
    const std::filesystem::path source = m_Dir / "tent.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const std::filesystem::path store = m_Dir / "tent.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store), "");
    const HeightQuantum& quantum = reader.Layout().Header.Quantum;
    for (uint8 level = 1; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& finer = reader.Layout().Levels[level - 1u];
        const PageStoreLevel& coarser = reader.Layout().Levels[level];
        const std::vector<float32> below = LevelGrid(reader, level - 1u);
        const std::vector<float32> grid = LevelGrid(reader, level);
        const auto at = [&](int64 x, int64 z) {
            return below[static_cast<std::size_t>(ClampSample(z, finer.SamplesZ)) * finer.SamplesX +
                         ClampSample(x, finer.SamplesX)];
        };
        for (uint32 j = 0; j < coarser.SamplesZ; ++j)
        {
            for (uint32 i = 0; i < coarser.SamplesX; ++i)
            {
                const auto across = [&](int64 z) {
                    return 0.25f * at(2 * int64(i) - 1, z) + 0.5f * at(2 * int64(i), z) + 0.25f * at(2 * int64(i) + 1, z);
                };
                const float32 tent = 0.25f * across(2 * int64(j) - 1) + 0.5f * across(2 * int64(j)) +
                                     0.25f * across(2 * int64(j) + 1);
                ASSERT_EQ(grid[static_cast<std::size_t>(j) * coarser.SamplesX + i], quantum.Decode(quantum.Encode(tent)))
                    << "level " << int(level) << " sample " << i << "," << j;
            }
        }
    }
}

TEST_F(PageStoreTest, TentKeepsEveryCoarserSampleAtItsLatticePosition)
{
    // A plane through the origin with a 1,600 m range (32-bit store, no quantization): the tent of
    // a plane centered on a sample is the sample, so level N sample j is the plane at level-0
    // sample j * 2^N wherever the tent stays inside the field. A cell-centered average would sit
    // half a finer cell off.
    constexpr uint32 kWidth = 401;
    constexpr uint32 kHeight = 257;
    const std::filesystem::path source = m_Dir / "plane.r32";
    WriteR32(source, kWidth, kHeight,
             [](uint32 x, uint32 z) { return 2.0f * static_cast<float32>(x) + 3.0f * static_cast<float32>(z); });
    const std::filesystem::path store = m_Dir / "plane.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store), "");
    ASSERT_EQ(reader.Layout().Header.Format, PageFieldFormat::HeightR32F);
    for (uint8 level = 1; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        const std::vector<float32> grid = LevelGrid(reader, level);
        const uint32 step = 1u << level;
        for (uint32 j = 1; (j + 1u) * step < kHeight; ++j)
            for (uint32 i = 1; (i + 1u) * step < kWidth; ++i)
                ASSERT_EQ(grid[static_cast<std::size_t>(j) * shape.SamplesX + i],
                          2.0f * static_cast<float32>(i * step) + 3.0f * static_cast<float32>(j * step))
                    << "level " << int(level) << " sample " << i << "," << j;
    }
}

TEST_F(PageStoreTest, PageBoundsHoldTheLevelZeroSurfaceUnderTheirFootprintIncludingTheSharedEdge)
{
    constexpr uint32 kWidth = 520;
    constexpr uint32 kHeight = 390;
    const std::filesystem::path source = m_Dir / "bounds.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const std::filesystem::path store = m_Dir / "bounds.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store), "");
    const PageStoreHeader& header = reader.Layout().Header;
    const std::vector<float32> level0 = LevelGrid(reader, 0);
    for (uint8 level = 0; level < reader.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = reader.Layout().Levels[level];
        const uint32 span = kPageOwnedSamples << level;
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
        {
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                float32 lowest = std::numeric_limits<float32>::max();
                float32 highest = std::numeric_limits<float32>::lowest();
                for (uint32 z = pz * span; z <= std::min(pz * span + span, kHeight - 1u); ++z)
                    for (uint32 x = px * span; x <= std::min(px * span + span, kWidth - 1u); ++x)
                    {
                        lowest = std::min(lowest, level0[static_cast<std::size_t>(z) * kWidth + x]);
                        highest = std::max(highest, level0[static_cast<std::size_t>(z) * kWidth + x]);
                    }
                const PageIndexEntry* entry = reader.Layout().FindPresent(PageAddress{0, level, px, pz});
                ASSERT_NE(entry, nullptr);
                if (px * span > kWidth - 1u || pz * span > kHeight - 1u)
                    continue; // a page wholly past the field's edge holds edge samples only
                if (level == 0)
                {
                    EXPECT_EQ(DecodeHeightWord(header, entry->MinWord), lowest);
                    EXPECT_EQ(DecodeHeightWord(header, entry->MaxWord), highest);
                }
                EXPECT_LE(DecodeHeightWord(header, entry->MinWord), lowest) << int(level) << " " << px << "," << pz;
                EXPECT_GE(DecodeHeightWord(header, entry->MaxWord), highest) << int(level) << " " << px << "," << pz;
            }
        }
    }
}

TEST_F(PageStoreTest, RecookOfAnEditedSourceRewritesOnlyThePagesWhoseBytesChanged)
{
    constexpr uint32 kWidth = 640;
    constexpr uint32 kHeight = 520;
    const std::filesystem::path before = m_Dir / "before.r32";
    WriteR32(before, kWidth, kHeight, Relief);
    const std::filesystem::path storeA = m_Dir / "terrain-a.gepage";
    ASSERT_EQ(Cook(before, kWidth, kHeight, storeA).Error, "");
    PageStoreLayout layoutA;
    {
        PageStoreReader reader;
        ASSERT_EQ(reader.Open(storeA), "");
        layoutA = reader.Layout();
    }

    // A small edit inside one level-0 page, within the source's range (the quantum is unchanged).
    const std::filesystem::path after = m_Dir / "after.r32";
    WriteR32(after, kWidth, kHeight, [](uint32 x, uint32 z) {
        const bool edited = x >= 300 && x < 306 && z >= 200 && z < 205;
        return edited ? 260.0f : Relief(x, z);
    });
    const std::filesystem::path storeB = m_Dir / "terrain-b.gepage";
    const HeightCookResult patched = Cook(after, kWidth, kHeight, storeB, storeA);
    ASSERT_EQ(patched.Error, "");
    EXPECT_TRUE(patched.Patched);
    EXPECT_FALSE(std::filesystem::exists(storeA)); // moved and patched, not copied

    const std::filesystem::path fresh = m_Dir / "terrain-fresh.gepage";
    const HeightCookResult full = Cook(after, kWidth, kHeight, fresh);
    ASSERT_EQ(full.Error, "");
    EXPECT_FALSE(full.Patched);

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(fresh), "");
    uint64 changed = 0;
    for (std::size_t i = 0; i < layoutA.Entries.size(); ++i)
        changed += layoutA.Entries[i].Hash != reader.Layout().Entries[i].Hash ? 1u : 0u;
    // The page with the edit and the one ancestor per coarser level above it.
    EXPECT_GE(changed, reader.Layout().Levels.size());
    EXPECT_LT(changed, layoutA.Entries.size() / 4u);
    EXPECT_EQ(patched.PagesWritten, changed);
    EXPECT_EQ(patched.PagesWritten + patched.PagesUnchanged, layoutA.Entries.size());
    EXPECT_EQ(FileBytes(storeB), FileBytes(fresh));
}

TEST_F(PageStoreTest, ARangeChangeCooksAFreshStoreAndLeavesThePreviousOne)
{
    constexpr uint32 kWidth = 200;
    constexpr uint32 kHeight = 200;
    const std::filesystem::path before = m_Dir / "before.r32";
    WriteR32(before, kWidth, kHeight, Relief);
    const std::filesystem::path storeA = m_Dir / "a.gepage";
    ASSERT_EQ(Cook(before, kWidth, kHeight, storeA).Error, "");
    const std::filesystem::path after = m_Dir / "after.r32";
    WriteR32(after, kWidth, kHeight, [](uint32 x, uint32 z) { return Relief(x, z) + (x == 7 && z == 9 ? 500.0f : 0.0f); });
    const std::filesystem::path storeB = m_Dir / "b.gepage";
    const HeightCookResult result = Cook(after, kWidth, kHeight, storeB, storeA);
    ASSERT_EQ(result.Error, "");
    EXPECT_FALSE(result.Patched);
    EXPECT_TRUE(std::filesystem::exists(storeA));
}

TEST_F(PageStoreTest, RangesPastSixHundredFiftyFiveMetersCookToThirtyTwoBitFloat)
{
    const std::filesystem::path wide = m_Dir / "wide.r32";
    WriteR32(wide, 150, 140, [](uint32 x, uint32 z) { return static_cast<float32>(x) * 5.0f + static_cast<float32>(z); });
    const std::filesystem::path wideStore = m_Dir / "wide.gepage";
    ASSERT_EQ(Cook(wide, 150, 140, wideStore).Error, "");
    PageStoreReader reader;
    ASSERT_EQ(reader.Open(wideStore), "");
    EXPECT_EQ(reader.Layout().Header.Format, PageFieldFormat::HeightR32F);
    // 32-bit pages hold the source exactly.
    const std::vector<float32> page = PageSamples(reader, PageAddress{0, 0, 0, 0});
    EXPECT_EQ(page[static_cast<std::size_t>(11) * kPageStrideSamples + 21], 20.0f * 5.0f + 10.0f);

    const std::filesystem::path narrow = m_Dir / "narrow.r32";
    WriteR32(narrow, 150, 140, [](uint32 x, uint32 z) { return static_cast<float32>(x) * 3.0f + static_cast<float32>(z); });
    const std::filesystem::path narrowStore = m_Dir / "narrow.gepage";
    ASSERT_EQ(Cook(narrow, 150, 140, narrowStore).Error, "");
    ASSERT_EQ(reader.Open(narrowStore), "");
    EXPECT_EQ(reader.Layout().Header.Format, PageFieldFormat::HeightR16);
    EXPECT_LE(reader.Layout().Header.Quantum.Step, kHeightStepMax);
}

TEST_F(PageStoreTest, SixteenBitSourcesCookLosslesslyAsNormalizedHeights)
{
    constexpr uint32 kWidth = 200;
    constexpr uint32 kHeight = 150;
    std::vector<uint16> words(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = static_cast<uint16>((i * 2654435761u) >> 16u);
    const std::filesystem::path source = m_Dir / "words.r16";
    std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char*>(words.data()),
                                                  static_cast<std::streamsize>(words.size() * 2u));
    const std::filesystem::path store = m_Dir / "words.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, store).Error, "");

    PageStoreReader reader;
    ASSERT_EQ(reader.Open(store), "");
    EXPECT_EQ(reader.Layout().Header.Units, PageHeightUnits::Normalized);
    const std::vector<float32> grid = LevelGrid(reader, 0);
    Terrain::HeightfieldData today;
    ASSERT_TRUE(today.LoadFromRawUInt16(source, kWidth, kHeight));
    for (std::size_t i = 0; i < grid.size(); ++i)
        ASSERT_EQ(std::bit_cast<uint32>(grid[i]), std::bit_cast<uint32>(today.GetRawSamples()[i])) << i;
}

TEST_F(PageStoreTest, CookRunsTheSameOnThePoolAsOnOneThread)
{
    constexpr uint32 kWidth = 700;
    constexpr uint32 kHeight = 300;
    const std::filesystem::path source = m_Dir / "pool.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    JobSystem::WorkStealingThreadPool pool(4);
    AssetIOService io;
    io.Start(pool, 1);
    // The pooled cook reads its scan chunks and bands through the reader threads.
    const std::filesystem::path pooled = m_Dir / "pooled.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, pooled, {}, &pool, &io).Error, "");
    io.Stop();
    pool.Shutdown();
    const std::filesystem::path serial = m_Dir / "serial.gepage";
    ASSERT_EQ(Cook(source, kWidth, kHeight, serial).Error, "");
    EXPECT_EQ(FileBytes(pooled), FileBytes(serial));
}

// ---- Sources and keys ------------------------------------------------------------------------------

TEST_F(PageStoreTest, SourcesTheCookerCannotReadAreRefusedWithTheFix)
{
    HeightCookSource source;
    EXPECT_NE(ResolveHeightCookSource(m_Dir / "dem.tif", 0, 0, source).find(".r32"), std::string::npos);
    EXPECT_NE(ResolveHeightCookSource(m_Dir / "dem.tiff", 0, 0, source).find("GeoTIFF"), std::string::npos);
    EXPECT_NE(ResolveHeightCookSource(m_Dir / "dem.exr", 0, 0, source).find("not a heightmap format"),
              std::string::npos);

    const std::filesystem::path raw = m_Dir / "grid.r32";
    WriteR32(raw, 100, 50, Relief);
    EXPECT_NE(ResolveHeightCookSource(raw, 100, 49, source).find("do not account"), std::string::npos);
    EXPECT_EQ(ResolveHeightCookSource(raw, 100, 50, source), "");
    EXPECT_EQ(source.Format, HeightSourceFormat::R32);

    // Past the store's axis bound (every page count of a face must fit 32 bits).
    const std::filesystem::path wide = m_Dir / "wide.r16";
    std::ofstream(wide, std::ios::binary).close();
    std::filesystem::resize_file(wide, (static_cast<uintmax_t>(kPageStoreMaxSamplesPerAxis) + 1u) * 2u * 2u);
    EXPECT_NE(ResolveHeightCookSource(wide, kPageStoreMaxSamplesPerAxis + 1u, 2, source).find("split it"),
              std::string::npos);

    const std::filesystem::path nodata = m_Dir / "nodata.r32";
    WriteR32(nodata, 100, 50, [](uint32 x, uint32 z) {
        return x == 40 && z == 20 ? std::numeric_limits<float32>::quiet_NaN() : 1.0f;
    });
    ASSERT_EQ(ResolveHeightCookSource(nodata, 100, 50, source), "");
    HeightSourceIdentity identity;
    EXPECT_NE(ScanHeightSource(source, nullptr, identity).find("nodata"), std::string::npos);
}

namespace
{

// A PNG's signature and header chunk only: what the decoder's size check reads.
void WritePngHeader(const std::filesystem::path& file, uint32 width, uint32 height, uint8 colorType)
{
    const auto bigEndian = [](uint32 value) {
        return std::array<char, 4>{char(value >> 24), char(value >> 16), char(value >> 8), char(value)};
    };
    std::ofstream out(file, std::ios::binary);
    out.write("\x89PNG\r\n\x1a\n", 8);
    out.write(bigEndian(13).data(), 4);
    out.write("IHDR", 4);
    out.write(bigEndian(width).data(), 4);
    out.write(bigEndian(height).data(), 4);
    const char rest[5] = {16, static_cast<char>(colorType), 0, 0, 0};
    out.write(rest, 5);
    out.write(bigEndian(0).data(), 4); // CRC, unchecked by the size check
    out.write(bigEndian(0).data(), 4); // an empty IDAT: where the header scan stops
    out.write("IDAT", 4);
    out.write(bigEndian(0).data(), 4);
}

} // namespace

TEST_F(PageStoreTest, PngHeightmapsPastTheDecodersLimitAreRefusedWithTheFix)
{
    const std::filesystem::path fits = m_Dir / "fits.png";
    WritePngHeader(fits, 32768, 16384, 0);
    Terrain::PngHeightmapSize size;
    EXPECT_EQ(Terrain::ResolvePngHeightmapSize(fits, size), "");
    EXPECT_EQ(size.Width, 32768u);

    const std::filesystem::path tooLarge = m_Dir / "large.png";
    WritePngHeader(tooLarge, 40000, 40000, 0);
    const std::string reason = Terrain::ResolvePngHeightmapSize(tooLarge, size);
    EXPECT_NE(reason.find("more than a PNG heightmap can hold"), std::string::npos);
    EXPECT_NE(reason.find(".r32"), std::string::npos);

    // Three 16-bit channels take three times the bytes: a grid a grayscale PNG holds, RGB does not.
    const std::filesystem::path rgb = m_Dir / "rgb.png";
    WritePngHeader(rgb, 32768, 16384, 2);
    EXPECT_NE(Terrain::ResolvePngHeightmapSize(rgb, size), "");

    // A palette image is checked at four channels: 20000 x 15000 passes as three, fails as four.
    const std::filesystem::path palette = m_Dir / "palette.png";
    WritePngHeader(palette, 20000, 15000, 3);
    EXPECT_NE(Terrain::ResolvePngHeightmapSize(palette, size), "");

    HeightCookSource source;
    EXPECT_EQ(ResolveHeightCookSource(tooLarge, 0, 0, source), reason);
}

TEST_F(PageStoreTest, CookKeyFollowsTheContentAndImportGridOnly)
{
    const HeightStoreCookInputs base{0x1111222233334444ull, HeightSourceFormat::R32, 8193, 2049};
    const uint64 key = ComputeHeightStoreKey(base);
    EXPECT_EQ(ComputeHeightStoreKey(base), key);
    HeightStoreCookInputs changed = base;
    changed.SourceContentHash ^= 1u;
    EXPECT_NE(ComputeHeightStoreKey(changed), key);
    changed = base;
    changed.Format = HeightSourceFormat::R16;
    EXPECT_NE(ComputeHeightStoreKey(changed), key);
    changed = base;
    changed.SamplesX = 2049;
    changed.SamplesZ = 8193;
    EXPECT_NE(ComputeHeightStoreKey(changed), key);
    // The inputs are the asset's content and import grid and nothing else: an entity, a scene, a
    // streaming budget or a material cannot reach the key. A field added here must change the bytes.
    static_assert(sizeof(HeightStoreCookInputs) == 24, "every cook input must decide the store's bytes");

    const GUID asset = GUID::Generate();
    EXPECT_EQ(HeightStoreFile(m_Dir, asset, 0xABCull).filename().string(), asset.ToString() + "-0000000000000abc.gepage");
}

TEST_F(PageStoreTest, TwoTerrainsOnOneHeightmapResolveOneStoreAndATouchKeepsTheKey)
{
    constexpr uint32 kWidth = 300;
    constexpr uint32 kHeight = 300;
    const std::filesystem::path source = m_Dir / "dem.r32";
    WriteR32(source, kWidth, kHeight, Relief);
    const GUID asset = GUID::Generate();
    const std::filesystem::path cache = m_Dir / "TerrainPages";

    // What a terrain's lookup does: the remembered identity when the file is as it was, else a
    // full read; then the key from the identity and the import grid.
    const auto storeFor = [&](uint32 samplesX, uint32 samplesZ) {
        HeightCookSource resolved;
        EXPECT_EQ(ResolveHeightCookSource(source, samplesX, samplesZ, resolved), "");
        std::optional<HeightSourceIdentity> identity = ReadHeightSourceMemo(cache, asset, source);
        if (!identity)
        {
            identity.emplace();
            EXPECT_EQ(ScanHeightSource(resolved, nullptr, *identity), "");
            EXPECT_TRUE(WriteHeightSourceMemo(cache, asset, source, *identity));
        }
        return HeightStoreFile(cache, asset,
                               ComputeHeightStoreKey({identity->ContentHash, resolved.Format, samplesX, samplesZ}));
    };

    const std::filesystem::path first = storeFor(kWidth, kHeight); // terrain 1, scene 1
    EXPECT_TRUE(ReadHeightSourceMemo(cache, asset, source).has_value());
    const std::filesystem::path second = storeFor(kWidth, kHeight); // terrain 2, scene 2
    EXPECT_EQ(first, second);

    // A touch changes the modification time, not the bytes: the memo misses, the full read finds
    // the same content, and the store keeps its key.
    std::filesystem::last_write_time(source, std::filesystem::last_write_time(source) + std::chrono::seconds(5));
    EXPECT_FALSE(ReadHeightSourceMemo(cache, asset, source).has_value());
    EXPECT_EQ(storeFor(kWidth, kHeight), first);

    // An edit in the middle of the file, past what the asset registry's sparse fingerprint reads,
    // re-keys the store.
    std::vector<float32> samples = ReadR32(source, kWidth, kHeight);
    samples[samples.size() / 2u + 777u] += 0.25f;
    std::ofstream(source, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 4u));
    EXPECT_NE(storeFor(kWidth, kHeight), first);
}

// ---- Generated provider ----------------------------------------------------------------------------

namespace
{

GeneratedHeightLattice NoiseLattice(float32 originX, float32 originZ, float32 tileWorld, uint32 tilesX, uint32 tilesZ)
{
    GeneratedHeightLattice lattice;
    lattice.Source = GeneratedHeightSource::Noise;
    lattice.TerrainOriginX = originX;
    lattice.TerrainOriginZ = originZ;
    lattice.TileWorldSize = tileWorld;
    lattice.TileSamples = Terrain::kMaxTileResolution;
    lattice.TilesX = tilesX;
    lattice.TilesZ = tilesZ;
    lattice.Noise = {TerrainECS::kTileNoiseFrequency, TerrainECS::kTileNoiseAmplitude, TerrainECS::kTileNoiseOctaves,
                     TerrainECS::kTileNoiseSeed};
    return lattice;
}

// Every level-0 sample of the provider against the tile fill of the tile that owns it.
void ExpectLevelZeroIsTheTileFill(const GeneratedHeightLattice& lattice)
{
    const GeneratedHeightPageProvider provider(lattice);
    ASSERT_FALSE(provider.Levels().empty());
    const PageStoreLevel& level0 = provider.Levels().front();
    const uint32 interval = lattice.TileSamples - 1u;
    ASSERT_EQ(level0.SamplesX, lattice.TilesX * interval + 1u);

    std::vector<std::vector<float32>> pages(static_cast<std::size_t>(level0.PagesX) * level0.PagesZ);
    for (uint32 pz = 0; pz < level0.PagesZ; ++pz)
        for (uint32 px = 0; px < level0.PagesX; ++px)
        {
            auto& page = pages[static_cast<std::size_t>(pz) * level0.PagesX + px];
            page.resize(kPageSampleCount);
            ASSERT_TRUE(provider.FillPage(PageAddress{0, 0, px, pz}, page));
        }

    TerrainECS::TiledTerrainConfig config;
    config.Base.Source = Components::TerrainBaseSource::ProceduralNoise;
    config.TileWorldSize = lattice.TileWorldSize;
    uint64 compared = 0;
    for (uint32 tz = 0; tz < lattice.TilesZ; ++tz)
    {
        for (uint32 tx = 0; tx < lattice.TilesX; ++tx)
        {
            // The tile job's own origin expression (TileStreamingManager::DispatchFullJob).
            const TerrainECS::TileCoord coord{static_cast<int32>(tx), static_cast<int32>(tz)};
            const float32 tileOriginX = lattice.TerrainOriginX + coord.X * lattice.TileWorldSize;
            const float32 tileOriginZ = lattice.TerrainOriginZ + coord.Z * lattice.TileWorldSize;
            Terrain::HeightfieldData tile(lattice.TileSamples, lattice.TileSamples, 0.0f);
            TerrainECS::FillTiledBaseRegion(tile, config, lattice.TerrainOriginX, lattice.TerrainOriginZ, tileOriginX,
                                            tileOriginZ, lattice.TileWorldSize, lattice.TileWorldSize, 0, 0,
                                            static_cast<int32>(interval), static_cast<int32>(interval));
            // A tile owns its samples but the last row and column, unless it is the grid's last.
            const uint32 lastX = tx + 1u == lattice.TilesX ? interval : interval - 1u;
            const uint32 lastZ = tz + 1u == lattice.TilesZ ? interval : interval - 1u;
            for (uint32 z = 0; z <= lastZ; ++z)
            {
                for (uint32 x = 0; x <= lastX; ++x)
                {
                    const uint32 gx = tx * interval + x;
                    const uint32 gz = tz * interval + z;
                    const auto& page = pages[static_cast<std::size_t>(gz / kPageOwnedSamples) * level0.PagesX +
                                             gx / kPageOwnedSamples];
                    const float32 paged = page[static_cast<std::size_t>(gz % kPageOwnedSamples + 1u) * kPageStrideSamples +
                                               gx % kPageOwnedSamples + 1u];
                    ASSERT_EQ(std::bit_cast<uint32>(paged), std::bit_cast<uint32>(tile.GetSample(x, z)))
                        << "tile " << tx << "," << tz << " sample " << x << "," << z;
                    ++compared;
                }
            }
        }
    }
    EXPECT_EQ(compared, static_cast<uint64>(level0.SamplesX) * level0.SamplesZ);
}

} // namespace

TEST(GeneratedHeightPageProviderTest, LevelZeroIsByteIdenticalToTheTileNoiseFill)
{
    // One sample per meter (1 km tiles) at the origin, and four per meter (256 m tiles) off it.
    ExpectLevelZeroIsTheTileFill(NoiseLattice(0.0f, 0.0f, 1024.0f, 3, 2));
    ExpectLevelZeroIsTheTileFill(NoiseLattice(-1536.5f, 733.25f, 256.0f, 2, 3));
    // Three samples per meter: a tile size and spacing with no short binary form, so a sample's
    // position from one global spacing rounds differently from the tile's corner plus its offset.
    ExpectLevelZeroIsTheTileFill(NoiseLattice(1000.1f, -77.7f, 1024.0f / 3.0f, 3, 1));
}

TEST(GeneratedHeightPageProviderTest, CoarserLevelsSitOnTheLevelZeroLatticeAndKeepItsMean)
{
    // One broad octave: nothing to band-limit, so level N sample j is level-0 sample j * 2^N.
    GeneratedHeightLattice broad = NoiseLattice(-100.0f, 50.0f, 1024.0f, 2, 1);
    broad.Noise.Octaves = 1;
    const GeneratedHeightPageProvider provider(broad);
    std::vector<float32> fine(kPageSampleCount);
    std::vector<float32> coarse(kPageSampleCount);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 0, 0, 0}, fine));
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 1, 0, 0}, coarse));
    for (uint32 r = 1; r < 64u; ++r)
        for (uint32 c = 1; c < 64u; ++c)
            ASSERT_EQ(coarse[static_cast<std::size_t>(r) * kPageStrideSamples + c],
                      fine[static_cast<std::size_t>(2u * r - 1u) * kPageStrideSamples + 2u * c - 1u]);

    // The default noise: a coarse level drops the octaves it cannot carry for their mean, so its
    // mean height stays the field's.
    const GeneratedHeightPageProvider full(NoiseLattice(0.0f, 0.0f, 1024.0f, 3, 3));
    const auto levelMean = [&](uint8 level) {
        double sum = 0.0;
        uint64 count = 0;
        std::vector<float32> page(kPageSampleCount);
        const PageStoreLevel& shape = full.Levels()[level];
        for (uint32 pz = 0; pz < shape.PagesZ; ++pz)
            for (uint32 px = 0; px < shape.PagesX; ++px)
            {
                EXPECT_TRUE(full.FillPage(PageAddress{0, level, px, pz}, page));
                for (uint32 r = 0; r < kPageOwnedSamples && pz * kPageOwnedSamples + r < shape.SamplesZ; ++r)
                    for (uint32 c = 0; c < kPageOwnedSamples && px * kPageOwnedSamples + c < shape.SamplesX; ++c)
                    {
                        sum += page[static_cast<std::size_t>(r + 1u) * kPageStrideSamples + c + 1u];
                        ++count;
                    }
            }
        return sum / static_cast<double>(count);
    };
    EXPECT_NEAR(levelMean(5), levelMean(0), 0.01);
}

TEST(GeneratedHeightPageProviderTest, FlatBaseIsZeroAndOutOfRangeAddressesFail)
{
    GeneratedHeightLattice flat = NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1);
    flat.Source = GeneratedHeightSource::Flat;
    const GeneratedHeightPageProvider provider(flat);
    std::vector<float32> page(kPageSampleCount, 1.0f);
    ASSERT_TRUE(provider.FillPage(PageAddress{0, 2, 1, 1}, page));
    EXPECT_TRUE(std::all_of(page.begin(), page.end(), [](float32 v) { return v == 0.0f; }));
    EXPECT_FALSE(provider.FillPage(PageAddress{0, 0, 9, 0}, page));
    EXPECT_FALSE(provider.FillPage(PageAddress{1, 0, 0, 0}, page));
    EXPECT_FALSE(provider.FillPage(PageAddress{0, static_cast<uint8>(provider.Levels().size()), 0, 0}, page));
}

// ---- Container and loader --------------------------------------------------------------------------

TEST_F(PageStoreTest, TerrainContainerReadsItsHeightFieldAtItsOffsetUnchanged)
{
    const std::filesystem::path source = m_Dir / "c.r32";
    WriteR32(source, 260, 200, Relief);
    const std::filesystem::path store = m_Dir / "c.gepage";
    ASSERT_EQ(Cook(source, 260, 200, store).Error, "");
    const std::filesystem::path container = m_Dir / "Cooked" / "Terrain" / "c.geterrain";
    const TerrainContainerField field{PageFieldKind::Height, store};
    ASSERT_EQ(WriteTerrainContainer(container, {&field, 1}), "");

    PageStoreReader loose;
    ASSERT_EQ(loose.Open(store), "");
    PageStoreReader packed;
    ASSERT_EQ(OpenTerrainContainerField(container, PageFieldKind::Height, packed), "");
    EXPECT_GT(packed.BaseOffset(), 0u);
    EXPECT_EQ(packed.Layout().Header.Key, loose.Layout().Header.Key);
    for (uint8 level = 0; level < loose.Layout().Levels.size(); ++level)
        for (uint32 pz = 0; pz < loose.Layout().Levels[level].PagesZ; ++pz)
            for (uint32 px = 0; px < loose.Layout().Levels[level].PagesX; ++px)
                EXPECT_EQ(PageSamples(packed, PageAddress{0, level, px, pz}), PageSamples(loose, PageAddress{0, level, px, pz}));

    const TerrainContainerField twice[2] = {field, field};
    EXPECT_NE(WriteTerrainContainer(m_Dir / "twice.geterrain", twice), "");
    EXPECT_NE(OpenTerrainContainerField(store, PageFieldKind::Height, packed).find("not a terrain container"),
              std::string::npos);
}

TEST_F(PageStoreTest, PagesLoadThroughTheAssetReaderThreadsAndDecodeOffThem)
{
    const std::filesystem::path source = m_Dir / "load.r32";
    WriteR32(source, 400, 300, Relief);
    const std::filesystem::path store = m_Dir / "load.gepage";
    ASSERT_EQ(Cook(source, 400, 300, store).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store), "");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetIOService io;
    io.Start(pool, 1);

    const auto load = [&](const PageAddress& address, std::shared_ptr<std::atomic<bool>> cancel) {
        auto loaded = std::make_shared<std::promise<std::vector<float32>>>();
        auto failed = std::make_shared<std::promise<std::string>>();
        PageLoadRequest request;
        request.Store = reader;
        request.StoreAsset = GUID::Generate();
        request.Address = address;
        request.Priority = AssetLoadPriority::High;
        request.Cancel = std::move(cancel);
        request.OnLoaded = [loaded](const PageAddress&, std::vector<float32> samples) { loaded->set_value(std::move(samples)); };
        request.OnFailed = [failed](const PageAddress&, const std::string& error) { failed->set_value(error); };
        SubmitHeightPageLoad(io, std::move(request));
        return std::make_pair(loaded->get_future(), failed->get_future());
    };

    for (const PageAddress address : {PageAddress{0, 0, 0, 0}, PageAddress{0, 0, 3, 2}, PageAddress{0, 1, 1, 1}})
    {
        auto [samples, failure] = load(address, std::make_shared<std::atomic<bool>>(false));
        ASSERT_EQ(samples.wait_for(kLoadTimeout), std::future_status::ready);
        EXPECT_EQ(samples.get(), PageSamples(*reader, address));
    }

    auto [absentSamples, absent] = load(PageAddress{0, 0, 9, 9}, nullptr);
    ASSERT_EQ(absent.wait_for(std::chrono::seconds(0)), std::future_status::ready); // inline
    EXPECT_NE(absent.get().find("not in the store"), std::string::npos);

    auto [cancelledSamples, cancelled] = load(PageAddress{0, 0, 1, 1}, std::make_shared<std::atomic<bool>>(true));
    ASSERT_EQ(cancelled.wait_for(kLoadTimeout), std::future_status::ready);
    // OnLoaded never ran: its promise was dropped with the request, unfulfilled.
    EXPECT_THROW(cancelledSamples.get(), std::future_error);

    io.Stop();
    pool.Shutdown();
}

TEST_F(PageStoreTest, APageLoadWhoseDecodeNeverRunsStillResolvesOnce)
{
    const std::filesystem::path source = m_Dir / "shutdown.r32";
    WriteR32(source, 200, 200, Relief);
    const std::filesystem::path store = m_Dir / "shutdown.gepage";
    ASSERT_EQ(Cook(source, 200, 200, store).Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store), "");

    // One worker, held by a job until the pool shuts down: the page's read completes and its
    // decode is queued behind that job, so the shutdown cancels the decode without running it.
    JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<bool> release{false};
    pool.Submit([&release]() {
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    AssetIOService io;
    io.Start(pool, 1);

    std::atomic<int> loaded{0};
    std::atomic<int> failed{0};
    PageLoadRequest request;
    request.Store = reader;
    request.StoreAsset = GUID::Generate();
    request.Address = PageAddress{0, 0, 0, 0};
    request.OnLoaded = [&loaded](const PageAddress&, std::vector<float32>) { ++loaded; };
    request.OnFailed = [&failed](const PageAddress&, const std::string&) { ++failed; };
    SubmitHeightPageLoad(io, std::move(request));

    const auto deadline = std::chrono::steady_clock::now() + kLoadTimeout;
    while (io.GetQueuedReadCount() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    io.Stop(); // joins the reader, which has read the page and queued its decode
    std::thread releaser([&release]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release.store(true);
    });
    pool.Shutdown();
    releaser.join();

    EXPECT_EQ(loaded.load(), 0);
    EXPECT_EQ(failed.load(), 1) << "exactly one of OnLoaded and OnFailed runs";
}

TEST_F(PageStoreTest, TheCacheFolderKeepsOneCurrentStorePerHeightmapAndSweepsDeadCooks)
{
    const std::filesystem::path source = m_Dir / "dem.r32";
    WriteR32(source, 300, 260, Relief);
    const std::filesystem::path folder = m_Dir / "TerrainPages";
    const HeightStoreCache cache(folder);
    const GUID asset = GUID::Generate();

    const HeightStoreCache::Result first = cache.Ensure(asset, source, 300, 260, nullptr, nullptr);
    ASSERT_EQ(first.Error, "");
    EXPECT_TRUE(first.Cooked);
    const HeightStoreCache::Result again = cache.Ensure(asset, source, 300, 260, nullptr, nullptr);
    EXPECT_EQ(again.Store, first.Store);
    EXPECT_FALSE(again.Cooked) << "a second terrain on the same heightmap re-cooked it";

    // A cook that died left its temporary store; an edit re-keys the heightmap.
    const std::filesystem::path orphan = first.Store.string() + ".2147483600-1-0.tmp";
    std::ofstream(orphan, std::ios::binary) << "partial";
    WriteR32(source, 300, 260, [](uint32 x, uint32 z) { return Relief(x, z) + (x == 150 && z == 100 ? 1.0f : 0.0f); });
    const HeightStoreCache::Result edited = cache.Ensure(asset, source, 300, 260, nullptr, nullptr);
    ASSERT_EQ(edited.Error, "");
    EXPECT_TRUE(edited.Cooked);
    EXPECT_NE(edited.Store, first.Store);
    EXPECT_FALSE(std::filesystem::exists(first.Store)) << "the previous key's store is gone";
    EXPECT_FALSE(std::filesystem::exists(orphan));
    std::size_t stores = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        stores += entry.path().extension() == ".gepage" ? 1u : 0u;
    EXPECT_EQ(stores, 1u);
}

// The progress the editor's cook banner shows: every source row read twice (identified, then built).
TEST_F(PageStoreTest, ACookReportsEverySourceRowReadTwice)
{
    const std::filesystem::path source = m_Dir / "dem.r32";
    WriteR32(source, 300, 260, Relief);
    const HeightStoreCache cache(m_Dir / "TerrainPages");
    const GUID asset = GUID::Generate();

    HeightCookProgress cooked;
    ASSERT_EQ(cache.Ensure(asset, source, 300, 260, nullptr, nullptr, nullptr, &cooked).Error, "");
    EXPECT_EQ(cooked.RowsTotal.load(), 2u * 260u);
    EXPECT_EQ(cooked.RowsDone.load(), cooked.RowsTotal.load());

    HeightCookProgress found;
    const HeightStoreCache::Result again = cache.Ensure(asset, source, 300, 260, nullptr, nullptr, nullptr, &found);
    ASSERT_FALSE(again.Cooked);
    EXPECT_EQ(found.RowsDone.load(), 260u) << "a found store is identified and not built again";
}

// ---- Residency ---------------------------------------------------------------------------------------

namespace
{

// Every page of a generated lattice, as a level rule closed under parents would want them.
std::vector<PageWant> EveryPage(const GeneratedHeightPageProvider& provider)
{
    std::vector<PageWant> wanted;
    for (uint32 level = 0; level < provider.Levels().size(); ++level)
        for (uint32 z = 0; z < provider.Levels()[level].PagesZ; ++z)
            for (uint32 x = 0; x < provider.Levels()[level].PagesX; ++x)
                wanted.push_back({PageAddress{0, uint8(level), x, z}, float32(x + z), false});
    return wanted;
}

// Frames until `done` holds (loads land between frames on the pool), at most a few seconds.
template <typename Done>
uint64 RunFramesUntil(PageResidencyManager& residency, uint64 frame, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline)
    {
        residency.Update(++frame, 1.0f / 60.0f, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return frame;
}

} // namespace

TEST(PageResidencyTest, WantedPagesLoadAssignAndThenAParkedCameraDoesNoWork)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(7, 128, 2, 0.4f);
    residency.Configure(7, 7, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);

    uint64 frame = 0;
    residency.Request(7, wanted);
    // Each frame's uploads carry their page's samples until the next Update.
    std::vector<float32> expected(kPageSampleCount);
    uint32 uploaded = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        for (const PageUploadRequest& upload : residency.FieldCache(7).Uploads())
        {
            EXPECT_TRUE(provider->FillPage(upload.Page.Address, expected));
            const std::vector<float32>* samples = residency.Samples(7, upload.Page.Address);
            EXPECT_NE(samples, nullptr);
            if (samples != nullptr)
                EXPECT_EQ(*samples, expected);
            ++uploaded;
        }
        return residency.FieldCache(7).ResidentCount() == wanted.size();
    });
    ASSERT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size());
    EXPECT_EQ(uploaded, wanted.size());

    // Past the arrival fades, a parked camera starts and assigns nothing.
    for (int i = 0; i < 40; ++i)
        residency.Update(++frame, 1.0f / 60.0f, false);
    for (int i = 0; i < 5; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
        EXPECT_EQ(residency.LoadsStartedThisFrame(), 0u);
        EXPECT_EQ(residency.FieldCache(7).AssignsThisFrame(), 0u);
        EXPECT_TRUE(residency.FieldCache(7).Transitions().empty());
    }
    // Uploaded, the pages keep no samples on the CPU (nothing there reads a resident page).
    EXPECT_EQ(residency.CpuBytes(), 0u);
    EXPECT_EQ(residency.Samples(7, wanted.front().Address), nullptr);
    pool.Shutdown();
}

TEST(PageResidencyTest, EveryStreamSharesOneUploadCapAndOneCpuBudget)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    PageResidencyBudget budget;
    budget.UploadsPerFrame = 3;
    budget.CpuBytes = 7u * kPageSampleCount * sizeof(float32); // 7 pages, fewer than both streams want
    PageResidencyManager residency(&pool, nullptr, budget);
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.ConfigureField(2, 128, 2, 0.0f);
    residency.Configure(2, 2, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);
    std::vector<PageWant> few(wanted.end() - 5, wanted.end()); // the top two levels, closed under parents
    residency.Request(1, few);
    residency.Request(2, few);

    uint64 frame = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        EXPECT_LE(residency.FieldCache(1).AssignsThisFrame() + residency.FieldCache(2).AssignsThisFrame(), 3u);
        return residency.FieldCache(1).ResidentCount() == 5u && residency.FieldCache(2).ResidentCount() == 5u;
    });
    EXPECT_EQ(residency.FieldCache(1).ResidentCount(), 5u);
    EXPECT_EQ(residency.FieldCache(2).ResidentCount(), 5u);

    // Once uploaded, both streams' pages leave the CPU budget.
    residency.Update(++frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.CpuBytes(), 0u);
    pool.Shutdown();
}

// Two terrains of one field divide its cache by the one order (design D5: the budget is the
// field's, whatever the number of terrains): the field holds its slot count and no more, both
// terrains' nearest pages are among them, and no page past the budget is ever loaded.
TEST(PageResidencyTest, TwoTerrainsOfAFieldShareItsSlotsNearestFirst)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 2, 2));
    const std::vector<PageWant> nearTerrain = EveryPage(*provider);
    std::vector<PageWant> farTerrain = nearTerrain;
    for (PageWant& want : farTerrain)
        want.Distance += 0.5f; // the two terrains' pages interleave in the one order
    constexpr uint32 kFieldSlots = 12;
    ASSERT_GT(nearTerrain.size(), kFieldSlots);

    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{}); // inline loads, drained a frame later
    residency.ConfigureField(1, kFieldSlots, 2, 0.0f);
    residency.Configure(10, 1, PageSource{nullptr, provider});
    residency.Configure(11, 1, PageSource{nullptr, provider});
    uint32 loads = 0;
    for (uint64 frame = 1; frame <= 40; ++frame)
    {
        residency.Request(10, nearTerrain);
        residency.Request(11, farTerrain);
        residency.Update(frame, 1.0f / 60.0f, false);
        loads += residency.LoadsStartedThisFrame();
    }

    const PageCache& cache = residency.FieldCache(1);
    EXPECT_EQ(cache.ResidentCount(), kFieldSlots);
    EXPECT_EQ(loads, kFieldSlots) << "pages past the field's budget were loaded";
    const PageAddress top = nearTerrain.back().Address;
    EXPECT_NE(cache.SlotOf(CachedPage{10, top}), kNoResidentSlot);
    EXPECT_NE(cache.SlotOf(CachedPage{11, top}), kNoResidentSlot);
    EXPECT_NE(cache.SlotOf(CachedPage{10, PageAddress{0, 0, 0, 0}}), kNoResidentSlot);
    EXPECT_NE(cache.SlotOf(CachedPage{11, PageAddress{0, 0, 0, 0}}), kNoResidentSlot);

    // A terrain that leaves takes its pages out of the field's cache at once.
    residency.Forget(11);
    residency.Request(10, nearTerrain);
    residency.Update(41, 1.0f / 60.0f, false);
    for (const CachedPage& page : cache.Resident())
        EXPECT_EQ(page.Stream, 10u);
}

// ---- Review fold (PR #3072): one priority across streams, bounded loads, retries, restarts --------------

TEST(PageResidencyTest, APinnedPageOfOneStreamLoadsBeforeAnotherStreamsFinePages)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> every = EveryPage(*provider);
    ASSERT_GT(every.size(), 4u);
    for (uint32 pinnedStream : {1u, 2u})
    {
        PageResidencyBudget budget;
        budget.LoadsInFlight = 2;
        PageResidencyManager residency(nullptr, nullptr, budget); // inline loads, drained a frame later
        residency.ConfigureField(1, 128, 2, 0.0f);
        residency.Configure(1, 1, PageSource{nullptr, provider});
        residency.ConfigureField(2, 128, 2, 0.0f);
        residency.Configure(2, 2, PageSource{nullptr, provider});
        const uint32 fineStream = pinnedStream == 1u ? 2u : 1u;
        PageWant pinned = every.back();
        pinned.Pinned = true;
        residency.Request(pinnedStream, std::vector<PageWant>{pinned});
        residency.Request(fineStream, every);
        residency.Update(1, 1.0f / 60.0f, false);
        residency.Update(2, 1.0f / 60.0f, false);
        EXPECT_NE(residency.Samples(pinnedStream, pinned.Address), nullptr)
            << "stream " << pinnedStream << "'s pinned page waited behind stream " << fineStream << "'s pages";
    }
}

TEST(PageResidencyTest, AStreamDoesNotLoadAndKeepMorePagesThanItsCacheCanHold)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> every = EveryPage(*provider);
    PageResidencyBudget budget;
    budget.CpuBytes = 4u * kPageSampleCount * sizeof(float32);
    PageResidencyManager residency(nullptr, nullptr, budget);
    residency.ConfigureField(1, 4, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.Request(1, every);
    for (uint64 frame = 1; frame <= 20; ++frame)
        residency.Update(frame, 1.0f / 60.0f, false);
    EXPECT_EQ(residency.FieldCache(1).ResidentCount(), 4u);
    EXPECT_LE(residency.CpuBytes(), budget.CpuBytes)
        << "pages on the CPU: " << residency.CpuBytes() / (kPageSampleCount * sizeof(float32)) << " of "
        << every.size() << " wanted, for a cache of 4 slots and a budget of 4 pages";
}

TEST_F(PageStoreTest, ATransientReadFailureIsRetried)
{
    const std::filesystem::path source = m_Dir / "transient.r32";
    WriteR32(source, 300, 260, Relief);
    const HeightStoreCache cache(m_Dir / "TerrainPages");
    const HeightStoreCache::Result store = cache.Ensure(GUID::Generate(), source, 300, 260, nullptr, nullptr);
    ASSERT_EQ(store.Error, "");
    auto reader = std::make_shared<PageStoreReader>();
    ASSERT_EQ(reader->Open(store.Store), "");
    std::vector<char> bytes;
    {
        std::ifstream in(store.Store, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const PageAddress top{0, uint8(reader->Layout().Levels.size() - 1u), 0, 0};
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 16, 2, 0.0f);
    residency.Configure(1, 1, PageSource{reader, nullptr});

    std::error_code ec;
    std::filesystem::resize_file(store.Store, 64, ec); // the read fails once (a store being rewritten)
    ASSERT_FALSE(ec) << ec.message();
    residency.Request(1, std::vector<PageWant>{PageWant{top, 0.0f, true}});
    residency.Update(1, 1.0f / 60.0f, false);
    residency.Update(2, 1.0f / 60.0f, false);
    ASSERT_EQ(residency.Samples(1, top), nullptr) << "the truncated read should have failed";

    {
        std::ofstream out(store.Store, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    for (uint64 frame = 3; frame < 200; ++frame)
    {
        residency.Request(1, std::vector<PageWant>{PageWant{top, 0.0f, true}});
        residency.Update(frame, 1.0f / 60.0f, false);
    }
    EXPECT_NE(residency.FieldCache(1).SlotOf(CachedPage{1, top}), kNoResidentSlot)
        << "the store is whole again, but the stream marked its top page unloadable for good";
}

TEST(PageResidencyTest, ALoadOfARestartedStreamsOldSourceNeverLands)
{
    JobSystem::WorkStealingThreadPool pool(1);
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    pool.Submit([gate] { gate.wait(); }, JobSystem::JobPriority::Background); // holds the one worker

    GeneratedHeightLattice flatLattice = NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1);
    flatLattice.Source = GeneratedHeightSource::Flat;
    const auto oldSource = std::make_shared<GeneratedHeightPageProvider>(flatLattice);
    const auto newSource = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const PageWant top = EveryPage(*newSource).back();

    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 16, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, oldSource});
    residency.Request(1, std::vector<PageWant>{top});
    residency.Update(1, 1.0f / 60.0f, false); // the old source's load queues behind the gate
    residency.Configure(1, 1, PageSource{nullptr, newSource}); // a re-cook restarts the stream
    residency.Request(1, std::vector<PageWant>{top});
    residency.Update(2, 1.0f / 60.0f, false); // the new source's load queues too
    release.set_value();

    uint64 frame = 2;
    frame = RunFramesUntil(residency, frame,
                           [&] { return residency.FieldCache(1).SlotOf(CachedPage{1, top.Address}) != kNoResidentSlot; });
    std::vector<float32> expected(kPageSampleCount);
    ASSERT_TRUE(newSource->FillPage(top.Address, expected));
    ASSERT_NE(residency.Samples(1, top.Address), nullptr); // the frame of its upload
    EXPECT_EQ(*residency.Samples(1, top.Address), expected) << "the stream holds the old source's page";
    EXPECT_EQ(residency.CpuBytes(), uint64(kPageSampleCount) * sizeof(float32))
        << "the old source's load was counted into the restarted stream";
    for (int i = 0; i < 10; ++i)
    {
        residency.Request(1, std::vector<PageWant>{top});
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
    EXPECT_EQ(residency.CpuBytes(), 0u) << "the old source's load landed after the restart";
    pool.Shutdown();
}

TEST(PageResidencyTest, ASmallPyramidUnderItsCapStaysWhollyResidentWithNoFurtherLoads)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    TerrainECS::PageRequestTerrain terrain;
    terrain.SamplesX = terrain.SamplesZ = 1025;
    terrain.Level0TexelX = terrain.Level0TexelZ = 1.0f;
    terrain.MaxHeight = 1.0f;
    terrain.CacheSlots = 512; // the desktop height cache: a pyramid of up to 128 pages is pinned whole
    TerrainECS::HeightPageRequester requester;
    requester.Configure(terrain);
    ASSERT_EQ(requester.FirstPinnedLevel(), 0u);

    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, terrain.CacheSlots, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    const Mathematics::Vector3 far{1.0e6f, 1.0e6f, 1.0e6f}; // nowhere near: only pinning keeps pages
    const TerrainECS::PageLevelView view = TerrainECS::MakePageLevelView(1080, 1.0f, 11.0f);
    std::vector<PageWant> wants;
    const std::size_t pages = EveryPage(*provider).size();
    uint64 frame = 0;
    frame = RunFramesUntil(residency, frame, [&] {
        requester.Build(std::span(&far, 1), view, wants);
        residency.Request(1, wants);
        return residency.FieldCache(1).ResidentCount() == pages;
    });
    ASSERT_EQ(residency.FieldCache(1).ResidentCount(), pages);
    for (int i = 0; i < 10; ++i)
    {
        requester.Build(std::span(&far, 1), view, wants);
        residency.Request(1, wants);
        residency.Update(++frame, 1.0f / 60.0f, false);
        EXPECT_EQ(residency.LoadsStartedThisFrame(), 0u);
        EXPECT_EQ(residency.FieldCache(1).ResidentCount(), pages);
    }
    pool.Shutdown();
}

// ---- Review fold 2 (PR #3072) -------------------------------------------------------------------------

TEST(PageResidencyTest, ForgettingAStreamWhileAnotherIsParkedIsSafe)
{
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const std::vector<PageWant> every = EveryPage(*provider);
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 128, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, provider});
    residency.ConfigureField(2, 128, 2, 0.0f);
    residency.Configure(2, 2, PageSource{nullptr, provider});
    uint64 frame = 0;
    for (int i = 0; i < 10; ++i)
    {
        residency.Request(1, every);
        residency.Request(2, every);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }
    residency.Forget(2); // a terrain deleted while the other's camera is parked
    for (int i = 0; i < 3; ++i)
    {
        residency.Request(1, every);
        EXPECT_NO_THROW(residency.Update(++frame, 1.0f / 60.0f, false)) << "frame " << i << " after Forget";
    }
    EXPECT_EQ(residency.FieldCache(1).ResidentCount(), every.size());
}

TEST(PageResidencyTest, AnOldSourcesFinishedLoadNeverLandsInARestartedStream)
{
    // Inline loads: the old source's page has finished (past any cancel check) before the restart,
    // so the epoch is the only guard. (The cancel token guards loads still queued at the restart;
    // ALoadOfARestartedStreamsOldSourceNeverLands covers both together.)
    GeneratedHeightLattice flatLattice = NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1);
    flatLattice.Source = GeneratedHeightSource::Flat;
    const auto oldSource = std::make_shared<GeneratedHeightPageProvider>(flatLattice);
    const auto newSource = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    const PageWant top = EveryPage(*newSource).back();
    PageResidencyManager residency(nullptr, nullptr, PageResidencyBudget{});
    residency.ConfigureField(1, 16, 2, 0.0f);
    residency.Configure(1, 1, PageSource{nullptr, oldSource});
    residency.Request(1, std::vector<PageWant>{top});
    residency.Update(1, 1.0f / 60.0f, false); // the old page is filled and queued
    residency.Configure(1, 1, PageSource{nullptr, newSource});
    for (uint64 frame = 2; frame < 8 && residency.FieldCache(1).SlotOf(CachedPage{1, top.Address}) == kNoResidentSlot;
         ++frame)
    {
        residency.Request(1, std::vector<PageWant>{top});
        residency.Update(frame, 1.0f / 60.0f, false);
    }
    std::vector<float32> expected(kPageSampleCount);
    ASSERT_TRUE(newSource->FillPage(top.Address, expected));
    ASSERT_NE(residency.Samples(1, top.Address), nullptr);
    EXPECT_EQ(*residency.Samples(1, top.Address), expected) << "the stream holds the old source's page";
}

TEST(PageResidencyTest, ACancelledCookLeavesNoStoreAndNoTemporaryFile)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory("cancelled_cook");
    std::filesystem::create_directories(dir);
    const std::filesystem::path source = dir / "dem.r32";
    WriteR32(source, 300, 260, Relief);
    HeightCookRequest request;
    ASSERT_EQ(ResolveHeightCookSource(source, 300, 260, request.Source), "");
    ASSERT_EQ(ScanHeightSource(request.Source, nullptr, request.Identity), "");
    request.Output = dir / "dem.gepage";
    const std::atomic<bool> cancelled{true}; // raised while the cook runs its first band
    request.Cancel = &cancelled;
    const HeightCookResult result = CookHeightPageStore(request);
    EXPECT_NE(result.Error.find("cancelled"), std::string::npos);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        files += entry.path() != source ? 1u : 0u;
    EXPECT_EQ(files, 0u) << "a cancelled cook left a store or a temporary file";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- Height page overlay (the modifiers' baked-page cache) ----------------------------------------

namespace
{

// The store's pyramid filter, written out: level L + 1 sample (x, z) is the [1 2 1] / 4 tent of
// level L over clamped indices (HeightPyramidBuilder).
std::vector<std::vector<float32>> TentPyramid(std::vector<float32> level0, const std::vector<PageStoreLevel>& levels)
{
    std::vector<std::vector<float32>> out{std::move(level0)};
    for (std::size_t level = 1; level < levels.size(); ++level)
    {
        const PageStoreLevel& finer = levels[level - 1];
        const PageStoreLevel& coarser = levels[level];
        std::vector<float32> next(static_cast<std::size_t>(coarser.SamplesX) * coarser.SamplesZ);
        const auto at = [&](int64 x, int64 z) {
            x = std::clamp<int64>(x, 0, finer.SamplesX - 1);
            z = std::clamp<int64>(z, 0, finer.SamplesZ - 1);
            return out.back()[static_cast<std::size_t>(z) * finer.SamplesX + static_cast<std::size_t>(x)];
        };
        for (uint32 z = 0; z < coarser.SamplesZ; ++z)
            for (uint32 x = 0; x < coarser.SamplesX; ++x)
            {
                float32 sum = 0.0f;
                for (int64 dz = -1; dz <= 1; ++dz)
                {
                    const float32 row = 0.25f * at(2 * int64{x} - 1, 2 * int64{z} + dz) +
                                        0.5f * at(2 * int64{x}, 2 * int64{z} + dz) +
                                        0.25f * at(2 * int64{x} + 1, 2 * int64{z} + dz);
                    sum += (dz == 0 ? 0.5f : 0.25f) * row;
                }
                next[static_cast<std::size_t>(z) * coarser.SamplesX + x] = sum;
            }
        out.push_back(std::move(next));
    }
    return out;
}

// A deterministic delta over `rect`, as a modifier footprint would leave.
std::vector<float32> RectDelta(const PageSampleRect& rect, float32 seed)
{
    std::vector<float32> delta(static_cast<std::size_t>(rect.Width()) * rect.Height());
    for (uint32 z = 0; z < rect.Height(); ++z)
        for (uint32 x = 0; x < rect.Width(); ++x)
            delta[static_cast<std::size_t>(z) * rect.Width() + x] =
                std::sin(seed + 0.37f * static_cast<float32>(x)) * std::cos(0.21f * static_cast<float32>(z));
    return delta;
}

std::vector<float32> Dense(const PageSampleRect& rect, const std::vector<float32>& delta, const PageStoreLevel& level0)
{
    std::vector<float32> dense(static_cast<std::size_t>(level0.SamplesX) * level0.SamplesZ, 0.0f);
    for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
        for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
            dense[static_cast<std::size_t>(z) * level0.SamplesX + x] =
                delta[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)];
    return dense;
}

void ExpectOverlayIsThePyramidOf(const HeightPageOverlay& overlay, const std::vector<std::vector<float32>>& pyramid,
                                 const std::vector<PageStoreLevel>& levels)
{
    for (uint32 level = 0; level < levels.size(); ++level)
        for (uint32 z = 0; z < levels[level].SamplesZ; ++z)
            for (uint32 x = 0; x < levels[level].SamplesX; ++x)
                ASSERT_NEAR(overlay.Delta(level, x, z),
                            pyramid[level][static_cast<std::size_t>(z) * levels[level].SamplesX + x], 1.0e-6f)
                    << "level " << level << " sample (" << x << ", " << z << ")";
}

} // namespace

TEST(HeightPageOverlayTest, EachLevelIsTheStoresTentOfTheBakedMinusTheBase)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    const PageSampleRect rect{100, 40, 250, 170};
    const std::vector<float32> delta = RectDelta(rect, 0.5f);
    const HeightPageOverlay overlay(levels, rect, delta);
    ExpectOverlayIsThePyramidOf(overlay, TentPyramid(Dense(rect, delta, levels.front()), levels), levels);
}

TEST(HeightPageOverlayTest, AnEditRebakesOnlyItsRectAndEqualsAFreshOverlay)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    const PageSampleRect rect{100, 40, 250, 170};
    std::vector<float32> delta = RectDelta(rect, 0.5f);
    HeightPageOverlay after(levels, rect, delta);
    const PageSampleRect edit{180, 90, 210, 140};
    const std::vector<float32> edited = RectDelta(edit, 2.0f);
    after.Rebake(edit, edited);
    for (uint32 z = edit.MinZ; z <= edit.MaxZ; ++z)
        for (uint32 x = edit.MinX; x <= edit.MaxX; ++x)
            delta[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)] =
                edited[static_cast<std::size_t>(z - edit.MinZ) * edit.Width() + (x - edit.MinX)];
    ExpectOverlayIsThePyramidOf(after, TentPyramid(Dense(rect, delta, levels.front()), levels), levels);
}

TEST(HeightPageOverlayTest, APageGainsItsLevelsDeltaUnderEveryStoredSampleApronIncluded)
{
    const std::vector<PageStoreLevel> levels = BuildPageStoreLevels(601, 300);
    // Level-0 samples 120 to 140 on both axes: across the edge between pages (1, 0) and (0, 0) and
    // the edge between (1, 0) and (1, 1), so page (1, 0)'s first apron column (x 127) and last apron
    // row (z 128) lie inside the overlay.
    const PageSampleRect rect{120, 120, 140, 140};
    const HeightPageOverlay overlay(levels, rect, RectDelta(rect, 0.5f));
    const uint32 pageX = 1u;
    const PageAddress page{0, 0, pageX, 0};
    std::vector<float32> samples(kPageSampleCount, 1.0f);
    ASSERT_TRUE(overlay.ApplyToPage(page, samples));
    const auto sampleAt = [&](uint32 stored, uint32 origin, uint32 count) {
        return std::min<uint32>(origin + stored > 0u ? origin + stored - 1u : 0u, count - 1u);
    };
    for (uint32 row = 0; row < kPageStrideSamples; ++row)
        for (uint32 i = 0; i < kPageStrideSamples; ++i)
        {
            const uint32 x = sampleAt(i, pageX * kPageOwnedSamples, levels[0].SamplesX);
            const uint32 z = sampleAt(row, 0u, levels[0].SamplesZ);
            ASSERT_FLOAT_EQ(samples[row * kPageStrideSamples + i], 1.0f + overlay.Delta(0, x, z));
        }
    const uint32 lastRow = kPageStrideSamples - 1u;
    // Stored row r holds level-0 row r - 1 on this page (z 124 is row 125); stored column i holds
    // x = 127 + i.
    EXPECT_NE(overlay.Delta(0, pageX * kPageOwnedSamples - 1u, 124u), 0.0f) << "the first apron column is reached";
    EXPECT_NE(samples[125u * kPageStrideSamples + 0u], 1.0f) << "the first apron column gains its delta";
    EXPECT_NE(overlay.Delta(0, 130u, kPageOwnedSamples), 0.0f) << "the last apron row is reached";
    EXPECT_NE(samples[lastRow * kPageStrideSamples + (130u - pageX * kPageOwnedSamples + 1u)], 1.0f)
        << "the last apron row gains its delta";
    std::vector<float32> far(kPageSampleCount, 1.0f);
    EXPECT_FALSE(overlay.ApplyToPage(PageAddress{0, 0, 4, 2}, far)) << "a page no modifier reaches is untouched";
    EXPECT_EQ(far, std::vector<float32>(kPageSampleCount, 1.0f));
}

TEST(PageResidencyTest, ARefreshedResidentPageIsRewrittenInItsSlotAndNeverLeavesIt)
{
    JobSystem::WorkStealingThreadPool pool(2);
    const auto provider = std::make_shared<GeneratedHeightPageProvider>(NoiseLattice(0.0f, 0.0f, 1024.0f, 1, 1));
    PageResidencyManager residency(&pool, nullptr, PageResidencyBudget{});
    residency.ConfigureField(7, 128, 2, 0.4f);
    residency.Configure(7, 7, PageSource{nullptr, provider});
    const std::vector<PageWant> wanted = EveryPage(*provider);
    residency.Request(7, wanted);
    uint64 frame = RunFramesUntil(residency, 0, [&] { return residency.FieldCache(7).ResidentCount() == wanted.size(); });
    ASSERT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size());
    for (int i = 0; i < 40; ++i)
    {
        residency.Request(7, wanted);
        residency.Update(++frame, 1.0f / 60.0f, false);
    }

    // An edit over level-0 samples 10 to 20: the level-0 page (0, 0) and its ancestors read them.
    const PageSampleRect edit{10, 10, 20, 20};
    const CachedPage edited{7, PageAddress{0, 0, 0, 0}};
    const uint32 slot = residency.FieldCache(7).SlotOf(edited);
    residency.Refresh(7, edit);
    std::vector<CachedPage> rewritten;
    frame = RunFramesUntil(residency, frame, [&] {
        residency.Request(7, wanted);
        EXPECT_EQ(residency.FieldCache(7).ResidentCount(), wanted.size()) << "a refreshed page left its slot";
        for (const PageUploadRequest& upload : residency.FieldCache(7).Uploads())
        {
            EXPECT_NE(residency.Samples(7, upload.Page.Address), nullptr) << "a rewrite without its samples";
            rewritten.push_back(upload.Page);
        }
        return rewritten.size() >= provider->Levels().size();
    });
    EXPECT_EQ(residency.FieldCache(7).SlotOf(edited), slot);
    EXPECT_NE(std::find(rewritten.begin(), rewritten.end(), edited), rewritten.end());
    for (const CachedPage& page : rewritten)
        EXPECT_TRUE(PageReadsLevel0Rect(page.Address, edit))
            << "level " << int(page.Address.Level) << " page (" << page.Address.X << ", " << page.Address.Z
            << ") was rewritten though the edit is not under it";
    EXPECT_EQ(rewritten.size(), provider->Levels().size()) << "one page per level reads the edit";
    pool.Shutdown();
}
