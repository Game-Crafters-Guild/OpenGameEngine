#include "Assets/AssetDependencyExtractor.h"
#include "AssetPathSyntax.h"

#include "AssetCore/Asset.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <cctype>
#include <filesystem>
#include <regex>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace GameEngine
{

namespace
{
// Fast, schema-aware extraction for .scene/.blueprint INI-ish files. Walks
// the content for `path="..."` / `path=...` / `@"path"` forms.
static std::vector<std::string> ExtractSceneIniPathRefs(const std::string& content)
{
    std::vector<std::string> out;

    auto push = [&](std::string_view v) {
        if (v.empty())
            return;
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
            v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
            v.remove_suffix(1);
        if (!v.empty())
            out.emplace_back(v);
    };

    size_t i = 0;
    while ((i = content.find("path", i)) != std::string::npos)
    {
        size_t j = i + 4;
        if (i > 0)
        {
            const unsigned char prev = static_cast<unsigned char>(content[i - 1]);
            if (std::isalnum(prev) || prev == '_')
            {
                i = j;
                continue;
            }
        }

        while (j < content.size() && std::isspace(static_cast<unsigned char>(content[j])))
            ++j;
        if (j >= content.size() || content[j] != '=')
        {
            i = j;
            continue;
        }
        ++j;
        while (j < content.size() && std::isspace(static_cast<unsigned char>(content[j])))
            ++j;
        if (j >= content.size())
            break;

        const char q = content[j];
        if (q == '"' || q == '\'')
        {
            const size_t start = j + 1;
            const size_t end = content.find(q, start);
            if (end != std::string::npos)
            {
                push(std::string_view(content).substr(start, end - start));
                i = end + 1;
                continue;
            }
            break;
        }
        else
        {
            const size_t start = j;
            size_t end = start;
            while (end < content.size())
            {
                const char c = content[end];
                if (std::isspace(static_cast<unsigned char>(c)) || c == ']' || c == '\r' || c == '\n')
                    break;
                ++end;
            }
            push(std::string_view(content).substr(start, end - start));
            i = end;
            continue;
        }
    }

    i = 0;
    while ((i = content.find("@\"", i)) != std::string::npos)
    {
        const size_t start = i + 2;
        const size_t end = content.find('"', start);
        if (end == std::string::npos)
            break;
        push(std::string_view(content).substr(start, end - start));
        i = end + 1;
    }

    std::unordered_set<std::string> seen;
    std::vector<std::string> dedup;
    dedup.reserve(out.size());
    for (auto& s : out)
    {
        if (seen.insert(s).second)
            dedup.push_back(std::move(s));
    }
    return dedup;
}
} // namespace

AssetDependencyInfo AssetDependencyExtractor::ExtractDependencies(const AssetMetadata& metadata,
                                                                  const AssetRegistry* registryOverride)
{
    AssetDependencyInfo info(metadata.Guid, metadata.Type);
    // A binary format holds no text references: read as text, its compressed bytes
    // match the path patterns at random, and the regex scan of a large file costs seconds.
    if (IsBinaryAssetType(metadata.Type))
        return info;
    try
    {
        std::string content;
        if (!ReadFileTextShared(metadata.Path, content))
        {
            Logger::Log::Warning("Could not open asset file for dependency extraction: {}",
                                 metadata.Path.string());
            return info;
        }

        info.Dependencies = ExtractGUIDReferences(content);
        info.DependencyPaths = ExtractPathReferences(content);

        if (metadata.Type == AssetType::Scene ||
            EndsWithIgnoreCase(metadata.Path.string(), ".scene") ||
            EndsWithIgnoreCase(metadata.Path.string(), ".blueprint"))
        {
            std::vector<std::string> extra = ExtractSceneIniPathRefs(content);
            if (!extra.empty())
            {
                std::unordered_set<std::string> seen(info.DependencyPaths.begin(),
                                                     info.DependencyPaths.end());
                for (auto& s : extra)
                {
                    if (seen.insert(s).second)
                        info.DependencyPaths.push_back(std::move(s));
                }
            }
        }

        (void)registryOverride; // deprecated: use ResolveDependencyPathsToGuids for phase-2 resolution.

        if (!info.Dependencies.empty() || !info.DependencyPaths.empty())
        {
            Logger::Log::Debug("Extracted {} GUID deps and {} path deps from asset '{}'",
                               info.Dependencies.size(),
                               info.DependencyPaths.size(),
                               metadata.Path.string());
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("Failed to extract dependencies from asset {}: {}",
                           metadata.Name, e.what());
    }

    return info;
}

void AssetDependencyExtractor::ResolveDependencyPathsToGuids(AssetDependencyInfo& info,
                                                             const AssetMetadata& metadata,
                                                             AssetRegistry& registry)
{
    const std::filesystem::path assetRoot = registry.GetAssetRoot();
    std::unordered_set<GUID> uniq(info.Dependencies.begin(), info.Dependencies.end());

    for (const std::string& p : info.DependencyPaths)
    {
        // A control character is never in an authored path; the file API reads a path
        // only up to a NUL, so "<NUL>..." would name the mount root itself.
        if (p.empty() || ContainsControlCharacter(p))
            continue;

        std::filesystem::path abs = p;
        std::string sourceAlias;
        std::filesystem::path sourceRelativePath;
        if (AssetPathSyntax::TrySplitAssetSourcePrefix(abs, sourceAlias, sourceRelativePath))
        {
            // "project:RenderPipelines/Shaders/fog.shaderpkg" names that source only, as the
            // runtime resolves it: never another mount's file at the same relative path.
            const std::filesystem::path sourceRoot = registry.GetSourceRoot(sourceAlias);
            if (sourceRoot.empty())
                continue;
            abs = (sourceRoot / sourceRelativePath).lexically_normal();
        }
        else if (abs.is_relative())
        {
            // Across the mounts, as the runtime resolves it, not against the project
            // root alone: an asset in another mount names its neighbours by path.
            if (!assetRoot.empty())
                abs = registry.ResolveRelativeAssetPath(abs);
            else
                abs = (metadata.Path.parent_path() / abs).lexically_normal();
        }

        std::error_code existsEc;
        if (!std::filesystem::exists(abs, existsEc))
            continue;

        const GUID g = registry.GetOrCreateAssetGUID(abs);
        if (!g.IsNull())
            uniq.insert(registry.ResolveGuid(g));
    }

    info.Dependencies.assign(uniq.begin(), uniq.end());
}

std::vector<GUID> AssetDependencyExtractor::ExtractGUIDReferences(const std::string& content)
{
    std::vector<GUID> guids;

    static const std::regex guidPattern(
        R"(\b[0-9a-fA-F]{8}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{12}\b)");

    std::sregex_iterator begin(content.begin(), content.end(), guidPattern);
    std::sregex_iterator end;

    for (std::sregex_iterator i = begin; i != end; ++i)
    {
        std::string guidStr = i->str();
        try
        {
            GUID guid(guidStr);
            if (!guid.IsNull())
                guids.push_back(guid);
        }
        catch (const std::exception&)
        {
            // Invalid GUID format, skip.
        }
    }

    return guids;
}

std::vector<std::string> AssetDependencyExtractor::ExtractPathReferences(const std::string& content)
{
    std::vector<std::string> paths;

    static const std::vector<std::regex> pathPatterns = {
        std::regex(R"(["']([^"']*\.(png|jpg|jpeg|bmp|tga|dds|hdr))["'])", std::regex_constants::icase),
        std::regex(R"(["']([^"']*\.(mat|material))["'])", std::regex_constants::icase),
        std::regex(R"(["']([^"']*\.(fbx|obj|dae|gltf|glb))["'])", std::regex_constants::icase),
        std::regex(R"(["']([^"']*\.(wav|mp3|ogg|flac))["'])", std::regex_constants::icase),
        std::regex(R"(["']([^"']*\.(xml|uxml|css|uss))["'])", std::regex_constants::icase),
        std::regex(R"(["']([^"']*\.(scene|blueprint))["'])", std::regex_constants::icase),
        // A project or package shader package is named with its source, as a render
        // pipeline pass names it ("project:RenderPipelines/Shaders/fog.shaderpkg"). An
        // unprefixed name is an engine package, which the export ships with the engine's
        // Shaders folder, not through the dependency walk.
        std::regex(R"(["']([A-Za-z0-9_-]+:[^"':]*\.shaderpkg)["'])", std::regex_constants::icase),
        std::regex(R"(@\"([^\"]+)\")", std::regex_constants::icase),
    };

    for (const auto& pattern : pathPatterns)
    {
        std::sregex_iterator begin(content.begin(), content.end(), pattern);
        std::sregex_iterator end;

        for (std::sregex_iterator i = begin; i != end; ++i)
        {
            if (i->size() > 1)
                paths.push_back(i->str(1));
        }
    }

    return paths;
}

} // namespace GameEngine
