#include "Assets/TextureAlphaProbe.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

namespace GameEngine {

namespace {

// Bounded file-prefix read: PNG ancillary chunks before IDAT (ICC profiles are
// the worst offenders) fit comfortably; KTX2 DFDs sit near the header.
constexpr size_t kProbeReadLimit = 256 * 1024;

uint32 ReadU32LE(const uint8* p)
{
    return static_cast<uint32>(p[0]) | (static_cast<uint32>(p[1]) << 8) |
           (static_cast<uint32>(p[2]) << 16) | (static_cast<uint32>(p[3]) << 24);
}

uint32 ReadU32BE(const uint8* p)
{
    return (static_cast<uint32>(p[0]) << 24) | (static_cast<uint32>(p[1]) << 16) |
           (static_cast<uint32>(p[2]) << 8) | static_cast<uint32>(p[3]);
}

// VkFormat values that provably encode no alpha channel. Everything absent
// from this list (BC1_RGBA punch-through, BC7, ASTC, RGBA formats, ...)
// reports MaybeAlpha.
bool VkFormatHasNoAlpha(uint32 vkFormat)
{
    switch (vkFormat)
    {
    case 4:   // R5G6B5_UNORM_PACK16
    case 5:   // B5G6R5_UNORM_PACK16
    case 9: case 10: case 11: case 12: case 13: case 14: case 15:          // R8_*
    case 16: case 17: case 18: case 19: case 20: case 21: case 22:         // R8G8_*
    case 23: case 24: case 25: case 26: case 27: case 28: case 29:         // R8G8B8_*
    case 30: case 31: case 32: case 33: case 34: case 35: case 36:         // B8G8R8_*
    case 70: case 71: case 72: case 73: case 74: case 75: case 76:         // R16_*
    case 77: case 78: case 79: case 80: case 81: case 82: case 83:         // R16G16_*
    case 84: case 85: case 86: case 87: case 88: case 89: case 90:         // R16G16B16_*
    case 98: case 99: case 100:                                            // R32_*
    case 101: case 102: case 103:                                          // R32G32_*
    case 104: case 105: case 106:                                          // R32G32B32_*
    case 122:            // B10G11R11_UFLOAT_PACK32
    case 123:            // E5B9G9R9_UFLOAT_PACK32
    case 131: case 132:  // BC1_RGB_{UNORM,SRGB}
    case 139: case 140:  // BC4_{UNORM,SNORM}
    case 141: case 142:  // BC5_{UNORM,SNORM}
    case 143: case 144:  // BC6H_{UFLOAT,SFLOAT}
    case 147: case 148:  // ETC2_R8G8B8_{UNORM,SRGB}
    case 153: case 154:  // EAC_R11_{UNORM,SNORM}
    case 155: case 156:  // EAC_R11G11_{UNORM,SNORM}
        return true;
    default:
        return false;
    }
}

TextureAlphaContent ProbeKtx2(const uint8* bytes, size_t size)
{
    // Header: identifier[12], vkFormat u32, typeSize, w, h, depth, layers,
    // faces, levels, supercompressionScheme, dfdByteOffset, dfdByteLength.
    if (size < 56)
        return TextureAlphaContent::MaybeAlpha;

    const uint32 vkFormat = ReadU32LE(bytes + 12);
    if (vkFormat != 0)
        return VkFormatHasNoAlpha(vkFormat) ? TextureAlphaContent::NoAlphaSource
                                            : TextureAlphaContent::MaybeAlpha;

    // Basis-encoded (VK_FORMAT_UNDEFINED): channel layout lives in the DFD.
    // 64-bit offset math throughout: dfdOff comes from the file, and a uint32
    // `dfdOff + 44` wraps for dfdOff >= 0xFFFFFFD4 — the guard would pass
    // while the (64-bit) pointer arithmetic below reads far out of bounds.
    const uint64 dfdOff = ReadU32LE(bytes + 48);
    const uint64 dfdLen = ReadU32LE(bytes + 52);
    // Basic descriptor block: 24-byte header + 16-byte samples, preceded by
    // the u32 dfdTotalSize.
    if (dfdLen < 4 + 24 + 16 || dfdOff + 4 + 24 + 16 > size)
        return TextureAlphaContent::MaybeAlpha;

    const uint8* block = bytes + dfdOff + 4;
    const uint32 blockSize = ReadU32LE(block + 4) >> 16;
    if (blockSize < 24 + 16 || dfdOff + 4 + blockSize > size)
        return TextureAlphaContent::MaybeAlpha;
    const uint32 sampleCount = (blockSize - 24) / 16;
    const uint8 colorModel = block[8];

    constexpr uint8 kColorModelEtc1s = 163;
    constexpr uint8 kColorModelUastc = 166;
    if (colorModel == kColorModelEtc1s)
    {
        // ETC1S: one RGB slice, alpha rides as a second sample when present.
        return sampleCount >= 2 ? TextureAlphaContent::MaybeAlpha
                                : TextureAlphaContent::NoAlphaSource;
    }
    if (colorModel == kColorModelUastc)
    {
        // UASTC: single sample; channel type 0=RGB, 3=RGBA, 4=RRR, 5=RRRG, 6=RG.
        // Whitelist the provably alpha-less types; everything else — RGBA,
        // RRRG, and any unknown/future value — may carry alpha.
        const uint8 channelType = block[24 + 3] & 0x0F;
        const bool provablyNoAlpha = channelType == 0 || channelType == 4 || channelType == 6;
        return provablyNoAlpha ? TextureAlphaContent::NoAlphaSource
                               : TextureAlphaContent::MaybeAlpha;
    }
    return TextureAlphaContent::MaybeAlpha;
}

TextureAlphaContent ProbePng(const uint8* bytes, size_t size)
{
    // Signature(8) + IHDR chunk: len(4) 'IHDR'(4) width(4) height(4)
    // bitDepth(1) colorType(1) ...
    if (size < 26)
        return TextureAlphaContent::MaybeAlpha;
    const uint8 colorType = bytes[25];
    if (colorType == 4 || colorType == 6) // gray+alpha / RGBA
        return TextureAlphaContent::MaybeAlpha;
    if (colorType != 0 && colorType != 2 && colorType != 3)
        return TextureAlphaContent::MaybeAlpha;

    // Gray / truecolor / palette can still discard through a tRNS chunk;
    // scan chunk headers until the first IDAT.
    size_t pos = 8;
    while (pos + 8 <= size)
    {
        const uint32 len = ReadU32BE(bytes + pos);
        const uint8* type = bytes + pos + 4;
        if (std::memcmp(type, "tRNS", 4) == 0)
            return TextureAlphaContent::MaybeAlpha;
        if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0)
            return TextureAlphaContent::NoAlphaSource;
        pos += 8ull + len + 4ull; // header + data + crc
    }
    return TextureAlphaContent::MaybeAlpha; // ran out of bytes before IDAT
}

TextureAlphaContent ProbeTga(const uint8* bytes, size_t size)
{
    // TGA has no magic; caller gates on the .tga extension.
    if (size < 18)
        return TextureAlphaContent::MaybeAlpha;
    const uint8 imageType = bytes[2] & 0x07; // strip RLE bit (8)
    const uint8 bpp = bytes[16];
    if (imageType == 3 && bpp == 8) // grayscale
        return TextureAlphaContent::NoAlphaSource;
    if (imageType == 2 && bpp == 24) // truecolor, no attribute bits possible
        return TextureAlphaContent::NoAlphaSource;
    return TextureAlphaContent::MaybeAlpha;
}

std::string LowerAscii(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

TextureAlphaContent ProbeTextureAlphaFromBytes(const uint8* bytes, size_t size,
                                               std::string_view extensionHint)
{
    if (!bytes || size < 4)
        return TextureAlphaContent::MaybeAlpha;

    static constexpr uint8 kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr uint8 kKtxSig[7] = {0xAB, 'K', 'T', 'X', ' ', '2', '0'};
    if (size >= 8 && std::memcmp(bytes, kPngSig, 8) == 0)
        return ProbePng(bytes, size);
    if (size >= 7 && std::memcmp(bytes, kKtxSig, 7) == 0)
        return ProbeKtx2(bytes, size);
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        return TextureAlphaContent::NoAlphaSource; // JPEG never carries alpha

    if (LowerAscii(extensionHint) == ".tga")
        return ProbeTga(bytes, size);

    return TextureAlphaContent::MaybeAlpha;
}

TextureAlphaContent ProbeTextureFileAlpha(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return TextureAlphaContent::MaybeAlpha;

    std::vector<uint8> buffer(kProbeReadLimit);
    f.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const size_t got = static_cast<size_t>(f.gcount());
    return ProbeTextureAlphaFromBytes(buffer.data(), got, path.extension().string());
}

} // namespace GameEngine
