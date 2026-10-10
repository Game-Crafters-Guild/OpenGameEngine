#include "Assets/AssetSearchProvider.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <filesystem>

namespace GameEngine
{

namespace
{
// Search dialog path line clips with overflow:hidden (no start-ellipsis); keep the
// string biased toward the asset name and parent folder when the full path is long.
constexpr size_t kMaxPathDetailChars = 52;

std::string ParentFolderAndFileName(const std::filesystem::path& relPath)
{
    std::string file = relPath.filename().generic_string();
    std::filesystem::path parent = relPath.parent_path();
    if (parent.empty() || parent == relPath.root_path())
        return file;
    std::string parName = parent.filename().generic_string();
    if (parName.empty() || parName == ".")
        return file;
    return parName + "/" + file;
}

std::string ClampDetailToSuffix(const std::string& s)
{
    if (s.size() <= kMaxPathDetailChars)
        return s;
    static const std::string kEllipsis = "\u2026";
    const size_t ellBytes = kEllipsis.size();
    if (kMaxPathDetailChars <= ellBytes)
        return kEllipsis.substr(0, kMaxPathDetailChars);
    const size_t keep = kMaxPathDetailChars - ellBytes;
    return kEllipsis + s.substr(s.size() - keep);
}

std::string FormatFullPathDetail(const std::filesystem::path& relPath)
{
    std::string full = relPath.generic_string();
    if (full.size() <= kMaxPathDetailChars)
        return full;

    std::string tail = ParentFolderAndFileName(relPath);
    static const std::string kEllipsis = "\u2026";
    std::string withLead = kEllipsis + "/" + tail;
    if (withLead.size() <= kMaxPathDetailChars)
        return withLead;

    // Filename / tail still too long: keep the rightmost part of the full path.
    return ClampDetailToSuffix(full);
}
} // namespace

AssetSearchProvider::AssetSearchProvider(AssetRegistry* registry)
    : m_Registry(registry)
{
}

void AssetSearchProvider::SetTypeFilter(const std::vector<AssetType>& types)
{
    m_TypeFilter.clear();
    m_TypeFilter.insert(types.begin(), types.end());
}

void AssetSearchProvider::SetMetadataFilter(std::function<bool(const AssetIndexRecord&)> filter)
{
    m_MetadataFilter = std::move(filter);
}

void AssetSearchProvider::SetShowFullPath(bool show)
{
    m_ShowFullPath = show;
}

void AssetSearchProvider::SetThumbnailProvider(IThumbnailProvider* provider)
{
    m_Thumbnails = provider;
}

void AssetSearchProvider::BeginSearch(const std::string& query, ResultSink sink)
{
    m_Cancelled.store(false, std::memory_order_relaxed);

    if (!m_Registry)
    {
        sink({}, /*isComplete=*/true);
        return;
    }

    // One lock-consistent snapshot of every asset, filtered by type in process.
    // Asking the registry per type walks it once per type AND needs a list of
    // types here — and a type missing from that list is a whole category of asset
    // nobody can search for. With no type filter, untyped entries drop out below:
    // they are the files the registry could not classify at all, not a category
    // the user asked for.
    const bool skipUntyped = m_TypeFilter.empty();
    const Vector<AssetIndexRecord> candidates = m_Registry->GetAssetIndexSnapshot();

    struct ScoredResult
    {
        SearchResultItem Item;
        int Score = 0;
    };

    std::vector<ScoredResult> scored;
    scored.reserve(candidates.size());

    auto assetRoot = m_Registry->GetAssetRoot();

    for (const auto& meta : candidates)
    {
        if (m_Cancelled.load(std::memory_order_relaxed))
            break;

        if (skipUntyped ? meta.Type == AssetType::Unknown : !m_TypeFilter.contains(meta.Type))
            continue;
        if (m_MetadataFilter && !m_MetadataFilter(meta))
            continue;

        std::string name = meta.Name;

        // Score: prefix match on name > substring on name > path match.
        // Defer pathStr construction until needed to avoid allocating per candidate.
        int score = 0;
        if (query.empty())
            score = 50; // Unfiltered: neutral score, rely on alphabetical order
        else if (StartsWithIgnoreCase(name, query))
            score = 100;
        else if (ContainsIgnoreCase(name, query))
            score = 75;
        else
        {
            std::string pathStr = meta.Path.generic_string();
            if (ContainsIgnoreCase(pathStr, query))
                score = 25;
            else
                continue; // No match
        }

        // Bonus for shorter names (more precise matches rank higher)
        if (name.size() < 32)
            score += static_cast<int>(32 - name.size());

        // Build detail text
        std::string detail;
        if (m_ShowFullPath)
        {
            // The root carries its own spelling and meta.Path is folded, so
            // the project-relative path is the registry's key-domain answer,
            // not a lexical subtraction (which would compare case and yield
            // nothing). Assets outside the project root — editor chrome,
            // package mounts — have no project-relative form; show the whole
            // path rather than an empty line. Rebuilt through char8_t so
            // non-ASCII survives on Windows.
            std::string rel;
            if (!AssetRegistry::TryComputeCanonicalRelativePath(assetRoot, meta.Path, rel))
            {
                const std::u8string u8 = meta.Path.generic_u8string();
                rel.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
            }
            const auto* relU8 = reinterpret_cast<const char8_t*>(rel.data());
            detail = FormatFullPathDetail(std::filesystem::path(relU8, relU8 + rel.size()));
        }
        else
        {
            auto parent = meta.Path.parent_path().filename();
            detail = ClampDetailToSuffix(parent.generic_string() + "/" +
                                         meta.Path.filename().generic_string());
        }

        SearchResultItem item;
        item.Id = ResultIdFor(meta.Guid);
        item.Label = std::move(name);
        item.Detail = std::move(detail);
        item.TypeKey = AssetTypeToString(meta.Type);
        item.Icon = SearchIcon::FromClass(GetIconClassForType(meta.Type));
        item.UserData = meta.Guid;

        if (meta.Type == AssetType::Texture)
        {
            item.Icon.ImagePath = meta.Path.string();
        }
        else if (m_Thumbnails)
        {
            const bool staticModelListThumb = (meta.Type == AssetType::Model);
            std::string thumb =
                m_Thumbnails->GetOrRequest(meta.Path, 64, nullptr, staticModelListThumb);
            if (!thumb.empty())
                item.Icon.ImagePath = std::move(thumb);
        }

        scored.push_back({std::move(item), score});
    }

    // Sort by score descending, then alphabetical
    std::sort(scored.begin(), scored.end(), [](const ScoredResult& a, const ScoredResult& b) {
        if (a.Score != b.Score)
            return a.Score > b.Score;
        return a.Item.Label < b.Item.Label;
    });

    std::vector<SearchResultItem> results;
    results.reserve(scored.size());
    for (auto& s : scored)
        results.push_back(std::move(s.Item));

    sink(std::move(results), /*isComplete=*/true);
}

void AssetSearchProvider::CancelSearch()
{
    m_Cancelled.store(true, std::memory_order_relaxed);
}

std::string AssetSearchProvider::GetPlaceholderText() const
{
    if (m_TypeFilter.size() == 1)
    {
        AssetType type = *m_TypeFilter.begin();
        return "Search " + AssetTypeToString(type) + "s...";
    }
    return "Search assets...";
}

SearchItemId AssetSearchProvider::ResultIdFor(const GUID& guid)
{
    return guid.IsNull() ? SearchItemId{0} : static_cast<SearchItemId>(std::hash<GUID>{}(guid));
}

std::string AssetSearchProvider::GetIconClassForType(AssetType type)
{
    switch (type)
    {
    case AssetType::Texture:        return "icon-texture";
    case AssetType::Material:       return "icon-material";
    case AssetType::Shader:         return "icon-shader";
    case AssetType::RenderPipeline: return "icon-shader";
    case AssetType::Model:          return "icon-model";
    case AssetType::Script:         return "icon-script";
    case AssetType::NativeSource:   return "icon-script";
    case AssetType::Audio:          return "icon-audio";
    case AssetType::Scene:          return "icon-scene";
    case AssetType::Animation:      return "icon-animation";
    case AssetType::Font:           return "icon-font";
    case AssetType::OceanDepthCache:
    case AssetType::OceanWaveSpectrum:
    case AssetType::OceanFFTCollision:
    case AssetType::OceanSettings:
    case AssetType::OceanPreset:
        return "icon-unknown";
    case AssetType::UILayout:       return "icon-unknown";
    case AssetType::UIStyle:        return "icon-unknown";
    case AssetType::Graph:          return "icon-unknown";
    case AssetType::XML:            return "icon-unknown";
    default:                        return "icon-unknown";
    }
}

} // namespace GameEngine
