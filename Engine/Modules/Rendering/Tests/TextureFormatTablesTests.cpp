// The two per-format tables the engine keeps — the texel size used to size
// copies and barriers (BytesPerPixel) and the human name used by diagnostics
// (ToString) — must agree on which formats exist. A format the engine can size
// but cannot name reads as "Unknown" in every capture, log and profiler row;
// a format it can name but not size fails readback with a bogus "block
// compressed" diagnosis. Neither table can be reviewed against the enum by eye,
// so pin the agreement.

#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using GameEngine::Rendering::BytesPerPixel;
using GameEngine::Rendering::ComputeTightCopyRows;
using GameEngine::Rendering::TextureFormat;
using GameEngine::Rendering::TightCopyRows;
using GameEngine::Rendering::ToString;

namespace
{
// Every enumerator, listed explicitly: a range-based scan over the underlying
// integers would silently pass over gaps and could not distinguish a real
// enumerator from an unused value.
const std::vector<TextureFormat>& AllFormats()
{
    static const std::vector<TextureFormat> kAll = {
        TextureFormat::RGBA8_UNORM,      TextureFormat::RGBA8_SRGB,
        TextureFormat::BGRA8_UNORM,      TextureFormat::BGRA8_SRGB,
        TextureFormat::R8_UINT,          TextureFormat::R8_SINT,
        TextureFormat::R8G8_UINT,        TextureFormat::R8G8_SINT,
        TextureFormat::RGBA8_UINT,       TextureFormat::RGBA8_SINT,
        TextureFormat::R16_UINT,         TextureFormat::R16_SINT,
        TextureFormat::R16G16_UINT,      TextureFormat::R16G16_SINT,
        TextureFormat::RGBA16_UINT,      TextureFormat::RGBA16_SINT,
        TextureFormat::R32_UINT,         TextureFormat::R32_SINT,
        TextureFormat::R32G32_UINT,      TextureFormat::R32G32_SINT,
        TextureFormat::R32G32B32_UINT,   TextureFormat::R32G32B32_SINT,
        TextureFormat::RGBA32_UINT,      TextureFormat::RGBA32_SINT,
        TextureFormat::R32G32B32A32_FLOAT, TextureFormat::R16G16B16A16_FLOAT,
        TextureFormat::R16G16B16A16_UNORM, TextureFormat::R11G11B10_FLOAT,
        TextureFormat::RGB10A2_UNORM,    TextureFormat::R16_FLOAT,
        TextureFormat::R16G16_FLOAT,     TextureFormat::R32_FLOAT,
        TextureFormat::R8_UNORM,         TextureFormat::R8G8_UNORM,
        TextureFormat::D32_FLOAT,        TextureFormat::D24_UNORM_S8_UINT,
        TextureFormat::D32_SFLOAT_S8_UINT,
    };
    return kAll;
}
} // namespace

// A sizeable format must also be nameable. This is the regression guard for the
// capture path: it reports the format of whatever render-graph resource it read,
// and "Unknown" there is indistinguishable from a genuinely unknown resource.
TEST(TextureFormatTables, EverySizeableFormatHasAName)
{
    for (TextureFormat f : AllFormats())
    {
        const uint32_t bpp = BytesPerPixel(f);
        if (bpp == 0)
            continue; // block-compressed: no linear texel size by design
        EXPECT_STRNE(ToString(f), "Unknown")
            << "format " << static_cast<uint32_t>(f) << " has a texel size (" << bpp
            << " bytes) but no name — diagnostics and captures will report Unknown";
    }
}

// The inverse: a named non-block format must be sizeable, or a readback of it
// fails with a "block-compressed" diagnosis that is simply wrong.
TEST(TextureFormatTables, EveryNamedUncompressedFormatHasATexelSize)
{
    for (TextureFormat f : AllFormats())
    {
        const std::string name = ToString(f);
        if (name.rfind("BC", 0) == 0)
            continue; // block-compressed
        EXPECT_GT(BytesPerPixel(f), 0u)
            << "format " << name << " is named but has no texel size";
    }
}

// Unknown is the sentinel both tables use for "no format": it must stay
// unnameable-as-a-real-format and unsizeable, because the readback path treats a
// zero desc format as absent.
TEST(TextureFormatTables, UnknownIsTheSentinel)
{
    EXPECT_EQ(static_cast<uint32_t>(TextureFormat::Unknown), 0u);
    EXPECT_STREQ(ToString(TextureFormat::Unknown), "Unknown");
    EXPECT_EQ(BytesPerPixel(TextureFormat::Unknown), 0u);
}

// Sizes the capture path depends on directly: the packed 32-bit formats (whose
// absence from a backend-local table sized a buffer barrier past the end of a
// readback buffer) and the narrow formats where a 4-byte fallback overshoots.
TEST(TextureFormatTables, PackedAndNarrowFormatSizes)
{
    EXPECT_EQ(BytesPerPixel(TextureFormat::R11G11B10_FLOAT), 4u);
    EXPECT_EQ(BytesPerPixel(TextureFormat::RGB10A2_UNORM), 4u);
    EXPECT_EQ(BytesPerPixel(TextureFormat::R8_UNORM), 1u);
    EXPECT_EQ(BytesPerPixel(TextureFormat::R16_FLOAT), 2u);
    EXPECT_EQ(BytesPerPixel(TextureFormat::R32_FLOAT), 4u);
    EXPECT_EQ(BytesPerPixel(TextureFormat::D32_FLOAT), 4u);
    EXPECT_EQ(BytesPerPixel(TextureFormat::R16G16B16A16_FLOAT), 8u);
}

// The rows of a tightly packed buffer-texture copy, which a zero row pitch
// means. A block-compressed row is ceil(width / 4) blocks of the format's block
// size, and a slice is ceil(height / 4) such rows. An uncompressed row is
// `width` texels, and a slice is `height` rows. The Metal copies take a zero
// slice pitch as RowBytes * RowCount, so a RowCount in texel rows puts every
// slice after the first of a multi-slice BC copy four times too far apart.
TEST(TextureFormatTables, TightCopyRowsCountRowsOfBlocks)
{
    const TightCopyRows bc1 = ComputeTightCopyRows(TextureFormat::BC1_UNORM, 10, 6);
    EXPECT_EQ(bc1.RowBytes, 24u); // 3 blocks of 8 bytes
    EXPECT_EQ(bc1.RowCount, 2u);
    const TightCopyRows bc7 = ComputeTightCopyRows(TextureFormat::BC7_UNORM, 5, 3);
    EXPECT_EQ(bc7.RowBytes, 32u); // 2 blocks of 16 bytes
    EXPECT_EQ(bc7.RowCount, 1u);
    const TightCopyRows rgba8 = ComputeTightCopyRows(TextureFormat::RGBA8_UNORM, 5, 3);
    EXPECT_EQ(rgba8.RowBytes, 20u);
    EXPECT_EQ(rgba8.RowCount, 3u);
}
