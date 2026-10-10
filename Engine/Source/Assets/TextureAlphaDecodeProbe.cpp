#include "Assets/TextureAlphaDecodeProbe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#if defined(GE_HAVE_KTX)
#include <ktx.h>
#include <vulkan/vulkan_core.h>
#endif

namespace GameEngine {

namespace {

constexpr size_t kRgbaChannels = 4;

/// Process-wide admission for the decode itself (see the header for why this
/// never waits). Scoped so every exit path -- including the early returns for
/// malformed containers -- releases.
class ProbeDecodeSlot
{
public:
    ProbeDecodeSlot()
    {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.Mutex);
        if (s.InFlight >= s.Max)
            return; // full: the caller reports Deferred rather than parking here
        ++s.InFlight;
        s.Peak = std::max(s.Peak, s.InFlight);
        m_Acquired = true;
    }

    ~ProbeDecodeSlot()
    {
        if (!m_Acquired)
            return;
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.Mutex);
        --s.InFlight;
    }

    bool Acquired() const { return m_Acquired; }

    ProbeDecodeSlot(const ProbeDecodeSlot&) = delete;
    ProbeDecodeSlot& operator=(const ProbeDecodeSlot&) = delete;

    static uint32 Peak()
    {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.Mutex);
        return static_cast<uint32>(s.Peak);
    }

    static void ResetPeak()
    {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.Mutex);
        s.Peak = s.InFlight;
    }

private:
    static size_t ResolveMax()
    {
        if (const char* v = std::getenv("GE_ALPHA_PROBE_DECODE_MAX"))
        {
            const unsigned long parsed = std::strtoul(v, nullptr, 10);
            if (parsed > 0)
                return static_cast<size_t>(parsed);
        }
        return kDefaultMaxConcurrentProbeDecodes;
    }

    struct State
    {
        std::mutex Mutex;
        size_t InFlight = 0;
        size_t Peak = 0;
        const size_t Max = ResolveMax();
    };

    static State& Get()
    {
        static State state;
        return state;
    }

    bool m_Acquired = false;
};

bool LooksLikeKtx2(const uint8* bytes, size_t size)
{
    static constexpr uint8 kKtxSig[7] = {0xAB, 'K', 'T', 'X', ' ', '2', '0'};
    return size >= 7 && std::memcmp(bytes, kKtxSig, 7) == 0;
}

/// Minimum of the alpha byte of each RGBA8 texel in [data, data + size).
uint8 MinAlphaOverRgba8(const uint8* data, size_t size)
{
    uint8 minAlpha = 255;
    for (size_t i = kRgbaChannels - 1; i < size; i += kRgbaChannels)
        minAlpha = std::min(minAlpha, data[i]);
    return minAlpha;
}

#if defined(GE_HAVE_KTX)
/// Total texels a container declares across every image it can address.
///
/// The mip chain is SUMMED, not multiplied by the level count: levels halve in
/// each axis, so a full chain is about 4/3 of the base image, not levels times
/// it. Multiplying overstates a 2048x2048x12-level atlas by 9x and a
/// 4096x4096x13 one by 10x, which is enough to push ordinary shipped textures
/// past any cap sized for them — a bound that refuses real content is as broken
/// as one that admits a gigabyte.
///
/// numLayers/numFaces are floored at 1: KTX2 stores layerCount 0 for non-array
/// textures, and a zero would make the whole product zero and wave everything
/// through.
uint64 DeclaredTexels(const ktxTexture* base)
{
    const uint64 images =
        std::max(1u, base->numLayers) * static_cast<uint64>(std::max(1u, base->numFaces));
    uint64 perImage = 0;
    for (uint32 level = 0; level < std::max(1u, base->numLevels); ++level)
        perImage += static_cast<uint64>(std::max(base->baseWidth >> level, 1u)) *
                    static_cast<uint64>(std::max(base->baseHeight >> level, 1u));
    return perImage * images;
}

/// KTX2 through libktx. Basis payloads (the case the header tier cannot see
/// through: UASTC channelType 3 declares an alpha channel whose content may
/// still be uniformly opaque) are transcoded to RGBA32 and scanned;
/// already-uncompressed RGBA8 containers are scanned in place.
///
/// BasisLZ/ETC1S is refused even though libktx would happily transcode it:
/// ETC1S stores alpha as a separate, much lossier slice, and the
/// kCookedAlphaDropBound measurement covered UASTC only (the shipped corpus is
/// 1742 UASTC files and zero ETC1S). Accepting it would be inheriting a bound
/// from a population it was never measured on.
std::optional<uint8> ProbeWithKtx(const uint8* bytes, size_t size)
{
    // Header first, WITHOUT the load bit: the texel bound has to be enforced
    // before any image data is allocated, or the bound is decorative. A 12 KB
    // cubemap header can declare 388 MiB of images, and a 33 KB array header
    // over a gigabyte.
    ktxTexture2* texture = nullptr;
    if (ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_NO_FLAGS, &texture) !=
            KTX_SUCCESS ||
        !texture)
        return std::nullopt;

    // 2D only: GetImageOffset addresses levels/layers/faces, not depth slices,
    // so a 3D container would leave texels unscanned.
    const bool acceptable = ktxTexture(texture)->numDimensions == 2 &&
                            DeclaredTexels(ktxTexture(texture)) <= kMaxProbeTexels &&
                            texture->supercompressionScheme != KTX_SS_BASIS_LZ;
    ktxTexture2_Destroy(texture);
    if (!acceptable)
        return std::nullopt;

    texture = nullptr;
    if (ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                     &texture) != KTX_SUCCESS ||
        !texture)
        return std::nullopt;

    ktxTexture* base = ktxTexture(texture);
    std::optional<uint8> result;

    bool usable = true;
    if (ktxTexture2_NeedsTranscoding(texture))
        usable = ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, KTX_TF_HIGH_QUALITY) ==
                 KTX_SUCCESS;
    else
        usable = texture->vkFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                 texture->vkFormat == VK_FORMAT_R8G8B8A8_SRGB;

    if (usable)
    {
        const ktx_uint8_t* data = ktxTexture_GetData(base);
        const ktx_size_t totalSize = ktxTexture_GetDataSize(base);
        uint8 minAlpha = 255;
        uint64 scannedImages = 0;
        // Every level is compressed independently, so a coarse mip can dip
        // below level 0's minimum; all of them have to be scanned.
        for (ktx_uint32_t level = 0; usable && level < base->numLevels; ++level)
        {
            const ktx_size_t levelSize = ktxTexture_GetImageSize(base, level);
            for (ktx_uint32_t layer = 0; usable && layer < base->numLayers; ++layer)
            {
                for (ktx_uint32_t face = 0; usable && face < base->numFaces; ++face)
                {
                    ktx_size_t offset = 0;
                    if (ktxTexture_GetImageOffset(base, level, layer, face, &offset) !=
                            KTX_SUCCESS ||
                        !data || offset + levelSize > totalSize)
                    {
                        usable = false;
                        break;
                    }
                    minAlpha = std::min(minAlpha, MinAlphaOverRgba8(data + offset, levelSize));
                    ++scannedImages;
                }
            }
        }
        // A degenerate container that yielded no images must not report the
        // vacuous 255 — that would demote on nothing at all.
        if (usable && scannedImages > 0)
            result = minAlpha;
    }

    ktxTexture2_Destroy(texture);
    return result;
}
#endif // GE_HAVE_KTX

/// Decode body. The caller already holds a ProbeDecodeSlot — the slot is taken
/// once at each public entry point rather than here, so the file entry point
/// can cover its read buffer with the same admission without re-entering a
/// non-recursive gate.
std::optional<uint8> ProbeAdmitted(const uint8* bytes, size_t size)
{
#if defined(GE_HAVE_KTX)
    // Only positively recognized containers reach a decoder, and only the ones
    // whose cooked delta is bounded (see the header). PNG and TGA decode
    // perfectly well and are still refused: the cook re-encodes them to BC7,
    // and that delta is not a function of their alpha.
    if (LooksLikeKtx2(bytes, size))
        return ProbeWithKtx(bytes, size);
#else
    (void)bytes;
    (void)size;
#endif
    return std::nullopt;
}

} // namespace

uint32 PeakConcurrentProbeDecodes() { return ProbeDecodeSlot::Peak(); }

void ResetPeakConcurrentProbeDecodes() { ProbeDecodeSlot::ResetPeak(); }

namespace {

constexpr TextureAlphaProbeResult kUnprobeable{TextureAlphaProbeStatus::Unprobeable, 0};
constexpr TextureAlphaProbeResult kDeferred{TextureAlphaProbeStatus::Deferred, 0};

TextureAlphaProbeResult Answered(std::optional<uint8> minAlpha)
{
    if (!minAlpha)
        return kUnprobeable;
    return {TextureAlphaProbeStatus::Answered, *minAlpha};
}

} // namespace

TextureAlphaProbeResult ProbeTextureMinAlphaFromBytes(const uint8* bytes, size_t size)
{
    if (!bytes || size == 0 || size > kMaxProbeSourceBytes)
        return kUnprobeable;

    const ProbeDecodeSlot slot;
    if (!slot.Acquired())
        return kDeferred;

    return Answered(ProbeAdmitted(bytes, size));
}

TextureAlphaProbeResult ProbeTextureMinAlphaFromFile(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize == 0 || fileSize > kMaxProbeSourceBytes)
        return kUnprobeable;

    // The slot covers the read buffer too: it is up to kMaxProbeSourceBytes
    // and would otherwise be an unbounded per-caller allocation sitting
    // alongside the decode's own.
    const ProbeDecodeSlot slot;
    if (!slot.Acquired())
        return kDeferred;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return kUnprobeable;
    std::vector<uint8> buffer(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (static_cast<size_t>(file.gcount()) != buffer.size())
        return kUnprobeable;

    return Answered(ProbeAdmitted(buffer.data(), buffer.size()));
}

} // namespace GameEngine
