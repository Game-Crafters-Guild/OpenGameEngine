#pragma once

#include "UI/Controls/SearchDialog.h"

#include <atomic>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "AssetCore/AssetTypes.h"
#include "AssetCore/Asset.h"

namespace GameEngine
{

class AssetRegistry;
class IThumbnailProvider;
struct AssetIndexRecord;

/// ISearchProvider implementation that searches the AssetRegistry.
/// Supports type filtering (e.g., textures only) and path display control.
class AssetSearchProvider : public ISearchProvider
{
public:
    explicit AssetSearchProvider(AssetRegistry* registry);

    /// Restrict results to specific asset types. Empty = all types.
    void SetTypeFilter(const std::vector<AssetType>& types);

    /// Optional semantic filter applied after type filtering. Takes the index
    /// row rather than full metadata: bulk enumeration reads a snapshot, and a
    /// filter needing dependency or subasset data would force a per-asset fetch.
    void SetMetadataFilter(std::function<bool(const AssetIndexRecord&)> filter);

    /// Whether Detail shows the full relative path or only parent folder + filename.
    /// Long paths are shortened so the tail (folder + name) stays visible in narrow UI.
    /// Default: true (full path when short; otherwise tail-preferring).
    void SetShowFullPath(bool show);

    /// Optional thumbnail provider for image previews in result icons.
    /// When set, cached thumbnails are used immediately; uncached assets
    /// fall back to the CSS class icon.
    void SetThumbnailProvider(IThumbnailProvider* provider);

    void BeginSearch(const std::string& query, ResultSink sink) override;
    void CancelSearch() override;
    std::string GetPlaceholderText() const override;

    /// Returns the CSS icon class name for a given asset type (e.g. "icon-model").
    /// Public so other UI controls (e.g. AssetField) can reuse the same mapping.
    static std::string GetIconClassForType(AssetType type);

    /// The SearchResultItem::Id this provider assigns to an asset, so a field can
    /// name its current value to SearchDialog::SetInitialSelection.
    static SearchItemId ResultIdFor(const GUID& guid);

private:

    AssetRegistry* m_Registry = nullptr;
    IThumbnailProvider* m_Thumbnails = nullptr;
    std::unordered_set<AssetType> m_TypeFilter;
    std::function<bool(const AssetIndexRecord&)> m_MetadataFilter;
    bool m_ShowFullPath = true;
    std::atomic<bool> m_Cancelled{false};
};

} // namespace GameEngine
