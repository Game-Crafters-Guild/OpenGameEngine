#pragma once

#include "Assets/RuntimeAssetMetadata.h"
#include "AssetCore/Asset.h"
#include "AssetCore/NineSlice.h"

#include <optional>
#include <string>
#include <vector>

namespace GameEngine {

/**
 * @brief Texture format types
 */
enum class TextureFormat {
    Unknown,
    R8,
    RG8,
    RGB8,
    RGBA8,
    R16F,
    RG16F,
    RGB16F,
    RGBA16F,
    R32F,
    RG32F,
    RGB32F,
    RGBA32F,
    // GPU block-compressed payloads (4x4 blocks). sRGB-ness is carried by
    // TextureColorSpace, not the format: the GPU view format (e.g. BC7_SRGB
    // vs BC7_UNORM) is chosen at upload time. BC1/BC4 use 8 bytes/block, the
    // rest 16. Produced by the import cook (TextureCook) and by authored
    // KTX2/BasisU containers (BC7 transcode target).
    BC1,
    BC4,
    BC5,
    BC6H,
    BC7
};

// True when the format holds GPU block-compressed blocks (not addressable as
// per-pixel channels).
inline bool IsBlockCompressedTextureFormat(TextureFormat fmt)
{
    switch (fmt)
    {
    case TextureFormat::BC1:
    case TextureFormat::BC4:
    case TextureFormat::BC5:
    case TextureFormat::BC6H:
    case TextureFormat::BC7:
        return true;
    default:
        return false;
    }
}

// GPU transcode target for universal-container textures (KTX2 BasisU/UASTC).
// The renderer sets this once at device init from the device's block-compression
// caps; asset decode (worker threads) reads it when picking the libktx transcode
// format. Default RGBA32 preserves the uncompressed pipeline on devices without
// BC support and before renderer init.
enum class TextureGpuTranscodeTarget : uint8 {
    RGBA32 = 0,
    BC7
};

/**
 * @brief Texture color space for sRGB/Linear handling.
 *
 * This is an asset-level classification that is used when choosing
 * GPU formats (e.g. RGBA8_SRGB vs RGBA8_UNORM) and for tooling.
 *
 * - Unknown: import/editor has not classified the texture yet.
 * - SRGB:    authored in sRGB space; use *_SRGB GPU formats.
 * - Linear:  non-sRGB data; use linear/UNORM or float formats.
 */
enum class TextureColorSpace : uint8 {
    Unknown = 0,
    SRGB,
    Linear
};

// kTextureColorSpaceMetaKey (AssetDatabase KV, keyed by GUID) overrides the
// extension-based color-space guess. Read/written via
// AssetRegistry::TryGetMetaValue / SetMetaValue. Values: "auto" (use the guess),
// "srgb", "linear". Set automatically on import for data textures (normal /
// metallic-roughness / AO / ...) and editable in the texture inspector. The
// engine reads it at upload time to pick *_SRGB vs *_UNORM.

// Shared extension default used before per-asset metadata overrides it.
TextureColorSpace GuessTextureColorSpace(const std::string& extensionLower);

// Parse a color-space meta string. Returns true and sets `out` for an explicit
// "srgb"/"linear"; returns false for "auto"/empty/unrecognized (caller keeps the guess).
bool ParseTextureColorSpaceMeta(const std::string& value, TextureColorSpace& out);

// Canonical meta string for an explicit color space ("srgb"/"linear"); "auto" for Unknown.
const char* TextureColorSpaceMetaValue(TextureColorSpace cs);

// kTextureSwizzleMetaKey stores a channel swizzle applied at upload. The value
// is a 4-character string, one source per output channel (R,G,B,A), each of:
// 'r' 'g' 'b' 'a' (source channel), '0' (zero) or '1' (one). "rgba" = identity.
// Editable in the texture inspector; useful for single-channel / repacked maps.

// Parse a swizzle string into 4 source chars (out[0..3] for R,G,B,A). Returns true
// only for a VALID, NON-identity swizzle (caller applies it); false for empty /
// "rgba" / malformed (caller leaves the texture untouched).
bool ParseTextureSwizzleMeta(const std::string& value, char out[4]);

// The NineSlice POD + NineSliceFill enum live in Assets/NineSlice.h (a small
// dependency-free header) so the UI texture cache and primitive path can use them
// without pulling in texture-format / GPU headers. The metadata key + parse/format
// helpers below bind that layout to a texture asset's AssetDatabase KV entry.

// kTextureNineSliceMetaKey stores the 9-slice layout above. Value is a compact
// versioned CSV, parsed/formatted by the
// functions below. Editable in the texture inspector; read by the UI renderer when
// the texture is used as a background image.

// Parse a 9-slice meta string into `out`. Returns true for a valid, enabled slice
// (caller applies it); false for empty / disabled / malformed (caller treats the
// texture as un-sliced). Cut values are clamped to non-decreasing order on parse.
bool ParseTextureNineSliceMeta(const std::string& value, NineSlice& out);

// Format a NineSlice into its canonical meta string (round-trips through Parse).
std::string TextureNineSliceMetaValue(const NineSlice& slice);

/**
 * @brief Texture filtering modes
 */
enum class TextureFilter {
    Nearest,
    Linear,
    NearestMipmapNearest,
    LinearMipmapNearest,
    NearestMipmapLinear,
    LinearMipmapLinear
};

/**
 * @brief Texture wrap modes
 */
enum class TextureWrap {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder
};

/**
 * @brief One mip level within TextureAsset pixel data (byte range into
 * GetPixelData() plus the level's texel dimensions).
 */
struct TextureMipDesc {
    uint64 Offset = 0;
    uint64 Size = 0;
    uint32 Width = 0;
    uint32 Height = 0;
};

/**
 * @brief Texture asset implementation
 */
class TextureAsset : public Asset {
public:
    TextureAsset(const GUID& guid, const std::filesystem::path& path);
    virtual ~TextureAsset();

    /**
     * @brief Set/get the process-wide GPU transcode target for container
     * formats (KTX2). Set once by the renderer after device caps are known.
     */
    static void SetGpuTranscodeTarget(TextureGpuTranscodeTarget target);
    static TextureGpuTranscodeTarget GetGpuTranscodeTarget();

    /**
     * @brief Load texture from file
     */
    bool Load() override;

    /**
     * @brief Load texture from memory data (for async loading)
     */
    bool LoadFromData(const Vector<uint8>& data) override;

    /**
     * @brief Unload texture from memory
     */
    void Unload() override;

    /**
     * @brief Textures hot-reload through the async pipeline: decode on a
     * worker, adopt the decoded payload in place on the main thread.
     */
    bool SupportsAsyncReload() const override { return true; }

    /**
     * @brief Get texture width
     */
    uint32 GetWidth() const { return m_Width; }

    /**
     * @brief Get texture height
     */
    uint32 GetHeight() const { return m_Height; }

    /**
     * @brief Get number of channels
     */
    uint32 GetChannels() const { return m_Channels; }

    /**
     * @brief Get texture format
     */
    TextureFormat GetFormat() const { return m_Format; }

	    /**
	     * @brief Get texture color space (sRGB vs Linear).
	     */
	    TextureColorSpace GetColorSpace() const { return m_ColorSpace; }

	    /**
	     * @brief Override texture color space (for tools / user workflows).
	     *
	     * This only affects how the texture will be treated when uploaded
	     * to the GPU (e.g. choice of *_SRGB vs *_UNORM formats). It does
	     * not modify the stored pixel data.
	     */
	    void SetColorSpace(TextureColorSpace colorSpace) { m_ColorSpace = colorSpace; }

    /**
     * @brief Get raw pixel data
     */
    const uint8* GetPixelData() const { return m_PixelData; }

    /**
     * @brief Get data size in bytes
     */
    uint64 GetDataSize() const { return m_DataSize; }

    /**
     * @brief Check if texture has mipmaps
     */
    bool HasMipmaps() const { return m_HasMipmaps; }

    /**
     * @brief Get number of mipmap levels
     */
    uint32 GetMipmapLevels() const { return m_MipmapLevels; }

    /**
     * @brief Per-mip byte ranges into GetPixelData(). Non-empty only for
     * container formats that author mip chains (KTX2); level 0 first.
     */
    const std::vector<TextureMipDesc>& GetMipChain() const { return m_MipChain; }

    /**
     * @brief True when GetPixelData() holds GPU block-compressed blocks
     * (not addressable as per-pixel channels).
     */
    bool IsBlockCompressed() const { return IsBlockCompressedTextureFormat(m_Format); }

    /**
     * @brief True when the payload came from a cook artifact that already baked
     * the per-asset swizzle meta into the pixels; upload must not re-apply it.
     */
    bool IsSwizzleBaked() const { return m_SwizzleBaked; }

    /**
     * @brief The file a CPU reader decodes for this texture's pixels: the packaged
     * portable payload when the texture loaded from an exported package (whose
     * source the export removes), otherwise the source.
     */
    const std::filesystem::path& GetCpuDecodePath() const
    {
        return m_PackagedPortablePath.empty() ? GetPath() : m_PackagedPortablePath;
    }

    /**
     * @brief Opt this asset into adopting an EXISTING cooked artifact even when
     * loaded outside an AssetManager context. For GPU-upload consumers only
     * (the render thread's synchronous GetOrUpload path): artifact adoption is
     * cheap (source hash + cache read + KTX2 parse), and a cache MISS never
     * cooks on these threads — cooking stays worker-only (async loads and
     * TextureService::RequestRecook). Direct Load() CPU consumers (terrain
     * masks, ocean extraction) must NOT set this: they rely on raw per-pixel
     * payloads and source channel counts.
     */
    void SetAdoptCookedArtifacts(bool adopt) { m_AdoptCookedArtifacts = adopt; }

    /**
     * @brief Set texture filtering
     */
    void SetFilter(TextureFilter minFilter, TextureFilter magFilter);

    /**
     * @brief Set texture wrapping
     */
    void SetWrap(TextureWrap wrapS, TextureWrap wrapT);

protected:
    bool AdoptReloadedPayload(Asset& staged) override;

private:
    enum class AlphaCoveragePolicy { Disabled, Enabled, Invalid };

    /**
     * @brief Derived-cache cook path for raster sources (PNG/JPG/TGA/BMP/HDR):
     * resolve per-asset cook settings from AssetDatabase meta, then load the
     * cached KTX2 artifact (or, inside an AssetManager load context, cook +
     * cache it) instead of raw-decoding the source. Returns false (caller falls
     * back to the raw decode) outside both an AssetManager context and the
     * SetAdoptCookedArtifacts opt-in, on a cache miss outside a load context,
     * for authored containers, or when the cook would be byte-identical to the
     * raw path. Invalid policy forbids fallback; enabled policy requires actual
     * decoded pixels and forbids placeholder success. See Assets/TextureCook.h.
     */
    bool TryLoadViaCook(const Vector<uint8>& sourceBytes, const String& extensionLower,
                        AlphaCoveragePolicy& coveragePolicy);

    /**
     * @brief Load texture using stb_image
     */
    bool LoadWithSTB();

    // Nullopt for authored input; packaged input succeeds or fails without raw fallback.
    std::optional<bool> LoadPackagedTexture(const Vector<uint8>* data);

    /**
     * @brief Load texture from memory data using stb_image
     */
    bool LoadWithSTBFromData(const Vector<uint8>& data);

    /**
     * @brief Load DDS texture
     */
    bool LoadDDS();

    /**
     * @brief Load DDS texture from memory data
     */
    bool LoadDDSFromData(const Vector<uint8>& data);

#if defined(GE_HAVE_KTX)
    /**
     * @brief Load KTX/KTX2 texture from file
     */
    bool LoadWithKTX();

    /**
     * @brief Load KTX/KTX2 texture from memory data
     */
    bool LoadWithKTXFromData(const Vector<uint8>& data);

    /**
     * @brief Shared tail of both KTX loaders: transcode (per the process-wide
     * GPU target), adopt the container's full mip chain and DFD color space.
     * Takes ownership semantics of nothing; caller destroys the ktx texture.
     */
    bool PopulateFromKtxTexture(void* ktxTexture2Ptr, const char* sourceName);
#endif


    /// Rasterize an SVG string via ThorVG and populate pixel data members.
    bool LoadFromSvgData(const std::string& svgText, const String& extensionLower);

    /**
     * @brief Determine format from file data
     */
    TextureFormat DetermineFormat(int channels, bool isHDR) const;

private:
	    uint32 m_Width;
	    uint32 m_Height;
	    uint32 m_Channels;
	    TextureFormat m_Format;
	    TextureColorSpace m_ColorSpace;
	    uint8* m_PixelData;
	    uint64 m_DataSize;
	    bool m_HasMipmaps;
	    bool m_SwizzleBaked = false;
	    bool m_AdoptCookedArtifacts = false;
	    std::filesystem::path m_PackagedPortablePath;
	    uint32 m_MipmapLevels;
	    std::vector<TextureMipDesc> m_MipChain;
	    TextureFilter m_MinFilter;
	    TextureFilter m_MagFilter;
	    TextureWrap m_WrapS;
	    TextureWrap m_WrapT;

    DISALLOW_COPY_AND_ASSIGN(TextureAsset);
};

} // namespace GameEngine
