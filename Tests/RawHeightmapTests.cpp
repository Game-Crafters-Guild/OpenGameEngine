#include <gtest/gtest.h>

#include "Terrain/Heightfield.h"
#include "TerrainECS/RawHeightmap.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

// A .r32 of `width` x `height` samples whose value names its own position: row * width + column.
// Read on the wrong grid, a sample reports the position it was really written at.
std::filesystem::path WriteIndexedR32(const std::string& name, uint32 width, uint32 height)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::vector<float32> row(width);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (uint32 z = 0; z < height; ++z)
    {
        for (uint32 x = 0; x < width; ++x)
            row[x] = static_cast<float32>(static_cast<uint64>(z) * width + x);
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(width * sizeof(float32)));
    }
    return path;
}

// The user's case: a 16384 x 4096 DEM holds 8192^2 samples, so with no declared grid it decodes as
// a square 8192 grid of the wrong stride (each decoded row is half a source row, rows alternate
// between the two halves) and renders as stripes. Declared in its import settings, every sample
// lands where it was exported.
TEST(RawHeightmap, ANonSquareGridDecodesOnTheGridItsImportSettingsDeclare)
{
    constexpr uint32 kWidth = 16384;
    constexpr uint32 kHeight = 4096;
    const std::filesystem::path path = WriteIndexedR32("ge_raw_heightmap_16384x4096.r32", kWidth, kHeight);

    Terrain::HeightfieldData declared;
    const std::string declaredError = DecodeRawHeightmap(path, kWidth, kHeight, declared);
    Terrain::HeightfieldData undeclared;
    const std::string undeclaredError = DecodeRawHeightmap(path, 0, 0, undeclared);
    std::filesystem::remove(path);

    ASSERT_TRUE(declaredError.empty()) << declaredError;
    ASSERT_EQ(declared.GetWidth(), kWidth);
    ASSERT_EQ(declared.GetHeight(), kHeight);
    for (const auto [x, z] : {std::pair{0u, 0u}, std::pair{kWidth - 1, 1u}, std::pair{123u, 2047u},
                              std::pair{kWidth - 1, kHeight - 1}})
    {
        EXPECT_EQ(declared.GetSample(x, z), static_cast<float32>(static_cast<uint64>(z) * kWidth + x))
            << "sample (" << x << ", " << z << ")";
    }

    // Undeclared, the count still reads as a square: this is the stripe decode the settings exist
    // to prevent. Row 1 of the square grid is the right half of source row 0.
    ASSERT_TRUE(undeclaredError.empty()) << undeclaredError;
    EXPECT_EQ(undeclared.GetWidth(), 8192u);
    EXPECT_EQ(undeclared.GetSample(0, 1), 8192.0f);
}

// A grid that is not square and declares nothing cannot be read at all. The refusal says what to
// do instead of leaving the author with a flat terrain and a guess.
TEST(RawHeightmap, AnUndeclaredNonSquareGridIsRefusedWithTheFix)
{
    RawHeightmapLayout layout;
    const std::string reason = ResolveRawHeightmapLayout(3000ull * 1000ull * sizeof(float32), sizeof(float32),
                                                         0, 0, layout);

    EXPECT_NE(reason.find("set Samples X (or Samples Z) in the heightmap's Import Settings"), std::string::npos)
        << reason;
    EXPECT_EQ(layout.Width, 0u);
}

// Declared settings that do not account for the file are refused rather than read on a grid the
// file was not written on; an undeclared square grid still needs no settings.
TEST(RawHeightmap, DeclaredSettingsMustMatchTheFileAndASquareNeedsNone)
{
    RawHeightmapLayout layout;
    // A 64^2-sample file reads as a square with nothing declared; declared 100 x 40 it does not add up.
    EXPECT_FALSE(ResolveRawHeightmapLayout(64ull * 64ull * sizeof(uint16), sizeof(uint16), 100, 40, layout).empty());
    EXPECT_FALSE(ResolveRawHeightmapLayout(129ull * 129ull * sizeof(float32) + 2ull, sizeof(float32), 0, 0, layout)
                     .empty());

    EXPECT_TRUE(ResolveRawHeightmapLayout(3000ull * 1000ull * sizeof(uint16), sizeof(uint16), 3000, 1000, layout)
                    .empty());
    EXPECT_EQ(layout.Width, 3000u);
    EXPECT_EQ(layout.Height, 1000u);
    EXPECT_TRUE(ResolveRawHeightmapLayout(513ull * 513ull * sizeof(uint16), sizeof(uint16), 0, 0, layout).empty());
    EXPECT_EQ(layout.Width, 513u);
    EXPECT_EQ(layout.Height, 513u);
}

// The user types one number: with Samples X set and Samples Z left at 0, Samples Z is what the file's
// sample count leaves. Typing them one after the other never passes through a refused half-set grid.
// A side that does not divide the file is refused with the nearest sides that do.
TEST(RawHeightmap, OneDeclaredSideDerivesTheOther)
{
    constexpr uint64 kSamples = 16384ull * 4096ull;
    RawHeightmapLayout layout;
    ASSERT_TRUE(ResolveRawHeightmapLayout(kSamples * sizeof(float32), sizeof(float32), 16384, 0, layout).empty());
    EXPECT_EQ(layout.Width, 16384u);
    EXPECT_EQ(layout.Height, 4096u);
    ASSERT_TRUE(ResolveRawHeightmapLayout(kSamples * sizeof(float32), sizeof(float32), 0, 4096, layout).empty());
    EXPECT_EQ(layout.Width, 16384u);
    EXPECT_EQ(layout.Height, 4096u);

    const std::string reason =
        ResolveRawHeightmapLayout(3000ull * 1000ull * sizeof(float32), sizeof(float32), 2999, 0, layout);
    EXPECT_NE(reason.find("2,500 or 3,000"), std::string::npos) << reason;
    EXPECT_EQ(layout.Width, 0u);

    // A side equal to the whole file leaves one row: refused, and the suggestion is a side that works,
    // never the file's own count.
    const std::string whole = ResolveRawHeightmapLayout(4096ull * sizeof(float32), sizeof(float32), 4096, 0, layout);
    EXPECT_NE(whole.find("the nearest that does is 2,048"), std::string::npos) << whole;

    // A prime count has no grid at all; the refusal says so rather than suggesting the count itself.
    const std::string prime = ResolveRawHeightmapLayout(7919ull * sizeof(float32), sizeof(float32), 100, 0, layout);
    EXPECT_NE(prime.find("no grid of 2 or more samples each way reads this file"), std::string::npos) << prime;
}

// A stored grid setting that is present but not a sample count must refuse, never read as "unset":
// unset falls back to the square guess, which is the stripe decode the settings exist to prevent.
TEST(RawHeightmap, AStoredSettingThatIsNotASampleCountIsRefused)
{
    uint32 count = 7;
    EXPECT_TRUE(ParseRawHeightmapSampleCount(kRawHeightmapWidthKey, std::nullopt, count).empty());
    EXPECT_EQ(count, 0u);
    EXPECT_TRUE(ParseRawHeightmapSampleCount(kRawHeightmapWidthKey, std::string("4096"), count).empty());
    EXPECT_EQ(count, 4096u);
    // The store cannot remove a value: a field set back to 0 stores "0", and an undo of the first edit
    // writes back "". Both are unset.
    for (const char* unset : {"0", ""})
    {
        EXPECT_TRUE(ParseRawHeightmapSampleCount(kRawHeightmapWidthKey, std::string(unset), count).empty()) << unset;
        EXPECT_EQ(count, 0u) << unset;
    }

    for (const char* text : {"abc", "-5", " 4096", "4096 ", "4096px"})
    {
        const std::string reason = ParseRawHeightmapSampleCount(kRawHeightmapWidthKey, std::string(text), count);
        EXPECT_NE(reason.find("Samples X is stored as"), std::string::npos) << "\"" << text << "\" read as: " << reason;
        EXPECT_EQ(count, 0u) << text;
    }
}

// From the settings as stored to the decode: the path ResolveHeightmapAsset takes after reading the
// asset's store. Declared, the non-square file decodes on its grid; a malformed setting refuses the
// decode instead of falling back to the square guess.
TEST(RawHeightmap, StoredSettingsReachTheDecode)
{
    constexpr uint32 kWidth = 256;
    constexpr uint32 kHeight = 64; // 128^2 samples: square-shaped by count
    const std::filesystem::path path = WriteIndexedR32("ge_raw_heightmap_settings_256x64.r32", kWidth, kHeight);

    RawHeightmapLayout declared;
    ASSERT_TRUE(ResolveDeclaredRawHeightmapLayout({std::string("256"), std::string("64")}, declared).empty());
    Terrain::HeightfieldData decoded;
    const std::string decodeError = DecodeRawHeightmap(path, declared.Width, declared.Height, decoded);

    RawHeightmapLayout malformed;
    const std::string malformedError =
        ResolveDeclaredRawHeightmapLayout({std::string("256px"), std::string("64")}, malformed);
    std::filesystem::remove(path);

    ASSERT_TRUE(decodeError.empty()) << decodeError;
    EXPECT_EQ(decoded.GetWidth(), kWidth);
    EXPECT_EQ(decoded.GetSample(kWidth - 1, 1), static_cast<float32>(kWidth + kWidth - 1));
    EXPECT_FALSE(malformedError.empty());
    EXPECT_EQ(malformed.Width, 0u);
}

} // namespace
