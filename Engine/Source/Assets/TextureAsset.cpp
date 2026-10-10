#include "Assets/TextureAsset.h"

#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/SvgRasterizer.h"
#include "Assets/TextureCook.h"
#include "Assets/PackagedTexturePayload.h"
#include "Core/Engine.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#if defined(GE_HAVE_STB)
#include <stb_image.h>
#endif
#if defined(GE_HAVE_KTX)
#include <ktx.h>
#include <vulkan/vulkan_core.h>
#endif

namespace GameEngine {

TextureColorSpace GuessTextureColorSpace(const std::string& extensionLower)
{
    if (extensionLower == ".png" || extensionLower == ".jpg" || extensionLower == ".jpeg" ||
        extensionLower == ".tga" || extensionLower == ".bmp" || extensionLower == ".psd" ||
        extensionLower == ".svg")
        return TextureColorSpace::SRGB;
    return TextureColorSpace::Linear;
}

bool ParseTextureColorSpaceMeta(const std::string& value, TextureColorSpace& out)
{
    if (value == "srgb" || value == "sRGB" || value == "SRGB")
    {
        out = TextureColorSpace::SRGB;
        return true;
    }
    if (value == "linear" || value == "Linear")
    {
        out = TextureColorSpace::Linear;
        return true;
    }
    // "auto", empty, or anything unrecognized -> keep the extension guess.
    return false;
}

const char* TextureColorSpaceMetaValue(TextureColorSpace cs)
{
    switch (cs)
    {
    case TextureColorSpace::SRGB:   return "srgb";
    case TextureColorSpace::Linear: return "linear";
    default:                        return "auto";
    }
}

bool ParseTextureSwizzleMeta(const std::string& value, char out[4])
{
    if (value.size() != 4)
        return false;
    for (int i = 0; i < 4; ++i)
    {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
        if (c != 'r' && c != 'g' && c != 'b' && c != 'a' && c != '0' && c != '1')
            return false;
        out[i] = c;
    }
    // Identity needs no work.
    return !(out[0] == 'r' && out[1] == 'g' && out[2] == 'b' && out[3] == 'a');
}

namespace
{
    const char* NineSliceFillToToken(NineSliceFill f)
    {
        switch (f)
        {
        case NineSliceFill::Tile:  return "tile";
        case NineSliceFill::Round: return "round";
        case NineSliceFill::Scale: return "scale";
        default:                   return "stretch";
        }
    }

    NineSliceFill NineSliceFillFromToken(const std::string& t)
    {
        if (t == "tile")  return NineSliceFill::Tile;
        if (t == "round") return NineSliceFill::Round;
        if (t == "scale") return NineSliceFill::Scale;
        return NineSliceFill::Stretch;
    }

    // Parse a decimal token into a uint16 (saturating). Rejects empty / non-digit.
    bool ParseU16Token(const std::string& s, uint16& dst)
    {
        if (s.empty())
            return false;
        unsigned long v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + static_cast<unsigned long>(c - '0');
            if (v > 65535ul)
                v = 65535ul;
        }
        dst = static_cast<uint16>(v);
        return true;
    }
}

bool ParseTextureNineSliceMeta(const std::string& value, NineSlice& out)
{
    // Schema: "<id>,enabled,x0,x1,x2,x3,y0,y1,y2,y3,fillX,fillY,center[,centerFill]"
    //   9s1 = 13 tokens (no centerFill -> Stretch); 9s2 = 14 tokens (adds centerFill).
    if (value.empty())
        return false;

    std::vector<std::string> tok;
    tok.reserve(14);
    for (size_t start = 0;;)
    {
        const size_t comma = value.find(',', start);
        tok.push_back(value.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }

    if (tok.size() < 13 || (tok[0] != "9s1" && tok[0] != "9s2"))
        return false;

    NineSlice s;
    s.Enabled = (tok[1] == "1");
    for (int i = 0; i < 4; ++i)
        if (!ParseU16Token(tok[2 + i], s.X[i]))
            return false;
    for (int i = 0; i < 4; ++i)
        if (!ParseU16Token(tok[6 + i], s.Y[i]))
            return false;
    s.FillX = NineSliceFillFromToken(tok[10]);
    s.FillY = NineSliceFillFromToken(tok[11]);
    s.FillCenter = (tok[12] == "1");
    s.CenterFill = (tok.size() >= 14) ? NineSliceFillFromToken(tok[13]) : NineSliceFill::Stretch;

    // Cuts must be non-decreasing per axis; repair rather than reject.
    for (int i = 1; i < 4; ++i)
    {
        if (s.X[i] < s.X[i - 1]) s.X[i] = s.X[i - 1];
        if (s.Y[i] < s.Y[i - 1]) s.Y[i] = s.Y[i - 1];
    }

    out = s;
    return s.Enabled;
}

std::string TextureNineSliceMetaValue(const NineSlice& slice)
{
    std::string r = "9s2,";
    r += slice.Enabled ? "1" : "0";
    for (int i = 0; i < 4; ++i) { r += ','; r += std::to_string(slice.X[i]); }
    for (int i = 0; i < 4; ++i) { r += ','; r += std::to_string(slice.Y[i]); }
    r += ','; r += NineSliceFillToToken(slice.FillX);
    r += ','; r += NineSliceFillToToken(slice.FillY);
    r += ','; r += slice.FillCenter ? "1" : "0";
    r += ','; r += NineSliceFillToToken(slice.CenterFill);
    return r;
}

TextureAsset::TextureAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::Texture, path)
    , m_Width(0)
    , m_Height(0)
    , m_Channels(0)
	    , m_Format(TextureFormat::Unknown)
	    , m_ColorSpace(TextureColorSpace::Unknown)
    , m_PixelData(nullptr)
    , m_DataSize(0)
    , m_HasMipmaps(false)
    , m_MipmapLevels(1)
    , m_MinFilter(TextureFilter::Linear)
    , m_MagFilter(TextureFilter::Linear)
    , m_WrapS(TextureWrap::Repeat)
    , m_WrapT(TextureWrap::Repeat)
{
}

TextureAsset::~TextureAsset() {
    Unload();
}

bool TextureAsset::Load() {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Debug("Loading texture: {}", GetPath().string());

    if (const auto packaged = LoadPackagedTexture(nullptr))
    {
        SetState(*packaged ? AssetState::Loaded : AssetState::Failed);
        return *packaged;
    }

    if (!Exists()) {
        Logger::Log::Error("Texture file does not exist: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    String extension = GetExtension();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });

    bool success = false;
    if (extension == ".dds") {
        success = LoadDDS();
    }
#if defined(GE_HAVE_KTX)
    else if (extension == ".ktx" || extension == ".ktx2") {
        success = LoadWithKTX();
    }
#else
    else if (extension == ".ktx" || extension == ".ktx2") {
        Logger::Log::Error("KTX texture support not available at build time for '{}'", GetPath().string());
        success = false;
    }
#endif
    else if (extension == ".svg")
    {
        std::string svgText;
        if (!ReadFileTextShared(GetPath(), svgText))
        {
            Logger::Log::Error("TextureAsset: failed to open SVG file '{}'", GetPath().string());
            success = false;
        }
        else
        {
            success = LoadFromSvgData(svgText, extension);
        }
    }
    else {
        success = LoadWithSTB();
    }

    if (success) {
        SetState(AssetState::Loaded);
        Logger::Log::Debug("Loaded texture: {}x{} {} channels", m_Width, m_Height, m_Channels);
    } else {
        SetState(AssetState::Failed);
        Logger::Log::Error("Failed to load texture: {}", GetPath().string());
    }

    return success;
}

bool TextureAsset::LoadFromData(const Vector<uint8>& data) {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Debug("Loading texture from memory data: {}", GetName());

    if (const auto packaged = LoadPackagedTexture(&data))
    {
        SetState(*packaged ? AssetState::Loaded : AssetState::Failed);
        return *packaged;
    }

    if (data.empty()) {
        Logger::Log::Error("Empty texture data provided");
        SetState(AssetState::Failed);
        return false;
    }

    String extension = GetExtension();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });

    bool success = false;
    if (extension == ".dds") {
        success = LoadDDSFromData(data);
    }
#if defined(GE_HAVE_KTX)
    else if (extension == ".ktx" || extension == ".ktx2") {
        success = LoadWithKTXFromData(data);
    }
#else
    else if (extension == ".ktx" || extension == ".ktx2") {
        Logger::Log::Error("KTX texture support not available at build time for '{}' (memory load)", GetName());
        success = false;
    }
#endif
    else if (extension == ".svg")
    {
        std::string svgText(reinterpret_cast<const char*>(data.data()), data.size());
        success = LoadFromSvgData(svgText, extension);
    }
    else {
        success = LoadWithSTBFromData(data);
    }

    if (success) {
        SetState(AssetState::Loaded);
        Logger::Log::Debug("Loaded texture from data: {}x{} {} channels", m_Width, m_Height, m_Channels);
    } else {
        SetState(AssetState::Failed);
        // A load abandoned by its own cancel flag (shutdown, eject, superseded
        // reload) is a withdrawn request, not a failure to report — a shutdown
        // on a cold cook cache cancels every in-flight texture at once.
        if (IsAssetDecodeCancelled())
            Logger::Log::Debug("Abandoned cancelled texture load from data: {}", GetName());
        else
            Logger::Log::Error("Failed to load texture from data: {}", GetName());
    }

    return success;
}

std::optional<bool> TextureAsset::LoadPackagedTexture([[maybe_unused]] const Vector<uint8>* data)
{
    if (GetGUID().IsNull())
        return std::nullopt;
    AssetManager* manager = AssetManager::GetThreadCurrent();
    const bool preferCompressed = (manager || m_AdoptCookedArtifacts) &&
        GetGpuTranscodeTarget() == TextureGpuTranscodeTarget::BC7;
    if (!manager && EngineCore::GetInstance().IsInitialized())
        manager = &EngineCore::GetInstance().GetAssetManager();
    if (!manager)
        return std::nullopt;
    const auto& registry = manager->GetRegistry();
    const auto payload = ResolvePackagedTexturePayload(registry, GetGUID(), preferCompressed);
    if (!payload)
        return std::nullopt;
#if defined(GE_HAVE_KTX)
    if (!payload->empty())
    {
        Vector<uint8> bytes;
        bool loaded = data ? LoadWithKTXFromData(*data) :
            ReadFileBytesShared(*payload, bytes) && LoadWithKTXFromData(bytes);
        // A damaged compressed variant can still use the shipped portable pixels.
        const auto portable = ResolvePackagedTexturePayload(registry, GetGUID(), false);
        if (!loaded && portable && !portable->empty() && *portable != *payload)
        {
            Unload();
            loaded = ReadFileBytesShared(*portable, bytes) && LoadWithKTXFromData(bytes);
        }
        if (loaded)
        {
            m_PackagedPortablePath = portable && !portable->empty() ? *portable : *payload;
            AssetMetadata metadata;
            std::string swizzle;
            char channels[4]{};
            if (registry.TryGetAssetMetadata(GetGUID(), metadata) &&
                registry.TryGetMetaValue(metadata.Path, kTextureSwizzleMetaKey, swizzle))
            {
                auto extension = metadata.Path.extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                m_SwizzleBaked = ParseTextureSwizzleMeta(swizzle, channels) && channels[0] != 0 && extension != ".hdr";
            }
            return true;
        }
    }
#endif
    Logger::Log::Error("TextureAsset[{}]: packaged payload missing or corrupt; rebuild the package", GetName());
    return false;
}

void TextureAsset::Unload() {
    if (m_PixelData) {
        delete[] m_PixelData;
        m_PixelData = nullptr;
    }

    m_Width = 0;
    m_Height = 0;
    m_Channels = 0;
    m_Format = TextureFormat::Unknown;
	    m_ColorSpace = TextureColorSpace::Unknown;
    m_DataSize = 0;
    m_HasMipmaps = false;
    m_SwizzleBaked = false;
    m_MipmapLevels = 1;
    m_MipChain.clear();

    SetState(AssetState::Unloaded);
    Logger::Log::Debug("Unloaded texture: {}", GetName());
}

bool TextureAsset::AdoptReloadedPayload(Asset& staged) {
    auto* other = dynamic_cast<TextureAsset*>(&staged);
    if (!other || !other->IsLoaded()) {
        return false;
    }

    // Swap exactly the members Unload() resets and the loaders write. The
    // previous payload moves into `staged`, which frees it when destroyed.
    // Sampler settings (filter/wrap) are user state that survives a Reload(),
    // so adoption preserves them too.
    std::swap(m_Width, other->m_Width);
    std::swap(m_Height, other->m_Height);
    std::swap(m_Channels, other->m_Channels);
    std::swap(m_Format, other->m_Format);
    std::swap(m_ColorSpace, other->m_ColorSpace);
    std::swap(m_PixelData, other->m_PixelData);
    std::swap(m_DataSize, other->m_DataSize);
    std::swap(m_HasMipmaps, other->m_HasMipmaps);
    std::swap(m_SwizzleBaked, other->m_SwizzleBaked);
    std::swap(m_MipmapLevels, other->m_MipmapLevels);
    std::swap(m_MipChain, other->m_MipChain);
    return true;
}

void TextureAsset::SetFilter(TextureFilter minFilter, TextureFilter magFilter) {
    m_MinFilter = minFilter;
    m_MagFilter = magFilter;
}

void TextureAsset::SetWrap(TextureWrap wrapS, TextureWrap wrapT) {
    m_WrapS = wrapS;
    m_WrapT = wrapT;
}

namespace
{
    uint32 CurrentProcessId()
    {
#if defined(_WIN32)
        return static_cast<uint32>(_getpid());
#else
        return static_cast<uint32>(getpid());
#endif
    }

    // Build the artifact beside its destination, then publish it, so concurrent
    // native readers see only complete replacements. Web readers validate a
    // potentially partial copy and treat it as a cache miss. The temp
    // name carries the PID + a process-wide sequence: MSVC ofstream opens
    // _SH_DENYNO, so two writers sharing one ".tmp" could interleave bytes into
    // a payload that still parses — unique temp names remove that class.
    // Orphaned temps (crash mid-write) are swept by RemoveStaleCookArtifacts on
    // the next successful cook of the same asset.
    bool WriteCookArtifact(const std::filesystem::path& file, const std::vector<uint8>& bytes)
    {
        static std::atomic<uint32> s_TmpSequence{0};
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::filesystem::path tmp = file;
        tmp += ".tmp." + std::to_string(CurrentProcessId()) + "." +
               std::to_string(s_TmpSequence.fetch_add(1, std::memory_order_relaxed));
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            if (!out.good())
                return false;
        }
        if (FileSystem::PublishFile(tmp, file))
            return true;
        // Cooks are deterministic, so a file another writer already published
        // under this content-derived name holds the same bytes.
        return std::filesystem::exists(file);
    }

    // Sweep stale cook artifacts for this GUID (older source/config keys).
    void RemoveStaleCookArtifacts(const std::filesystem::path& dir,
                                  const std::string& guidPrefix,
                                  const std::filesystem::path& keep)
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::filesystem::path& p = it->path();
            if (p != keep && p.filename().string().rfind(guidPrefix, 0) == 0)
                std::filesystem::remove(p, ec);
        }
    }
}

bool TextureAsset::TryLoadViaCook(const Vector<uint8>& sourceBytes, const String& extensionLower,
                                  AlphaCoveragePolicy& coveragePolicy)
{
    coveragePolicy = AlphaCoveragePolicy::Disabled;
    // Cook policy by caller context:
    //   - Inside an AssetManager load (worker / reload dispatch): full cook —
    //     adopt a cached artifact, or cook + cache on miss.
    //   - Outside a load context with SetAdoptCookedArtifacts(true) (the GPU
    //     upload path — render-thread GetOrUpload): adopt an EXISTING artifact
    //     only; a miss falls back to the raw decode (the encode is seconds-to-
    //     minutes scale and must never run on the render/main thread) and
    //     converges when a worker-side load or recook next cooks it.
    //   - Plain direct Load() (terrain masks, ocean extraction): raw decode —
    //     CPU consumers keep raw per-pixel payloads and source channel counts.
    AssetManager* assetManager = AssetManager::GetThreadCurrent();
    // Captured before the adoption-only fallback below reassigns assetManager;
    // only the full cook path (stb + libktx) reads it.
    [[maybe_unused]] const bool canCookOnMiss = assetManager != nullptr;
    if (!assetManager)
    {
        if (!m_AdoptCookedArtifacts || !EngineCore::GetInstance().IsInitialized())
            return false;
        assetManager = &EngineCore::GetInstance().GetAssetManager();
    }
    if (sourceBytes.empty())
        return false;

    // Resolve the registry's canonical path by GUID: the asset's own recorded
    // path may be project-relative (async pipeline), which the path-keyed
    // source/meta lookups don't match.
    auto& registry = assetManager->GetRegistry();
    AssetMetadata registryMeta{};
    if (!registry.TryGetAssetMetadata(GetGUID(), registryMeta) || registryMeta.Path.empty())
        return false;
    const std::filesystem::path& assetPath = registryMeta.Path;
    const bool isHDR = extensionLower == ".hdr";

    TextureCookInputs inputs;
    std::string coverageError;
    if (!ResolveTextureCookInputs(extensionLower,
        [&registry, &assetPath](const char* key, std::string& value) {
            return registry.TryGetMetaValue(assetPath, key, value);
        }, inputs, coverageError))
    {
        Logger::Log::Error("TextureAsset[{}]: invalid alpha coverage policy: {}", GetName(), coverageError);
        coveragePolicy = AlphaCoveragePolicy::Invalid;
        return false;
    }
    coveragePolicy = inputs.Settings.PreserveAlphaCoverage ? AlphaCoveragePolicy::Enabled : AlphaCoveragePolicy::Disabled;
    // Invalid opt-in policy is never hidden by the raw fallback or kill switch.
    // Valid raw uploads use the same builder even when cooking is disabled.
#if !defined(GE_HAVE_STB)
    if (inputs.Settings.PreserveAlphaCoverage)
    {
        Logger::Log::Error("TextureAsset[{}]: alpha coverage requires the source-image decoder", GetName());
        coveragePolicy = AlphaCoveragePolicy::Invalid;
        return false;
    }
#endif
    // Kill switch (GE_TEXTURE_COOK_DISABLE=1): raw decode, no cook, no cache
    // I/O — the ops escape hatch and the A/B baseline. Not an exact pre-branch
    // frame: the shader Z-reconstruct stays active (see IsTextureCookDisabled).
    if (IsTextureCookDisabled())
        return false;
#if defined(GE_HAVE_KTX) && defined(GE_HAVE_STB)
    const std::optional<std::filesystem::path> cacheRoot = registry.TryGetCacheRoot(GetGUID());
    if (!cacheRoot)
    {
        Logger::Log::Debug("TextureAsset[{}]: no derived-cache root — cook unavailable", GetName());
        return false;
    }

    const bool hasSwizzle = inputs.Swizzle[0] != 0;

    // BC viability mirrors the KTX2 transcode target: TextureService publishes
    // BC7 exactly when the device reported textureCompressionBC.
    const bool deviceSupportsBC = GetGpuTranscodeTarget() == TextureGpuTranscodeTarget::BC7;
    TextureCookOutput output = ResolveTextureCookOutput(
        inputs.Settings, isHDR, deviceSupportsBC, /*encoderAvailable=*/true);

    // A cook that neither compresses, mips, nor bakes a swizzle would be
    // byte-equivalent to the raw decode — skip the cache churn.
    if (!NeedsTextureCook(inputs, output))
        return false;

    // A package's textures load through their packaged payload (LoadPackagedTexture);
    // one that needs a cook here carries none, so it raw-decodes and never runs an
    // encoder on the player's machine.
    if (!registry.GetDerivedArtifactPolicy(GetGUID()).CooksOnMiss)
    {
        Logger::Log::Error("TextureAsset[{}]: packaged texture has no packaged payload; rebuild the package. "
                           "Using raw decode without runtime compression.",
                           GetName());
        return false;
    }

    const uint64 srcHash = ComputeTextureCookSourceHash(sourceBytes.data(), sourceBytes.size());
    const std::filesystem::path cacheDir = *cacheRoot / AssetDatabase::kTextureCacheDirectoryName;
    const std::string guidPrefix = GetGUID().ToString() + "-";
    std::filesystem::path cacheFile =
        cacheDir / TextureCookArtifactName(GetGUID(), srcHash, inputs, output);

    // Hit: adopt the cooked container. A corrupt artifact falls through to a
    // recook (fail visible, never a failed load).
    // The cook bakes the swizzle for LDR sources only (CookTexture skips float
    // payloads), so the flag must not claim a bake for HDR — the upload path's
    // "unapplied swizzle" warning stays honest there.
    const bool swizzleBaked = hasSwizzle && !isHDR;

    const auto adoptArtifact = [&]() {
        Vector<uint8> cooked;
        if (!ReadFileBytesShared(cacheFile, cooked) || cooked.empty())
            return false;
        if (LoadWithKTXFromData(cooked))
        {
            m_SwizzleBaked = swizzleBaked;
            return true;
        }
        Logger::Log::Warning("TextureAsset[{}]: cooked texture rejected (corrupt): {}",
                             GetName(), cacheFile.generic_string());
        delete[] m_PixelData;
        m_PixelData = nullptr;
        m_MipChain.clear();
        return false;
    };
    if (adoptArtifact())
        return true;

    // A development build with no BC encoder linked can still adopt the
    // uncompressed artifact. Artifact reads themselves never require an encoder.
    if (output != TextureCookOutput::Uncompressed && !IsTextureCookEncoderAvailable())
    {
        output = TextureCookOutput::Uncompressed;
        if (!NeedsTextureCook(inputs, output))
            return false;
        cacheFile = cacheDir / TextureCookArtifactName(GetGUID(), srcHash, inputs, output);
        if (adoptArtifact())
            return true;
    }

    // Miss: cook now (worker thread), cache, then adopt. Adoption-only callers
    // (render/main thread) stop here and raw-decode instead.
    if (!canCookOnMiss)
        return false;

    static const char* kOutputNames[] = {"Uncompressed", "BC1", "BC4", "BC5", "BC6H", "BC7"};

    // Bracket the cook. A block-compression encode of a large source runs for
    // seconds to minutes on the worker, and every material sampling this texture
    // renders its bindless default (white albedo, flat normal) until it lands.
    // With only the completion line below, that window leaves no trace at all —
    // a screenshot taken inside it is a plausible frame of an unbound texture,
    // indistinguishable from a binding failure.
    Logger::Log::Info("Texture cook: {} -> {} started ({} KB source)",
                      GetName(), kOutputNames[static_cast<uint8>(output)],
                      sourceBytes.size() / 1024);

    // The decode job publishes its cancel flag for the whole call tree; a cook
    // running outside one (synchronous load, tooling) simply isn't cancellable.
    const auto cookStart = std::chrono::steady_clock::now();
    std::vector<uint8> cookedBytes;
    std::string cookError;
    const auto cancelRequested = CurrentAssetDecodeCancellation();
    if (!CookTexture(sourceBytes.data(), sourceBytes.size(), std::string(extensionLower),
                     inputs, output, cookedBytes, cookError,
                     [cancelRequested]() {
                         return cancelRequested && cancelRequested->load(std::memory_order_acquire);
                     },
                     /*quality=*/{}, assetManager->GetTextureCookWorkers()))
    {
        // Debug, not a warning: undecodable sources (test placeholders) are
        // routine here, and so is a cancelled load — that one reports
        // kTextureCookCancelledError on its way to abandoning the load rather
        // than to the raw decode this line names.
        Logger::Log::Debug("TextureAsset[{}]: cook skipped ({}) — using raw decode",
                           GetName(), cookError);
        return false;
    }

    const double cookMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - cookStart)
                              .count();
    Logger::Log::Info("Texture cook: {} -> {} ({} KB) in {:.0f} ms ({})",
                      GetName(), kOutputNames[static_cast<uint8>(output)],
                      cookedBytes.size() / 1024, cookMs, cacheFile.filename().string());

    if (!WriteCookArtifact(cacheFile, cookedBytes))
        Logger::Log::Warning("TextureAsset[{}]: failed to write cook cache '{}'",
                             GetName(), cacheFile.generic_string());
    else
        RemoveStaleCookArtifacts(cacheDir, guidPrefix, cacheFile);

    Vector<uint8> cookedVec(cookedBytes.begin(), cookedBytes.end());
    if (!LoadWithKTXFromData(cookedVec))
    {
        Logger::Log::Warning("TextureAsset[{}]: freshly cooked texture failed to load — using raw decode",
                             GetName());
        delete[] m_PixelData;
        m_PixelData = nullptr;
        m_MipChain.clear();
        return false;
    }
    m_SwizzleBaked = swizzleBaked;
    return true;
#else
    (void)sourceBytes;
    (void)extensionLower;
    return false;
#endif
}

bool TextureAsset::LoadWithSTB() {
    // Read through the shared-open path (never lets stb fopen the file
    // directly — see AssetCore/SharedFileRead.h), then decode from memory.
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(GetPath(), bytes) || bytes.empty()) {
        Logger::Log::Error("Failed to read image file '{}'", GetPath().string());
        return false;
    }

    String extension = GetExtension();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });

    // Derived-cache cook (BCn + mips) replaces the raw decode when available.
    AlphaCoveragePolicy coveragePolicy;
    if (TryLoadViaCook(bytes, extension, coveragePolicy))
        return true;
    if (coveragePolicy == AlphaCoveragePolicy::Invalid)
        return false;
#if defined(GE_HAVE_STB)
    int width = 0, height = 0, channels = 0;
    // Decide whether this is an HDR texture based on file extension
    const bool isHDRFile = (extension == ".hdr");

    // For glTF baseColor textures we do not flip (UVs match), but if needed:
    stbi_set_flip_vertically_on_load(false);

    if (isHDRFile) {
        float* data = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                             &width, &height, &channels, 0);
        if (!data) {
            Logger::Log::Error("STB failed to load HDR image '{}': {}", GetPath().string(), stbi_failure_reason());
            return false;
        }
        m_Width = static_cast<uint32>(width);
        m_Height = static_cast<uint32>(height);
        m_Channels = static_cast<uint32>(channels);
        m_Format = DetermineFormat(m_Channels, true);
	        m_ColorSpace = GuessTextureColorSpace(extension);
        m_DataSize = static_cast<uint64>(m_Width) * static_cast<uint64>(m_Height) *
                     static_cast<uint64>(m_Channels) * sizeof(float);
        m_PixelData = new uint8[m_DataSize];
        std::memcpy(m_PixelData, data, static_cast<size_t>(m_DataSize));
        stbi_image_free(data);
        return true;
    } else {
        unsigned char* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                    &width, &height, &channels, 0);
        if (!data) {
            Logger::Log::Error("STB failed to load image '{}': {}", GetPath().string(), stbi_failure_reason());
            return false;
        }
        m_Width = static_cast<uint32>(width);
        m_Height = static_cast<uint32>(height);
        m_Channels = static_cast<uint32>(channels);
        m_Format = DetermineFormat(m_Channels, false);
	        m_ColorSpace = GuessTextureColorSpace(extension);
        m_DataSize = static_cast<uint64>(m_Width) * static_cast<uint64>(m_Height) *
                     static_cast<uint64>(m_Channels);
        m_PixelData = new uint8[m_DataSize];
        std::memcpy(m_PixelData, data, static_cast<size_t>(m_DataSize));
        stbi_image_free(data);
        return true;
    }
#else
    // Fallback: keep the previous 2x2 dummy so the app still runs without stb
    Logger::Log::Warning("STB not available at build time; using dummy texture for {}", GetPath().string());
    m_Width = 2; m_Height = 2; m_Channels = 4; m_Format = DetermineFormat(m_Channels, false);
	    m_ColorSpace = TextureColorSpace::SRGB;
    m_DataSize = m_Width * m_Height * m_Channels;
    m_PixelData = new uint8[m_DataSize];
    const uint8 pattern[] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255 };
    std::memcpy(m_PixelData, pattern, sizeof(pattern));
    return true;
#endif
}

bool TextureAsset::LoadWithSTBFromData(const Vector<uint8>& data) {
    AlphaCoveragePolicy coveragePolicy;
    String extension = GetExtension();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    {
        // Derived-cache cook (BCn + mips) replaces the raw decode when available.
        if (TryLoadViaCook(data, extension, coveragePolicy))
            return true;
        if (coveragePolicy == AlphaCoveragePolicy::Invalid)
            return false;
        // A cook that stopped because the load was cancelled must not fall
        // through to the raw decode: the whole load is being abandoned, and
        // decoding here would just re-spend the time the cancel reclaimed.
        if (IsAssetDecodeCancelled())
            return false;
    }
#if defined(GE_HAVE_STB)
    int width=0, height=0, channels=0;
    stbi_set_flip_vertically_on_load(false);
    // Worker loads arrive as bytes too: retain HDR range on a missing package
    // bake just as the direct file path does, without an implicit LDR conversion.
    if (extension == ".hdr")
    {
        float* pixels = stbi_loadf_from_memory(data.data(), static_cast<int>(data.size()),
                                              &width, &height, &channels, 0);
        if (!pixels)
        {
            Logger::Log::Error("STB failed to load HDR image '{}': {}", GetPath().string(), stbi_failure_reason());
            return false;
        }
        m_Width = static_cast<uint32>(width);
        m_Height = static_cast<uint32>(height);
        m_Channels = static_cast<uint32>(channels);
        m_Format = DetermineFormat(m_Channels, true);
        m_ColorSpace = TextureColorSpace::Linear;
        m_DataSize = static_cast<uint64>(m_Width) * m_Height * m_Channels * sizeof(float);
        m_PixelData = new uint8[m_DataSize];
        std::memcpy(m_PixelData, pixels, static_cast<size_t>(m_DataSize));
        stbi_image_free(pixels);
        return true;
    }
    unsigned char* imageData = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), &width, &height, &channels, 0);
    if (!imageData) {
        if (coveragePolicy == AlphaCoveragePolicy::Enabled)
        {
            Logger::Log::Error("TextureAsset[{}]: alpha coverage requires decoded source pixels; placeholder refused", GetName());
            return false;
        }
        // Some tests intentionally store placeholder "PNG" text to validate metadata/hot-reload flows
        // without committing binary files to the repo. Treat this as a dummy texture rather than a hard failure,
        // otherwise the editor can spam retries (e.g., background images / asset previews repeatedly requesting it).
        const auto isPlaceholderPng = [&]() -> bool
        {
            if (data.empty())
                return false;
            // Quick scan for sentinel strings used by tests.
            const char* begin = reinterpret_cast<const char*>(data.data());
            const char* end = begin + data.size();
            auto contains = [&](const char* needle) -> bool
            {
                if (!needle || !*needle)
                    return false;
                const size_t nlen = std::strlen(needle);
                if (nlen == 0 || (size_t)(end - begin) < nlen)
                    return false;
                for (const char* p = begin; p + nlen <= end; ++p)
                {
                    if (std::memcmp(p, needle, nlen) == 0)
                        return true;
                }
                return false;
            };
            return contains("PNG_PLACEHOLDER_DATA") || contains("# Placeholder PNG file");
        }();

        if (isPlaceholderPng)
        {
            Logger::Log::Warning("TextureAsset: Placeholder PNG data detected for '{}'; using dummy texture", GetName());
            m_Width = 2;
            m_Height = 2;
            m_Channels = 4;
            m_Format = DetermineFormat(m_Channels, false);
            m_ColorSpace = TextureColorSpace::SRGB;
            m_DataSize = m_Width * m_Height * m_Channels;
            m_PixelData = new uint8[m_DataSize];
            const uint8 pattern[] = { 255,0,255,255, 0,0,0,255, 0,0,0,255, 255,0,255,255 };
            std::memcpy(m_PixelData, pattern, sizeof(pattern));
            return true;
        }

        Logger::Log::Error("STB failed to load image from memory for '{}': {}", GetName(), stbi_failure_reason());
        return false;
    }
    m_Width = static_cast<uint32>(width);
    m_Height = static_cast<uint32>(height);
    m_Channels = static_cast<uint32>(channels);
    m_Format = DetermineFormat(m_Channels, false);
    m_ColorSpace = GuessTextureColorSpace(extension);
    m_DataSize = static_cast<uint64>(m_Width) * static_cast<uint64>(m_Height) * static_cast<uint64>(m_Channels);
    m_PixelData = new uint8[m_DataSize];
    std::memcpy(m_PixelData, imageData, static_cast<size_t>(m_DataSize));
    stbi_image_free(imageData);
    return true;
#else
    Logger::Log::Warning("STB not available at build time; using dummy memory texture for {}", GetName());
    m_Width = 2; m_Height = 2; m_Channels = 4; m_Format = DetermineFormat(m_Channels, false);
	    m_ColorSpace = TextureColorSpace::SRGB;
    m_DataSize = m_Width * m_Height * m_Channels;
    m_PixelData = new uint8[m_DataSize];
    const uint8 pattern[] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255 };
    std::memcpy(m_PixelData, pattern, sizeof(pattern));
    return true;
#endif
}

bool TextureAsset::LoadDDS() {
    // TODO: Implement DDS loading
    Logger::Log::Warning("DDS texture loading not yet implemented");
    return false;
}

bool TextureAsset::LoadDDSFromData([[maybe_unused]] const Vector<uint8>& data) {
    // TODO: Implement DDS loading from memory
    Logger::Log::Warning("DDS texture loading from memory not yet implemented");
    return false;
}

namespace
{
    // Written once by the renderer at device init, read by asset decode on
    // worker threads (see TextureGpuTranscodeTarget in the header).
    std::atomic<TextureGpuTranscodeTarget> s_GpuTranscodeTarget{TextureGpuTranscodeTarget::RGBA32};
}

void TextureAsset::SetGpuTranscodeTarget(TextureGpuTranscodeTarget target) {
    s_GpuTranscodeTarget.store(target, std::memory_order_relaxed);
}

TextureGpuTranscodeTarget TextureAsset::GetGpuTranscodeTarget() {
    return s_GpuTranscodeTarget.load(std::memory_order_relaxed);
}

#if defined(GE_HAVE_KTX)
bool TextureAsset::LoadWithKTX() {
    // Read through the shared-open path (never lets libktx fopen the file
    // directly — see AssetCore/SharedFileRead.h), then decode from memory.
    Vector<uint8> bytes;
    if (!ReadFileBytesShared(GetPath(), bytes)) {
        Logger::Log::Error("Failed to read KTX file '{}'", GetPath().string());
        return false;
    }
    return LoadWithKTXFromData(bytes);
}

bool TextureAsset::LoadWithKTXFromData(const Vector<uint8>& data) {
    if (data.empty()) {
        Logger::Log::Error("TextureAsset KTX loader: empty data for '{}'", GetName());
        return false;
    }

    ktxTexture2* ktexture = nullptr;
    KTX_error_code kerr = ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(data.data()),
        static_cast<ktx_size_t>(data.size()),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &ktexture);
    if (kerr != KTX_SUCCESS || !ktexture) {
        Logger::Log::Error("Failed to create ktxTexture2 from memory for '{}': {}", GetName(), ktxErrorString(kerr));
        return false;
    }

    const bool ok = PopulateFromKtxTexture(ktexture, GetName().c_str());
    ktxTexture2_Destroy(ktexture);
    return ok;
}

bool TextureAsset::PopulateFromKtxTexture(void* ktxTexture2Ptr, const char* sourceName) {
    auto* ktexture = static_cast<ktxTexture2*>(ktxTexture2Ptr);
    ktxTexture* base = ktxTexture(ktexture);

    // We only support 2D, non-array, non-cubemap textures at the asset level.
    if (base->numDimensions != 2 || base->numFaces != 1 || base->numLayers != 1) {
        Logger::Log::Error("TextureAsset KTX loader only supports 2D, non-array textures for now: '{}'", sourceName);
        return false;
    }

    // BasisU/UASTC payloads transcode to the GPU-appropriate target: BC7 when
    // the renderer reported block-compression support, RGBA32 otherwise.
    // Non-transcoding containers may carry raw BCn payloads directly (the
    // import cook writes those); map their vkFormat to the asset format.
    bool isBlockCompressed = false;
    TextureFormat blockFormat = TextureFormat::Unknown;
    uint32 blockChannels = 0;
    if (ktxTexture2_NeedsTranscoding(ktexture)) {
        const bool wantBC7 = GetGpuTranscodeTarget() == TextureGpuTranscodeTarget::BC7;
        const ktx_transcode_fmt_e target = wantBC7 ? KTX_TTF_BC7_RGBA : KTX_TTF_RGBA32;
        const KTX_error_code kerr = ktxTexture2_TranscodeBasis(ktexture, target, KTX_TF_HIGH_QUALITY);
        if (kerr != KTX_SUCCESS) {
            Logger::Log::Error("Failed to transcode Basis KTX2 texture '{}' (target {}): {}",
                               sourceName, wantBC7 ? "BC7" : "RGBA32", ktxErrorString(kerr));
            return false;
        }
        if (wantBC7) {
            isBlockCompressed = true;
            blockFormat = TextureFormat::BC7;
            blockChannels = 4;
        }
    } else {
        switch (ktexture->vkFormat) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            blockFormat = TextureFormat::BC1; blockChannels = 4; break;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            blockFormat = TextureFormat::BC4; blockChannels = 1; break;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            blockFormat = TextureFormat::BC5; blockChannels = 2; break;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            blockFormat = TextureFormat::BC6H; blockChannels = 3; break;
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            blockFormat = TextureFormat::BC7; blockChannels = 4; break;
        default:
            break;
        }
        isBlockCompressed = blockFormat != TextureFormat::Unknown;
    }

    const ktx_uint8_t* srcData = ktxTexture_GetData(base);
    const ktx_size_t totalDataSize = ktxTexture_GetDataSize(base);
    if (!srcData || totalDataSize == 0) {
        Logger::Log::Error("TextureAsset KTX loader: no image data for '{}'", sourceName);
        return false;
    }

    if (isBlockCompressed) {
        m_Channels = blockChannels;
        m_Format = blockFormat;
    } else {
        uint32_t numComponents = 0;
        uint32_t componentByteLength = 0;
        ktxTexture2_GetComponentInfo(ktexture, &numComponents, &componentByteLength);
        if (numComponents == 0 || componentByteLength == 0) {
            Logger::Log::Error("TextureAsset KTX loader: invalid component info for '{}'", sourceName);
            return false;
        }
        // DetermineFormat only distinguishes 8-bit UNORM from 32-bit float.
        // A 16-bit container (half / UNORM16) would be mislabeled R*32F and
        // downstream float readers would over-read a half-sized buffer.
        if (componentByteLength != 1 && componentByteLength != 4) {
            Logger::Log::Error(
                "TextureAsset KTX loader: unsupported component size {} bytes for '{}' (expected 8-bit or 32-bit float)",
                componentByteLength, sourceName);
            return false;
        }
        m_Channels = numComponents;
        const bool isHDR = (componentByteLength > 1);
        m_Format = DetermineFormat(static_cast<int>(m_Channels), isHDR);
    }

    m_Width = static_cast<uint32>(base->baseWidth);
    m_Height = static_cast<uint32>(base->baseHeight);

    // Adopt the container's full mip chain. libktx keeps levels in KTX2 file
    // order (smallest level first), so repack level-0-first: consumers rely on
    // GetPixelData() addressing mip 0 at byte 0.
    const uint32 numLevels = base->numLevels > 0 ? static_cast<uint32>(base->numLevels) : 1u;
    m_MipChain.clear();
    m_MipChain.reserve(numLevels);

    uint64 packedSize = 0;
    for (uint32 level = 0; level < numLevels; ++level) {
        ktx_size_t levelOffset = 0;
        if (ktxTexture_GetImageOffset(base, level, 0, 0, &levelOffset) != KTX_SUCCESS) {
            Logger::Log::Error("TextureAsset KTX loader: bad mip offset (level {}) for '{}'", level, sourceName);
            return false;
        }
        const ktx_size_t levelSize = ktxTexture_GetImageSize(base, level);
        if (levelSize == 0 || levelOffset + levelSize > totalDataSize) {
            Logger::Log::Error("TextureAsset KTX loader: mip {} range [{} + {}] exceeds data size {} for '{}'",
                               level, levelOffset, levelSize, totalDataSize, sourceName);
            return false;
        }
        TextureMipDesc desc;
        desc.Offset = packedSize; // destination offset in the repacked buffer
        desc.Size = static_cast<uint64>(levelSize);
        desc.Width = std::max(m_Width >> level, 1u);
        desc.Height = std::max(m_Height >> level, 1u);
        m_MipChain.push_back(desc);
        packedSize += desc.Size;
    }

    m_DataSize = packedSize;
    m_PixelData = new uint8[m_DataSize];
    for (uint32 level = 0; level < numLevels; ++level) {
        ktx_size_t levelOffset = 0;
        ktxTexture_GetImageOffset(base, level, 0, 0, &levelOffset);
        std::memcpy(m_PixelData + m_MipChain[level].Offset, srcData + levelOffset,
                    static_cast<size_t>(m_MipChain[level].Size));
    }

    m_HasMipmaps = numLevels > 1;
    m_MipmapLevels = numLevels;

    // The KTX2 DFD transfer function is authoritative for the container. The
    // per-asset AssetDatabase meta override (assets.texture.colorSpace) is
    // still applied on top at upload time.
    m_ColorSpace = (ktxTexture2_GetOETF_e(ktexture) == KHR_DF_TRANSFER_SRGB)
        ? TextureColorSpace::SRGB
        : TextureColorSpace::Linear;

    return true;
}
#endif

bool TextureAsset::LoadFromSvgData(const std::string& svgText, const String& extensionLower)
{
    AssetManager* assetManager = AssetManager::GetThreadCurrent();
    if (!assetManager && EngineCore::GetInstance().IsInitialized())
        assetManager = &EngineCore::GetInstance().GetAssetManager();

    std::filesystem::path assetPath = GetPath();
    bool isEditorUiAsset = false;
    if (assetManager)
    {
        auto& registry = assetManager->GetRegistry();
        AssetMetadata registryMeta{};
        if (!GetGUID().IsNull() && registry.TryGetAssetMetadata(GetGUID(), registryMeta) &&
            !registryMeta.Path.empty())
        {
            assetPath = registryMeta.Path;
        }
        isEditorUiAsset = assetManager->GetSourceAliasForPath(assetPath) == kAssetSourceAliasEditor;
    }

    const float32 defaultSize = isEditorUiAsset
        ? GetSvgRasterizerUserScale()
        : GetSvgTextureRasterizerDefaultSize();
    uint32 targetPixels = static_cast<uint32>(defaultSize + 0.5f);
    if (assetManager)
    {
        std::string metaValue;
        if (assetManager->GetRegistry().TryGetMetaValue(assetPath, kSvgRasterSizeMetaKey, metaValue))
            ParseSvgRasterSizeMeta(metaValue, targetPixels);
    }

    SvgRasterizedImage img{};
    if (!RasterizeSvgToRgbaAtSize(svgText, static_cast<float32>(targetPixels), img) ||
        img.Width == 0 || img.Height == 0 || !img.Data)
    {
        Logger::Log::Error("TextureAsset: failed to rasterize SVG for '{}'", GetName());
        return false;
    }

    m_Width = img.Width;
    m_Height = img.Height;
    m_Channels = 4;
    m_Format = TextureFormat::RGBA8;
    m_ColorSpace = GuessTextureColorSpace(extensionLower);
    m_DataSize = static_cast<uint64>(img.DataSize);
    m_PixelData = img.Data.release();
    return true;
}

TextureFormat TextureAsset::DetermineFormat(int channels, bool isHDR) const {
    if (isHDR) {
        switch (channels) {
            case 1: return TextureFormat::R32F;
            case 2: return TextureFormat::RG32F;
            case 3: return TextureFormat::RGB32F;
            case 4: return TextureFormat::RGBA32F;
            default: return TextureFormat::Unknown;
        }
    } else {
        switch (channels) {
            case 1: return TextureFormat::R8;
            case 2: return TextureFormat::RG8;
            case 3: return TextureFormat::RGB8;
            case 4: return TextureFormat::RGBA8;
            default: return TextureFormat::Unknown;
        }
    }
}

} // namespace GameEngine
