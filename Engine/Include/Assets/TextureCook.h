#pragma once

#include "Assets/RuntimeAssetMetadata.h"
#include "Assets/TextureAsset.h"
#include "Types/StringId.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {

class TextureCookWorkers;

// Import cook for source textures (PNG/JPG/TGA/BMP/HDR): decode once, build a
// gamma-correct mip chain, optionally block-compress (BCn), and serialize to a
// KTX2 blob cached in the project's derived-artifact cache (mirrors the mesh
// LOD cook — see ModelAsset::LoadOrGenerateLODs). Settings persist per-asset in
// AssetDatabase KV meta (same pattern as assets.texture.colorSpace).
//
// The format axis is deliberately engine-level (TextureCookOutput), not BCn
// specific: a future mobile/Apple axis adds ASTC outputs behind the same
// resolve + encode seams without touching settings, keys, or the loader.

// User-facing compression choice (inspector dropdown / meta value).
enum class TextureCookCompression : uint8 {
    Auto = 0,     // resolve from usage (see ResolveTextureCookOutput)
    BC1,          // explicit only: RGB, 4 bpp; banding risk on smooth gradients
    BC4,          // single channel (R), 4 bpp
    BC5,          // two channel (RG), 8 bpp; tangent-space normals (Z rebuilt in shader)
    BC6H,         // HDR RGB, 8 bpp, unsigned half
    BC7,          // RGBA, 8 bpp
    None          // never block-compress (mips still apply)
};

// Texture usage category driving the Auto compression choice. Resolved from the
// slots that bind the texture, at material bind time in the editor (TextureService
// widens the stored usage to serve each slot, WidenTextureCookUsage) and at export
// time for packaged content (CollectPackagedTextureUsages), because a packaged
// mount derives no import data at runtime. The texture inspector sets it per
// asset; a later bind whose slot needs a wider usage widens that choice, and a bind
// no one cook serves together with it keeps it.
enum class TextureCookUsage : uint8 {
    Auto = 0,     // unknown: cook stays uncompressed (CPU readers keep pixel access)
    Color,        // albedo / emissive (sRGB)  -> BC7
    Normal,       // tangent-space normal maps -> BC5 (+ per-mip renormalize)
    Mask,         // single-channel data (AO / roughness / metallic) -> BC4
    Packed        // multi-channel packed data (metallicRoughness / ORM) -> BC7
};

// Shared material-slot interpretation for renderer binds and offline export.
// Unknown/custom slots return Auto; colour slots retain the source-format colour default.
//
// It is also the one table that decides a slot's colour space: everything that is
// not Color is linear data (IsLinearTextureSlot). The triplanar normal layers
// (top/side/bottom) carry tangent-space normals and are gamma-corrupted if they
// upload as sRGB, which is why they are listed; the three triplanar ALBEDO names
// hold colour and are deliberately absent from that set. Re-classifying a slot
// here therefore changes both its compression and its upload format.
TextureCookUsage TextureCookUsageForMaterialSlot(StringId slotName);

// The name of a slot TextureCookUsageForMaterialSlot classifies, for messages that
// report a usage conflict; empty for any other slot (those map to Auto, which never
// conflicts).
std::string_view TextureCookUsageSlotName(StringId slotName);

// The usage whose cook serves a texture tagged `current` that a slot needing `required` also
// binds. The data usages form a chain, Auto < Mask < Packed: a Packed cook (BC7) keeps the
// channel a Mask slot reads, a Mask cook (BC4) drops the channels a Packed slot reads, so the
// wider usage serves both and a narrower slot never narrows the tag, whoever set it. Color and
// Normal sit above Auto only. std::nullopt reports a pair no single cook serves (a colour or
// normal texture that a data slot, or the other of the two, also binds). Symmetric, so bindings
// that do not conflict widen to the same usage in any order.
std::optional<TextureCookUsage> WidenTextureCookUsage(TextureCookUsage current, TextureCookUsage required);

// Whether a usage carries non-colour data: sampled UNORM, never sRGB. Colour —
// and unclassified Auto — keeps the source-extension guess, which is already
// right for LDR (sRGB) and HDR (linear) sources alike.
bool IsLinearTextureUsage(TextureCookUsage usage);

// The same question asked of a material slot. It is what the renderer tags a
// data texture with at bind time, what the export writes into staged package
// metadata, and what a package's committed colour-space rows are reconciled
// against.
bool IsLinearTextureSlot(StringId slotName);

// Per-texture sampler filtering (kTextureFilterMetaKey) overrides the material's
// TextureFilter for every slot this texture binds to. Inherit (absent / "inherit")
// keeps the material's choice.
enum class TextureFilterMode : uint8 {
    Inherit = 0,  // the material's TextureFilter decides
    Trilinear,    // linear min/mag, linear between mips, 16x anisotropic where the device allows
    Bilinear,     // linear min/mag, nearest mip
    Point         // nearest everything (pixel art, data lookups)
};

// Meta parse/format helpers. Parse returns false for empty/"auto"/unrecognized
// (caller keeps the default); Format returns the canonical meta string.
bool ParseTextureCookCompressionMeta(const std::string& value, TextureCookCompression& out);
const char* TextureCookCompressionMetaValue(TextureCookCompression c);
bool ParseTextureCookUsageMeta(const std::string& value, TextureCookUsage& out);
const char* TextureCookUsageMetaValue(TextureCookUsage u);
bool ParseTextureFilterMeta(const std::string& value, TextureFilterMode& out);
const char* TextureFilterMetaValue(TextureFilterMode f);

// Resolved cook settings for one texture (meta over defaults).
struct TextureCookSettings {
    TextureCookCompression Compression = TextureCookCompression::Auto;
    TextureCookUsage Usage = TextureCookUsage::Auto;
    bool MipsEnabled = true;
    uint32 MipLimit = 0; // 0 = full chain to 1x1; otherwise max level count
    bool PreserveAlphaCoverage = false;
    float AlphaCoverageCutoff = 0.5f; // explicit reference; never inferred from a material
};

// Coverage metadata is strict: absent/"0" disables it and ignores the cutoff;
// "1" enables a finite [0,1] cutoff (absent = 0.5). Failure leaves settings
// unchanged. Use the same parser for worker cooks, raw uploads and the inspector.
bool ParseTextureAlphaCoverageMeta(const std::string& enabled, const std::string& cutoff,
                                   TextureCookSettings& settings, std::string& error);
std::string TextureAlphaCutoffMetaValue(float cutoff);
// Active coverage requires LDR alpha, non-normal usage and requested compression
// that preserves alpha, even if a particular device would fall back to RGBA8.
bool ValidateTextureAlphaCoverageSettings(const TextureCookSettings& settings,
                                          bool isFloat, std::string& error);

// Concrete output format the cook encodes to.
enum class TextureCookOutput : uint8 {
    Uncompressed = 0, // RGBA8 (LDR) or RGBA32F (HDR), still mipped
    BC1,
    BC4,
    BC5,
    BC6H,
    BC7
};

// True when a BCn encoder is compiled in (DirectXTex + libktx writer).
bool IsTextureCookEncoderAvailable();

// True when the cook is disabled process-wide via GE_TEXTURE_COOK_DISABLE=1
// (authoring kill switch; published package payloads still load). For authored
// sources, this is also the A/B baseline lever — disabled runs the raw decode
// path with no cook or cache I/O. NOT an exact pre-branch frame: the shader
// tangent-normal Z-reconstruct (GE_DecodeTangentNormal) is unconditional, so
// maps whose stored Z differs from the hemisphere reconstruction (e.g. noisy
// JPEG normals) still shade through the new decode. Read once.
bool IsTextureCookDisabled();

// Decode one LDR block-compressed level back to tightly-packed RGBA8
// (verification/tooling path — quality A/Bs and tests; not a runtime decode).
// Fails for BC6H (HDR) and when no decoder is compiled in.
bool DecodeBlockPayloadRGBA8(TextureFormat format, uint32 width, uint32 height,
                             const uint8* payload, size_t payloadSize,
                             std::vector<uint8>& outRgba8);

// Resolve the concrete output. Explicit compression wins; Auto maps usage ->
// format (Color/Packed -> BC7, Normal -> BC5, Mask -> BC4, HDR sources -> BC6H,
// unknown usage -> Uncompressed). Auto never picks BC1 (gradient banding on
// smooth/stylized content); BC1 is an explicit size-critical opt-in. Any BC
// output degrades to Uncompressed when the device lacks BC sampling or no
// encoder is compiled in — the cook then still bakes the mip chain.
TextureCookOutput ResolveTextureCookOutput(const TextureCookSettings& settings,
                                           bool sourceIsHDR,
                                           bool deviceSupportsBC,
                                           bool encoderAvailable);

// Everything (besides the source bytes) that keys the cooked artifact.
struct TextureCookInputs {
    TextureCookSettings Settings;
    TextureColorSpace ColorSpace = TextureColorSpace::Unknown;
    // Upload swizzle baked into the cook ('r'/'g'/'b'/'a'/'0'/'1' per output
    // channel); Swizzle[0] == 0 means identity/none.
    char Swizzle[4] = {0, 0, 0, 0};
};

// One resolver for live registry metadata and the immutable staged manifest.
// Explicit metadata wins over source-extension defaults. Invalid coverage policy
// returns false with an error; callers must not silently use the default policy.
// The callback returns false for an absent key; defaults then match raw loading.
bool ResolveTextureCookInputs(
    const std::string& extensionLower,
    const std::function<bool(const char*, std::string&)>& readMetadata,
    TextureCookInputs& inputs, std::string& error);
bool IsTextureCookSourceExtension(const std::string& extensionLower);
bool NeedsTextureCook(const TextureCookInputs& inputs, TextureCookOutput output);

// Encoder search quality defaults to the build configuration, but export passes
// the Player target quality explicitly. It is not a per-texture setting, but it
// keys the artifact, because the two searches emit different bytes for the same
// input and the same settings.
enum class TextureCookEncodeQuality : uint32 {
    Full = 0,     // DirectXTex's default (max-quality) search
    QuickBC7 = 1  // BC7 mode-6 only (TEX_COMPRESS_BC7_QUICK)
};

// Default quality for this build. Explicit export quality is passed to both
// the encoder and artifact key; non-BC7 outputs always use Full.
TextureCookEncodeQuality TextureCookEncodeQualityFor(TextureCookOutput output);

// A relocation-independent artifact name shared by build staging and loading.
// cacheRoot/Tex is chosen by the owning asset mount, outside this key recipe.
std::string TextureCookArtifactName(const GUID& guid, uint64 sourceHash,
                                    const TextureCookInputs& inputs, TextureCookOutput output,
                                    std::optional<TextureCookEncodeQuality> quality = {});

// FNV-1a-64 over the full source payload (same recipe as the LOD cook key).
uint64 ComputeTextureCookSourceHash(const uint8* bytes, size_t size);

// Folds settings, color space, swizzle, resolved output, the cook version, the
// encoder-library versions (DirectXTex / stb_image_resize2) and — for block
// outputs — the encode quality into the config half of the cache key. Any change
// lands on a fresh filename.
uint64 ComputeTextureCookConfigHash(const TextureCookInputs& inputs, TextureCookOutput output,
                                    std::optional<TextureCookEncodeQuality> quality = {});

// Mip level count for a base extent under the cook settings (full chain to
// 1x1, clamped by MipLimit; 1 when mips are disabled).
uint32 ComputeTextureCookMipCount(uint32 width, uint32 height, const TextureCookSettings& settings);

// One mip level, tightly packed RGBA (uint8 x4, or float x4 for HDR sources).
struct TextureCookMipLevel {
    std::vector<uint8> Bytes;
    uint32 Width = 0;
    uint32 Height = 0;
};

// Successive-halving mip chain builder — the ONE resampling recipe, shared by the
// cook and by the synchronous raw-decode upload path (TextureService::GetOrUpload),
// so a first bind and the cooked artifact that later replaces it filter identically.
// `chain` must already hold level 0; levels are appended until it holds `mipCount`
// (see ComputeTextureCookMipCount). sRGB color filters through stbir's sRGB path
// with alpha-weighted RGBA; linear data filters straight per-channel; float (HDR)
// filters linear per-channel. Normal usage re-unit-lengths XYZ after each
// downsample (ignored for float payloads).
// Opt-in coverage requires exactly one RGBA8 base level. Build the ordinary full
// chain first, then scale only nonbase alpha: >= cutoff texel coverage is the
// nearest attainable to the post-swizzle base under uniform UNORM8 alpha scale.
// Ties prefer scale closest to 1, then the smaller scale. Zero alpha stays zero;
// tied/1x1 levels cannot always match the base. RGB and level 0 remain identical
// to the ordinary chain. This is a precompression texel guarantee, not a promise
// about BC7 error or bilinear/trilinear sampled coverage. Invalid active settings
// fail visibly; callers must not silently substitute uncorrected mips.
// Returns false when a downsample fails or no resampler is compiled in (no stb).
// Only callers with coverage disabled may degrade to the existing partial chain.
bool BuildTextureCookMipChain(std::vector<TextureCookMipLevel>& chain, uint32 mipCount,
                              bool isFloat, bool srgb, const TextureCookSettings& settings,
                              std::string* error = nullptr);

// Sentinel outError from CookTexture when `cancelRequested` was observed set —
// it separates an abandoned cook from a genuine encode failure in the log line
// the caller emits, and is what the cook's own tests assert on. The load path
// does not compare it: TextureAsset re-reads the ambient decode flag
// (IsAssetDecodeCancelled) to decide whether to skip the raw-decode fallback,
// which also covers a cancel that arrives after a real failure.
inline constexpr const char* kTextureCookCancelledError = "cancelled";

// Default encode granularity: a level is block-compressed in horizontal bands of
// roughly this many blocks, with cancellation checked between them. It bounds
// how long a cancel waits and is the unit spread across workers, so it is a
// latency knob, not a quality one: the output is byte-identical whatever the value.
inline constexpr uint32 kTextureCookBandBlocks = 512;

// Block-compress one tightly-packed RGBA level (uint8 x4, or float x4 when
// `isFloat`) into the tight ceil(w/4) x ceil(h/4) block payload DirectXTex
// produces. `output` must be a block format; Uncompressed fails. An LDR BC7
// payload then passes through RepairBC7TransparentEndpoints, so a block with a
// transparent source texel keeps alpha 0 at its transparent endpoints.
//
// The level is issued as a series of horizontal block-row bands with
// `cancelRequested` polled between them, so a cancel can preempt a level in
// progress: a whole-level DirectX::Compress call is opaque — no progress or
// cancellation callback — and is minutes long on a large base level. Banding is
// not a fork of the library, just the same public call over sub-images, and it
// is byte-exact because BCn blocks encode independently (no cross-block state at
// TEX_COMPRESS_DEFAULT; dithering, which would diffuse error across block rows,
// is not enabled) and every band starts on a block-row boundary, so the only
// partial block row is the image's own bottom edge. Each band writes its own
// slice of the payload.
//
// With `workers`, the bands are spread across the pool workers the asset decode
// gate leaves to texture work (TextureCookWorkers::RunBands), the calling thread
// encoding bands too and alone polling `cancelRequested`; without, the calling
// thread encodes them in order. The bytes are the same either way — asserted over
// several formats, odd extents and full mip chains by
// TextureCookParallel.SpreadEncodeMatchesOneThread — and so is a failure: an
// encoder that throws propagates its throw to the caller from whichever thread
// encoded the band.
//
// `bandBlocks` is the target blocks per band (0 = kTextureCookBandBlocks); a
// value at or above the level's block count issues one whole-level call. Band
// size never changes the output bytes — asserted over a full mip chain, several
// formats and both band extremes by
// TextureCookDeterminism.BandedEncodeMatchesWholeLevel.
//
// Granularity has a floor of one block row, so for a level wider than
// bandBlocks * 4 pixels a band IS one block row and the wait scales with width:
// at the default that is widths above 2048px. Returns false with outError ==
// kTextureCookCancelledError on cancel, and leaves outBytes empty on any failure.
bool CompressTextureCookLevel(const TextureCookMipLevel& level, bool isFloat,
                              TextureCookOutput output, uint32 bandBlocks,
                              std::vector<uint8>& outBytes, std::string& outError,
                              const std::function<bool()>& cancelRequested = {},
                              std::optional<TextureCookEncodeQuality> quality = {},
                              TextureCookWorkers* workers = nullptr);

// Cook source bytes into a KTX2 blob. Decodes with stb (extLower selects the
// HDR float path for ".hdr"), applies the swizzle, builds the mip chain
// (sRGB-aware filtering for sRGB color, straight linear for data, per-mip
// renormalization for Usage == Normal), encodes to `output`, and serializes a
// KTX2 container whose DFD transfer function carries the color space.
// Deterministic: identical bytes for identical inputs. Returns false with
// outError set on decode/encode failure (caller falls back to the raw path).
//
// The optional synchronous callback is never retained. It is polled before each
// mip level and between block-row bands within a level's encode, so
// a cancel is honored in bounded time rather than at the end of the cook. A
// whole-level DirectXTex::Compress call is opaque — no progress or cancellation
// callback — and is minutes long on a large base level, so the encode is issued
// as a series of bands instead: the same public API over sub-images, and
// byte-identical because BCn blocks encode independently. A cancelled cook
// returns false with outError == kTextureCookCancelledError and leaves
// outKtx2Bytes empty, so nothing reaches the derived cache.
//
// `workers` spreads each level's bands across a pool (see
// CompressTextureCookLevel); null encodes on the calling thread. The editor's
// cook and the build's pass the AssetManager's (AssetManager::GetTextureCookWorkers).
bool CookTexture(const uint8* srcBytes, size_t srcSize, const std::string& extLower,
                 const TextureCookInputs& inputs, TextureCookOutput output,
                 std::vector<uint8>& outKtx2Bytes, std::string& outError,
                 const std::function<bool()>& cancelRequested = {},
                 std::optional<TextureCookEncodeQuality> quality = {},
                 TextureCookWorkers* workers = nullptr);

} // namespace GameEngine
