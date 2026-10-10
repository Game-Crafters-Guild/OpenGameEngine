#include "Assets/FbxLoaderOptions.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "Types/Fnv1a.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace GameEngine {

namespace {

// kFbxImportGeometryVersion is the geometry-format salt, declared in the header
// (FbxLoaderOptions.h) so caches beyond this TU — notably the HLOD proxy cache —
// fold the same value.

bool ReadFbxMetaBool(const std::filesystem::path& assetPath, const char* key, bool fallback) {
    if (assetPath.empty())
        return fallback;
    // Loading a model outside a fully-initialized engine (a bare model-import test
    // harness) leaves no asset manager to carry per-asset overrides; use the default.
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return fallback;
    std::string value;
    if (!assetManager->GetRegistry().TryGetMetaValue(assetPath, key, value))
        return fallback;
    return !(value == "0" || value == "false" || value == "False");
}

void ReadFbxMetaBakeRotation(const std::filesystem::path& assetPath, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (assetPath.empty())
        return;
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;
    std::string value;
    if (!assetManager->GetRegistry().TryGetMetaValue(
            assetPath, FbxLoaderOptions::kBakeRotationKey, value))
        return;
    float parsed[3] = {};
    int n = 0;
    const char* p = value.c_str();
    while (n < 3 && *p)
    {
        char* end = nullptr;
        parsed[n] = std::strtof(p, &end);
        if (end == p)
            break;
        ++n;
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t')
            ++p;
    }
    if (n == 3)
    {
        out[0] = parsed[0];
        out[1] = parsed[1];
        out[2] = parsed[2];
    }
}

void ReadFbxMetaCsvBool3(const std::filesystem::path& assetPath, const char* key, bool out[3],
                         bool missing0, bool missing1, bool missing2)
{
    out[0] = missing0;
    out[1] = missing1;
    out[2] = missing2;
    if (assetPath.empty())
        return;
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;
    std::string value;
    if (!assetManager->GetRegistry().TryGetMetaValue(assetPath, key, value))
        return;
    int n = 0;
    const char* p = value.c_str();
    while (n < 3 && *p)
    {
        while (*p == ',' || *p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        out[n] = !(*p == '0' || *p == 'f' || *p == 'F');
        ++n;
        while (*p && *p != ',')
            ++p;
    }
}

void ReadFbxMetaBakeRotationAxes(const std::filesystem::path& assetPath, bool out[3])
{
    // No bake kv: axes off. Bake kv without an axes mask: all on (legacy).
    out[0] = out[1] = out[2] = false;
    if (assetPath.empty())
        return;
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;
    auto& registry = assetManager->GetRegistry();
    std::string bakeValue;
    const bool hasBake = registry.TryGetMetaValue(assetPath, FbxLoaderOptions::kBakeRotationKey, bakeValue);
    std::string axesValue;
    if (!registry.TryGetMetaValue(assetPath, FbxLoaderOptions::kBakeRotationAxesKey, axesValue))
    {
        if (hasBake)
            out[0] = out[1] = out[2] = true;
        return;
    }
    int n = 0;
    const char* p = axesValue.c_str();
    while (n < 3 && *p)
    {
        while (*p == ',' || *p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        out[n] = !(*p == '0' || *p == 'f' || *p == 'F');
        ++n;
        while (*p && *p != ',')
            ++p;
    }
}

void ReadProjectFbxBool(const nlohmann::json& doc, const char* key, bool& value) {
    const auto it = doc.find(key);
    if (it != doc.end() && it->is_boolean())
        value = it->get<bool>();
}

FbxLoaderOptions ParseProjectFbxLoaderOptions(const std::filesystem::path& settingsPath) {
    FbxLoaderOptions options{};
    std::ifstream in(settingsPath, std::ios::binary | std::ios::in);
    if (!in.is_open())
        return options;

    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_object())
        return options;

    ReadProjectFbxBool(doc, FbxLoaderOptions::kGenerateMissingTangentsKey, options.GenerateMissingTangents);
    ReadProjectFbxBool(doc, FbxLoaderOptions::kCleanSkinWeightsKey, options.CleanSkinWeights);
    ReadProjectFbxBool(doc, FbxLoaderOptions::kAdjustPivotsKey, options.AdjustPivots);
    ReadProjectFbxBool(doc, FbxLoaderOptions::kPreserveGeometryTransformsKey, options.PreserveGeometryTransforms);
    ReadProjectFbxBool(doc, FbxLoaderOptions::kImportEmbeddedTexturesKey, options.ImportEmbeddedTextures);
    return options;
}

// ResolveFbxLoaderOptions runs once per model resolve — model loads, HLOD
// proxy bakes, reimports — so an open+parse of ProjectSettings.json per call
// adds up. Cache the parsed options keyed on the settings file's stat
// fingerprint: a stat per resolve is cheap, and mtime/size revalidation
// invalidates on ANY writer (settings panel save, git checkout, hand edit)
// without needing a cross-module change-notification hookup. There is no
// engine-visible ProjectSettings change broadcast today.
FbxLoaderOptions LoadProjectFbxLoaderOptions() {
    const std::filesystem::path workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return FbxLoaderOptions{};

    const std::filesystem::path settingsPath =
        (workspaceRoot / ".Editor" / "ProjectSettings.json").lexically_normal();

    // A missing file stats with both sentinels distinct from any real file.
    constexpr int64_t kMissingStamp = -1;
    int64_t mtime = kMissingStamp;
    int64_t size = kMissingStamp;
    std::error_code ec;
    if (const auto writeTime = std::filesystem::last_write_time(settingsPath, ec); !ec)
        mtime = writeTime.time_since_epoch().count();
    ec.clear();
    if (const auto fileSize = std::filesystem::file_size(settingsPath, ec); !ec)
        size = static_cast<int64_t>(fileSize);

    static std::mutex s_CacheMutex; // resolves run concurrently on loader workers
    static std::filesystem::path s_CachedPath;
    static int64_t s_CachedMtime = 0;
    static int64_t s_CachedSize = 0;
    static bool s_CachePrimed = false;
    static FbxLoaderOptions s_CachedOptions{};

    {
        std::lock_guard<std::mutex> lock(s_CacheMutex);
        if (s_CachePrimed && s_CachedPath == settingsPath &&
            s_CachedMtime == mtime && s_CachedSize == size)
        {
            return s_CachedOptions;
        }
    }

    const FbxLoaderOptions options = ParseProjectFbxLoaderOptions(settingsPath);

    {
        std::lock_guard<std::mutex> lock(s_CacheMutex);
        s_CachedPath = settingsPath;
        s_CachedMtime = mtime;
        s_CachedSize = size;
        s_CachedOptions = options;
        s_CachePrimed = true;
    }
    return options;
}

} // namespace

FbxLoaderOptions ResolveFbxLoaderOptions(const std::filesystem::path& assetPath) {
    FbxLoaderOptions options = LoadProjectFbxLoaderOptions();

    const bool useGlobalSettings =
        ReadFbxMetaBool(assetPath, FbxLoaderOptions::kUseGlobalKey, true);
    if (!useGlobalSettings) {
        options.GenerateMissingTangents = ReadFbxMetaBool(
            assetPath, FbxLoaderOptions::kGenerateMissingTangentsKey, options.GenerateMissingTangents);
        options.CleanSkinWeights = ReadFbxMetaBool(
            assetPath, FbxLoaderOptions::kCleanSkinWeightsKey, options.CleanSkinWeights);
        options.AdjustPivots = ReadFbxMetaBool(
            assetPath, FbxLoaderOptions::kAdjustPivotsKey, options.AdjustPivots);
        options.PreserveGeometryTransforms = ReadFbxMetaBool(
            assetPath, FbxLoaderOptions::kPreserveGeometryTransformsKey, options.PreserveGeometryTransforms);
        options.ImportEmbeddedTextures = ReadFbxMetaBool(
            assetPath, FbxLoaderOptions::kImportEmbeddedTexturesKey, options.ImportEmbeddedTextures);
    }
    // Bake rotation is always per-asset. A project-wide 180° yaw would
    // invert every FBX, including ones that already face +Z.
    ReadFbxMetaBakeRotation(assetPath, options.Axis.BakeRotationDeg);
    ReadFbxMetaBakeRotationAxes(assetPath, options.Axis.BakeRotationAxisEnabled);
    EnsureFbxMirrorAxesMeta(assetPath);
    ReadFbxMetaCsvBool3(assetPath, FbxLoaderOptions::kMirrorAxesKey,
                        options.Axis.MirrorAxis, true, false, false);
    return options;
}

void EnsureFbxMirrorAxesMeta(const std::filesystem::path& assetPath)
{
    if (assetPath.empty())
        return;
    if (!ModelAsset::UsesAxisImportOptions(ModelAsset::FormatFromPath(assetPath)))
        return;
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;
    auto& registry = assetManager->GetRegistry();
    std::string existing;
    if (registry.TryGetMetaValue(assetPath, FbxLoaderOptions::kMirrorAxesKey, existing))
        return;
    registry.SetMetaValue(assetPath, FbxLoaderOptions::kMirrorAxesKey, "1,0,0");
}

uint64 HashFbxLoaderOptions(const FbxLoaderOptions& options) {
    uint64 h = Hashing::kFnv1a64OffsetBasis;
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.GenerateMissingTangents));
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.CleanSkinWeights));
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.AdjustPivots));
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.PreserveGeometryTransforms));
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.ImportEmbeddedTextures));
    float bake[3];
    options.Axis.EffectiveBakeRotationDeg(bake);
    for (int i = 0; i < 3; ++i)
    {
        h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.Axis.BakeRotationAxisEnabled[i]));
        uint32 bits = 0;
        std::memcpy(&bits, &bake[i], sizeof(bits));
        h = Hashing::Fnv1a64Value(h, bits);
    }
    for (int i = 0; i < 3; ++i)
        h = Hashing::Fnv1a64Value(h, static_cast<uint8>(options.Axis.MirrorAxis[i]));
    h = Hashing::Fnv1a64Value(h, kFbxImportGeometryVersion);
    // Guarantee non-zero so the caller can use 0 as the "no parse options"
    // sentinel; only the (astronomically unlikely) hash-to-zero case is remapped.
    return h ? h : 0x9E3779B97F4A7C15ull;
}

} // namespace GameEngine
