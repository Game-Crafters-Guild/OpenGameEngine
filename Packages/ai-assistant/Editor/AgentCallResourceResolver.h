#pragma once

#include "AssetCore/AssetTypes.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class AssetRegistry;

/// An asset a call names (AgentCallMedia::Resources), as its row shows it.
struct AgentCallResource
{
    /// The reference as the call gave it: a path or a GUID.
    std::string Reference;
    /// The asset's file name ("Brick.mat"); for an asset that is not there, the file name the
    /// reference gives.
    std::string Name;
    /// The asset's file, for its thumbnail and Open; empty when it is not there.
    std::filesystem::path Path;
    AssetType Type = AssetType::Unknown;
    /// The asset is in the project's asset database.
    bool Found = false;

    bool operator==(const AgentCallResource&) const = default;
};

/// Resolves the assets the assistant names through the asset database: one implementation for
/// every call row. GUID text (ParserExtraction::LooksLikeGuid) is looked up as a GUID; anything
/// else is a path, resolved the way the engine resolves an asset path (`resolvePath`), then
/// looked up.
class AgentCallResourceResolver
{
public:
    /// Maps a path as a call wrote it to the path the asset database knows.
    using PathResolver = std::function<std::filesystem::path(const std::filesystem::path&)>;

    AgentCallResourceResolver(const AssetRegistry& registry, PathResolver resolvePath);

    /// The engine's asset database and path resolution; nullopt where the engine has no asset
    /// manager.
    static std::optional<AgentCallResourceResolver> ForEngine();

    /// The asset `reference` names: a model, material or texture of the database (Found); a
    /// path to one that is not there (not Found, its file name kept); nullopt for a GUID the
    /// database does not know or an asset of another type, which the row does not show.
    std::optional<AgentCallResource> Resolve(std::string_view reference) const;
    /// The assets `references` name, in order, each asset once: a path and a GUID of one
    /// asset, or two spellings of one path (found or not), are one tile; the references
    /// Resolve leaves out are left out.
    std::vector<AgentCallResource> ResolveAll(std::span<const std::string> references) const;

private:
    /// `reference` as a path the asset database knows, normalized.
    std::filesystem::path ResolvedPath(std::string_view reference) const;

    const AssetRegistry* m_Registry = nullptr;
    PathResolver m_ResolvePath;
};
} // namespace GameEngine
