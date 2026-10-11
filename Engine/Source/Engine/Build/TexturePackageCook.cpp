#include "Engine/Build/TexturePackageCook.h"

#include "AssetCore/SharedFileRead.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/PackagedTexturePayload.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <unordered_set>

namespace GameEngine
{
namespace fs = std::filesystem;
namespace
{
bool ValidArtifact(const Vector<uint8>& bytes, TextureCookOutput output,
                   const TextureCookInputs& inputs, bool hdr)
{
    if (bytes.empty())
        return false;
    TextureAsset texture(GUID::Null(), "cooked-validation.ktx2");
    if (!texture.LoadFromData(bytes) || !texture.GetWidth() || !texture.GetHeight())
        return false;
    // Indexed by TextureCookOutput: a new output must add its format here, or
    // validation would read past the table and accept whatever it found.
    const TextureFormat expected[] = {hdr ? TextureFormat::RGBA32F : TextureFormat::RGBA8,
        TextureFormat::BC1, TextureFormat::BC4, TextureFormat::BC5, TextureFormat::BC6H, TextureFormat::BC7};
    static_assert(std::size(expected) == static_cast<size_t>(TextureCookOutput::BC7) + 1,
                  "TextureCookOutput and the expected-format table must stay in step");
    const bool supportsSrgb = !hdr && (output == TextureCookOutput::Uncompressed ||
        output == TextureCookOutput::BC1 || output == TextureCookOutput::BC7);
    const auto expectedColorSpace = supportsSrgb ? inputs.ColorSpace : TextureColorSpace::Linear;
    return texture.GetFormat() == expected[static_cast<size_t>(output)] &&
           texture.GetColorSpace() == expectedColorSpace &&
           texture.GetMipmapLevels() == ComputeTextureCookMipCount(
               texture.GetWidth(), texture.GetHeight(), inputs.Settings);
}

bool PublishArtifact(const fs::path& destination, const Vector<uint8>& bytes, std::string& error)
{
    static std::atomic<uint64> sequence{0};
    fs::create_directories(destination.parent_path());
    fs::path temporary = destination;
    temporary += ".tmp." + std::to_string(sequence.fetch_add(1));
    struct Cleanup
    {
        fs::path Path;
        ~Cleanup() { std::error_code ec; fs::remove(Path, ec); }
    } cleanup{temporary};
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        {
            error = "Could not write cooked texture: " + temporary.string();
            return false;
        }
        stream.close();
        if (stream.fail())
        {
            error = "Could not finish cooked texture: " + temporary.string();
            return false;
        }
    }
    if (!FileSystem::PublishFile(temporary, destination))
    {
        error = "Could not publish cooked texture: " + destination.string();
        return false;
    }
    return true;
}

// One usage for every slot that binds a texture, one bit per usage. An unknown slot may read the
// texture any way, so it stays Auto (uncompressed); otherwise the slots widen to one usage
// (WidenTextureCookUsage), and a pair no single cook serves stays Auto too.
TextureCookUsage WidenBoundUsages(unsigned boundUsages)
{
    if (boundUsages & (1u << static_cast<unsigned>(TextureCookUsage::Auto)))
        return TextureCookUsage::Auto;
    std::optional<TextureCookUsage> usage = TextureCookUsage::Auto;
    for (const auto candidate : {TextureCookUsage::Color, TextureCookUsage::Normal, TextureCookUsage::Mask,
                                 TextureCookUsage::Packed})
        if (usage && (boundUsages & (1u << static_cast<unsigned>(candidate))))
            usage = WidenTextureCookUsage(*usage, candidate);
    return usage.value_or(TextureCookUsage::Auto);
}
}

std::unordered_map<GUID, TextureCookUsage> CollectPackagedTextureUsages(
    const fs::path& contentRoot, const AssetManifest& manifest, const AssetManager& manager)
{
    std::unordered_map<GUID, unsigned> bindings;
    std::unordered_map<GUID, const AssetManifestEntry*> textures;
    for (const auto& entry : manifest.entries)
        if (entry.type == AssetType::Texture && !entry.guid.IsNull()) textures.emplace(entry.guid, &entry);
    const auto findTexture = [&textures](const GUID& guid) -> const AssetManifestEntry* {
        const auto found = textures.find(guid);
        return found == textures.end() ? nullptr : found->second;
    };
    for (const auto& entry : manifest.entries)
    {
        if (entry.type != AssetType::Material) continue;
        // Parse the copied bytes with authoring path context. MaterialX resolves
        // relative image paths here; a null load context keeps its parser from
        // registering assets in the caller's registry as a side effect.
        MaterialAsset material(entry.guid, entry.sourcePath);
        Vector<uint8> bytes;
        bool loaded = false;
        if (ReadFileBytesShared(contentRoot / entry.outputPath, bytes))
        {
            AssetManager::ScopedThreadAssetManager readOnlyParse(nullptr);
            loaded = material.LoadFromData(bytes);
        }
        if (!loaded)
        {
            Logger::Log::Warning("Build: Cannot classify texture bindings from material '{}'", entry.outputPath.string());
            continue;
        }
        for (const auto& [slot, reference] : material.GetDocument().textures)
        {
            if (reference.empty() || reference.starts_with("__embedded:")) continue;
            const auto& registry = manager.GetRegistry();
            const GUID parsed(reference);
            GUID guid = registry.ResolveGuid(parsed);
            auto* texture = guid.IsNull() ? nullptr : findTexture(guid);
            // V2 path refs and V3 stale-GUID path companions use the existing
            // source-aware resolver, without registering or rewriting assets.
            const auto path = material.GetDocument().texturePaths.find(slot);
            AssetMetadata metadata;
            const bool knownGuid = !guid.IsNull() && registry.TryGetAssetMetadata(guid, metadata) && !metadata.Path.empty();
            if (!texture && !knownGuid && (parsed.IsNull() || path != material.GetDocument().texturePaths.end()))
            {
                guid = registry.GetAssetGUID(manager.ResolveAssetPathFromReference(
                    parsed.IsNull() ? reference : path->second, entry.sourcePath));
                texture = guid.IsNull() ? nullptr : findTexture(guid);
            }
            if (!texture || texture->type != AssetType::Texture)
            {
                Logger::Log::Warning("Build: Unresolved texture binding '{}' in material '{}' slot '{}'", reference,
                                     entry.outputPath.string(), slot);
                continue;
            }
            bindings[guid] |= 1u << static_cast<unsigned>(TextureCookUsageForMaterialSlot(HashStringId(slot)));
        }
    }
    std::unordered_map<GUID, TextureCookUsage> usages;
    for (const auto& [guid, mask] : bindings)
        usages[guid] = WidenBoundUsages(mask);
    return usages;
}

void StageTextureImportMetadata(const AssetRegistry& registry, const AssetManifestEntry& entry,
                                TextureCookUsage materialUsage, AssetDatabase::AssetRecord& record)
{
    std::string value;
    for (const auto& key : kRuntimeAssetMetadataKeys)
        if (key.Type == AssetType::Texture && registry.TryGetMetaValue(entry.sourcePath, key.Name, value))
            record.kv[key.Name] = value;
    // An authored usage wins over the slots' unless a slot needs it wider: the stored tag does
    // not record whether a person or a material bind wrote it, and a narrower cook drops channels
    // the slot reads.
    TextureCookUsage authored = TextureCookUsage::Auto;
    const auto existing = record.kv.find(kTextureUsageMetaKey);
    if (existing != record.kv.end() && ParseTextureCookUsageMeta(existing->second, authored))
        materialUsage = WidenTextureCookUsage(authored, materialUsage).value_or(authored);
    if (materialUsage != TextureCookUsage::Auto)
        record.kv[kTextureUsageMetaKey] = TextureCookUsageMetaValue(materialUsage);
    TextureColorSpace authoredColor;
    const auto color = record.kv.find(kTextureColorSpaceMetaKey);
    const bool explicitColor = color != record.kv.end() && ParseTextureColorSpaceMeta(color->second, authoredColor);
    if (!explicitColor && IsLinearTextureUsage(materialUsage))
        record.kv[kTextureColorSpaceMetaKey] = TextureColorSpaceMetaValue(TextureColorSpace::Linear);
}

bool StagePackagedTextureCooks(const fs::path& contentRoot,
                              const AssetManifest& manifest,
                              const AssetRegistry& sourceRegistry,
                              TextureCookEncodeQuality targetBc7Quality,
                              TextureCookWorkers* encodeWorkers,
                              JobSystem::WorkStealingThreadPool* manifestParsePool,
                              const std::function<bool()>& cancelRequested,
                              TexturePackageCookStats& stats,
                              std::string& error)
{
    stats = {};
    error.clear();
    const auto cancelled = [&] { return cancelRequested && cancelRequested(); };
    std::map<fs::path, std::unique_ptr<AssetDatabase::AssetStore_TextJsonl>> stores;
    std::unordered_set<std::string> staged;
    std::vector<fs::path> replacedSources;
    try
    {
        for (const auto& entry : manifest.entries)
        {
            if (cancelled()) { error = kTextureCookCancelledError; return false; }
            std::string extension = entry.outputPath.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!IsTextureCookSourceExtension(extension))
                continue; // DDS/KTX/SVG retain their existing native loading paths.
            if (entry.type != AssetType::Texture || entry.guid.IsNull())
            {
                ++stats.SkippedTextures;
                Logger::Log::Warning("Build: Texture '{}' has no registered texture identity; shipping raw pixels without a prebuilt cook",
                                     entry.outputPath.string());
                continue;
            }

            const bool package = !entry.sourceAlias.empty() &&
                entry.sourceAlias != kAssetSourceAliasProject && entry.sourceAlias != kAssetSourceAliasEditor;
            const fs::path mountRoot = package ? contentRoot / "Packages" / entry.sourceAlias : contentRoot;
            const fs::path assetRoot = mountRoot / "Assets";
            auto& store = stores[mountRoot];
            if (!store)
            {
                store = std::make_unique<AssetDatabase::AssetStore_TextJsonl>(manifestParsePool);
                if (!store->LoadFromFile(assetRoot / ".assetmanifest", &error))
                    return false;
            }
            AssetDatabase::AssetRecord record;
            if (!store->TryGetAsset(entry.guid, record))
            {
                error = "Texture missing from staged asset manifest: " + entry.outputPath.string();
                return false;
            }
            const fs::path source = (assetRoot / record.path).lexically_normal();
            if (source != (contentRoot / entry.outputPath).lexically_normal())
            {
                error = "Texture manifest path disagrees with staged asset: " + entry.outputPath.string();
                return false;
            }
            TextureCookInputs inputs;
            if (!ResolveTextureCookInputs(extension,
                [&record](const char* key, std::string& value) {
                    const auto found = record.kv.find(key);
                    if (found == record.kv.end()) return false;
                    value = found->second;
                    return true;
                }, inputs, error))
            {
                error = "Invalid texture import policy for '" + entry.outputPath.string() + "': " + error;
                return false;
            }
            // No material binds an unclassified texture and no author classified it, so its
            // consumers include readers that open the source by path rather than through
            // TextureAsset: a terrain heightmap (TerrainService::ResolveHeightmapAsset reads
            // 16-bit samples the RGBA8 payload cannot hold). Its source ships beside the
            // payload.
            const bool unclassified = inputs.Settings.Usage == TextureCookUsage::Auto &&
                inputs.Settings.Compression == TextureCookCompression::Auto && extension != ".hdr";
            if (unclassified)
            {
                ++stats.UnclassifiedTextures;
                Logger::Log::Warning("Build: Texture '{}' has unresolved or conflicting material usage; Auto keeps uncompressed pixels. Set per-texture Usage or Compression to override",
                                     entry.outputPath.string());
            }
            const bool hdr = extension == ".hdr";
            const std::array outputs = {
                ResolveTextureCookOutput(inputs.Settings, hdr, true, IsTextureCookEncoderAvailable()),
                ResolveTextureCookOutput(inputs.Settings, hdr, false, IsTextureCookEncoderAvailable())};
            if (!NeedsTextureCook(inputs, outputs[0]) && !NeedsTextureCook(inputs, outputs[1]))
                continue;
            Vector<uint8> sourceBytes;
            if (!ReadFileBytesShared(source, sourceBytes) || sourceBytes.empty())
            {
                error = "Could not read staged texture: " + source.string();
                return false;
            }
            const uint64 sourceHash = ComputeTextureCookSourceHash(sourceBytes.data(), sourceBytes.size());
            const auto cacheRoot = sourceRegistry.TryGetCacheRoot(entry.guid);
            ++stats.Textures;
            for (const auto output : outputs)
            {
                if (cancelled()) { error = kTextureCookCancelledError; return false; }
                const auto filename = TextureCookArtifactName(entry.guid, sourceHash, inputs, output, targetBc7Quality);
                record.kv[output == TextureCookOutput::Uncompressed ? kTexturePackagedPortableMetaKey :
                    kTexturePackagedCompressedMetaKey] = filename;
                // The artifact publishes into the staged mount's own Tex/, which is
                // where the packaged game's read-only mount of that content looks
                // (its cache root defaults to the content itself). The reuse probe
                // below reads the SOURCE mount's derived cache instead
                // (TryGetCacheRoot: the project's .Cache, or the host's
                // Packages/<alias> cache for a git or engine package), where the
                // editor's own cooks land. Creation permission must not choose a
                // path.
                const auto destination = mountRoot / AssetDatabase::kTextureCacheDirectoryName / filename;
                if (!staged.insert(destination.generic_string()).second)
                    continue;
                Vector<uint8> cooked;
                bool reused = ReadFileBytesShared(destination, cooked) && ValidArtifact(cooked, output, inputs, hdr);
                if (!reused && cacheRoot)
                    reused = ReadFileBytesShared(*cacheRoot / AssetDatabase::kTextureCacheDirectoryName / filename, cooked) &&
                             ValidArtifact(cooked, output, inputs, hdr);
                if (!reused)
                {
                    Logger::Log::Info("Build: Baking texture '{}' ({})", entry.outputPath.string(), filename);
                    std::vector<uint8> encoded;
                    if (!CookTexture(sourceBytes.data(), sourceBytes.size(), extension,
                                     inputs, output, encoded, error, cancelRequested, targetBc7Quality,
                                     encodeWorkers))
                    {
                        if (error != kTextureCookCancelledError)
                            error = "Texture bake failed for '" + entry.outputPath.string() + "': " + error;
                        return false;
                    }
                    cooked.assign(encoded.begin(), encoded.end());
                    if (!ValidArtifact(cooked, output, inputs, hdr))
                    {
                        error = "Cooked texture failed validation: " + entry.outputPath.string();
                        return false;
                    }
                }
                if (cancelled()) { error = kTextureCookCancelledError; return false; }
                if (!PublishArtifact(destination, cooked, error))
                    return false;
                ++stats.Artifacts;
                stats.ReusedArtifacts += reused ? 1 : 0;
                stats.Bytes += cooked.size();
            }
            if (!store->UpsertAsset(record, &error))
                return false;
            if (!unclassified)
                replacedSources.push_back(source);
        }
        if (cancelled()) { error = kTextureCookCancelledError; return false; }
        for (const auto& [mountRoot, store] : stores)
            if (!store->SaveToFile(mountRoot / "Assets" / ".assetmanifest", &error))
                return false;
        for (const auto& source : replacedSources)
            fs::remove(source);
        return true;
    }
    catch (const fs::filesystem_error& e)
    {
        error = "Texture package staging failed: " + std::string(e.what());
        return false;
    }
}
} // namespace GameEngine
