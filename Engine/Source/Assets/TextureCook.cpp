#include "Assets/TextureCook.h"

#include "Assets/TextureCookWorkers.h"
#include "Assets/Textures/BC7TransparentEndpointRepair.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <climits>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

#if defined(GE_HAVE_STB)
#include <stb_image.h>
#include <stb_image_resize2.h>
#endif
#if defined(GE_HAVE_KTX)
#include <ktx.h>
#include <vulkan/vulkan_core.h>
#endif
#if defined(GE_HAVE_DIRECTXTEX)
#include <DirectXTex.h>
#endif

namespace GameEngine {

namespace
{
struct SlotUsage
{
    std::string_view Name;
    StringId Slot;
    TextureCookUsage Usage;
};

constexpr SlotUsage MakeSlotUsage(std::string_view name, TextureCookUsage usage)
{
    return {name, HashStringId(name), usage};
}

constexpr SlotUsage kSlotUsages[] = {
    MakeSlotUsage("albedoMap", TextureCookUsage::Color),
    MakeSlotUsage("emissiveMap", TextureCookUsage::Color),
    MakeSlotUsage("triplanarAlbedoTop", TextureCookUsage::Color),
    MakeSlotUsage("triplanarAlbedoSide", TextureCookUsage::Color),
    MakeSlotUsage("triplanarAlbedoBottom", TextureCookUsage::Color),
    MakeSlotUsage("normalMap", TextureCookUsage::Normal),
    MakeSlotUsage("coatNormalMap", TextureCookUsage::Normal),
    MakeSlotUsage("triplanarNormalTop", TextureCookUsage::Normal),
    MakeSlotUsage("triplanarNormalSide", TextureCookUsage::Normal),
    MakeSlotUsage("triplanarNormalBottom", TextureCookUsage::Normal),
    MakeSlotUsage("metallicRoughnessMap", TextureCookUsage::Packed),
    MakeSlotUsage("aoMap", TextureCookUsage::Mask),
    MakeSlotUsage("roughnessMap", TextureCookUsage::Mask),
    MakeSlotUsage("metallicMap", TextureCookUsage::Mask),
    // The relief march reads R of a linear height map (1 = the polygon surface).
    MakeSlotUsage("heightMap", TextureCookUsage::Mask),
};

const SlotUsage* FindSlotUsage(StringId slotName)
{
    for (const auto& entry : kSlotUsages)
    {
        if (entry.Slot == slotName)
            return &entry;
    }
    return nullptr;
}
} // namespace

TextureCookUsage TextureCookUsageForMaterialSlot(StringId slotName)
{
    const SlotUsage* entry = FindSlotUsage(slotName);
    return entry ? entry->Usage : TextureCookUsage::Auto;
}

std::string_view TextureCookUsageSlotName(StringId slotName)
{
    const SlotUsage* entry = FindSlotUsage(slotName);
    return entry ? entry->Name : std::string_view{};
}

std::optional<TextureCookUsage> WidenTextureCookUsage(TextureCookUsage current, TextureCookUsage required)
{
    if (required == TextureCookUsage::Auto || required == current)
        return current;
    if (current == TextureCookUsage::Auto)
        return required;
    // Two different usages, neither Auto: Mask and Packed are the only pair one cook serves.
    const bool maskAndPacked = (current == TextureCookUsage::Mask && required == TextureCookUsage::Packed) ||
                               (current == TextureCookUsage::Packed && required == TextureCookUsage::Mask);
    if (!maskAndPacked)
        return std::nullopt;
    return TextureCookUsage::Packed;
}

bool IsLinearTextureUsage(TextureCookUsage usage)
{
    return usage == TextureCookUsage::Normal || usage == TextureCookUsage::Mask ||
           usage == TextureCookUsage::Packed;
}

bool IsLinearTextureSlot(StringId slotName)
{
    return IsLinearTextureUsage(TextureCookUsageForMaterialSlot(slotName));
}

bool ResolveTextureCookInputs(
    const std::string& extensionLower,
    const std::function<bool(const char*, std::string&)>& readMetadata,
    TextureCookInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    std::string value;
    if (readMetadata(kTextureCompressionMetaKey, value))
        ParseTextureCookCompressionMeta(value, inputs.Settings.Compression);
    value.clear();
    if (readMetadata(kTextureUsageMetaKey, value))
        ParseTextureCookUsageMeta(value, inputs.Settings.Usage);
    value.clear();
    if (readMetadata(kTextureMipsMetaKey, value))
        inputs.Settings.MipsEnabled = value != "0";
    value.clear();
    if (readMetadata(kTextureMipLimitMetaKey, value) && !value.empty())
        inputs.Settings.MipLimit = static_cast<uint32>(std::strtoul(value.c_str(), nullptr, 10));
    inputs.ColorSpace = GuessTextureColorSpace(extensionLower);
    value.clear();
    if (readMetadata(kTextureColorSpaceMetaKey, value))
        ParseTextureColorSpaceMeta(value, inputs.ColorSpace);
    value.clear();
    if (!readMetadata(kTextureSwizzleMetaKey, value) || !ParseTextureSwizzleMeta(value, inputs.Swizzle))
        std::memset(inputs.Swizzle, 0, sizeof(inputs.Swizzle));
    std::string coverageEnabled, coverageCutoff;
    readMetadata(kTextureAlphaCoverageMetaKey, coverageEnabled);
    readMetadata(kTextureAlphaCutoffMetaKey, coverageCutoff);
    return ParseTextureAlphaCoverageMeta(coverageEnabled, coverageCutoff, inputs.Settings, error) &&
           ValidateTextureAlphaCoverageSettings(inputs.Settings, extensionLower == ".hdr", error);
}

bool IsTextureCookSourceExtension(const std::string& extensionLower)
{
    return extensionLower == ".png" || extensionLower == ".jpg" || extensionLower == ".jpeg" ||
           extensionLower == ".bmp" || extensionLower == ".tga" || extensionLower == ".hdr" ||
           extensionLower == ".psd" || extensionLower == ".gif" || extensionLower == ".pic" ||
           extensionLower == ".pnm" || extensionLower == ".ppm" || extensionLower == ".pgm";
}

bool NeedsTextureCook(const TextureCookInputs& inputs, TextureCookOutput output)
{
    return output != TextureCookOutput::Uncompressed || inputs.Settings.MipsEnabled || inputs.Swizzle[0] != 0;
}

std::string TextureCookArtifactName(const GUID& guid, uint64 sourceHash,
                                    const TextureCookInputs& inputs, TextureCookOutput output,
                                    std::optional<TextureCookEncodeQuality> quality)
{
    char hashes[35];
    std::snprintf(hashes, sizeof(hashes), "-%016llx-%016llx",
                  static_cast<unsigned long long>(sourceHash),
                  static_cast<unsigned long long>(ComputeTextureCookConfigHash(inputs, output, quality)));
    return guid.ToString() + hashes + ".ktx2";
}

namespace
{
    // Bump when the cook recipe changes (filtering, encoder settings, container
    // layout): every cached artifact re-keys to a clean miss.
    constexpr uint32 kTextureCookVersion = 2;

    // Encoder-library versions folded into the config hash: a vcpkg bump that
    // changes artifact bytes under an unchanged recipe must re-key to a clean
    // miss, never serve a stale artifact (mirrors GE_MESHOPT_VERSION in the
    // mesh-LOD cook). DirectXTex bytes only reach block-compressed outputs;
    // stbir shapes every mip chain.
#if defined(GE_HAVE_DIRECTXTEX)
    constexpr uint32 kDirectXTexVersion = DIRECTX_TEX_VERSION;
#else
    constexpr uint32 kDirectXTexVersion = 0;
#endif
#if defined(GE_STBIR_VERSION)
    constexpr const char* kStbirVersion = GE_STBIR_VERSION;
#else
    constexpr const char* kStbirVersion = "stbir-none";
#endif

    constexpr uint64 kFnvOffset = 14695981039346656037ull;
    constexpr uint64 kFnvPrime = 1099511628211ull;

    uint64 FnvAppend(uint64 hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= kFnvPrime;
        }
        return hash;
    }
}

bool ParseTextureCookCompressionMeta(const std::string& value, TextureCookCompression& out)
{
    if (value == "bc1") { out = TextureCookCompression::BC1; return true; }
    if (value == "bc4") { out = TextureCookCompression::BC4; return true; }
    if (value == "bc5") { out = TextureCookCompression::BC5; return true; }
    if (value == "bc6h") { out = TextureCookCompression::BC6H; return true; }
    if (value == "bc7") { out = TextureCookCompression::BC7; return true; }
    if (value == "none") { out = TextureCookCompression::None; return true; }
    return false; // "auto" / empty / unrecognized -> keep default
}

const char* TextureCookCompressionMetaValue(TextureCookCompression c)
{
    switch (c)
    {
    case TextureCookCompression::BC1:  return "bc1";
    case TextureCookCompression::BC4:  return "bc4";
    case TextureCookCompression::BC5:  return "bc5";
    case TextureCookCompression::BC6H: return "bc6h";
    case TextureCookCompression::BC7:  return "bc7";
    case TextureCookCompression::None: return "none";
    default:                           return "auto";
    }
}

bool ParseTextureCookUsageMeta(const std::string& value, TextureCookUsage& out)
{
    if (value == "color") { out = TextureCookUsage::Color; return true; }
    if (value == "normal") { out = TextureCookUsage::Normal; return true; }
    if (value == "mask") { out = TextureCookUsage::Mask; return true; }
    if (value == "packed") { out = TextureCookUsage::Packed; return true; }
    return false;
}

const char* TextureCookUsageMetaValue(TextureCookUsage u)
{
    switch (u)
    {
    case TextureCookUsage::Color:  return "color";
    case TextureCookUsage::Normal: return "normal";
    case TextureCookUsage::Mask:   return "mask";
    case TextureCookUsage::Packed: return "packed";
    default:                       return "auto";
    }
}

bool ParseTextureFilterMeta(const std::string& value, TextureFilterMode& out)
{
    if (value == "trilinear") { out = TextureFilterMode::Trilinear; return true; }
    if (value == "bilinear") { out = TextureFilterMode::Bilinear; return true; }
    if (value == "point") { out = TextureFilterMode::Point; return true; }
    return false;
}

const char* TextureFilterMetaValue(TextureFilterMode f)
{
    switch (f)
    {
    case TextureFilterMode::Trilinear: return "trilinear";
    case TextureFilterMode::Bilinear:  return "bilinear";
    case TextureFilterMode::Point:     return "point";
    default:                           return "inherit";
    }
}

bool ParseTextureAlphaCoverageMeta(const std::string& enabled, const std::string& cutoff,
                                   TextureCookSettings& settings, std::string& error)
{
    error.clear();
    if (enabled.empty() || enabled == "0")
    {
        settings.PreserveAlphaCoverage = false;
        settings.AlphaCoverageCutoff = 0.5f;
        return true;
    }
    if (enabled != "1")
    {
        error = "alpha coverage enable must be 0 or 1";
        return false;
    }
    float value = 0.5f;
    if (!cutoff.empty())
    {
#if defined(__APPLE__)
        // Floating-point from_chars requires macOS 26. Use an explicit C locale
        // on older deployment targets without changing the process locale.
        locale_t locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        char* end = nullptr;
        const int savedErrno = errno;
        errno = 0;
        if (locale) value = strtof_l(cutoff.c_str(), &end, locale);
        const bool rangeError = errno == ERANGE && (value == 0.0f || !std::isfinite(value));
        errno = savedErrno;
        if (locale) freelocale(locale);
        // Match from_chars: no leading whitespace/plus or hexadecimal input.
        const bool decimalStart = (cutoff[0] >= '0' && cutoff[0] <= '9') ||
                                  cutoff[0] == '-' || cutoff[0] == '.';
        const bool parsedAll = locale && decimalStart && cutoff.find_first_of("xX") == std::string::npos &&
                               end == cutoff.data() + cutoff.size() && !rangeError;
#else
        const auto parsed = std::from_chars(cutoff.data(), cutoff.data() + cutoff.size(), value);
        const bool parsedAll = parsed.ec == std::errc{} && parsed.ptr == cutoff.data() + cutoff.size();
#endif
        if (!parsedAll ||
            !std::isfinite(value) || value < 0.0f || value > 1.0f)
        {
            error = "alpha coverage cutoff must be a finite number in [0,1]";
            return false;
        }
    }
    settings.PreserveAlphaCoverage = true;
    settings.AlphaCoverageCutoff = value == 0.0f ? 0.0f : value;
    return true;
}

std::string TextureAlphaCutoffMetaValue(float cutoff)
{
    if (!std::isfinite(cutoff) || cutoff < 0.0f || cutoff > 1.0f)
        return {};
    if (cutoff == 0.0f) cutoff = 0.0f; // canonical positive zero
    char text[32];
    const auto result = std::to_chars(text, text + sizeof(text), cutoff,
                                      std::chars_format::general,
                                      std::numeric_limits<float>::max_digits10);
    return result.ec == std::errc{} ? std::string(text, result.ptr) : std::string{};
}

bool ValidateTextureAlphaCoverageSettings(const TextureCookSettings& settings,
                                          bool isFloat, std::string& error)
{
    error.clear();
    if (!settings.PreserveAlphaCoverage)
        return true;
    if (!std::isfinite(settings.AlphaCoverageCutoff) || settings.AlphaCoverageCutoff < 0.0f ||
        settings.AlphaCoverageCutoff > 1.0f)
        error = "alpha coverage cutoff must be finite and in [0,1]";
    else if (isFloat || settings.Usage == TextureCookUsage::Normal)
        error = "alpha coverage requires LDR color alpha, not HDR or normal-map data";
    else if ((settings.Compression != TextureCookCompression::Auto &&
              settings.Compression != TextureCookCompression::None &&
              settings.Compression != TextureCookCompression::BC7) ||
             (settings.Compression == TextureCookCompression::Auto &&
              settings.Usage == TextureCookUsage::Mask))
        error = "alpha coverage requires requested uncompressed or BC7 output";
    return error.empty();
}

bool IsTextureCookEncoderAvailable()
{
#if defined(GE_HAVE_DIRECTXTEX) && defined(GE_HAVE_KTX)
    return true;
#else
    return false;
#endif
}

bool IsTextureCookDisabled()
{
    static const bool disabled = []() {
        const char* v = std::getenv("GE_TEXTURE_COOK_DISABLE");
        return v && v[0] == '1';
    }();
    return disabled;
}

bool DecodeBlockPayloadRGBA8(TextureFormat format, uint32 width, uint32 height,
                             const uint8* payload, size_t payloadSize,
                             std::vector<uint8>& outRgba8)
{
    outRgba8.clear();
#if defined(GE_HAVE_DIRECTXTEX)
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    switch (format)
    {
    case TextureFormat::BC1: dxgi = DXGI_FORMAT_BC1_UNORM; break;
    case TextureFormat::BC4: dxgi = DXGI_FORMAT_BC4_UNORM; break;
    case TextureFormat::BC5: dxgi = DXGI_FORMAT_BC5_UNORM; break;
    case TextureFormat::BC7: dxgi = DXGI_FORMAT_BC7_UNORM; break;
    default: return false; // BC6H (HDR) and non-block formats unsupported here
    }

    const uint32 blocksW = (width + 3) / 4;
    const uint32 blocksH = (height + 3) / 4;
    DirectX::Image src{};
    src.width = width;
    src.height = height;
    src.format = dxgi;
    src.rowPitch = static_cast<size_t>(blocksW) * (format == TextureFormat::BC7 ? 16 : (format == TextureFormat::BC5 ? 16 : 8));
    src.slicePitch = src.rowPitch * blocksH;
    src.pixels = const_cast<uint8*>(payload);
    if (payloadSize < src.slicePitch)
        return false;

    DirectX::ScratchImage decoded;
    if (FAILED(DirectX::Decompress(src, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)) ||
        !decoded.GetImage(0, 0, 0))
        return false;
    const DirectX::Image* img = decoded.GetImage(0, 0, 0);
    outRgba8.resize(static_cast<size_t>(width) * height * 4);
    for (uint32 y = 0; y < height; ++y)
        std::memcpy(outRgba8.data() + static_cast<size_t>(y) * width * 4,
                    img->pixels + static_cast<size_t>(y) * img->rowPitch,
                    static_cast<size_t>(width) * 4);
    return true;
#else
    (void)format; (void)width; (void)height; (void)payload; (void)payloadSize;
    return false;
#endif
}

#if defined(GE_HAVE_DIRECTXTEX)
namespace
{
    // BCn blocks are 4x4 pixels.
    constexpr uint32 kBlockDim = 4;

    DXGI_FORMAT DxgiTargetFor(TextureCookOutput out)
    {
        switch (out)
        {
        case TextureCookOutput::BC1:  return DXGI_FORMAT_BC1_UNORM;
        case TextureCookOutput::BC4:  return DXGI_FORMAT_BC4_UNORM;
        case TextureCookOutput::BC5:  return DXGI_FORMAT_BC5_UNORM;
        case TextureCookOutput::BC6H: return DXGI_FORMAT_BC6H_UF16;
        case TextureCookOutput::BC7:  return DXGI_FORMAT_BC7_UNORM;
        default:                      return DXGI_FORMAT_UNKNOWN;
        }
    }
}
#endif

#if defined(GE_HAVE_DIRECTXTEX)
namespace
{
    // One level's encode, split into block-row bands that each land at their own
    // offset of the level's payload. BCn blocks encode independently (no
    // cross-block state at TEX_COMPRESS_DEFAULT; dithering, which would diffuse
    // error across block rows, is not enabled) and every band starts on a
    // block-row boundary, so the payload is byte-identical however the bands are
    // sized and whichever thread encodes each one.
    struct LevelBands
    {
        const TextureCookMipLevel* Level = nullptr;
        bool IsFloat = false;
        TextureCookOutput Output = TextureCookOutput::BC7;
        DXGI_FORMAT SourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT TargetFormat = DXGI_FORMAT_UNKNOWN;
        DirectX::TEX_COMPRESS_FLAGS Flags = DirectX::TEX_COMPRESS_DEFAULT;
        size_t RowPitch = 0;      // source bytes per pixel row
        size_t BlockRowBytes = 0; // payload bytes per block row
        uint32 BandHeight = 0;    // pixel rows per band, a multiple of kBlockDim
        uint32 BandCount = 0;
        uint8* Payload = nullptr; // the level's whole payload, sized up front
        std::atomic<long> FirstFailure{0}; // HRESULT of the first band that failed
    };

    bool EncodeLevelBand(LevelBands& bands, uint32 band)
    {
        const TextureCookMipLevel& level = *bands.Level;
        const uint32 y = band * bands.BandHeight;

        DirectX::Image src{};
        src.width = level.Width;
        src.height = std::min(bands.BandHeight, level.Height - y);
        src.format = bands.SourceFormat;
        src.rowPitch = bands.RowPitch;
        src.slicePitch = bands.RowPitch * src.height;
        src.pixels = const_cast<uint8*>(level.Bytes.data()) + static_cast<size_t>(y) * bands.RowPitch;

        DirectX::ScratchImage compressed;
        const HRESULT hr = DirectX::Compress(src, bands.TargetFormat, bands.Flags,
                                             DirectX::TEX_THRESHOLD_DEFAULT, compressed);
        const DirectX::Image* img = SUCCEEDED(hr) ? compressed.GetImage(0, 0, 0) : nullptr;
        const size_t expectedBytes = bands.BlockRowBytes * ((src.height + kBlockDim - 1) / kBlockDim);
        if (!img || img->slicePitch != expectedBytes)
        {
            long expected = 0;
            bands.FirstFailure.compare_exchange_strong(expected, FAILED(hr) ? static_cast<long>(hr) : E_FAIL);
            return false;
        }
        // DirectXTex lifts transparent BC7 endpoints to alpha 1/255 or 4/255 by its p-bit vote;
        // see BC7TransparentEndpointRepair.h. Per block, so banding stays byte-exact.
        if (bands.Output == TextureCookOutput::BC7 && !bands.IsFloat)
            RepairBC7TransparentEndpoints(src.pixels, static_cast<uint32>(src.width), static_cast<uint32>(src.height),
                                          std::span<uint8>(img->pixels, img->slicePitch));
        std::memcpy(bands.Payload + static_cast<size_t>(y / kBlockDim) * bands.BlockRowBytes, img->pixels,
                    img->slicePitch);
        return true;
    }
}
#endif

bool CompressTextureCookLevel(const TextureCookMipLevel& level, bool isFloat,
                              TextureCookOutput output, uint32 bandBlocks,
                              std::vector<uint8>& outBytes, std::string& outError,
                              const std::function<bool()>& cancelRequested,
                              std::optional<TextureCookEncodeQuality> quality,
                              TextureCookWorkers* workers)
{
    outBytes.clear();
#if defined(GE_HAVE_DIRECTXTEX)
    const DXGI_FORMAT dstFormat = DxgiTargetFor(output);
    if (dstFormat == DXGI_FORMAT_UNKNOWN)
    {
        outError = "not a block-compressed output";
        return false;
    }

    LevelBands bands;
    bands.Level = &level;
    bands.IsFloat = isFloat;
    bands.Output = output;
    bands.SourceFormat = isFloat ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    bands.TargetFormat = dstFormat;
    bands.RowPitch = static_cast<size_t>(level.Width) * 4 * (isFloat ? sizeof(float) : 1);

    // No TEX_COMPRESS_PARALLEL: it forks an OpenMP thread team PER CALLING
    // THREAD, so concurrent cooks each fork their own (observed live: 1,074
    // threads, 7.2 GB WS, machine-wide starvation while the frame's ECS wave
    // waited behind the cooks). One texture spreads across the JobSystem's
    // workers through `workers` instead, within a budget shared by every cook.
    if (output == TextureCookOutput::BC7 &&
        quality.value_or(TextureCookEncodeQualityFor(output)) == TextureCookEncodeQuality::QuickBC7)
        bands.Flags |= DirectX::TEX_COMPRESS_BC7_QUICK;
    const uint32 blocksPerRow = (level.Width + kBlockDim - 1) / kBlockDim;
    const uint32 targetBlocks = bandBlocks != 0 ? bandBlocks : kTextureCookBandBlocks;
    // Clamp to the level's own block rows: a huge bandBlocks over a narrow level
    // (few blocks per row) otherwise scales the row count past 2^30, and the
    // multiply to pixels below wraps bandHeight to 0. The first band is then a
    // zero-height image, which the encoder rejects (E_INVALIDARG) — failing a
    // cook that should have succeeded. The clamp is also exactly the documented
    // contract: a band at or above the level issues one whole-level call.
    const uint32 blockRowsInLevel = std::max(1u, (level.Height + kBlockDim - 1) / kBlockDim);
    const uint32 bandBlockRows =
        std::clamp(targetBlocks / std::max(1u, blocksPerRow), 1u, blockRowsInLevel);
    bands.BandHeight = bandBlockRows * kBlockDim;
    bands.BandCount = (blockRowsInLevel + bandBlockRows - 1) / bandBlockRows;
    const size_t blockBytes = DirectX::BitsPerPixel(dstFormat) * kBlockDim * kBlockDim / 8;
    bands.BlockRowBytes = static_cast<size_t>(blocksPerRow) * blockBytes;
    outBytes.resize(bands.BlockRowBytes * blockRowsInLevel);
    bands.Payload = outBytes.data();

    const auto encodeBand = [&bands](uint32 band) { return EncodeLevelBand(bands, band); };
    bool encoded = true;
    if (workers)
    {
        encoded = workers->RunBands(bands.BandCount, encodeBand, cancelRequested);
    }
    else
    {
        for (uint32 band = 0; band < bands.BandCount && encoded; ++band)
            encoded = !(cancelRequested && cancelRequested()) && encodeBand(band);
    }
    if (encoded)
        return true;

    // A partial payload must never reach the container: drop every band.
    outBytes.clear();
    const long failure = bands.FirstFailure.load();
    if (failure != 0)
        outError = "BC encode failed (hr=" + std::to_string(failure) + ")";
    else
        outError = kTextureCookCancelledError;
    return false;
#else
    (void)level;
    (void)isFloat;
    (void)output;
    (void)bandBlocks;
    (void)cancelRequested;
    (void)quality;
    (void)workers;
    outError = "BC encoder not compiled in";
    return false;
#endif
}

TextureCookOutput ResolveTextureCookOutput(const TextureCookSettings& settings,
                                           bool sourceIsHDR,
                                           bool deviceSupportsBC,
                                           bool encoderAvailable)
{
    const bool bcViable = deviceSupportsBC && encoderAvailable;

    TextureCookOutput wanted = TextureCookOutput::Uncompressed;
    switch (settings.Compression)
    {
    case TextureCookCompression::BC1:  wanted = TextureCookOutput::BC1; break;
    case TextureCookCompression::BC4:  wanted = TextureCookOutput::BC4; break;
    case TextureCookCompression::BC5:  wanted = TextureCookOutput::BC5; break;
    case TextureCookCompression::BC6H: wanted = TextureCookOutput::BC6H; break;
    case TextureCookCompression::BC7:  wanted = TextureCookOutput::BC7; break;
    case TextureCookCompression::None: return TextureCookOutput::Uncompressed;
    case TextureCookCompression::Auto:
        if (sourceIsHDR)
        {
            // HDR content (sky / environment) compresses to BC6H regardless of
            // usage; LDR usages below never apply to float sources.
            wanted = TextureCookOutput::BC6H;
        }
        else
        {
            switch (settings.Usage)
            {
            case TextureCookUsage::Color:  wanted = TextureCookOutput::BC7; break;
            case TextureCookUsage::Normal: wanted = TextureCookOutput::BC5; break;
            case TextureCookUsage::Mask:   wanted = TextureCookOutput::BC4; break;
            case TextureCookUsage::Packed: wanted = TextureCookOutput::BC7; break;
            case TextureCookUsage::Auto:
            default:
                // Unknown usage: stay uncompressed so CPU consumers (UI
                // backgrounds, terrain masks) keep per-pixel access. In the
                // editor a material bind tags a usage and re-keys the cook
                // (TextureService's usage tag); for packaged content the export
                // resolves it once (CollectPackagedTextureUsages), because a
                // packaged mount derives no import data at runtime. A texture no
                // material document references stays here.
                return TextureCookOutput::Uncompressed;
            }
        }
        break;
    }

    return bcViable ? wanted : TextureCookOutput::Uncompressed;
}

uint64 ComputeTextureCookSourceHash(const uint8* bytes, size_t size)
{
    return FnvAppend(kFnvOffset, bytes, size);
}

TextureCookEncodeQuality TextureCookEncodeQualityFor(TextureCookOutput output)
{
#if defined(_DEBUG)
    // Debug DirectXTex's default (max-quality) BC7 search is pathologically slow
    // (minutes per level; profiled in D3DX_BC7::Exhaustive), so Debug cooks the
    // mode-6-only quick encode — a documented Debug-config quality tradeoff.
    // It is a DIFFERENT artifact from the full-quality one, which is why it is
    // folded into the cache key rather than left implicit in the build config.
    return output == TextureCookOutput::BC7 ? TextureCookEncodeQuality::QuickBC7
                                            : TextureCookEncodeQuality::Full;
#else
    (void)output;
    return TextureCookEncodeQuality::Full;
#endif
}

uint64 ComputeTextureCookConfigHash(const TextureCookInputs& inputs, TextureCookOutput output,
                                   std::optional<TextureCookEncodeQuality> quality)
{
    const auto encodeQuality = output == TextureCookOutput::BC7
        ? quality.value_or(TextureCookEncodeQualityFor(output)) : TextureCookEncodeQuality::Full;
    // Canonical debuggable key string. The encoder-version folds tie the key to
    // the built libraries, so keys can differ across platforms/toolchains —
    // fine: the derived cache is machine-local by design.
    char swizzle[5] = {'-', '-', '-', '-', 0};
    if (inputs.Swizzle[0] != 0)
        for (int i = 0; i < 4; ++i)
            swizzle[i] = inputs.Swizzle[i];

    std::string key = "texcook.v" + std::to_string(kTextureCookVersion);
    key += "|cmp="; key += TextureCookCompressionMetaValue(inputs.Settings.Compression);
    key += "|use="; key += TextureCookUsageMetaValue(inputs.Settings.Usage);
    key += "|mips="; key += inputs.Settings.MipsEnabled ? '1' : '0';
    key += "|lim="; key += std::to_string(inputs.Settings.MipLimit);
    key += "|cs="; key += TextureColorSpaceMetaValue(inputs.ColorSpace);
    key += "|sw="; key += swizzle;
    key += "|out="; key += std::to_string(static_cast<uint32>(output));
    key += "|mip="; key += kStbirVersion;
    if (inputs.Settings.PreserveAlphaCoverage)
    {
        // Preserve every legacy key when disabled. Only the opt-in recipe is
        // versioned; exact float bits distinguish cutoff-adjacent references.
        const float cutoff = inputs.Settings.AlphaCoverageCutoff == 0.0f
            ? 0.0f : inputs.Settings.AlphaCoverageCutoff;
        key += "|acov=1|cut=";
        key += std::to_string(std::bit_cast<uint32>(cutoff));
    }
    if (output != TextureCookOutput::Uncompressed)
    {
        key += "|enc="; key += std::to_string(kDirectXTexVersion);
        // The encoder's search quality is a build property, so without it a
        // Debug-cooked (quick) artifact keys identically to a full-quality one
        // and is silently adopted by DebugFast/Release.
        key += "|q="; key += std::to_string(static_cast<uint32>(encodeQuality));
    }
    return FnvAppend(kFnvOffset, key.data(), key.size());
}

uint32 ComputeTextureCookMipCount(uint32 width, uint32 height, const TextureCookSettings& settings)
{
    if (!settings.MipsEnabled || width == 0 || height == 0)
        return 1;
    uint32 levels = 1;
    uint32 w = width, h = height;
    while (w > 1 || h > 1)
    {
        w = std::max(w / 2u, 1u);
        h = std::max(h / 2u, 1u);
        ++levels;
    }
    if (settings.MipLimit > 0)
        levels = std::min(levels, settings.MipLimit);
    return levels;
}

#if defined(GE_HAVE_STB)

namespace
{
    using AlphaHistogram = std::array<uint64, 256>;

    AlphaHistogram AlphaCounts(const TextureCookMipLevel& mip)
    {
        AlphaHistogram counts{};
        for (size_t i = 3; i < mip.Bytes.size(); i += 4)
            ++counts[mip.Bytes[i]];
        return counts;
    }

    uint8 ScaleAlphaByte(uint32 alpha, double scale)
    {
        return static_cast<uint8>(std::clamp(std::floor(alpha * scale + 0.5), 0.0, 255.0));
    }

    void PreserveMipAlphaCoverage(std::vector<TextureCookMipLevel>& chain, float cutoff)
    {
        // The material discards alpha < cutoff: cutoff 0 already admits every
        // texel, including transparent zero. UNORM8 conversion also defines 1.
        uint32 passingByte = 0;
        while (passingByte < 255 && static_cast<float>(passingByte) / 255.0f < cutoff)
            ++passingByte;
        if (passingByte == 0)
            return;
        const auto base = AlphaCounts(chain[0]);
        const uint64 basePixels = static_cast<uint64>(chain[0].Width) * chain[0].Height;
        uint64 basePassing = 0;
        for (uint32 a = passingByte; a < 256; ++a) basePassing += base[a];

        for (size_t level = 1; level < chain.size(); ++level)
        {
            auto& mip = chain[level];
            const auto histogram = AlphaCounts(mip);
            const uint64 pixels = static_cast<uint64>(mip.Width) * mip.Height;
            const auto error = [&](uint64 count) {
                // Admission bounds each count to uint32, so products fit uint64.
                const uint64 have = count * basePixels, want = basePassing * pixels;
                return have > want ? have - want : want - have;
            };
            uint64 unscaledPassing = 0;
            for (uint32 a = passingByte; a < 256; ++a) unscaledPassing += histogram[a];
            double bestScale = 1.0;
            uint64 bestError = error(unscaledPassing);
            const auto consider = [&](uint64 count, double scale) {
                const uint64 candidateError = error(count);
                const double distortion = std::abs(scale - 1.0);
                const double bestDistortion = std::abs(bestScale - 1.0);
                if (candidateError < bestError ||
                    (candidateError == bestError && (distortion < bestDistortion ||
                     (distortion == bestDistortion && scale < bestScale))))
                {
                    bestError = candidateError;
                    bestScale = scale;
                }
            };
            // Coverage changes only when one of the 255 nonzero alpha bins
            // crosses the quantized cutoff. Test each side of those boundaries,
            // plus the already-tested identity; no repeated whole-image scans.
            uint64 above = 0;
            for (uint32 a = 255; a > 0; --a)
            {
                if (!histogram[a]) continue;
                const double boundary = (static_cast<double>(passingByte) - 0.5) / a;
                double before = boundary, after = boundary;
                while (ScaleAlphaByte(a, before) >= passingByte)
                    before = std::nextafter(before, 0.0);
                while (ScaleAlphaByte(a, after) < passingByte)
                    after = std::nextafter(after, std::numeric_limits<double>::infinity());
                consider(above, before);
                above += histogram[a];
                consider(above, after);
            }
            if (bestScale == 1.0) continue;
            std::array<uint8, 256> scaled{};
            for (uint32 a = 0; a < 256; ++a) scaled[a] = ScaleAlphaByte(a, bestScale);
            for (size_t i = 3; i < mip.Bytes.size(); i += 4) mip.Bytes[i] = scaled[mip.Bytes[i]];
        }
    }

    // Re-unit-length XYZ after filtering (same recipe as Tools/TextureCompiler):
    // filtered normals shorten, which flattens relief at distance.
    void RenormalizeNormalsRGBA8(TextureCookMipLevel& mip)
    {
        const size_t count = static_cast<size_t>(mip.Width) * mip.Height;
        for (size_t i = 0; i < count; ++i)
        {
            uint8* px = mip.Bytes.data() + i * 4;
            float x = static_cast<float>(px[0]) / 255.0f * 2.0f - 1.0f;
            float y = static_cast<float>(px[1]) / 255.0f * 2.0f - 1.0f;
            float z = static_cast<float>(px[2]) / 255.0f * 2.0f - 1.0f;
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len > 1e-5f)
            {
                x /= len; y /= len; z /= len;
            }
            else
            {
                x = 0.0f; y = 0.0f; z = 1.0f;
            }
            const auto toByte = [](float v) {
                return static_cast<uint8>(std::clamp((v * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
            };
            px[0] = toByte(x);
            px[1] = toByte(y);
            px[2] = toByte(z);
        }
    }
}

bool BuildTextureCookMipChain(std::vector<TextureCookMipLevel>& chain, uint32 mipCount, bool isFloat,
                              bool srgb, const TextureCookSettings& settings, std::string* error)
{
    if (error) error->clear();
    std::string validationError;
    if (!ValidateTextureAlphaCoverageSettings(settings, isFloat, validationError))
    {
        if (error) *error = validationError;
        return false;
    }
    if (settings.PreserveAlphaCoverage)
    {
        const uint64 pixels = chain.empty() ? 0 : static_cast<uint64>(chain[0].Width) * chain[0].Height;
        if (chain.size() != 1 || !pixels || pixels > std::numeric_limits<uint32>::max() ||
            chain[0].Width > INT_MAX || chain[0].Height > INT_MAX ||
            pixels * 4 != chain[0].Bytes.size() || mipCount == 0 ||
            mipCount > ComputeTextureCookMipCount(chain[0].Width, chain[0].Height, TextureCookSettings{}))
        {
            if (error) *error = "alpha coverage requires one valid tightly packed RGBA8 base and a natural mip count";
            return false;
        }
    }
    while (chain.size() < mipCount)
    {
        const TextureCookMipLevel& src = chain.back();
        TextureCookMipLevel dst;
        dst.Width = std::max(src.Width / 2u, 1u);
        dst.Height = std::max(src.Height / 2u, 1u);
        dst.Bytes.resize(static_cast<size_t>(dst.Width) * dst.Height * 4 * (isFloat ? sizeof(float) : 1));

        const void* ok = nullptr;
        if (isFloat)
        {
            ok = stbir_resize_float_linear(
                reinterpret_cast<const float*>(src.Bytes.data()), static_cast<int>(src.Width),
                static_cast<int>(src.Height), 0,
                reinterpret_cast<float*>(dst.Bytes.data()), static_cast<int>(dst.Width),
                static_cast<int>(dst.Height), 0, STBIR_4CHANNEL);
        }
        else if (srgb)
        {
            ok = stbir_resize_uint8_srgb(
                src.Bytes.data(), static_cast<int>(src.Width), static_cast<int>(src.Height), 0,
                dst.Bytes.data(), static_cast<int>(dst.Width), static_cast<int>(dst.Height), 0,
                STBIR_RGBA);
        }
        else
        {
            ok = stbir_resize_uint8_linear(
                src.Bytes.data(), static_cast<int>(src.Width), static_cast<int>(src.Height), 0,
                dst.Bytes.data(), static_cast<int>(dst.Width), static_cast<int>(dst.Height), 0,
                STBIR_4CHANNEL);
        }
        if (!ok)
            return false;
        if (settings.Usage == TextureCookUsage::Normal && !isFloat)
            RenormalizeNormalsRGBA8(dst);
        chain.push_back(std::move(dst));
    }
    if (settings.PreserveAlphaCoverage)
        PreserveMipAlphaCoverage(chain, settings.AlphaCoverageCutoff);
    return true;
}

#else // !GE_HAVE_STB

bool BuildTextureCookMipChain(std::vector<TextureCookMipLevel>& chain, uint32 mipCount, bool isFloat, bool,
                              const TextureCookSettings& settings, std::string* error)
{
    std::string validationError;
    if (!ValidateTextureAlphaCoverageSettings(settings, isFloat, validationError) || settings.PreserveAlphaCoverage)
    {
        if (error) *error = validationError.empty() ? "alpha coverage requires the mip resampler" : validationError;
        return false;
    }
    // No resampler compiled in: report failure unless the requested count is
    // already satisfied, so the caller falls back to the levels it holds.
    return chain.size() >= mipCount;
}

#endif // GE_HAVE_STB

#if defined(GE_HAVE_STB) && defined(GE_HAVE_KTX)

namespace
{
    void ApplySwizzleRGBA8(std::vector<uint8>& pixels, const char swizzle[4])
    {
        const size_t count = pixels.size() / 4;
        for (size_t i = 0; i < count; ++i)
        {
            uint8* px = pixels.data() + i * 4;
            const uint8 src[4] = {px[0], px[1], px[2], px[3]};
            for (int c = 0; c < 4; ++c)
            {
                switch (swizzle[c])
                {
                case 'r': px[c] = src[0]; break;
                case 'g': px[c] = src[1]; break;
                case 'b': px[c] = src[2]; break;
                case 'a': px[c] = src[3]; break;
                case '0': px[c] = 0; break;
                case '1': px[c] = 255; break;
                default: break;
                }
            }
        }
    }

    uint32 VkFormatFor(TextureCookOutput out, bool srgb, bool isFloat)
    {
        switch (out)
        {
        case TextureCookOutput::BC1:  return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case TextureCookOutput::BC4:  return VK_FORMAT_BC4_UNORM_BLOCK;
        case TextureCookOutput::BC5:  return VK_FORMAT_BC5_UNORM_BLOCK;
        case TextureCookOutput::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case TextureCookOutput::BC7:  return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        default:
            if (isFloat)
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
} // namespace

bool CookTexture(const uint8* srcBytes, size_t srcSize, const std::string& extLower,
                 const TextureCookInputs& inputs, TextureCookOutput output,
                 std::vector<uint8>& outKtx2Bytes, std::string& outError,
                 const std::function<bool()>& cancelRequested,
                 std::optional<TextureCookEncodeQuality> quality,
                 TextureCookWorkers* workers)
{
    const auto isCancelled = [&cancelRequested]
    { return cancelRequested && cancelRequested(); };

    outKtx2Bytes.clear();
    if (!srcBytes || srcSize == 0)
    {
        outError = "empty source";
        return false;
    }

    if (isCancelled())
    {
        outError = kTextureCookCancelledError;
        return false;
    }

#if !defined(GE_HAVE_DIRECTXTEX)
    if (output != TextureCookOutput::Uncompressed)
    {
        outError = "BC encoder not compiled in";
        return false;
    }
#endif

    const bool isFloat = extLower == ".hdr";
    const bool srgb = inputs.ColorSpace == TextureColorSpace::SRGB && !isFloat;
    if (!ValidateTextureAlphaCoverageSettings(inputs.Settings, isFloat, outError))
        return false;
    if (inputs.Settings.PreserveAlphaCoverage && output != TextureCookOutput::Uncompressed &&
        output != TextureCookOutput::BC7)
    {
        outError = "alpha coverage output must retain RGBA";
        return false;
    }

    // Decode the source to tightly-packed RGBA (8-bit or float).
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(false);
    TextureCookMipLevel base;
    if (isFloat)
    {
        float* data = stbi_loadf_from_memory(srcBytes, static_cast<int>(srcSize),
                                             &width, &height, &channels, 4);
        if (!data || width <= 0 || height <= 0)
        {
            outError = std::string("decode failed: ") +
                       (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
            return false;
        }
        base.Bytes.assign(reinterpret_cast<uint8*>(data),
                          reinterpret_cast<uint8*>(data) +
                              static_cast<size_t>(width) * height * 4 * sizeof(float));
        stbi_image_free(data);
    }
    else
    {
        unsigned char* data = stbi_load_from_memory(srcBytes, static_cast<int>(srcSize),
                                                    &width, &height, &channels, 4);
        if (!data || width <= 0 || height <= 0)
        {
            outError = std::string("decode failed: ") +
                       (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
            return false;
        }
        base.Bytes.assign(data, data + static_cast<size_t>(width) * height * 4);
        stbi_image_free(data);
    }
    base.Width = static_cast<uint32>(width);
    base.Height = static_cast<uint32>(height);

    // Bake the upload swizzle so block-compressed payloads honor it too (the
    // upload path cannot swizzle blocks; see TextureAsset::IsSwizzleBaked).
    if (!isFloat && inputs.Swizzle[0] != 0)
        ApplySwizzleRGBA8(base.Bytes, inputs.Swizzle);

    const uint32 mipCount = ComputeTextureCookMipCount(base.Width, base.Height, inputs.Settings);
    std::vector<TextureCookMipLevel> chain;
    chain.reserve(mipCount);
    chain.push_back(std::move(base));
    if (!BuildTextureCookMipChain(chain, mipCount, isFloat, srgb, inputs.Settings, &outError))
    {
        if (outError.empty()) outError = "mip downsample failed";
        return false;
    }

    // Encode levels. This check catches a cancel between levels;
    // CompressTextureCookLevel checks again between block-row bands within a
    // level, which is what bounds the wait on a large base level. Bailing
    // anywhere here produces no bytes, so the caller writes no cache entry.
    std::vector<std::vector<uint8>> levelPayloads(chain.size());
    for (size_t i = 0; i < chain.size(); ++i)
    {
        if (isCancelled())
        {
            outKtx2Bytes.clear();
            outError = kTextureCookCancelledError;
            return false;
        }

        if (output == TextureCookOutput::Uncompressed)
        {
            levelPayloads[i] = std::move(chain[i].Bytes);
        }
        else if (!CompressTextureCookLevel(chain[i], isFloat, output, /*bandBlocks=*/0,
                                          levelPayloads[i], outError, cancelRequested, quality, workers))
        {
            outKtx2Bytes.clear();
            return false;
        }
    }

    // Serialize KTX2. The vkFormat carries block layout + transfer function;
    // the loader treats the container DFD as authoritative for color space.
    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = VkFormatFor(output, srgb, isFloat);
    createInfo.baseWidth = chain[0].Width;
    createInfo.baseHeight = chain[0].Height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = static_cast<ktx_uint32_t>(chain.size());
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    KTX_error_code err = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (err != KTX_SUCCESS)
    {
        outError = std::string("ktxTexture2_Create failed: ") + ktxErrorString(err);
        return false;
    }

    for (size_t level = 0; level < levelPayloads.size(); ++level)
    {
        err = ktxTexture_SetImageFromMemory(ktxTexture(texture), static_cast<ktx_uint32_t>(level), 0, 0,
                                            levelPayloads[level].data(), levelPayloads[level].size());
        if (err != KTX_SUCCESS)
        {
            outError = std::string("ktx SetImageFromMemory failed: ") + ktxErrorString(err);
            ktxTexture_Destroy(ktxTexture(texture));
            return false;
        }
    }

    // Pin the writer metadata so cook output stays byte-stable across libktx
    // version bumps (the library would otherwise stamp its own version string).
    const char* writer = "GameEngine.TextureCook v1";
    ktxHashList_AddKVPair(&texture->kvDataHead, KTX_WRITER_KEY,
                          static_cast<ktx_uint32_t>(std::strlen(writer) + 1), writer);

    ktx_uint8_t* mem = nullptr;
    ktx_size_t memSize = 0;
    err = ktxTexture_WriteToMemory(ktxTexture(texture), &mem, &memSize);
    ktxTexture_Destroy(ktxTexture(texture));
    if (err != KTX_SUCCESS || !mem || memSize == 0)
    {
        outError = std::string("ktx WriteToMemory failed: ") + ktxErrorString(err);
        if (mem)
            free(mem);
        return false;
    }

    outKtx2Bytes.assign(mem, mem + memSize);
    free(mem);
    return true;
}

#else // !(GE_HAVE_STB && GE_HAVE_KTX)

bool CookTexture(const uint8*, size_t, const std::string&, const TextureCookInputs&,
                 TextureCookOutput, std::vector<uint8>&, std::string& outError,
                 const std::function<bool()>&, std::optional<TextureCookEncodeQuality>, TextureCookWorkers*)
{
    outError = "texture cook requires stb + libktx at build time";
    return false;
}

#endif

} // namespace GameEngine
