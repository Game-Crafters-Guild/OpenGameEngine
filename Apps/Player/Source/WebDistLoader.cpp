#include "WebDistLoader.h"

#include "WebPackReader.h"

#include "Logger/Logger.h"
#include "Types/Fnv1a.h"

#include <emscripten/wget.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace GameEngine::WebPlayer
{
namespace
{

// Written by Tools/Web/export_web_player.py, which documents the schema. The
// player reads exactly this version: an older dist served to a newer player is
// a re-export, not something to guess at.
constexpr const char kManifestName[] = "player-manifest.json";
constexpr int kManifestSchemaVersion = 1;

// emscripten_wget_data hands back a malloc'd buffer the caller owns.
struct MallocDeleter
{
    void operator()(void* pointer) const { std::free(pointer); }
};
using FetchedBytes = std::unique_ptr<std::uint8_t[], MallocDeleter>;

// Fetch a URL relative to the page. The dist is self-contained and deployed as
// one directory, so every file it names sits beside the manifest.
bool Fetch(const std::string& url, FetchedBytes& outData, std::size_t& outSize,
           std::string& outError)
{
    void* buffer = nullptr;
    int size = 0;
    int error = 0;
    emscripten_wget_data(url.c_str(), &buffer, &size, &error);
    if (error != 0 || buffer == nullptr)
    {
        outError = "cannot fetch '" + url + "' — is the dist complete and served from this "
                                            "directory?";
        return false;
    }
    outData.reset(static_cast<std::uint8_t*>(buffer));
    outSize = static_cast<std::size_t>(size);
    return true;
}

bool LoadPack(const nlohmann::json& pack, const std::filesystem::path& root, std::string& outError)
{
    const auto file = pack.value("file", std::string{});
    const auto role = pack.value("role", std::string{"?"});
    const auto declaredBytes = pack.value("bytes", std::uint64_t{0});
    const auto declaredEntries = pack.value("entries", std::uint64_t{0});
    const auto declaredHash = pack.value("fnv1a64", std::string{});
    if (file.empty())
    {
        outError = "manifest lists a pack with no file name";
        return false;
    }

    FetchedBytes data;
    std::size_t size = 0;
    if (!Fetch(file, data, size, outError))
        return false;

    // Size and hash together catch the two ways a dist goes stale in practice:
    // a truncated transfer, and a cached pack from a previous export.
    if (size != declaredBytes)
    {
        outError = file + ": manifest declares " + std::to_string(declaredBytes) + " bytes but " +
                   std::to_string(size) + " arrived";
        return false;
    }
    char hash[19] = {};
    std::snprintf(hash, sizeof(hash), "0x%016llx",
                  static_cast<unsigned long long>(Hashing::Fnv1a64(data.get(), size)));
    if (!declaredHash.empty() && declaredHash != hash)
    {
        outError = file + ": content hash " + hash + " does not match the manifest's " +
                   declaredHash + " — the dist and the manifest are from different exports";
        return false;
    }

    const PackUnpackResult unpacked = UnpackWebPack({data.get(), size}, root);
    if (!unpacked.Success)
    {
        outError = file + ": " + unpacked.Error;
        return false;
    }
    if (unpacked.FileCount != declaredEntries)
    {
        outError = file + ": unpacked " + std::to_string(unpacked.FileCount) + " files but the " +
                   "manifest declares " + std::to_string(declaredEntries);
        return false;
    }

    Logger::Log::Info("WebDist: {} pack '{}' -> {} files ({} bytes)", role, file,
                      unpacked.FileCount, size);
    return true;
}

} // namespace

bool LoadWebDist(const std::filesystem::path& root, WebDistInfo& outInfo, std::string& outError)
{
    FetchedBytes manifestBytes;
    std::size_t manifestSize = 0;
    if (!Fetch(kManifestName, manifestBytes, manifestSize, outError))
        return false;

    nlohmann::json manifest =
        nlohmann::json::parse(std::string_view(reinterpret_cast<const char*>(manifestBytes.get()),
                                               manifestSize),
                              nullptr, /*allow_exceptions*/ false);
    if (manifest.is_discarded() || !manifest.is_object())
    {
        outError = std::string(kManifestName) + " is not valid JSON";
        return false;
    }

    const int schemaVersion = manifest.value("schemaVersion", 0);
    if (schemaVersion != kManifestSchemaVersion)
    {
        outError = std::string(kManifestName) + " is schema version " +
                   std::to_string(schemaVersion) + " but this player reads " +
                   std::to_string(kManifestSchemaVersion) + " — re-export with the matching tools";
        return false;
    }

    outInfo.ProjectName = manifest.value("project", std::string{});
    outInfo.EntryScene = manifest.value("entryScene", std::string{});

    const auto packs = manifest.find("packs");
    if (packs == manifest.end() || !packs->is_array() || packs->empty())
    {
        outError = std::string(kManifestName) + " names no content packs";
        return false;
    }
    for (const nlohmann::json& pack : *packs)
    {
        if (!LoadPack(pack, root, outError))
            return false;
    }

    // C# on wasm is Phase 5 of the web platform plan and is toolchain-blocked;
    // the exporter always writes this empty. Saying so beats loading nothing
    // silently if a future dist populates it before this runtime can host it.
    const auto assemblies = manifest.find("managedAssemblies");
    if (assemblies != manifest.end() && assemblies->is_array() && !assemblies->empty())
    {
        Logger::Log::Warning("WebDist: manifest lists {} managed assembly(ies); this player has "
                             "no scripting runtime and ignores them",
                             assemblies->size());
    }

    return true;
}

} // namespace GameEngine::WebPlayer
