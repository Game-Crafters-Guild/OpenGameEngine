#include "AgentCallResourceResolver.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Assets/Parsers/ParserExtractionHelpers.h"
#include "Core/Engine.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace
{
bool IsShownType(AssetType type)
{
    return type == AssetType::Model || type == AssetType::Material || type == AssetType::Texture;
}

std::filesystem::path WrittenPath(std::string_view reference)
{
    return std::filesystem::path(std::u8string(reference.begin(), reference.end()));
}
} // namespace

AgentCallResourceResolver::AgentCallResourceResolver(const AssetRegistry& registry, PathResolver resolvePath)
    : m_Registry(&registry)
    , m_ResolvePath(std::move(resolvePath))
{
}

std::optional<AgentCallResourceResolver> AgentCallResourceResolver::ForEngine()
{
    const AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return std::nullopt;
    return AgentCallResourceResolver(assets->GetRegistry(), [assets](const std::filesystem::path& path) {
        return assets->ResolveAssetPath(path);
    });
}

std::optional<AgentCallResource> AgentCallResourceResolver::Resolve(std::string_view reference) const
{
    AgentCallResource resource;
    resource.Reference = std::string(reference);
    AssetMetadata metadata;
    if (ParserExtraction::LooksLikeGuid(reference))
    {
        if (!m_Registry->TryGetAssetMetadata(GUID(String(reference)), metadata) || !IsShownType(metadata.Type))
            return std::nullopt;
    }
    else
    {
        resource.Name = WrittenPath(reference).filename().string();
        resource.Type = GetAssetTypeFromExtension(GetCompoundExtensionFromPath(resource.Reference));
        if (!IsShownType(resource.Type))
            return std::nullopt;
        const std::filesystem::path resolved = ResolvedPath(reference);
        if (resolved.empty() || !m_Registry->TryGetAssetMetadata(resolved, metadata) || !IsShownType(metadata.Type))
            return resource;
    }
    resource.Path = metadata.Path;
    // The stem as the asset was spelled; the database's path is case-folded.
    resource.Name = metadata.Name + metadata.Extension;
    resource.Type = metadata.Type;
    resource.Found = true;
    return resource;
}

std::vector<AgentCallResource> AgentCallResourceResolver::ResolveAll(std::span<const std::string> references) const
{
    std::vector<AgentCallResource> resources;
    for (const std::string& reference : references)
    {
        std::optional<AgentCallResource> resource = Resolve(reference);
        if (!resource)
            continue;
        const auto sameAsset = [this, &resource](const AgentCallResource& shown) {
            return resource->Found ? shown.Found && shown.Path == resource->Path
                                   : !shown.Found && ResolvedPath(shown.Reference) == ResolvedPath(resource->Reference);
        };
        if (std::none_of(resources.begin(), resources.end(), sameAsset))
            resources.push_back(std::move(*resource));
    }
    return resources;
}

std::filesystem::path AgentCallResourceResolver::ResolvedPath(std::string_view reference) const
{
    return m_ResolvePath(WrittenPath(reference)).lexically_normal();
}
} // namespace GameEngine
