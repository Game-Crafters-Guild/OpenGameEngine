#include "Inspectors/TextureInspector.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/SvgRasterizer.h"
#include "Assets/TextureAsset.h"
#include "Assets/TextureCook.h"
#include "AssetCore/SharedFileRead.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "UI/EditorIcons.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/TextureCoveragePolicyNotice.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/NineSliceField.h"
#include "UI/Controls/Toggle.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/SetAssetMetaValueCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

// Dropdown choices. "Auto" leaves the texture to the extension guess + the
// engine's data-slot auto-classification; sRGB/Linear are explicit overrides.
enum class ColorSpaceChoice
{
    Auto,
    SRGB,
    Linear
};

constexpr EnumEntry<ColorSpaceChoice> kColorSpaceChoices[] = {
    {ColorSpaceChoice::Auto,   "Auto (from file)"},
    {ColorSpaceChoice::SRGB,   "sRGB (color)"},
    {ColorSpaceChoice::Linear, "Linear (data / normal)"},
};

const char* ChoiceToMeta(ColorSpaceChoice c)
{
    switch (c)
    {
    case ColorSpaceChoice::SRGB:   return "srgb";
    case ColorSpaceChoice::Linear: return "linear";
    default:                       return "auto";
    }
}

ColorSpaceChoice ReadCurrentChoice(const std::filesystem::path& assetPath)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    TextureColorSpace cs = TextureColorSpace::Unknown;
    if (registry.TryGetMetaValue(assetPath, kTextureColorSpaceMetaKey, value) &&
        ParseTextureColorSpaceMeta(value, cs))
    {
        return cs == TextureColorSpace::SRGB ? ColorSpaceChoice::SRGB : ColorSpaceChoice::Linear;
    }
    return ColorSpaceChoice::Auto;
}

// Compression + usage dropdown entries (import cook — see Assets/TextureCook.h).
constexpr EnumEntry<TextureCookCompression> kCompressionChoices[] = {
    {TextureCookCompression::Auto, "Auto (from usage)"},
    {TextureCookCompression::BC7,  "BC7 (RGBA, high quality)"},
    {TextureCookCompression::BC5,  "BC5 (normal XY)"},
    {TextureCookCompression::BC4,  "BC4 (single channel)"},
    {TextureCookCompression::BC6H, "BC6H (HDR)"},
    {TextureCookCompression::BC1,  "BC1 (RGB, size-critical)"},
    {TextureCookCompression::None, "Uncompressed"},
};

constexpr EnumEntry<TextureCookUsage> kUsageChoices[] = {
    {TextureCookUsage::Auto,   "Auto (from material slot)"},
    {TextureCookUsage::Color,  "Color (albedo / emissive)"},
    {TextureCookUsage::Normal, "Normal map"},
    {TextureCookUsage::Mask,   "Mask (single channel)"},
    {TextureCookUsage::Packed, "Packed data (ORM)"},
};

// The same two lists with the entries that cannot carry an enabled Preserve Alpha Coverage
// policy marked, so the conflict is visible in the list instead of after the texture stops
// loading. Static labels, so the entries an EnumField holds never outlive their text. The
// asserts below keep the two lists in step: same length, same values, same order.
constexpr EnumEntry<TextureCookCompression> kCompressionChoicesWithAlphaCoverage[] = {
    {TextureCookCompression::Auto, "Auto (from usage) - keeps alpha unless Usage is Mask"},
    {TextureCookCompression::BC7,  "BC7 (RGBA, high quality)"},
    {TextureCookCompression::BC5,  "BC5 (normal XY) - drops alpha coverage"},
    {TextureCookCompression::BC4,  "BC4 (single channel) - drops alpha coverage"},
    {TextureCookCompression::BC6H, "BC6H (HDR) - drops alpha coverage"},
    {TextureCookCompression::BC1,  "BC1 (RGB, size-critical) - drops alpha coverage"},
    {TextureCookCompression::None, "Uncompressed"},
};

constexpr EnumEntry<TextureCookUsage> kUsageChoicesWithAlphaCoverage[] = {
    {TextureCookUsage::Auto,   "Auto (from material slot)"},
    {TextureCookUsage::Color,  "Color (albedo / emissive)"},
    {TextureCookUsage::Normal, "Normal map - drops alpha coverage"},
    {TextureCookUsage::Mask,   "Mask (single channel) - needs BC7 or Uncompressed"},
    {TextureCookUsage::Packed, "Packed data (ORM)"},
};

// Usage is the one row the loaded payload does not record: the format and the
// mip count say what compression and mips produced, nothing says which usage
// drove them. On a source this editor cannot author, an unauthored usage says
// so rather than claiming "Auto (from material slot)" — no material bind will
// ever set it there, because a published package's settings are its own.
constexpr EnumEntry<TextureCookUsage> kUsageChoicesUnauthored[] = {
    {TextureCookUsage::Auto,   "Not set"},
    {TextureCookUsage::Color,  "Color (albedo / emissive)"},
    {TextureCookUsage::Normal, "Normal map"},
    {TextureCookUsage::Mask,   "Mask (single channel)"},
    {TextureCookUsage::Packed, "Packed data (ORM)"},
};

static_assert(std::size(kCompressionChoices) == std::size(kCompressionChoicesWithAlphaCoverage));
static_assert(std::size(kUsageChoices) == std::size(kUsageChoicesWithAlphaCoverage));
static_assert(std::size(kUsageChoices) == std::size(kUsageChoicesUnauthored));

template <typename T, size_t N>
constexpr bool SameValuesInOrder(const EnumEntry<T> (&plain)[N], const EnumEntry<T> (&marked)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (plain[i].value != marked[i].value)
            return false;
    return true;
}

static_assert(SameValuesInOrder(kCompressionChoices, kCompressionChoicesWithAlphaCoverage));
static_assert(SameValuesInOrder(kUsageChoices, kUsageChoicesWithAlphaCoverage));
static_assert(SameValuesInOrder(kUsageChoices, kUsageChoicesUnauthored));

constexpr EnumEntry<TextureFilterMode> kFilterChoices[] = {
    {TextureFilterMode::Inherit,   "Inherit (material)"},
    {TextureFilterMode::Trilinear, "Trilinear"},
    {TextureFilterMode::Bilinear,  "Bilinear (no mip blend)"},
    {TextureFilterMode::Point,     "Point (nearest)"},
};

TextureFilterMode ReadCurrentFilter(const std::filesystem::path& assetPath)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    TextureFilterMode f = TextureFilterMode::Inherit;
    if (registry.TryGetMetaValue(assetPath, kTextureFilterMetaKey, value))
        ParseTextureFilterMeta(value, f);
    return f;
}

TextureCookCompression ReadCurrentCompression(const std::filesystem::path& assetPath)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    TextureCookCompression c = TextureCookCompression::Auto;
    if (registry.TryGetMetaValue(assetPath, kTextureCompressionMetaKey, value))
        ParseTextureCookCompressionMeta(value, c);
    return c;
}

TextureCookUsage ReadCurrentUsage(const std::filesystem::path& assetPath)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    TextureCookUsage u = TextureCookUsage::Auto;
    if (registry.TryGetMetaValue(assetPath, kTextureUsageMetaKey, value))
        ParseTextureCookUsageMeta(value, u);
    return u;
}

// Usage is the one row the payload does not record; whether the package
// authored one decides between its value and "Not set".
bool HasAuthoredUsage(const std::filesystem::path& assetPath)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    TextureCookUsage parsed = TextureCookUsage::Auto;
    return registry.TryGetMetaValue(assetPath, kTextureUsageMetaKey, value) &&
           ParseTextureCookUsageMeta(value, parsed);
}

// On a source this editor cannot write to, the settings in the panel cannot
// change the texture in front of the author, and a source that publishes no
// store has nothing stored to show at all: every row falls back to an engine
// default nobody chose. So the three cook-time rows the payload itself records
// read off the payload, and the panel agrees with the format line above it
// instead of contradicting it. The rest keep their stored value — an
// unauthored mip cap, filter, swizzle and coverage policy resolve to no cap,
// the material's filter, the identity swizzle and off, which is what applies
// at load and bind time whoever published the file.
bool ShowsLoadedCookState(const AssetImportSettingsOrigin& origin)
{
    return !origin.Writable;
}

// The colour space the upload will pick: an authored override wins (it is read
// at upload time on every source), otherwise the payload's own — for a cooked
// KTX2 artifact that is the container's transfer function.
ColorSpaceChoice LoadedColorSpace(const TextureAsset& tex,
                                  const std::filesystem::path& assetPath)
{
    const ColorSpaceChoice authored = ReadCurrentChoice(assetPath);
    if (authored != ColorSpaceChoice::Auto)
        return authored;
    switch (tex.GetColorSpace())
    {
    case TextureColorSpace::SRGB:   return ColorSpaceChoice::SRGB;
    case TextureColorSpace::Linear: return ColorSpaceChoice::Linear;
    default:                        return ColorSpaceChoice::Auto;
    }
}

// The codec in the bound payload. The payload wins over the stored choice
// here: a cook can fall back (no device BC support, an unsupported channel
// count) or not have run at all, and what the author is looking at is the
// result either way.
TextureCookCompression LoadedCompression(const TextureAsset& tex)
{
    switch (tex.GetFormat())
    {
    case TextureFormat::BC1:  return TextureCookCompression::BC1;
    case TextureFormat::BC4:  return TextureCookCompression::BC4;
    case TextureFormat::BC5:  return TextureCookCompression::BC5;
    case TextureFormat::BC6H: return TextureCookCompression::BC6H;
    case TextureFormat::BC7:  return TextureCookCompression::BC7;
    default:                  return TextureCookCompression::None;
    }
}

// Human-readable name for the loaded payload's format (evidence line: what the
// cook actually produced for this asset).
const char* AssetFormatName(TextureFormat f)
{
    switch (f)
    {
    case TextureFormat::BC1:  return "BC1";
    case TextureFormat::BC4:  return "BC4";
    case TextureFormat::BC5:  return "BC5";
    case TextureFormat::BC6H: return "BC6H";
    case TextureFormat::BC7:  return "BC7";
    case TextureFormat::RGBA8: return "RGBA8";
    case TextureFormat::RGB8:  return "RGB8";
    case TextureFormat::RG8:   return "RG8";
    case TextureFormat::R8:    return "R8";
    case TextureFormat::RGBA32F: return "RGBA32F";
    case TextureFormat::RGB32F:  return "RGB32F";
    default: return "?";
    }
}

// Appended to the format line while the payload it reports is being rebuilt. A
// cook-affecting edit lands asynchronously and can take tens of seconds on a
// large source, so the line says the numbers beside it are the ones going away.
const char* const kRebuildInFlightSuffix = "   cooking…";

// The evidence line itself: what the loaded payload is, and whether it is being
// replaced right now.
std::string TextureFormatLineText(const TextureAsset& tex, const GUID& guid)
{
    std::string text = std::to_string(tex.GetWidth()) + " x " + std::to_string(tex.GetHeight()) +
                       "  (" + std::to_string(tex.GetChannels()) + " ch)  " +
                       AssetFormatName(tex.GetFormat()) +
                       (tex.GetMipmapLevels() > 1
                            ? ", " + std::to_string(tex.GetMipmapLevels()) + " mips"
                            : std::string(", no mips"));
    if (EngineCore::GetInstance().GetAssetManager().IsReloadInFlight(guid))
        text += kRebuildInFlightSuffix;
    return text;
}

// Per-output-channel swizzle source.
enum class SwizzleSource { R, G, B, A, Zero, One };

constexpr EnumEntry<SwizzleSource> kSwizzleSources[] = {
    {SwizzleSource::R,    "R"},
    {SwizzleSource::G,    "G"},
    {SwizzleSource::B,    "B"},
    {SwizzleSource::A,    "A"},
    {SwizzleSource::Zero, "0"},
    {SwizzleSource::One,  "1"},
};

char SwizzleSourceToChar(SwizzleSource s)
{
    switch (s)
    {
    case SwizzleSource::G:    return 'g';
    case SwizzleSource::B:    return 'b';
    case SwizzleSource::A:    return 'a';
    case SwizzleSource::Zero: return '0';
    case SwizzleSource::One:  return '1';
    default:                  return 'r';
    }
}

SwizzleSource CharToSwizzleSource(char c)
{
    switch (c)
    {
    case 'g': return SwizzleSource::G;
    case 'b': return SwizzleSource::B;
    case 'a': return SwizzleSource::A;
    case '0': return SwizzleSource::Zero;
    case '1': return SwizzleSource::One;
    default:  return SwizzleSource::R;
    }
}

std::array<char, 4> ReadCurrentSwizzle(const std::filesystem::path& assetPath)
{
    std::array<char, 4> sw = {'r', 'g', 'b', 'a'};
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    std::string value;
    if (registry.TryGetMetaValue(assetPath, kTextureSwizzleMetaKey, value) && value.size() == 4)
    {
        for (int i = 0; i < 4; ++i)
        {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
            if (c == 'r' || c == 'g' || c == 'b' || c == 'a' || c == '0' || c == '1')
                sw[i] = c;
        }
    }
    return sw;
}

// What an import-setting write needs besides the value: the stack it records
// on, and the panel rebuild that re-reads the store, so an undo puts the
// controls back to what the store now holds rather than the value last picked.
struct MetaWriteContext
{
    Editor::UndoRedoService* Undo = nullptr;
    std::function<void()> RequestRefresh;
    // A read-only panel never writes. The controls refuse the user's input
    // already; this is the same rule for the panel's own programmatic paths,
    // which are not typed and so are not stopped by a disabled control.
    bool Writable = false;
};

// Persist a changed texture import setting. The callback runs during UI event
// dispatch on the main thread, where it is NOT safe to evict/re-upload the GPU
// texture (mid-frame teardown) or block on an async asset reload (the load's
// completion needs the main thread -> deadlock). So we only write the .meta; the
// setting takes effect the next time the texture is loaded (re-open the project,
// or the deferred re-upload request below). The dropdown updates its own display.
void WriteTextureMeta(const std::filesystem::path& assetPath, const char* key, const std::string& value)
{
    EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(assetPath, key, value);
    // Ask RenderServices to re-upload this texture at its next safe point (the
    // render thread), non-blocking. Safe to call from the UI thread: it only
    // queues the GUID; the actual evict/re-upload happens on the render thread.
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID guid = assets.ResolveAssetGuid(assetPath);
    if (auto* rs = EngineCore::GetInstance().GetRenderServices(); rs && !guid.IsNull())
        rs->Textures().RequestReupload(guid);
}

// Cook-affecting settings (compression / usage / mips / color space / swizzle)
// key the derived-cache artifact, so a change must re-run the asset decode +
// cook — a plain re-upload would keep serving the stale payload. RequestRecook
// queues an async reload (worker decode, main-thread adopt); the resulting
// AssetReloaded event evicts + re-uploads. Non-blocking, UI-thread safe.
void WriteTextureCookMeta(const std::filesystem::path& assetPath, const char* key, const std::string& value)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    assets.GetRegistry().SetMetaValue(assetPath, key, value);
    const GUID guid = assets.ResolveAssetGuid(assetPath);
    if (auto* rs = EngineCore::GetInstance().GetRenderServices(); rs && !guid.IsNull())
    {
        rs->Textures().RequestRecook(guid);
        rs->Textures().RequestReupload(guid); // immediate view-format refresh while the recook lands
    }
}

// Payload-affecting settings outside the texture cook (currently SVG raster
// size) still need an async asset reload, but must not go through RequestRecook:
// its cook kill switch intentionally suppresses work when texture cooking is off.
void WriteTexturePayloadMeta(const std::filesystem::path& assetPath, const char* key, const std::string& value)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    assets.GetRegistry().SetMetaValue(assetPath, key, value);
    const GUID guid = assets.ResolveAssetGuid(assetPath);
    if (!guid.IsNull())
        assets.RequestAsyncReload(guid);
}

// A control on a source that refuses metadata writes is handed over disabled:
// it shows the value and refuses input, so nothing takes an edit that would be
// dropped on the next refresh. The controls carry the state one by one rather
// than a wrapper carrying it for the whole block: a wrapper that blocks pointer
// events takes the row tooltips and the Nine-Slice disclosure with it, and both
// have to keep working on a read-only asset.
void SetControlWritable(UIElement* control, bool writable)
{
    if (control)
        control->SetDisabled(!writable);
}

// Every import-setting edit lands here. These rows write into a store file
// that ships with the texture's package and is tracked by that package's
// repository, so the edit belongs on the undo stack like any other inspector
// field. Without a stack — a panel built outside the editor's command flow —
// the write still lands, unrecorded.
using MetaWriteFn = void (*)(const std::filesystem::path&, const char*, const std::string&);

void CommitTextureMeta(const MetaWriteContext& writes, const std::filesystem::path& assetPath,
                       const char* key, std::string value, MetaWriteFn write)
{
    if (!writes.Writable)
        return;

    // A value the store already holds is not an edit: writing it would spend a
    // recook and an upload on nothing and put a no-op step on the undo stack.
    std::string stored;
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    if (registry.TryGetMetaValue(assetPath, key, stored) && stored == value)
        return;

    if (!writes.Undo)
    {
        write(assetPath, key, value);
        return;
    }
    // The command keys on the asset's GUID and hands back the path the registry
    // holds when it runs, so an undo after a rename still writes to this texture.
    writes.Undo->Execute(std::make_unique<Editor::SetAssetMetaValueCommand>(
        assetPath, key, std::move(value),
        [key, write](const std::filesystem::path& path, const std::string& v)
        { write(path, key, v); },
        writes.RequestRefresh));
}

bool IsSvgTexturePath(const std::filesystem::path& assetPath)
{
    std::string extension = assetPath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".svg";
}

uint32 ReadCurrentSvgRasterSize(const std::filesystem::path& assetPath)
{
    uint32 targetPixels = static_cast<uint32>(GetSvgTextureRasterizerDefaultSize() + 0.5f);
    std::string value;
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    if (registry.TryGetMetaValue(assetPath, kSvgRasterSizeMetaKey, value))
        ParseSvgRasterSizeMeta(value, targetPixels);
    return targetPixels;
}

std::vector<Dropdown::Option> BuildSvgRasterSizeOptions(float32 sourceWidth,
                                                        float32 sourceHeight)
{
    std::vector<Dropdown::Option> options;
    for (uint32 pixels = kMinSvgRasterSize; pixels <= kMaxSvgRasterSize; pixels *= 2u)
    {
        const std::string value = std::to_string(pixels);
        uint32 width = pixels;
        uint32 height = pixels;
        CalculateSvgRasterDimensions(
            sourceWidth, sourceHeight, static_cast<float32>(pixels), width, height);
        options.push_back(
            {value, std::to_string(width) + "x" + std::to_string(height)});
    }
    return options;
}

int FindSvgRasterSizeOption(const std::vector<Dropdown::Option>& options, uint32 pixels)
{
    int selectedIndex = 0;
    uint32 selectedDistance = std::numeric_limits<uint32>::max();
    uint32 optionPixels = kMinSvgRasterSize;
    for (size_t i = 0; i < options.size(); ++i, optionPixels *= 2u)
    {
        const uint32 distance = pixels > optionPixels ? pixels - optionPixels : optionPixels - pixels;
        if (distance < selectedDistance)
        {
            selectedDistance = distance;
            selectedIndex = static_cast<int>(i);
        }
    }
    return selectedIndex;
}

constexpr EnumEntry<NineSliceFill> kEdgeFillModes[] = {
    {NineSliceFill::Stretch, "Stretch"},
    {NineSliceFill::Tile,    "Tile"},
    {NineSliceFill::Round,   "Round"},
};
constexpr EnumEntry<NineSliceFill> kCenterFillModes[] = {
    {NineSliceFill::Stretch, "Stretch"},
    {NineSliceFill::Tile,    "Tile"},
    {NineSliceFill::Round,   "Round"},
    {NineSliceFill::Scale,   "Scale (fit)"},
};

// One editor-session clipboard for a 9-slice layout, so the same border setup can
// be pasted onto other textures. Paste rescales by the size ratio, so it transfers
// correctly across assets authored at the same proportions but different sizes.
struct CopiedNineSliceLayout
{
    NineSlice slice;
    uint32_t  sw = 0;
    uint32_t  sh = 0;
    bool      valid = false;
};
CopiedNineSliceLayout g_CopiedNineSliceLayout;
std::unique_ptr<INativeContextMenu> g_NineSliceLayoutMenu;

// Shared state for the interactive 9-slice section: the canvas field and the four
// numeric inset fields read/write one NineSlice that the callbacks keep in sync and
// persist to the texture asset's metadata. Control pointers are kept so Paste Layout
// can refresh every widget at once.
struct NineSliceEditState
{
    std::filesystem::path path;
    GUID                  guid;
    uint32_t              sw = 0;
    uint32_t              sh = 0;
    NineSlice             slice;
    bool                  usePercent = false; // show/enter insets as % of texture size
    NineSliceField*       field = nullptr;
    FloatField*           fL = nullptr;  // corner insets
    FloatField*           fR = nullptr;
    FloatField*           fT = nullptr;
    FloatField*           fB = nullptr;
    FloatField*           fLI = nullptr; // inner (center-boundary) insets — independent mode
    FloatField*           fRI = nullptr;
    FloatField*           fTI = nullptr;
    FloatField*           fBI = nullptr;
    Toggle*               slicedToggle = nullptr;
    Toggle*               independentToggle = nullptr;
    Toggle*               drawCenterToggle = nullptr;
    EnumField<NineSliceFill>* fillXField = nullptr;
    EnumField<NineSliceFill>* fillYField = nullptr;
    EnumField<NineSliceFill>* centerFillField = nullptr;
    UIElement*            gated = nullptr;          // dimmed + non-interactive unless Sliced
    UIElement*            innerContainer = nullptr; // inner-inset rows, shown only when independent
    bool                  independent = false;
};

void BuildNineSliceSection(UIElement* root, TextureAsset* tex, const std::filesystem::path& assetPath,
                           Platform::Window* window, bool settingsWritable,
                           const MetaWriteContext& writes)
{
    const uint32_t sw = tex->GetWidth();
    const uint32_t sh = tex->GetHeight();
    if (sw == 0 || sh == 0)
        return;

    auto st = std::make_shared<NineSliceEditState>();
    st->path = assetPath;
    st->guid = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(assetPath);
    st->sw = sw;
    st->sh = sh;

    // Load the stored slice; default an unstored/degenerate one to "no borders"
    // (cuts sitting at the texture edges, so the user drags them inward).
    {
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        std::string value;
        NineSlice s;
        if (registry.TryGetMetaValue(assetPath, kTextureNineSliceMetaKey, value))
            ParseTextureNineSliceMeta(value, s);
        if (s.X[0] == 0 && s.X[3] == 0 && s.Y[0] == 0 && s.Y[3] == 0)
        {
            s.X[2] = s.X[3] = static_cast<uint16>(sw);
            s.Y[2] = s.Y[3] = static_cast<uint16>(sh);
        }
        st->slice = s;
    }

    // Independent (gapped) editing mode is implied by any non-coincident cut pair.
    st->independent = (st->slice.X[0] != st->slice.X[1]) || (st->slice.X[2] != st->slice.X[3]) ||
                      (st->slice.Y[0] != st->slice.Y[1]) || (st->slice.Y[2] != st->slice.Y[3]);

    auto persist = [st, writes]() {
        CommitTextureMeta(writes, st->path, kTextureNineSliceMetaKey,
                          TextureNineSliceMetaValue(st->slice), &WriteTextureMeta);
    };

    // Insets are stored as source texels but shown either as pixels or, when
    // usePercent, as a percentage of the texture size along that axis — so the same
    // proportions transfer cleanly between differently-sized textures.
    auto toDisplay = [st](int texel, bool isX) -> float {
        if (!st->usePercent)
            return static_cast<float>(texel);
        const float dim = isX ? static_cast<float>(st->sw) : static_cast<float>(st->sh);
        return dim > 0.0f ? texel * 100.0f / dim : 0.0f;
    };
    auto fromDisplay = [st](float value, bool isX) -> int {
        if (!st->usePercent)
            return static_cast<int>(std::lround(value));
        const float dim = isX ? static_cast<float>(st->sw) : static_cast<float>(st->sh);
        return static_cast<int>(std::lround(value / 100.0f * dim));
    };
    auto syncFields = [st, toDisplay]() {
        if (st->fL)  st->fL->SetValueWithoutNotify(toDisplay(static_cast<int>(st->slice.X[0]), true));
        if (st->fR)  st->fR->SetValueWithoutNotify(toDisplay(static_cast<int>(st->sw - st->slice.X[3]), true));
        if (st->fT)  st->fT->SetValueWithoutNotify(toDisplay(static_cast<int>(st->slice.Y[0]), false));
        if (st->fB)  st->fB->SetValueWithoutNotify(toDisplay(static_cast<int>(st->sh - st->slice.Y[3]), false));
        if (st->fLI) st->fLI->SetValueWithoutNotify(toDisplay(static_cast<int>(st->slice.X[1]), true));
        if (st->fRI) st->fRI->SetValueWithoutNotify(toDisplay(static_cast<int>(st->sw - st->slice.X[2]), true));
        if (st->fTI) st->fTI->SetValueWithoutNotify(toDisplay(static_cast<int>(st->slice.Y[1]), false));
        if (st->fBI) st->fBI->SetValueWithoutNotify(toDisplay(static_cast<int>(st->sh - st->slice.Y[2]), false));
    };

    // Show the unit ("px" or "%") as a faded trailing label on each inset field
    // (TextFieldBase::SetSuffix), tracking the current mode. The fields keep their
    // plain numeric format/parse, so math expressions (e.g. 800-200) still work.
    auto applyUnitSuffix = [st]() {
        const char* unit = st->usePercent ? "%" : "px";
        for (FloatField* f : {st->fL, st->fR, st->fT, st->fB, st->fLI, st->fRI, st->fTI, st->fBI})
            if (f)
            {
                f->SetSuffix(unit);
                f->SetSuffixAlignment(SuffixAlignment::AfterText);
            }
    };

    // Collapsible "Nine-Slice" section header.
    auto foldoutOwned = std::make_unique<Foldout>();
    foldoutOwned->SetTitle("Nine-Slice");
    foldoutOwned->AddClass("inspector-foldout");
    foldoutOwned->Collapse();
    Foldout* foldout = foldoutOwned.get();
    root->AddChild(std::move(foldoutOwned));

    // Disable the slice controls unless "Sliced" is on. The disabled wrapper takes no
    // press, key or focus for anything inside it (the UI manager's delivery rule for
    // disabled subtrees), while hover, and so the controls' tooltips, and the wheel still
    // reach them; inspector.css dims it (.nine-slice-gated:disabled).
    auto applyGate = [st]() {
        if (st->gated)
            st->gated->SetDisabled(!st->slice.Enabled);
    };

    // The enable toggle stays interactive and gates everything below it.
    st->slicedToggle = InspectorDrag::AddToggleRow(foldout, "Sliced", st->slice.Enabled,
        [st, persist, applyGate](bool on) { st->slice.Enabled = on; applyGate(); persist(); },
        "Render this texture as a 9-slice (corners fixed, edges + center stretch) when "
        "used as a UI background image.");
    SetControlWritable(st->slicedToggle, settingsWritable);

    auto gatedOwned = std::make_unique<UIElement>();
    gatedOwned->AddClass("nine-slice-gated");
    st->gated = gatedOwned.get();
    foldout->AddChild(std::move(gatedOwned));

    {
        auto fieldOwned = std::make_unique<NineSliceField>();
        st->field = fieldOwned.get();
        st->field->SetTexture(st->guid, sw, sh);
        st->field->SetSlice(st->slice);
        st->field->SetOnChanging([st, syncFields](const NineSlice& s) {
            st->slice = s;
            syncFields();
        });
        st->field->SetOnChanged([st, syncFields, persist](const NineSlice& s) {
            st->slice = s;
            syncFields();
            persist();
        });
        st->gated->AddChild(std::move(fieldOwned));
    }
    st->field->SetIndependent(st->independent);
    SetControlWritable(st->field, settingsWritable);

    st->independentToggle = InspectorDrag::AddToggleRow(st->gated, "Independent Edges", st->independent,
        [st, persist, syncFields](bool on) {
            st->independent = on;
            if (st->field)
                st->field->SetIndependent(on);
            if (st->innerContainer)
                st->innerContainer->Overrides().Set(Style::Display, on ? DisplayMode::Block : DisplayMode::None);
            if (!on)
            {
                // Collapse gaps: snap each inner cut back onto its corner.
                st->slice.X[1] = st->slice.X[0];
                st->slice.X[2] = st->slice.X[3];
                st->slice.Y[1] = st->slice.Y[0];
                st->slice.Y[2] = st->slice.Y[3];
                if (st->field)
                    st->field->SetSlice(st->slice);
            }
            syncFields();
            persist();
        },
        "Split each side into two handles so a gap can open between the corner and the "
        "center band. Gap bands are dropped when the texture is rendered.");
    SetControlWritable(st->independentToggle, settingsWritable);

    // Re-display the same cuts as pixels or percentages; storage stays in texels.
    // Presentation only — it writes nothing — so it stays live on a read-only source.
    InspectorDrag::AddToggleRow(st->gated, "Percent (%)", st->usePercent,
        [st, syncFields, applyUnitSuffix](bool on) { st->usePercent = on; applyUnitSuffix(); syncFields(); },
        "Show and enter insets as a percentage of the texture size (allows decimals, "
        "e.g. 33.33) instead of pixels.");

    // 'which' picks the border; the numeric fields edit the CORNER insets. The field
    // value arrives in display units (px or %) and converts to texels first. Shared
    // mode moves the coincident pair; independent mode moves only the corner cut (the
    // inner/center cut is dragged on the canvas), keeping cuts ordered.
    auto applyInset = [st, fromDisplay](char which, float dispV) {
        const bool ind = st->independent;
        const int v = fromDisplay(dispV, which == 'L' || which == 'R');
        switch (which)
        {
        case 'L':
            if (ind) st->slice.X[0] = static_cast<uint16>(std::clamp(v, 0, static_cast<int>(st->slice.X[1])));
            else     st->slice.X[0] = st->slice.X[1] = static_cast<uint16>(std::clamp(v, 0, static_cast<int>(st->slice.X[2])));
            break;
        case 'R': {
            int cut = static_cast<int>(st->sw) - v;
            if (ind) st->slice.X[3] = static_cast<uint16>(std::clamp(cut, static_cast<int>(st->slice.X[2]), static_cast<int>(st->sw)));
            else     st->slice.X[2] = st->slice.X[3] = static_cast<uint16>(std::clamp(cut, static_cast<int>(st->slice.X[1]), static_cast<int>(st->sw)));
            break; }
        case 'T':
            if (ind) st->slice.Y[0] = static_cast<uint16>(std::clamp(v, 0, static_cast<int>(st->slice.Y[1])));
            else     st->slice.Y[0] = st->slice.Y[1] = static_cast<uint16>(std::clamp(v, 0, static_cast<int>(st->slice.Y[2])));
            break;
        case 'B': {
            int cut = static_cast<int>(st->sh) - v;
            if (ind) st->slice.Y[3] = static_cast<uint16>(std::clamp(cut, static_cast<int>(st->slice.Y[2]), static_cast<int>(st->sh)));
            else     st->slice.Y[2] = st->slice.Y[3] = static_cast<uint16>(std::clamp(cut, static_cast<int>(st->slice.Y[1]), static_cast<int>(st->sh)));
            break; }
        default:
            break;
        }
        if (st->field) st->field->SetSlice(st->slice);
    };

    st->fL = InspectorDrag::AddFloatRowWithDrag(st->gated, "Left", toDisplay(static_cast<int>(st->slice.X[0]), true),
        [applyInset](float v) { applyInset('L', v); },
        [applyInset, persist, syncFields](float v) { applyInset('L', v); syncFields(); persist(); },
        0.0f, "Left border inset (px or %, supports arithmetic, e.g. 800-200).", 0.0f);
    st->fR = InspectorDrag::AddFloatRowWithDrag(st->gated, "Right", toDisplay(static_cast<int>(st->sw - st->slice.X[3]), true),
        [applyInset](float v) { applyInset('R', v); },
        [applyInset, persist, syncFields](float v) { applyInset('R', v); syncFields(); persist(); },
        0.0f, "Right border inset (px or %).", 0.0f);
    st->fT = InspectorDrag::AddFloatRowWithDrag(st->gated, "Top", toDisplay(static_cast<int>(st->slice.Y[0]), false),
        [applyInset](float v) { applyInset('T', v); },
        [applyInset, persist, syncFields](float v) { applyInset('T', v); syncFields(); persist(); },
        0.0f, "Top border inset (px or %).", 0.0f);
    st->fB = InspectorDrag::AddFloatRowWithDrag(st->gated, "Bottom", toDisplay(static_cast<int>(st->sh - st->slice.Y[3]), false),
        [applyInset](float v) { applyInset('B', v); },
        [applyInset, persist, syncFields](float v) { applyInset('B', v); syncFields(); persist(); },
        0.0f, "Bottom border inset (px or %).", 0.0f);
    for (FloatField* inset : {st->fL, st->fR, st->fT, st->fB})
        SetControlWritable(inset, settingsWritable);

    // Inner-edge insets: the second cut per side, exposing the gap. Only shown in
    // independent mode (the gap [outer..inner] is dropped when rendered).
    auto innerOwned = std::make_unique<UIElement>();
    innerOwned->AddClass("nine-slice-inner");
    st->innerContainer = innerOwned.get();
    st->gated->AddChild(std::move(innerOwned));

    auto applyInner = [st, fromDisplay](char which, float dispV) {
        const int v = fromDisplay(dispV, which == 'L' || which == 'R');
        switch (which)
        {
        case 'L': st->slice.X[1] = static_cast<uint16>(std::clamp(v, static_cast<int>(st->slice.X[0]), static_cast<int>(st->slice.X[2]))); break;
        case 'R': { int cut = static_cast<int>(st->sw) - v; st->slice.X[2] = static_cast<uint16>(std::clamp(cut, static_cast<int>(st->slice.X[1]), static_cast<int>(st->slice.X[3]))); break; }
        case 'T': st->slice.Y[1] = static_cast<uint16>(std::clamp(v, static_cast<int>(st->slice.Y[0]), static_cast<int>(st->slice.Y[2]))); break;
        case 'B': { int cut = static_cast<int>(st->sh) - v; st->slice.Y[2] = static_cast<uint16>(std::clamp(cut, static_cast<int>(st->slice.Y[1]), static_cast<int>(st->slice.Y[3]))); break; }
        default: break;
        }
        if (st->field) st->field->SetSlice(st->slice);
    };

    st->fLI = InspectorDrag::AddFloatRowWithDrag(st->innerContainer, "Left Inner", toDisplay(static_cast<int>(st->slice.X[1]), true),
        [applyInner](float v) { applyInner('L', v); },
        [applyInner, persist, syncFields](float v) { applyInner('L', v); syncFields(); persist(); },
        0.0f, "Inner cut for the left side (where the center band starts). The gap between "
           "Left and Left Inner is dropped when rendered.", 0.0f);
    st->fRI = InspectorDrag::AddFloatRowWithDrag(st->innerContainer, "Right Inner", toDisplay(static_cast<int>(st->sw - st->slice.X[2]), true),
        [applyInner](float v) { applyInner('R', v); },
        [applyInner, persist, syncFields](float v) { applyInner('R', v); syncFields(); persist(); },
        0.0f, "Inner cut for the right side (where the center band ends).", 0.0f);
    st->fTI = InspectorDrag::AddFloatRowWithDrag(st->innerContainer, "Top Inner", toDisplay(static_cast<int>(st->slice.Y[1]), false),
        [applyInner](float v) { applyInner('T', v); },
        [applyInner, persist, syncFields](float v) { applyInner('T', v); syncFields(); persist(); },
        0.0f, "Inner cut for the top side.", 0.0f);
    st->fBI = InspectorDrag::AddFloatRowWithDrag(st->innerContainer, "Bottom Inner", toDisplay(static_cast<int>(st->sh - st->slice.Y[2]), false),
        [applyInner](float v) { applyInner('B', v); },
        [applyInner, persist, syncFields](float v) { applyInner('B', v); syncFields(); persist(); },
        0.0f, "Inner cut for the bottom side.", 0.0f);
    for (FloatField* inset : {st->fLI, st->fRI, st->fTI, st->fBI})
        SetControlWritable(inset, settingsWritable);

    st->innerContainer->Overrides().Set(Style::Display, st->independent ? DisplayMode::Block : DisplayMode::None);

    // Now that every inset field exists, show the unit suffix on each.
    applyUnitSuffix();

    // Each fill dropdown writes one NineSlice member, then re-previews + persists; the
    // body is identical bar the member, so wire all three through one helper.
    auto wireFill = [st, persist](EnumField<NineSliceFill>* f, NineSliceFill NineSlice::* member) {
        f->SetOnValueChanged([st, persist, member](NineSliceFill v) {
            st->slice.*member = v;
            if (st->field) st->field->SetSlice(st->slice);
            persist();
        });
    };
    st->fillXField = InspectorUI::AddEnumRow(st->gated, "Edge Fill X", kEdgeFillModes, st->slice.FillX,
        "How the TOP & BOTTOM edges fill horizontally: Stretch scales the band, Tile "
        "repeats it, Round repeats at a size snapped to a whole number of tiles.");
    wireFill(st->fillXField, &NineSlice::FillX);
    st->fillYField = InspectorUI::AddEnumRow(st->gated, "Edge Fill Y", kEdgeFillModes, st->slice.FillY,
        "How the LEFT & RIGHT edges fill vertically (Stretch / Tile / Round).");
    wireFill(st->fillYField, &NineSlice::FillY);
    st->centerFillField = InspectorUI::AddEnumRow(st->gated, "Center Fill", kCenterFillModes, st->slice.CenterFill,
        "How the CENTER region fills: Stretch, Tile, Round, or Scale (fit preserving "
        "aspect ratio, centered).");
    wireFill(st->centerFillField, &NineSlice::CenterFill);
    for (EnumField<NineSliceFill>* fill : {st->fillXField, st->fillYField, st->centerFillField})
        SetControlWritable(fill->GetDropdown(), settingsWritable);

    st->drawCenterToggle = InspectorDrag::AddToggleRow(st->gated, "Draw Center", st->slice.FillCenter,
        [st, persist](bool on) { st->slice.FillCenter = on; if (st->field) st->field->SetSlice(st->slice); persist(); },
        "Draw the center region (off = hollow frame: only the border ring is drawn).");
    SetControlWritable(st->drawCenterToggle, settingsWritable);

    auto refreshControls = [st, syncFields, applyGate]() {
        if (st->field)
        {
            st->field->SetSlice(st->slice);
            st->field->SetIndependent(st->independent);
        }
        if (st->innerContainer)
            st->innerContainer->Overrides().Set(Style::Display, st->independent ? DisplayMode::Block : DisplayMode::None);
        if (st->slicedToggle)      st->slicedToggle->SetValueWithoutNotify(st->slice.Enabled);
        if (st->independentToggle) st->independentToggle->SetValueWithoutNotify(st->independent);
        if (st->drawCenterToggle)  st->drawCenterToggle->SetValueWithoutNotify(st->slice.FillCenter);
        if (st->fillXField)        st->fillXField->SetEntries(kEdgeFillModes, st->slice.FillX);
        if (st->fillYField)        st->fillYField->SetEntries(kEdgeFillModes, st->slice.FillY);
        if (st->centerFillField)   st->centerFillField->SetEntries(kCenterFillModes, st->slice.CenterFill);
        syncFields();
        applyGate();
    };

    auto copyLayout = [st]() {
        g_CopiedNineSliceLayout = {st->slice, st->sw, st->sh, true};
    };

    // Paste rescales by the size ratio so a layout authored at one size lands
    // correctly on a proportionally-scaled sibling.
    auto pasteLayout = [st, refreshControls, persist]() {
        const CopiedNineSliceLayout& c = g_CopiedNineSliceLayout;
        if (!c.valid)
            return;
        const float rx = c.sw > 0 ? static_cast<float>(st->sw) / static_cast<float>(c.sw) : 1.0f;
        const float ry = c.sh > 0 ? static_cast<float>(st->sh) / static_cast<float>(c.sh) : 1.0f;
        for (int i = 0; i < 4; ++i)
        {
            st->slice.X[i] = static_cast<uint16>(std::clamp(static_cast<int>(std::lround(c.slice.X[i] * rx)), 0, static_cast<int>(st->sw)));
            st->slice.Y[i] = static_cast<uint16>(std::clamp(static_cast<int>(std::lround(c.slice.Y[i] * ry)), 0, static_cast<int>(st->sh)));
        }
        st->slice.Enabled    = c.slice.Enabled;
        st->slice.FillCenter = c.slice.FillCenter;
        st->slice.FillX      = c.slice.FillX;
        st->slice.FillY      = c.slice.FillY;
        st->slice.CenterFill = c.slice.CenterFill;
        st->independent = (st->slice.X[0] != st->slice.X[1]) || (st->slice.X[2] != st->slice.X[3]) ||
                          (st->slice.Y[0] != st->slice.Y[1]) || (st->slice.Y[2] != st->slice.Y[3]);

        refreshControls();
        persist();
    };

    auto resetLayout = [st, refreshControls, persist]() {
        const bool wasEnabled = st->slice.Enabled;
        NineSlice reset;
        reset.Enabled = wasEnabled;
        reset.X[2] = reset.X[3] = static_cast<uint16>(st->sw);
        reset.Y[2] = reset.Y[3] = static_cast<uint16>(st->sh);
        st->slice = reset;
        st->independent = false;
        refreshControls();
        persist();
    };

    auto menuOwned = std::make_unique<Button>();
    menuOwned->AddClass("icon-button");
    menuOwned->AddClass("inspector-section-header-options");
    menuOwned->AddClass("nine-slice-header-menu");
    menuOwned->SetTooltip("Nine-Slice options");
    menuOwned->RegisterEventHandler(kEventButtonClick,
        [window, copyLayout, pasteLayout, resetLayout, settingsWritable](UIEvent& e) {
        UIElement& button = *e.CurrentTarget;
        if (!window)
            return;
        if (!g_NineSliceLayoutMenu)
            g_NineSliceLayoutMenu = CreateContextMenu();
        if (!g_NineSliceLayoutMenu)
            return;

        enum : uint32_t
        {
            kCmdCopyLayout = 1,
            kCmdPasteLayout = 2,
            kCmdResetLayout = 3,
        };

        g_NineSliceLayoutMenu->Clear();
        g_NineSliceLayoutMenu->AddItem(0, "Copy Layout", kCmdCopyLayout);
        g_NineSliceLayoutMenu->SetItemIcon(kCmdCopyLayout, EditorIcons::kCopy);
        // Copy reads the layout and stays available; the two that write it do not.
        g_NineSliceLayoutMenu->AddItem(0, "Paste Layout", kCmdPasteLayout,
            (settingsWritable && g_CopiedNineSliceLayout.valid) ? MenuItemFlag_None
                                                                : MenuItemFlag_Disabled);
        g_NineSliceLayoutMenu->SetItemIcon(kCmdPasteLayout, EditorIcons::kPaste);
        g_NineSliceLayoutMenu->AddSeparator(0);
        g_NineSliceLayoutMenu->AddItem(0, "Reset Layout", kCmdResetLayout,
            settingsWritable ? MenuItemFlag_None : MenuItemFlag_Disabled);
        g_NineSliceLayoutMenu->SetItemIcon(kCmdResetLayout, EditorIcons::kReset);
        g_NineSliceLayoutMenu->SetCommandHandler([copyLayout, pasteLayout, resetLayout](uint32_t cmd) {
            switch (cmd)
            {
            case kCmdCopyLayout:  copyLayout(); break;
            case kCmdPasteLayout: pasteLayout(); break;
            case kCmdResetLayout: resetLayout(); break;
            default: break;
            }
        });

        const int x = static_cast<int>(button.GetLayoutX());
        const int y = static_cast<int>(button.GetLayoutY() + button.GetLayoutHeight());
        g_NineSliceLayoutMenu->Show(window, x, y);
    });
    if (UIElement* header = foldout->GetHeader())
        header->AddChild(std::move(menuOwned));

    applyGate(); // initial dim/disable to match the loaded Sliced state
}

void BuildTextureInspector(const InspectorContext& ctx, TextureAsset* tex)
{
    UIElement* const root = ctx.Parent;
    if (!root || !tex)
        return;

    const std::filesystem::path assetPath = tex->GetPath();
    const AssetImportSettingsOrigin origin =
        EngineCore::GetInstance().GetAssetManager().GetRegistry().GetImportSettingsOrigin(assetPath);

    InspectorUI::AddTextBlock(root, assetPath.string(), "inspector-asset-path");

    // An asset from a package is not in the project's own tree, and its
    // import settings are the package's. Name the package where the type is,
    // so the answer is in view before the settings below are read.
    UIElement* typeRow = InspectorUI::AddRow(root);
    InspectorUI::AddTextBlock(typeRow, "Type: Texture", "inspector-asset-type-line");
    if (!origin.SourceAlias.empty() && origin.SourceAlias != kAssetSourceAliasProject)
    {
        auto badge = std::make_unique<Label>();
        badge->AddClass("inspector-provenance-badge");
        badge->AddClass("package");
        badge->SetText(origin.SourceAlias);
        badge->SetTooltip("This texture belongs to the " + origin.SourceAlias +
                          " package, not to this project.");
        typeRow->AddChild(std::move(badge));
    }

    const GUID assetGuid = tex->GetGUID();
    auto formatLineOwned = std::make_unique<Label>();
    formatLineOwned->AddClass("inspector-asset-type-line");
    formatLineOwned->SetText(TextureFormatLineText(*tex, assetGuid));
    Label* const formatLine = formatLineOwned.get();
    root->AddChild(std::move(formatLineOwned));

    InspectorUI::AddTextBlock(root, "Import Settings", "inspector-section-subheader");

    // An asset whose source refuses metadata writes shows the import settings
    // its package authored, but nothing typed here can be saved back into it.
    // Say so, name the package the author has to edit instead, and hand every
    // field over disabled: a control that accepts a value it will drop on the
    // next refresh is worse than one that never offered it.
    const bool settingsWritable = origin.Writable;
    const bool loadedState = ShowsLoadedCookState(origin);

    // The recook an import-setting edit queues replaces the payload seconds to
    // tens of seconds later, and only the line above reports it — the rows below
    // read the store, which the edit already updated. So re-present that one line
    // rather than let the panel rebuild and take the keyboard focus, an open
    // popup and the scroll position with it. Two cases hand the reload back for
    // the rebuild: a source size change, because the rows built from the size are
    // shaped by it, and a read-only block, where several rows report the payload
    // rather than the store. Both pointers stay live for as long as the callback
    // does: the panel clears these with the tree they point into, and holds the
    // asset it shows.
    if (ctx.AssetReloadedCallbacks)
    {
        const uint32_t builtWidth = tex->GetWidth();
        const uint32_t builtHeight = tex->GetHeight();
        ctx.AssetReloadedCallbacks->push_back(
            [formatLine, tex, assetGuid, builtWidth, builtHeight, loadedState]() -> bool
            {
                if (loadedState || tex->GetWidth() != builtWidth || tex->GetHeight() != builtHeight)
                    return false;
                formatLine->SetText(TextureFormatLineText(*tex, assetGuid));
                return true;
            });
    }

    const MetaWriteContext writes{ctx.Undo, ctx.RequestInspectorRefresh, settingsWritable};
    if (!settingsWritable)
    {
        std::string notice = origin.SourceAlias.empty()
            ? std::string("These import settings are read-only here.")
            : "These import settings come from the " + origin.SourceAlias +
                  " package and are read-only here.";
        notice += " The values shown are the ones this texture is loaded with.";
        notice += origin.SourceAlias.empty()
            ? " Change them where this texture is authored and republish it."
            : " Change them in that package's own repository and republish it.";
        root->AddChild(std::make_unique<EditorUI::InspectorNotice>(notice));
    }

    // Every row below writes into the asset's metadata, and on a source that
    // refuses those writes each control is handed over disabled one by one:
    // the focus walk tests each element's own enabled flag, so a flag on this
    // wrapper alone would leave Tab and Enter reaching every control inside it.
    auto settingsOwned = std::make_unique<UIElement>();
    settingsOwned->AddClass("inspector-asset-settings");
    UIElement* const settings = settingsOwned.get();
    root->AddChild(std::move(settingsOwned));

    if (IsSvgTexturePath(assetPath))
    {
        const uint32 rasterSize = ReadCurrentSvgRasterSize(assetPath);
        float32 sourceWidth = static_cast<float32>(tex->GetWidth());
        float32 sourceHeight = static_cast<float32>(tex->GetHeight());
        std::string svgSource;
        float32 measuredWidth = 0.0f;
        float32 measuredHeight = 0.0f;
        if (ReadFileTextShared(assetPath, svgSource) &&
            GetSvgSourceSize(svgSource, measuredWidth, measuredHeight))
        {
            sourceWidth = measuredWidth;
            sourceHeight = measuredHeight;
        }

        const std::vector<Dropdown::Option> options =
            BuildSvgRasterSizeOptions(sourceWidth, sourceHeight);
        Dropdown* rasterSizeDropdown = InspectorUI::AddDropdownRow(
            settings, "Raster Size", options, FindSvgRasterSizeOption(options, rasterSize),
            "Maximum raster bounds for this SVG. Aspect ratio is preserved. "
            "This overrides the project's SVG texture default for this asset.");
        rasterSizeDropdown->SetAutoWidthPopup(true);
        rasterSizeDropdown->SetOnValueChanged([assetPath, writes](const std::string& value) {
            CommitTextureMeta(writes, assetPath, kSvgRasterSizeMetaKey, value,
                              &WriteTexturePayloadMeta);
        });
        SetControlWritable(rasterSizeDropdown, settingsWritable);
    }

    auto* field = InspectorUI::AddEnumRow(settings, "Color Space", kColorSpaceChoices,
        loadedState ? LoadedColorSpace(*tex, assetPath) : ReadCurrentChoice(assetPath),
        "How the GPU interprets this texture. Color maps are sRGB; normal / "
        "metallic-roughness / AO and other data maps must be Linear. Auto uses the "
        "file type and auto-tags data slots as Linear on first use.");
    field->SetOnValueChanged([assetPath, writes](ColorSpaceChoice v) {
        CommitTextureMeta(writes, assetPath, kTextureColorSpaceMetaKey, ChoiceToMeta(v),
                          &WriteTextureCookMeta);
    });
    SetControlWritable(field->GetDropdown(), settingsWritable);

    // One source for this row's value: the re-label below re-selects it, and a
    // re-label that picks a different value than the row was built with fires
    // the row's value-changed callback as if the user had edited it.
    const TextureCookCompression currentCompression =
        loadedState ? LoadedCompression(*tex) : ReadCurrentCompression(assetPath);
    auto* compressionField = InspectorUI::AddEnumRow(settings, "Compression", kCompressionChoices,
        currentCompression,
        "GPU block compression cooked at import. Auto picks from the usage below "
        "(color/ORM -> BC7, normals -> BC5, masks -> BC4, HDR -> BC6H; unknown usage "
        "stays uncompressed). BC1 is a size-critical opt-in — it bands on smooth "
        "gradients. Requires device BC support; falls back to uncompressed otherwise.");
    SetControlWritable(compressionField->GetDropdown(), settingsWritable);

    const bool usageUnauthored = loadedState && !HasAuthoredUsage(assetPath);
    auto* usageField = InspectorUI::AddEnumRow(settings,
        "Usage", usageUnauthored ? kUsageChoicesUnauthored : kUsageChoices,
        ReadCurrentUsage(assetPath),
        "What this texture holds — drives the Auto compression choice and normal-map "
        "mip renormalization. Auto-tagged from the material slot on first bind.");
    SetControlWritable(usageField->GetDropdown(), settingsWritable);

    // Mips: enabled toggle + optional level cap (0 = full chain to 1x1).
    {
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        std::string value;
        // The chain in the bound payload, not the stored intent: on a source
        // this editor cannot write to, the payload's level count is the answer
        // — the format line above reads it from the same payload.
        bool mipsOn = true;
        if (loadedState)
            mipsOn = tex->GetMipmapLevels() > 1;
        else if (registry.TryGetMetaValue(assetPath, kTextureMipsMetaKey, value))
            mipsOn = value != "0";
        Toggle* mipsToggle = InspectorDrag::AddToggleRow(settings, "Generate Mips", mipsOn,
            [assetPath, writes](bool on) {
                CommitTextureMeta(writes, assetPath, kTextureMipsMetaKey, on ? "1" : "0",
                                  &WriteTextureCookMeta);
            },
            "Cook a full mip chain at import (gamma-correct for sRGB color; "
            "renormalized for normal maps). Fixes distant aliasing and shimmer.");
        SetControlWritable(mipsToggle, settingsWritable);

        value.clear();
        float mipLimit = 0.0f;
        if (registry.TryGetMetaValue(assetPath, kTextureMipLimitMetaKey, value) && !value.empty())
            mipLimit = static_cast<float>(std::strtoul(value.c_str(), nullptr, 10));
        FloatField* mipLimitField = InspectorDrag::AddFloatRowWithDrag(settings, "Mip Limit", mipLimit,
            [](float) {},
            [assetPath, writes](float v) {
                const uint32_t levels = static_cast<uint32_t>(std::lround(std::max(v, 0.0f)));
                CommitTextureMeta(writes, assetPath, kTextureMipLimitMetaKey,
                                  std::to_string(levels), &WriteTextureCookMeta);
            },
            0.0f, "Maximum mip level count (0 = full chain down to 1x1).", 0.0f);
        SetControlWritable(mipLimitField, settingsWritable);
    }

    {
        // The alpha-coverage policy is only satisfiable for some Compression and Usage
        // choices, and an unsatisfiable one refuses the texture's next load rather than
        // cooking uncorrected mips. So the block re-reads the panel's current values after
        // every write that can invalidate it, and the two rows above carry the conflict in
        // their own lists while the policy is on.
        std::string extension = assetPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto readCoverageInputs = [assetPath, extension]() {
            auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
            Editor::TextureCoveragePolicyInputs inputs;
            registry.TryGetMetaValue(assetPath, kTextureAlphaCoverageMetaKey, inputs.Enabled);
            registry.TryGetMetaValue(assetPath, kTextureAlphaCutoffMetaKey, inputs.Cutoff);
            inputs.Compression = ReadCurrentCompression(assetPath);
            inputs.Usage = ReadCurrentUsage(assetPath);
            inputs.SourceIsHighDynamicRange = extension == ".hdr";
            return inputs;
        };

        auto coverage = std::make_shared<Editor::TextureCoveragePolicyRows>(
            Editor::AddTextureCoveragePolicyRows(settings, settingsWritable, readCoverageInputs,
                [assetPath, writes](bool on) {
                    CommitTextureMeta(writes, assetPath, kTextureAlphaCoverageMetaKey,
                                      on ? "1" : "0", &WriteTextureCookMeta);
                },
                [assetPath, writes](float value) {
                    std::string canonical = TextureAlphaCutoffMetaValue(value);
                    if (!canonical.empty())
                        CommitTextureMeta(writes, assetPath, kTextureAlphaCutoffMetaKey,
                                          std::move(canonical), &WriteTextureCookMeta);
                }));

        // Re-label the OTHER list, never the one that just fired: SetEntries rebuilds the
        // dropdown's options, and doing that from inside its own value-changed callback would
        // tear the control down mid-notification.
        auto markCompressionList = [assetPath, compressionField, readCoverageInputs,
                                    loadedState, currentCompression]() {
            const TextureCookCompression current =
                loadedState ? currentCompression : ReadCurrentCompression(assetPath);
            if (Editor::ReadTextureCoveragePolicy(readCoverageInputs()).Enabled)
                compressionField->SetEntries(kCompressionChoicesWithAlphaCoverage, current);
            else
                compressionField->SetEntries(kCompressionChoices, current);
        };
        auto markUsageList = [assetPath, usageField, readCoverageInputs, usageUnauthored]() {
            const TextureCookUsage current = ReadCurrentUsage(assetPath);
            if (Editor::ReadTextureCoveragePolicy(readCoverageInputs()).Enabled)
                usageField->SetEntries(kUsageChoicesWithAlphaCoverage, current);
            else if (usageUnauthored)
                usageField->SetEntries(kUsageChoicesUnauthored, current);
            else
                usageField->SetEntries(kUsageChoices, current);
        };

        compressionField->SetOnValueChanged(
            [assetPath, writes, coverage, markUsageList](TextureCookCompression v) {
                CommitTextureMeta(writes, assetPath, kTextureCompressionMetaKey,
                                  TextureCookCompressionMetaValue(v), &WriteTextureCookMeta);
                coverage->Refresh();
                markUsageList();
            });
        usageField->SetOnValueChanged(
            [assetPath, writes, coverage, markCompressionList](TextureCookUsage v) {
            CommitTextureMeta(writes, assetPath, kTextureUsageMetaKey,
                              TextureCookUsageMetaValue(v), &WriteTextureCookMeta);
            coverage->Refresh();
            markCompressionList();
        });

        // The enable toggle changes whether either list carries markers at all, and it is not
        // one of the two lists, so both are safe to re-label from it.
        *coverage->OnPolicyChanged = [markCompressionList, markUsageList]() {
            markCompressionList();
            markUsageList();
        };
        markCompressionList();
        markUsageList();
    }

    auto* filterField = InspectorUI::AddEnumRow(settings, "Filtering", kFilterChoices,
        ReadCurrentFilter(assetPath),
        "How this texture is sampled in every material that binds it. Inherit keeps the "
        "material's Texture Filter; Point is for pixel art and data lookups.");
    filterField->SetOnValueChanged([assetPath, writes](TextureFilterMode v) {
        CommitTextureMeta(writes, assetPath, kTextureFilterMetaKey, TextureFilterMetaValue(v),
                          &WriteTextureCookMeta);
    });
    SetControlWritable(filterField->GetDropdown(), settingsWritable);

    // Channel swizzle: four compact [channel-letter + dropdown] cells inline on a
    // single row (like a vector field). Each picks a source channel or constant
    // (0/1); applied as a channel remap at upload.
    auto swizzle = std::make_shared<std::array<char, 4>>(ReadCurrentSwizzle(assetPath));
    {
        UIElement* row = InspectorUI::AddRow(settings);
        InspectorUI::AddLabel(row, "Swizzle",
            "Per-channel source (R/G/B/A or 0/1). For single-channel or repacked maps.");
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

        auto strip = std::make_unique<UIElement>();
        strip->AddClass("inspector-swizzle-strip");
        UIElement* stripRaw = strip.get();
        fieldContainer->AddChild(std::move(strip));

        const char* kChan[4] = {"R", "G", "B", "A"};
        for (int ch = 0; ch < 4; ++ch)
        {
            auto cell = std::make_unique<UIElement>();
            cell->AddClass("inspector-swizzle-cell");
            UIElement* cellRaw = cell.get();
            stripRaw->AddChild(std::move(cell));

            auto chanLabel = std::make_unique<Label>();
            chanLabel->SetText(kChan[ch]);
            chanLabel->AddClass("inspector-swizzle-channel");
            cellRaw->AddChild(std::move(chanLabel));

            auto chanField = std::make_unique<EnumField<SwizzleSource>>();
            chanField->AddClass("inspector-swizzle-enum");
            chanField->SetEntries(kSwizzleSources, CharToSwizzleSource((*swizzle)[ch]));
            chanField->GetDropdown()->AddClass("inspector-dropdown");
            EnumField<SwizzleSource>* chanFieldRaw = chanField.get();
            cellRaw->AddChild(std::move(chanField));

            chanFieldRaw->SetOnValueChanged([assetPath, writes, swizzle, ch](SwizzleSource v) {
                (*swizzle)[ch] = SwizzleSourceToChar(v);
                CommitTextureMeta(writes, assetPath, kTextureSwizzleMetaKey,
                                  std::string(swizzle->begin(), swizzle->end()),
                                  &WriteTextureMeta);
            });
            SetControlWritable(chanFieldRaw->GetDropdown(), settingsWritable);
        }
    }

    BuildNineSliceSection(settings, tex, assetPath, ctx.Window, settingsWritable, writes);
}

} // namespace

void RegisterTextureInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::Texture,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;
            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* tex = dynamic_cast<TextureAsset*>(asset);
            if (!tex)
                return;
            BuildTextureInspector(ctx, tex);
        });
}

} // namespace GameEngine
