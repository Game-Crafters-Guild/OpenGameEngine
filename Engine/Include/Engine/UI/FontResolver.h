#pragma once

#include "AssetCore/GUID.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetRegistry;
}

namespace GameEngine::Engine::UI
{

enum class FontStyle : uint8_t
{
    Normal = 0,
    Italic = 1,
    Oblique = 2
};

enum class FontVariant : uint8_t
{
    Normal = 0,
    SmallCaps = 1
};

// Resolved font bytes returned to callers (TTF/OTF/etc.).
struct FontBytes
{
    std::vector<std::uint8_t> bytes;
    int faceIndex = 0;     // reserved for collections (.ttc); currently unused by FontAtlas
    std::string debugName; // optional diagnostics (path, OS family, etc.)
    // GUID of the originating asset when the font was located via AssetRegistry.
    // Null for OS-system-font fallbacks. Hot-reload listeners (UIManager) use
    // this to evict the matching FontAtlas on AssetModified/AssetReloaded.
    GUID assetGuid = GUID::Null();

    bool IsValid() const { return !bytes.empty(); }
};

using FontResolveCallback = std::function<void(FontBytes)>;

// Runtime index that maps a CSS `font-family` name to a font asset path.
// This is designed for fast lookups (O(1) average) after a one-time build.
class FontFamilyIndex
{
  public:
    explicit FontFamilyIndex(AssetRegistry& registry);

    // Normalize a CSS font-family name into a stable lookup key (case-insensitive, trimmed).
    static std::string NormalizeKey(const std::string& family);

    // Rebuild the index from the AssetRegistry (fonts must be registered).
    // Safe to call multiple times; not thread-safe with concurrent queries.
    void Rebuild();

    // The index is a snapshot: a font registered after the last Rebuild
    // (async asset scan still running at first resolve, project load) would
    // otherwise be unresolvable forever. Rebuilds when the registry's font
    // population differs from the snapshot's source count and returns whether
    // a rebuild ran. Intended for the resolve-miss path — a stale index that
    // still hits is fine.
    bool RebuildIfSourceChanged();

    // Resolve an input family name to an absolute on-disk font file path.
    // Returns false if no candidate exists in the registry.
    bool TryResolveFontAssetPath(const std::string& family,
                                 int weight,
                                 FontStyle style,
                                 std::filesystem::path& outAbsPath,
                                 std::string* outDebugName = nullptr,
                                 GUID* outAssetGuid = nullptr) const;

  private:
    struct Candidate
    {
        GUID guid = GUID::Null();
        std::filesystem::path path; // absolute
        int weight = 400;
        FontStyle style = FontStyle::Normal;
        std::string debugName;
    };

    AssetRegistry& m_Registry;
    bool m_Built = false;
    size_t m_SourceFontCount = 0; // registry font count at last Rebuild
    std::unordered_map<std::string, std::vector<Candidate>> m_CandidatesByKey; // normalized family key -> candidates

    static void DeriveCandidateTraitsFromFilename(const std::filesystem::path& p, int& outWeight, FontStyle& outStyle);
};

// High-level resolver that returns font bytes for a requested family name.
// Step 1: resolve the registry's Font assets by family names and traits derived
// from their filenames.
// Step 2 (later): fall back to OS system fonts (platform-specific).
//
// This resolver is async-capable: if constructed with a JobSystem, it will read
// file bytes off-thread; otherwise it resolves synchronously on the caller thread.
class FontResolver
{
  public:
    explicit FontResolver(AssetRegistry& registry, JobSystem::WorkStealingThreadPool* jobSystem = nullptr);

    void RebuildIndex();

    // Resolve `family` to font bytes. The callback may be invoked from a worker thread.
    void ResolveAsync(const std::string& family,
                      int weight,
                      FontStyle style,
                      FontVariant variant,
                      FontResolveCallback onReady);

    // Synchronous probe: true if an on-disk font would be found (registry asset or OS install).
    // Does not read font bytes, load faces, or update the async negative cache.
    bool TryIsFamilyResolvable(const std::string& family,
                               int weight = 400,
                               FontStyle style = FontStyle::Normal);

    void SetNegativeCacheTtl(std::chrono::milliseconds ttl) { m_NegativeTtl = ttl; }
    void SetAllowSystemFonts(bool allow) { m_AllowSystemFonts = allow; }

  private:
    FontFamilyIndex m_Index;
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;
    bool m_AllowSystemFonts = true;

    // Shared state for dedupe and negative caching across multiple UIManagers/windows.
    std::mutex m_Mutex;
    std::unordered_map<std::string, std::vector<FontResolveCallback>> m_InFlight; // key -> callbacks
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_NegativeUntil;
    std::chrono::milliseconds m_NegativeTtl{std::chrono::milliseconds(2000)};
    std::once_flag m_BuildOnce;
};

} // namespace GameEngine::Engine::UI

