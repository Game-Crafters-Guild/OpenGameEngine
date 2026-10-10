#include "Scene/SceneIO.h"

#include "AssetCore/PathNormalization.h"
#include "AssetCore/SharedFileRead.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/SceneBlueprintInstance.h"
#include "Components/SceneSubsceneInstance.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/UnresolvedComponentStore.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Scene
{
static thread_local SceneIOError g_LastError{};

const SceneIOError& GetLastSceneIOError()
{
    return g_LastError;
}

void ClearLastSceneIOError()
{
    g_LastError = SceneIOError{};
}

bool SkipIsOutstanding(const ECS::World& world, const SceneLoadSkip& skip)
{
    // Nothing preserved it, so nothing can retire it: the file still holds text this build cannot
    // read, and a save still drops it. Permanently outstanding.
    if (!skip.preserved)
        return true;

    const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    if (!store)
        return false;
    const std::vector<ECS::PreservedField>* fields = store->FieldsFor(skip.entityHandle);
    if (!fields)
        return false;

    // Matched on the AUTHORED spellings the census and the store both carry, case-insensitively,
    // for the same reason DiscardField is: the authored key is what the inspector displays and
    // what the user discards by.
    const auto equalsFold = [](std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        const auto lower = [](char c)
        { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (lower(a[i]) != lower(b[i]))
                return false;
        }
        return true;
    };
    for (const ECS::PreservedField& f : *fields)
    {
        if (equalsFold(f.Component, skip.component) && equalsFold(f.Field, skip.field))
            return true;
    }
    return false;
}

std::size_t OutstandingSkipCount(const ECS::World& world, const SceneLoadDegradation& census)
{
    std::size_t n = 0;
    for (const SceneLoadSkip& s : census.skips)
        n += SkipIsOutstanding(world, s) ? 1u : 0u;
    return n;
}

namespace
{
// Identity an [embed] block had before `guid=` existed: derived from the containing
// scene's GUID, so renaming the scene moved every embed with it. Two callers must
// agree on it byte for byte — the load fallback for files that predate the attribute,
// and the save path, which persists exactly this value on first write so already-saved
// references keep resolving. Frozen: changing it orphans every unmigrated embed.
GUID DeriveLegacyEmbedGuid(ISceneAssetResolver* resolver,
                           const std::filesystem::path& sceneFile,
                           std::string_view embedId)
{
    GUID sceneGuid = GUID::Null();
    if (resolver)
        sceneGuid = resolver->GetOrCreateAssetGuid(sceneFile);
    if (sceneGuid.IsNull())
    {
        sceneGuid = GUID::Derive(GUID::Null(),
                                 AssetPaths::NormalizeForRegistryKey(sceneFile.lexically_normal()));
    }
    return GUID::Derive(sceneGuid, "embed:" + std::string(embedId));
}

// Insert `name="value"` immediately before a section header's closing bracket,
// leaving the rest of the authored line — attribute order, spacing, trailing
// text — untouched. Returns the line unchanged if it has no closing bracket.
std::string InsertHeaderAttribute(const std::string& headerLine,
                                  std::string_view name,
                                  std::string_view value)
{
    const size_t close = headerLine.rfind(']');
    if (close == std::string::npos)
        return headerLine;

    std::string out = headerLine.substr(0, close);
    out += ' ';
    out += name;
    out += "=\"";
    out += value;
    out += '"';
    out += headerLine.substr(close);
    return out;
}
} // namespace

bool TryResolveResourceIdToAssetReference(const SceneLoadContext& ctx,
                                         std::string_view resourceId,
                                         AssetReference& outRef,
                                         std::string* outError)
{
    if (!ctx.SceneFile)
    {
        if (outError)
            *outError = "No active scene load context";
        return false;
    }

    // Resources
    if (auto pit = ctx.ResourceAbsPathById.find(std::string(resourceId));
        pit != ctx.ResourceAbsPathById.end())
    {
        const std::filesystem::path& absPath = pit->second;
        const std::string guidStr = (ctx.ResourceGuidById.count(std::string(resourceId)) > 0)
                                        ? ctx.ResourceGuidById.at(std::string(resourceId))
                                        : std::string{};

        GUID guid = GUID::Null();
        if (!guidStr.empty())
        {
            try
            {
                guid = GUID(guidStr);
                if (ctx.Resolver)
                    guid = ctx.Resolver->ResolveGuid(guid);
            }
            catch (...)
            {
                guid = GUID::Null();
            }
        }
        if (guid.IsNull() && !absPath.empty())
        {
            if (ctx.Resolver)
            {
                guid = ctx.Resolver->GetOrCreateAssetGuid(absPath);
            }
            else
            {
                // Tests/tools can still get stable identity without the registry.
                // NormalizeForRegistryKey gives the same GUID across platforms
                // for the same logical path (NFC + Unicode case fold).
                guid = GUID::Derive(GUID::Null(),
                                    AssetPaths::NormalizeForRegistryKey(absPath.lexically_normal()));
            }
        }

        if (ctx.Resolver && !guid.IsNull())
        {
            std::filesystem::path canonicalPath;
            AssetType canonicalType = AssetType::Unknown;
            if (ctx.Resolver->TryGetPathAndType(guid, canonicalPath, canonicalType))
            {
                outRef = AssetReference(guid, canonicalType, canonicalPath.string());
                return true;
            }
        }

        // Infer type from extension (fallback when metadata isn't available).
        if (!absPath.empty())
        {
            const auto ext = absPath.extension().string();
            const AssetType type = GetAssetTypeFromExtension(ext);
            outRef = AssetReference(guid, type, absPath.string());
            if (outRef.guid.IsNull() || outRef.type == AssetType::Unknown)
            {
                if (outError)
                    *outError = "Failed to resolve asset reference (unknown type or guid)";
                return false;
            }
            return true;
        }

        if (outError)
            *outError = "Failed to resolve asset reference";
        return false;
    }

    // Embeds
    if (auto eit = ctx.EmbedsById.find(std::string(resourceId));
        eit != ctx.EmbedsById.end())
    {
        std::string typeLower = eit->second.Type;
        for (auto& ch : typeLower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        AssetType type = AssetType::Unknown;
        if (typeLower == "material")
            type = AssetType::Material;
        else if (typeLower == "scene")
            type = AssetType::Scene;
        else if (typeLower == "shader")
            type = AssetType::Shader;
        else if (typeLower == "renderpipeline" || typeLower == "render_pipeline")
            type = AssetType::RenderPipeline;
        else if (typeLower == "font")
            type = AssetType::Font;

        if (type == AssetType::Unknown)
        {
            if (outError)
                *outError = "Embed '" + std::string(resourceId) + "' has unsupported asset type '" + eit->second.Type + "'";
            return false;
        }

        // Persisted identity wins: it is what already-saved references baked, and it
        // survives a rename of the containing scene. Only files that predate the
        // attribute fall back to deriving from the scene GUID.
        // A malformed guid= parses to null (GUID's string constructor zero-fills
        // rather than throwing), so the IsNull test covers both "absent" and
        // "unparseable" and lands both on the legacy derivation.
        GUID embedGuid = GUID::Null();
        if (!eit->second.Guid.empty())
        {
            embedGuid = GUID(eit->second.Guid);
            if (ctx.Resolver && !embedGuid.IsNull())
                embedGuid = ctx.Resolver->ResolveGuid(embedGuid);
        }
        if (embedGuid.IsNull())
            embedGuid = DeriveLegacyEmbedGuid(ctx.Resolver, *ctx.SceneFile, resourceId);

        // If a materializer is available, ensure we create/register a real asset for this embed.
        if (ctx.EmbedMaterializer)
        {
            if (auto it = ctx.EmbedCache.find(embedGuid); it != ctx.EmbedCache.end())
            {
                outRef = it->second;
                return true;
            }

            if (!ctx.EmbedMaterializer->Materialize(*ctx.SceneFile,
                                                    embedGuid,
                                                    resourceId,
                                                    type,
                                                    eit->second.Type,
                                                    eit->second.Properties,
                                                    outRef,
                                                    outError))
            {
                if (outError && outError->empty())
                    *outError = "Failed to materialize embed asset";
                return false;
            }
            ctx.EmbedCache.emplace(embedGuid, outRef);
            return true;
        }

        // Fallback: stable identity + type only (not materialized).
        outRef = AssetReference(embedGuid, type, ("<embed:" + std::string(resourceId) + ">"));
        return true;
    }

    if (outError)
        *outError = "Unknown resource/embed id";
    return false;
}

bool TryGetEmbedDefinition(const SceneLoadContext& ctx,
                           std::string_view embedId,
                           std::string& outType,
                           std::unordered_map<std::string, std::string>& outProperties,
                           std::string* outError)
{
    outType.clear();
    outProperties.clear();

    auto it = ctx.EmbedsById.find(std::string(embedId));
    if (it == ctx.EmbedsById.end())
    {
        if (outError)
            *outError = "Unknown embed id";
        return false;
    }

    outType = it->second.Type;
    outProperties = it->second.Properties;
    return true;
}

namespace
{
// Convert an absolute or already-relative path into a project-relative path
// using the given asset root. If the path is outside the root or the root is
// empty, returns the path as a generic string unchanged.
std::string RelativizeToAssetRoot(const std::filesystem::path& abs,
                                  const std::filesystem::path& assetRoot)
{
    if (abs.empty())
        return {};
    if (assetRoot.empty())
        return abs.generic_string();

    std::error_code ec;
    auto rel = std::filesystem::relative(abs, assetRoot, ec);
    // generic_string() on `path::native()` is std::wstring on Windows and
    // std::string on POSIX — comparing against generic_string() is portable.
    const std::string relGeneric = rel.generic_string();
    if (ec || rel.empty() || relGeneric.rfind("..", 0) == 0)
        return abs.generic_string();
    return relGeneric;
}

// Map an authored asset path to the file that supplies it. The resolver owns the
// mount search (project first, then the other registered sources; an `alias:`
// prefix pins one), so a scene shipped on the editor mount binds its editor-mount
// assets while a project asset at the same relative path still wins. Without a
// resolver, or when it cannot name a candidate at all, the path anchors to the
// asset root, then to the owner file's directory — the shape tools and tests
// without an AssetManager-backed resolver take, since a resolver that has one
// answers with its project-root candidate even on a miss.
std::filesystem::path ResolveAuthoredAssetPath(const SceneLoadContext& ctx,
                                               const std::filesystem::path& ownerFile,
                                               const std::filesystem::path& authoredPath)
{
    if (authoredPath.is_absolute())
        return authoredPath;
    if (ctx.Resolver)
    {
        const std::filesystem::path supplied = ctx.Resolver->ResolveAssetPath(authoredPath);
        if (!supplied.empty())
            return supplied.lexically_normal();
    }
    if (!ctx.AssetRoot.empty())
        return (ctx.AssetRoot / authoredPath).lexically_normal();
    if (!ownerFile.empty())
        return (ownerFile.parent_path() / authoredPath).lexically_normal();
    return authoredPath;
}

// Resolve an authored path string to an absolute path using the save
// context's asset root (or the scene file's directory as a fallback).
std::filesystem::path AbsolutizeAuthoredPath(const SceneSaveContext& ctx, std::string_view authoredPath)
{
    std::filesystem::path p((std::string(authoredPath)));
    if (p.is_absolute())
        return p;
    if (!ctx.AssetRoot.empty())
        return (ctx.AssetRoot / p).lexically_normal();
    if (ctx.SceneFile)
        return (ctx.SceneFile->parent_path() / p).lexically_normal();
    return p;
}

// Resolve a GUID from an authored path via the save context's
// resolver. Returns empty string when no resolver is bound or the path
// can't be resolved. When `mintIfUnknown` is true, a path that the
// registry doesn't know is allocated a fresh GUID via
// GetOrCreateAssetGuid — this commits a registry mutation as a side
// effect of save, so callers opt in deliberately.
//
// Two sites share this: FormatAssetReferenceForSave (mintIfUnknown=false,
// per-property heal preserves authored data) and the [resource] save
// pass (mintIfUnknown=true, declared resources are expected to bind
// even for paths the registry didn't observe yet).
std::string HealGuidFromAuthoredPath(const SceneSaveContext& ctx, std::string_view authoredPath, bool mintIfUnknown)
{
    if (authoredPath.empty() || !ctx.Resolver)
        return {};
    ISceneAssetResolver* resolver = ctx.Resolver;

    const auto abs = AbsolutizeAuthoredPath(ctx, authoredPath);
    GUID g = GUID::Null();
    AssetType t = AssetType::Unknown;
    if (resolver->TryGetGuidAndType(abs, g, t) && !g.IsNull())
        return resolver->ResolveGuid(g).ToString();

    if (mintIfUnknown)
    {
        const GUID minted = resolver->GetOrCreateAssetGuid(abs);
        if (!minted.IsNull())
            return resolver->ResolveGuid(minted).ToString();
    }
    return {};
}

// The path the scene authored for `guid` when the load could not bind it (UnresolvedComponentStore),
// or empty. Components keep the GUID alone, so without this a save of a scene with a missing asset
// writes `[guid=...]` and the path that would heal the reference once the asset returns is gone.
std::string_view RecordedUnresolvedReferencePath(const SceneSaveContext& ctx, const GUID& guid)
{
    if (!ctx.Unresolved || guid.IsNull())
        return {};
    const std::string* path = ctx.Unresolved->FindUnresolvedReference(guid.ToString());
    return path ? std::string_view(*path) : std::string_view{};
}
} // namespace

std::string FormatAssetReferenceForSave(const SceneSaveContext& ctx,
                                        const GUID& guid,
                                        std::string_view authoredPath)
{
    if (authoredPath.empty())
        authoredPath = RecordedUnresolvedReferencePath(ctx, guid);

    // Both inputs may be empty (e.g. cleared asset slot). Caller should typically
    // skip emission in that case; we return an empty AssetRef that will round-trip
    // back to a parse error, surfacing the misuse loudly.
    if (guid.IsNull() && authoredPath.empty())
        return std::string("[]");

    std::string outPath;
    std::string outGuid;

    if (ctx.Resolver)
    {
        ISceneAssetResolver* resolver = ctx.Resolver;
        const auto& assetRoot = ctx.AssetRoot;

        // Healing pass 1: trust the guid if the resolver knows it.
        if (!guid.IsNull())
        {
            const GUID canonical = resolver->ResolveGuid(guid);
            std::filesystem::path canonicalPath;
            AssetType canonicalType = AssetType::Unknown;
            if (resolver->TryGetPathAndType(canonical, canonicalPath, canonicalType))
            {
                outPath = RelativizeToAssetRoot(canonicalPath, assetRoot);
                outGuid = canonical.ToString();
                return FormatAssetRef(outPath, outGuid);
            }
            outGuid = canonical.ToString();
        }

        // Healing pass 2: guid unknown but path was authored — derive guid
        // from path via the registry. mintIfUnknown=false because per-property
        // saves should preserve authored data; if the registry doesn't know
        // the path (e.g. a freshly-pasted asset awaiting scan) we keep the
        // path as-is and emit it without a guid.
        if (!authoredPath.empty())
        {
            const auto abs = AbsolutizeAuthoredPath(ctx, authoredPath);
            const std::string healed = HealGuidFromAuthoredPath(ctx, authoredPath, /*mintIfUnknown=*/false);
            if (!healed.empty())
            {
                outPath = RelativizeToAssetRoot(abs, assetRoot);
                outGuid = healed;
                return FormatAssetRef(outPath, outGuid);
            }
            outPath = RelativizeToAssetRoot(abs, assetRoot);
        }
    }
    else
    {
        // No resolver: emit whichever fields the caller supplied as-is.
        if (!authoredPath.empty())
            outPath = std::string(authoredPath);
        if (!guid.IsNull())
            outGuid = guid.ToString();
    }

    return FormatAssetRef(outPath, outGuid);
}

bool TryResolveAssetReference(const SceneLoadContext& ctx,
                              const SceneValue& value,
                              AssetType expectedType,
                              AssetReference& outRef,
                              std::string* outError)
{
    auto resolveByGuidThenPath = [&](const GUID& parsedGuid,
                                     std::string_view authoredPath) -> bool
    {
        // Path 1: GUID known to the registry — done.
        if (!parsedGuid.IsNull() && ctx.Resolver)
        {
            const GUID canonical = ctx.Resolver->ResolveGuid(parsedGuid);
            std::filesystem::path canonicalPath;
            AssetType canonicalType = AssetType::Unknown;
            if (ctx.Resolver->TryGetPathAndType(canonical, canonicalPath, canonicalType))
            {
                outRef = AssetReference(canonical,
                                        expectedType != AssetType::Unknown ? expectedType : canonicalType,
                                        canonicalPath.string());
                return true;
            }
        }

        // Path 2 (cross-project recovery): GUID unknown but the authored path names a
        // file on some mount.
        if (!authoredPath.empty() && ctx.Resolver)
        {
            const std::filesystem::path abs =
                ResolveAuthoredAssetPath(ctx, ctx.SceneFile ? *ctx.SceneFile : std::filesystem::path{},
                                         std::filesystem::path(std::string(authoredPath)));
            const GUID derived = ctx.Resolver->GetOrCreateAssetGuid(abs);
            if (!derived.IsNull())
            {
                outRef = AssetReference(derived, expectedType, abs.string());
                return true;
            }
        }

        // Path 3 (no resolver, or resolver couldn't bind either guid or path):
        // build a degraded ref from whatever we have. Returning false here would
        // fail the entire scene load just because one asset reference is stale,
        // so we keep the guid+path that was authored and let the asset manager
        // surface a load error per-asset later.
        //
        // Recordless GUIDs still follow redirects: derived subasset identities
        // (embedded clips, bridge materials) never have records of their own,
        // so a rename-healed subasset reference always reaches this fallback —
        // the cascade redirect is the only thing that can carry it forward to
        // the identity the reloaded container re-derives. A GUID a redirect
        // moved is healthy, not broken; only genuinely unknown GUIDs warn.
        //
        // The component keeps the GUID alone, so a genuinely unknown GUID is recorded
        // on the target world with its authored path: a save writes the path back, the
        // reference heals through Path 2 once the file returns, and the editor's
        // Missing Assets panel names the path.
        if (!parsedGuid.IsNull())
        {
            GUID degraded = parsedGuid;
            if (ctx.Resolver)
            {
                const GUID chased = ctx.Resolver->ResolveGuid(parsedGuid);
                if (!chased.IsNull() && chased != parsedGuid)
                {
                    degraded = chased;
                }
                else
                {
                    Logger::Log::Warning(
                        "Scene: AssetReference {} (path '{}') is unknown to the registry; "
                        "loading as degraded reference. Asset will fail to materialize until "
                        "the registry rebinds it.",
                        parsedGuid.ToString(),
                        authoredPath);
                }
            }
            if (degraded == parsedGuid && ctx.TargetWorld)
                ctx.TargetWorld->GetUnresolvedComponents().NoteUnresolvedReference(parsedGuid.ToString(),
                                                                                   std::string(authoredPath));
            outRef = AssetReference(degraded, expectedType, std::string(authoredPath));
            return true;
        }
        if (outError)
            *outError = "AssetReference: cannot resolve — no GUID and no usable path";
        return false;
    };

    switch (value.Kind)
    {
    case SceneValueKind::String:
    {
        // Legacy bare GUID string (current dominant form).
        try
        {
            const GUID g(value.StringValue);
            return resolveByGuidThenPath(g, "");
        }
        catch (...)
        {
            if (outError)
                *outError = "AssetReference: string value is not a valid GUID: '" + value.StringValue + "'";
            return false;
        }
    }
    case SceneValueKind::AssetRef:
    {
        const std::string_view path = AssetRefPath(value);
        const std::string_view guidStr = AssetRefGuid(value);
        GUID g = GUID::Null();
        if (!guidStr.empty())
        {
            try { g = GUID(std::string(guidStr)); }
            catch (...)
            {
                if (outError)
                    *outError = std::string("AssetReference: invalid guid '") + std::string(guidStr) + "'";
                return false;
            }
        }
        return resolveByGuidThenPath(g, path);
    }
    case SceneValueKind::ResourceRef:
        return TryResolveResourceIdToAssetReference(ctx, value.StringValue, outRef, outError);
    case SceneValueKind::Int:
        if (value.IntValue == 0)
        {
            // Sentinel: integer 0 is the "clear this slot" form for asset
            // properties; emitted by schemas that want to null out a
            // previously-assigned ref without writing a synthetic GUID.
            outRef = AssetReference{};
            return true;
        }
        if (outError)
            *outError = "AssetReference: integer value must be 0 (clear) — got " + std::to_string(value.IntValue);
        return false;
    default:
        if (outError)
            *outError = "AssetReference: unsupported value kind";
        return false;
    }
}

namespace
{
static void SetError(const std::filesystem::path& file, int line, const std::string& msg)
{
    g_LastError.file = file;
    g_LastError.line = line;
    g_LastError.message = msg;
}

static std::atomic<uint64_t> g_AdditiveImportCounter{0};

static bool ReadTextFile(const std::filesystem::path& p, std::string& out)
{
    return ReadFileTextShared(p, out);
}

static bool WriteTextFileAtomic(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f.good())
            return false;
    }
    return FileSystem::PublishFile(tmp, p);
}

static std::string Trim(std::string s)
{
    // Compute the trimmed bounds, then erase once from each end — the old front s.erase(begin())
    // loop shifted the whole string per leading space (O(n^2)); erase(0, b) shifts once.
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    s.erase(e);
    s.erase(0, b);
    return s;
}

static bool StartsWith(const std::string& s, const char* prefix)
{
    if (!prefix)
        return false;
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

struct Tuple3
{
    float x = 0, y = 0, z = 0;
};

struct Section
{
    std::string type; // scene, blueprint, resource, entity, blueprint(instance), subscene
    std::unordered_map<std::string, std::string> attrs; // raw attr values (already unquoted for strings)
    std::vector<std::pair<int, std::string>> bodyLines; // {lineNo, text}
    int headerLine = 0;
};

static bool ParseSectionHeader(const std::string& line, std::string& outType, std::unordered_map<std::string, std::string>& outAttrs)
{
    std::string t = Trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']')
        return false;
    t = t.substr(1, t.size() - 2);
    t = Trim(t);
    if (t.empty())
        return false;

    // Split: first token = section type, then key=value tokens separated by spaces.
    // Values may be quoted; we do not support escaped quotes in v1.
    std::string type;
    size_t i = 0;
    while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])))
    {
        type.push_back(t[i]);
        ++i;
    }
    outType = ToLowerAscii(type);

    while (i < t.size())
    {
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
            ++i;
        if (i >= t.size())
            break;

        // key
        std::string key;
        while (i < t.size() && (std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_' || t[i] == '.'))
        {
            key.push_back(t[i]);
            ++i;
        }
        key = ToLowerAscii(key);
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
            ++i;
        if (i >= t.size() || t[i] != '=' || key.empty())
            return false;
        ++i;
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
            ++i;
        if (i >= t.size())
            return false;

        // value: quoted string or identifier/number until whitespace
        std::string value;
        if (t[i] == '"')
        {
            ++i;
            while (i < t.size() && t[i] != '"')
            {
                value.push_back(t[i]);
                ++i;
            }
            if (i >= t.size() || t[i] != '"')
                return false;
            ++i;
        }
        else
        {
            while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])))
            {
                value.push_back(t[i]);
                ++i;
            }
        }

        outAttrs[key] = value;
    }
    return true;
}

struct SceneDoc
{
    bool isBlueprint = false;
    std::string name;
    int version = 0;

    struct PropValue
    {
        std::string value;
        int line = 0;
        // Authored spelling of the property key (keys are lowercased for schema
        // matching); empty means "same as the lowered key". Only the unknown-
        // component preservation path echoes this back.
        std::string originalKey;
    };

    // Insertion-ordered flat storage of a component's properties. Replaces a nested
    // unordered_map; the parse path only ever appends and the load/save/validate paths
    // only ever range-iterate, so a vector keeps file order deterministic and avoids the
    // per-entity, per-component map allocations.
    // components: [ { "transform", [ {"position", {"(1, 2, 3)", line}}, ... ] }, ... ]
    struct ComponentProps
    {
        std::string name;
        // Authored spelling of the component name (see PropValue::originalKey).
        std::string originalName;
        std::vector<std::pair<std::string, PropValue>> props;
    };
    using ComponentProperties = std::vector<ComponentProps>;

    struct Include
    {
        std::filesystem::path path;
        int line = 0;
    };
    std::vector<Include> includes;

    struct Embed
    {
        std::string id;
        std::string type;
        std::string guid; // optional; absent in files authored before embed identity was persisted
        std::unordered_map<std::string, PropValue> properties;
        int headerLine = 0;
    };
    std::unordered_map<std::string, Embed> embeds;

    struct Resource
    {
        std::string id;
        std::string path;
        std::string guid; // optional
        int headerLine = 0;
    };
    std::unordered_map<std::string, Resource> resources;

    struct Entity
    {
        std::string id;
        std::optional<std::string> parent;
        ComponentProperties components;
        int headerLine = 0;
    };
    std::vector<Entity> entities; // in file order

    struct BlueprintInstance
    {
        std::string id;
        std::optional<std::string> parent;
        std::string sourceResourceId;
        // Overrides (root only), stored as component properties.
        ComponentProperties components;
        // Remove directives (-ComponentName)
        std::unordered_set<std::string> removedComponents;
        int headerLine = 0;
    };
    std::vector<BlueprintInstance> blueprintInstances;

    struct Subscene
    {
        std::string id;
        std::string sourceResourceId;
        Tuple3 offset{};
        bool hasOffset = false;
        bool enabled = true;
        int headerLine = 0;
    };
    std::vector<Subscene> subscenes;

    bool hasHierarchyUi = false;
    SceneHierarchyUi hierarchyUi;

    bool hasEditorCamera = false;
    SceneEditorCamera editorCamera;

};


static void SplitCommaSeparatedTags(std::string_view blob, std::vector<std::string>& out)
{
    out.clear();
    size_t start = 0;
    while (start <= blob.size())
    {
        const size_t comma = blob.find(',', start);
        const size_t end = (comma == std::string::npos) ? blob.size() : comma;
        std::string_view tag(blob.data() + start, end - start);
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front())))
            tag.remove_prefix(1);
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back())))
            tag.remove_suffix(1);
        if (!tag.empty())
            out.emplace_back(tag);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
}

static void SplitSemicolonSeparatedTags(std::string_view blob, std::vector<std::string>& out)
{
    out.clear();
    size_t start = 0;
    while (start <= blob.size())
    {
        const size_t semi = blob.find(';', start);
        const size_t end = (semi == std::string::npos) ? blob.size() : semi;
        std::string_view tag(blob.data() + start, end - start);
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front())))
            tag.remove_prefix(1);
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back())))
            tag.remove_suffix(1);
        if (!tag.empty())
            out.emplace_back(tag);
        if (semi == std::string::npos)
            break;
        start = semi + 1;
    }
}

// Splits "Component.prop = value". outComp/outProp are LOWERCASED (schema and
// registry matching key off the lowered form); outCompAuthored/outPropAuthored
// receive the authored spelling so the unknown-component preservation path can
// echo the input casing instead of the normalized one.
static bool ParseComponentLine(const std::string& line, std::string& outComp, std::string& outProp,
                               std::string& outValue,
                               std::string* outCompAuthored = nullptr,
                               std::string* outPropAuthored = nullptr)
{
    // Component.prop = value
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;
    std::string left = Trim(line.substr(0, eq));
    std::string right = Trim(line.substr(eq + 1));
    const size_t dot = left.find('.');
    if (dot == std::string::npos)
        return false;

    // Multi-dot locators are ambiguous: `LocalPack.LocalTag.Value` is a
    // namespace-QUALIFIED managed blob component ("LocalPack.LocalTag") with
    // field "Value", while `Anim.states.idle` is component "Anim" with a nested
    // property path. Resolve by asking the reflection registry, longest
    // component prefix first; only a REGISTERED qualified name wins. Unresolved
    // multi-dot lines keep the first-dot split so the preservation path
    // round-trips them byte-stably.
    const std::string lowered = ToLowerAscii(left);
    if (lowered.find('.', dot + 1) != std::string::npos)
    {
        size_t split = lowered.rfind('.');
        while (split != std::string::npos && split > dot)
        {
            const std::string candidate = Trim(lowered.substr(0, split));
            if (ECS::ComponentFieldRegistry::FindByName(candidate) != 0)
            {
                outComp = candidate;
                outProp = Trim(lowered.substr(split + 1));
                outValue = right;
                // lowered and left are index-aligned (ASCII lowering).
                if (outCompAuthored)
                    *outCompAuthored = Trim(left.substr(0, split));
                if (outPropAuthored)
                    *outPropAuthored = Trim(left.substr(split + 1));
                return !outComp.empty() && !outProp.empty();
            }
            split = lowered.rfind('.', split - 1);
        }
    }

    outComp = ToLowerAscii(Trim(left.substr(0, dot)));
    outProp = ToLowerAscii(Trim(left.substr(dot + 1)));
    outValue = right;
    if (outCompAuthored)
        *outCompAuthored = Trim(left.substr(0, dot));
    if (outPropAuthored)
        *outPropAuthored = Trim(left.substr(dot + 1));
    return !outComp.empty() && !outProp.empty();
}

// Find-or-append the ComponentProps entry for `compName` (already lowercased). Preserves the
// nested-map's coalescing behavior: properties of a component spread across the file land in a
// single grouped entry, matching the previous unordered_map keyed insert. `authoredName` is the
// input spelling, recorded on first append for the preservation path.
static SceneDoc::ComponentProps& FindOrAppendComponent(SceneDoc::ComponentProperties& comps,
                                                       const std::string& compName,
                                                       const std::string& authoredName = {})
{
    for (auto& c : comps)
    {
        if (c.name == compName)
            return c;
    }
    comps.push_back(SceneDoc::ComponentProps{compName, authoredName.empty() ? compName : authoredName, {}});
    return comps.back();
}

// Set a property on a component, mirroring the old map's last-write-wins overwrite for a repeated
// property name within the same component (single entry, not a duplicate).
static void SetComponentProp(SceneDoc::ComponentProps& comp, const std::string& prop,
                             SceneDoc::PropValue value)
{
    for (auto& kv : comp.props)
    {
        if (kv.first == prop)
        {
            kv.second = std::move(value);
            return;
        }
    }
    comp.props.emplace_back(prop, std::move(value));
}

static bool ParsePropertyLine(const std::string& line, std::string& outKey, std::string& outValue)
{
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;
    outKey = Trim(line.substr(0, eq));
    outValue = Trim(line.substr(eq + 1));
    return !outKey.empty();
}

static bool ParseDocument(const std::filesystem::path& filePath,
                          const std::string& text,
                          SceneDoc& out,
                          std::string* outError,
                          int* outErrorLine = nullptr)
{
    if (outErrorLine)
        *outErrorLine = 0;

    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    std::vector<Section> sections;
    Section* cur = nullptr;

    auto pushSection = [&](const std::string& headerLine)
    {
        std::string type;
        std::unordered_map<std::string, std::string> attrs;
        if (!ParseSectionHeader(headerLine, type, attrs))
            return false;
        sections.push_back(Section{type, std::move(attrs), {}, lineNo});
        cur = &sections.back();
        return true;
    };

    while (std::getline(in, line))
    {
        ++lineNo;
        std::string t = Trim(line);
        if (t.empty())
            continue;
        if (t[0] == ';')
            continue;
        if (t.front() == '[' && t.back() == ']')
        {
            if (!pushSection(t))
            {
                if (outError)
                    *outError = "Invalid section header at line " + std::to_string(lineNo);
                if (outErrorLine)
                    *outErrorLine = lineNo;
                return false;
            }
            continue;
        }
        if (!cur)
        {
            if (outError)
                *outError = "Content before header at line " + std::to_string(lineNo);
            if (outErrorLine)
                *outErrorLine = lineNo;
            return false;
        }
        cur->bodyLines.push_back({lineNo, std::move(t)});
    }

    if (sections.empty())
    {
        if (outError)
            *outError = "Empty scene file";
        return false;
    }

    // Header must be first section: [scene] or [blueprint]
    {
        const Section& h = sections.front();
        if (h.type != "scene" && h.type != "blueprint")
        {
            if (outError)
                *outError = "First section must be [scene] or [blueprint]";
            if (outErrorLine)
                *outErrorLine = h.headerLine;
            return false;
        }
        out.isBlueprint = (h.type == "blueprint");
        auto itVer = h.attrs.find("version");
        if (itVer == h.attrs.end())
        {
            if (outError)
                *outError = "Missing version in header";
            if (outErrorLine)
                *outErrorLine = h.headerLine;
            return false;
        }
        out.version = std::atoi(itVer->second.c_str());
        auto itName = h.attrs.find("name");
        out.name = (itName != h.attrs.end()) ? itName->second : filePath.stem().string();
    }

    for (size_t si = 1; si < sections.size(); ++si)
    {
        const Section& s = sections[si];
        if (s.type == "include")
        {
            auto itPath = s.attrs.find("path");
            if (itPath == s.attrs.end() || itPath->second.empty())
            {
                if (outError)
                    *outError = "Missing include path at line " + std::to_string(s.headerLine);
                if (outErrorLine)
                    *outErrorLine = s.headerLine;
                return false;
            }
            SceneDoc::Include inc{};
            inc.path = itPath->second;
            if (inc.path.is_relative())
                inc.path = (filePath.parent_path() / inc.path).lexically_normal();
            inc.line = s.headerLine;
            out.includes.push_back(std::move(inc));
            continue;
        }
        if (s.type == "resource")
        {
            SceneDoc::Resource r{};
            auto itId = s.attrs.find("id");
            auto itPath = s.attrs.find("path");
            if (itId == s.attrs.end() || itPath == s.attrs.end())
                continue;
            r.id = itId->second;
            r.path = itPath->second;
            if (auto itGuid = s.attrs.find("guid"); itGuid != s.attrs.end())
                r.guid = itGuid->second;
            r.headerLine = s.headerLine;
            if (out.resources.find(r.id) != out.resources.end())
            {
                if (outError)
                    *outError = "Duplicate resource id '" + r.id + "' at line " + std::to_string(s.headerLine);
                if (outErrorLine)
                    *outErrorLine = s.headerLine;
                return false;
            }
            out.resources[r.id] = std::move(r);
            continue;
        }
        if (s.type == "embed")
        {
            SceneDoc::Embed e{};
            auto itId = s.attrs.find("id");
            auto itType = s.attrs.find("type");
            if (itId == s.attrs.end() || itType == s.attrs.end())
            {
                if (outError)
                    *outError = "Embed missing id/type at line " + std::to_string(s.headerLine);
                if (outErrorLine)
                    *outErrorLine = s.headerLine;
                return false;
            }
            e.id = itId->second;
            e.type = itType->second;
            if (auto itGuid = s.attrs.find("guid"); itGuid != s.attrs.end())
                e.guid = itGuid->second;
            e.headerLine = s.headerLine;
            if (out.embeds.find(e.id) != out.embeds.end())
            {
                if (outError)
                    *outError = "Duplicate embed id '" + e.id + "' at line " + std::to_string(s.headerLine);
                if (outErrorLine)
                    *outErrorLine = s.headerLine;
                return false;
            }
            for (const auto& [ln, textLine] : s.bodyLines)
            {
                std::string key, val;
                if (!ParsePropertyLine(textLine, key, val))
                    continue;
                e.properties[ToLowerAscii(key)] = SceneDoc::PropValue{val, ln};
            }
            out.embeds[e.id] = std::move(e);
            continue;
        }
        if (s.type == "entity")
        {
            SceneDoc::Entity e{};
            auto itId = s.attrs.find("id");
            if (itId == s.attrs.end())
                continue;
            e.id = itId->second;
            e.headerLine = s.headerLine;
            if (auto itParent = s.attrs.find("parent"); itParent != s.attrs.end() && !itParent->second.empty())
                e.parent = itParent->second;

            for (const auto& [ln, l] : s.bodyLines)
            {
                std::string comp, prop, value, compAuthored, propAuthored;
                if (!ParseComponentLine(l, comp, prop, value, &compAuthored, &propAuthored))
                    continue;
                SetComponentProp(FindOrAppendComponent(e.components, comp, compAuthored), prop,
                                 SceneDoc::PropValue{value, ln, propAuthored});
            }

            out.entities.push_back(std::move(e));
            continue;
        }
        if (s.type == "blueprint")
        {
            // Blueprint instance section in a scene
            SceneDoc::BlueprintInstance bi{};
            auto itId = s.attrs.find("id");
            auto itSrc = s.attrs.find("source");
            if (itId == s.attrs.end() || itSrc == s.attrs.end())
                continue;
            bi.id = itId->second;
            bi.sourceResourceId = itSrc->second;
            bi.headerLine = s.headerLine;
            if (auto itParent = s.attrs.find("parent"); itParent != s.attrs.end() && !itParent->second.empty())
                bi.parent = itParent->second;

            for (const auto& [ln, l] : s.bodyLines)
            {
                if (!l.empty() && l[0] == '-')
                {
                    bi.removedComponents.insert(ToLowerAscii(Trim(l.substr(1))));
                    continue;
                }
                // Additions: "Component" or "+Component"
                {
                    std::string t = Trim(l);
                    if (!t.empty() && t[0] == '+')
                        t = Trim(t.substr(1));
                    if (!t.empty() && t.find('=') == std::string::npos && t.find('.') == std::string::npos)
                    {
                        FindOrAppendComponent(bi.components, ToLowerAscii(t), t); // mark component present with no explicit properties
                        continue;
                    }
                }
                std::string comp, prop, value, compAuthored, propAuthored;
                if (!ParseComponentLine(l, comp, prop, value, &compAuthored, &propAuthored))
                    continue;
                SetComponentProp(FindOrAppendComponent(bi.components, comp, compAuthored), prop,
                                 SceneDoc::PropValue{value, ln, propAuthored});
            }

            out.blueprintInstances.push_back(std::move(bi));
            continue;
        }
        if (s.type == "subscene")
        {
            SceneDoc::Subscene ss{};
            auto itId = s.attrs.find("id");
            auto itSrc = s.attrs.find("source");
            if (itId == s.attrs.end() || itSrc == s.attrs.end())
                continue;
            ss.id = itId->second;
            ss.sourceResourceId = itSrc->second;
            ss.headerLine = s.headerLine;
            for (const auto& [ln, l] : s.bodyLines)
            {
                // offset = (x, y, z)
                if (StartsWith(ToLowerAscii(l), "offset"))
                {
                    const size_t eq = l.find('=');
                    if (eq != std::string::npos)
                    {
                        Float3 f3{};
                        if (!ParseFloat3(l.substr(eq + 1), f3))
                        {
                            if (outError)
                                *outError = "Invalid subscene offset in '" + ss.id + "' at line " + std::to_string(ln);
                            if (outErrorLine)
                                *outErrorLine = ln;
                            return false;
                        }
                        ss.offset = Tuple3{f3.X, f3.Y, f3.Z};
                        ss.hasOffset = true;
                    }
                }
                if (StartsWith(ToLowerAscii(l), "enabled"))
                {
                    const size_t eq = l.find('=');
                    if (eq != std::string::npos)
                    {
                        SceneValue v{};
                        std::string perr;
                        if (!ParseValue(l.substr(eq + 1), v, &perr))
                        {
                            if (outError)
                                *outError = "Invalid subscene enabled in '" + ss.id + "' at line " + std::to_string(ln) + ": " + perr;
                            if (outErrorLine)
                                *outErrorLine = ln;
                            return false;
                        }
                        if (v.Kind == SceneValueKind::Bool)
                            ss.enabled = v.BoolValue;
                        else if (v.Kind == SceneValueKind::Int)
                            ss.enabled = (v.IntValue != 0);
                        else
                        {
                            if (outError)
                                *outError = "Subscene enabled must be true/false in '" + ss.id + "' at line " + std::to_string(ln);
                            if (outErrorLine)
                                *outErrorLine = ln;
                            return false;
                        }
                    }
                }
            }
            out.subscenes.push_back(std::move(ss));
            continue;
        }
        if (s.type == "hierarchy_ui")
        {
            if (out.hasHierarchyUi)
                continue;
            out.hasHierarchyUi = true;
            for (const auto& [ln, l] : s.bodyLines)
            {
                (void)ln;
                std::string key, val;
                if (!ParsePropertyLine(l, key, val))
                    continue;
                const std::string k = ToLowerAscii(key);
                std::string valDecoded;
                if (!val.empty() && val.front() == '"')
                {
                    if (!ParseQuotedString(val, valDecoded))
                        valDecoded = std::move(val);
                }
                else
                    valDecoded = std::move(val);

                if (k == "expanded")
                    SplitCommaSeparatedTags(valDecoded, out.hierarchyUi.expandedSceneEntityTags);
                else if (k == "selection")
                    SplitSemicolonSeparatedTags(valDecoded, out.hierarchyUi.selectionSceneEntityTagsOrdered);
            }
            continue;
        }
        if (s.type == "editor_camera")
        {
            if (out.hasEditorCamera)
                continue;
            out.hasEditorCamera = true;
            for (const auto& [ln, l] : s.bodyLines)
            {
                (void)ln;
                std::string key, val;
                if (!ParsePropertyLine(l, key, val))
                    continue;
                const std::string k = ToLowerAscii(key);
                std::string valDecoded;
                if (!val.empty() && val.front() == '"')
                {
                    if (!ParseQuotedString(val, valDecoded))
                        valDecoded = std::move(val);
                }
                else
                    valDecoded = std::move(val);

                auto parseFloat = [](const std::string& s, float& out) {
                    try { out = std::stof(s); return true; } catch (...) { return false; }
                };

                if (k == "pos_x") parseFloat(valDecoded, out.editorCamera.PosX);
                else if (k == "pos_y") parseFloat(valDecoded, out.editorCamera.PosY);
                else if (k == "pos_z") parseFloat(valDecoded, out.editorCamera.PosZ);
                else if (k == "yaw_deg") parseFloat(valDecoded, out.editorCamera.YawDeg);
                else if (k == "pitch_deg") parseFloat(valDecoded, out.editorCamera.PitchDeg);
                else if (k == "distance") parseFloat(valDecoded, out.editorCamera.Distance);
                else if (k == "is_2d")
                {
                    std::string lower = ToLowerAscii(valDecoded);
                    out.editorCamera.Is2D = (lower == "true" || lower == "1");
                }
            }
            continue;
        }
    }

    return true;
}

// Records `id` as used by the section on `headerLine`. False, with the cause and the fix, when an
// earlier section already uses it: entity and blueprint-instance ids share one namespace.
static bool ClaimEntityId(std::unordered_map<std::string, int>& firstLineById, const std::string& id,
                          int headerLine, std::string* outError, int* outErrorLine)
{
    const auto [it, inserted] = firstLineById.emplace(id, headerLine);
    if (inserted)
        return true;
    if (outError)
        *outError = "Duplicate entity id \"" + id + "\". Line " + std::to_string(it->second) +
                    " already uses this id. Give one of the two entities a different id.";
    if (outErrorLine)
        *outErrorLine = headerLine;
    return false;
}

static bool ValidateDocument(const SceneDoc& doc, std::string* outError, int* outErrorLine = nullptr)
{
    if (outErrorLine)
        *outErrorLine = 0;

    if (doc.version != 1)
    {
        if (outError)
            *outError = "Unsupported version";
        return false;
    }

    if (doc.isBlueprint && doc.entities.empty())
    {
        if (outError)
            *outError = "Blueprint file must contain at least one [entity] section";
        if (outErrorLine)
            *outErrorLine = 1;
        return false;
    }

    // Embed/resource ids must not collide (both are referenced as #id).
    for (const auto& kv : doc.embeds)
    {
        if (doc.resources.find(kv.first) != doc.resources.end())
        {
            if (outError)
                *outError = "Duplicate id '" + kv.first + "' used for both [resource] and [embed]";
            if (outErrorLine)
                *outErrorLine = kv.second.headerLine;
            return false;
        }
    }

    // id -> header line of its first use, so the message can point at both sections.
    std::unordered_map<std::string, int> ids;
    ids.reserve(doc.entities.size() + doc.blueprintInstances.size());
    for (const auto& e : doc.entities)
    {
        if (!e.id.empty() && !ClaimEntityId(ids, e.id, e.headerLine, outError, outErrorLine))
            return false;
    }
    for (const auto& b : doc.blueprintInstances)
    {
        if (!b.id.empty() && !ClaimEntityId(ids, b.id, b.headerLine, outError, outErrorLine))
            return false;
    }

    // Parent refs must exist in the same file (for base entities and blueprint instance root).
    auto hasId = [&](const std::string& id) { return ids.find(id) != ids.end(); };
    for (const auto& e : doc.entities)
    {
        if (e.parent && *e.parent == e.id)
        {
            if (outError)
                *outError = "Entity '" + e.id + "' cannot parent itself";
            if (outErrorLine)
                *outErrorLine = e.headerLine;
            return false;
        }
        if (e.parent && !hasId(*e.parent))
        {
            if (outError)
                *outError = "Missing parent '" + *e.parent + "' for entity '" + e.id + "'";
            if (outErrorLine)
                *outErrorLine = e.headerLine;
            return false;
        }
    }
    for (const auto& b : doc.blueprintInstances)
    {
        if (b.parent && *b.parent == b.id)
        {
            if (outError)
                *outError = "Blueprint instance '" + b.id + "' cannot parent itself";
            if (outErrorLine)
                *outErrorLine = b.headerLine;
            return false;
        }
        if (b.parent && !hasId(*b.parent))
        {
            if (outError)
                *outError = "Missing parent '" + *b.parent + "' for blueprint instance '" + b.id + "'";
            if (outErrorLine)
                *outErrorLine = b.headerLine;
            return false;
        }
        if (!b.sourceResourceId.empty() && doc.resources.find(b.sourceResourceId) == doc.resources.end())
        {
            if (outError)
                *outError = "Missing resource '" + b.sourceResourceId + "' for blueprint instance '" + b.id + "'";
            if (outErrorLine)
                *outErrorLine = b.headerLine;
            return false;
        }
    }
    for (const auto& s : doc.subscenes)
    {
        if (!s.sourceResourceId.empty() && doc.resources.find(s.sourceResourceId) == doc.resources.end())
        {
            if (outError)
                *outError = "Missing resource '" + s.sourceResourceId + "' for subscene '" + s.id + "'";
            if (outErrorLine)
                *outErrorLine = s.headerLine;
            return false;
        }
    }

    // Included embed/resource ids are already merged into doc; just ensure embeds don't collide with resources ids? (allowed)

    // Cycle detection for base entities only (blueprint instance roots participate too).
    std::unordered_map<std::string, std::optional<std::string>> parentMap;
    for (const auto& e : doc.entities)
        parentMap[e.id] = e.parent;
    for (const auto& b : doc.blueprintInstances)
        parentMap[b.id] = b.parent;

    enum class Mark : uint8_t
    {
        None,
        Temp,
        Perm
    };
    std::unordered_map<std::string, Mark> marks;
    for (const auto& kv : parentMap)
        marks[kv.first] = Mark::None;

    std::function<bool(const std::string&)> dfs = [&](const std::string& id) -> bool
    {
        Mark& m = marks[id];
        if (m == Mark::Perm)
            return true;
        if (m == Mark::Temp)
            return false;
        m = Mark::Temp;
        const auto it = parentMap.find(id);
        if (it != parentMap.end() && it->second.has_value())
        {
            const std::string& p = *it->second;
            if (parentMap.find(p) != parentMap.end())
            {
                if (!dfs(p))
                    return false;
            }
        }
        m = Mark::Perm;
        return true;
    };

    for (const auto& kv : parentMap)
    {
        if (!dfs(kv.first))
        {
            if (outError)
                *outError = "Cycle detected in parent chain";
            if (outErrorLine)
                *outErrorLine = 0;
            return false;
        }
    }

    // Validate and scan value references in all parsed property values (schema-agnostic).
    auto hasResourceId = [&](const std::string& id) -> bool
    {
        return doc.resources.find(id) != doc.resources.end() || doc.embeds.find(id) != doc.embeds.end();
    };

    // A value this parser cannot read is only worth aborting the load for where nothing downstream
    // can report it. Entity and blueprint-instance component properties are applied tolerantly: the
    // apply records the failure in the census and keeps the authored text for the re-save, which is
    // strictly better than losing the whole scene over one line. Embed properties have no such
    // path, so a syntax error there still stops the load rather than failing later somewhere worse.
    //
    // Dangling entity and resource references stay fatal either way: those are structural claims
    // about the document, not values a component is free to decline.
    enum class BadValueSyntax
    {
        Fatal,
        DeferToApply
    };

    auto scanValueRefs = [&](const SceneDoc::PropValue& pv, const std::string& context,
                             BadValueSyntax onBadSyntax) -> bool
    {
        SceneValue v{};
        std::string perr;
        if (!ParseValue(pv.value, v, &perr))
        {
            if (onBadSyntax == BadValueSyntax::DeferToApply)
                return true;
            if (outError)
                *outError = "Invalid value syntax for " + context + " at line " + std::to_string(pv.line) + ": " + perr;
            if (outErrorLine)
                *outErrorLine = pv.line;
            return false;
        }

        std::function<bool(const SceneValue&)> walk = [&](const SceneValue& n) -> bool
        {
            if (n.Kind == SceneValueKind::EntityRef)
            {
                if (ids.find(n.StringValue) == ids.end())
                {
                    if (outError)
                        *outError = "Dangling entity reference '$" + n.StringValue + "' in " + context + " at line " + std::to_string(pv.line);
                    if (outErrorLine)
                        *outErrorLine = pv.line;
                    return false;
                }
            }
            else if (n.Kind == SceneValueKind::ResourceRef)
            {
                if (!hasResourceId(n.StringValue))
                {
                    if (outError)
                        *outError = "Dangling resource reference '#" + n.StringValue + "' in " + context + " at line " + std::to_string(pv.line);
                    if (outErrorLine)
                        *outErrorLine = pv.line;
                    return false;
                }
            }

            for (const auto& c : n.Items)
            {
                if (!walk(c))
                    return false;
            }
            return true;
        };

        return walk(v);
    };

    for (const auto& e : doc.entities)
    {
        for (const auto& comp : e.components)
        {
            for (const auto& propKv : comp.props)
            {
                const std::string& prop = propKv.first;
                const SceneDoc::PropValue& pv = propKv.second;
                if (!scanValueRefs(pv, "entity '" + e.id + "' " + comp.name + "." + prop,
                                   BadValueSyntax::DeferToApply))
                    return false;
            }
        }
    }
    for (const auto& b : doc.blueprintInstances)
    {
        for (const auto& comp : b.components)
        {
            for (const auto& propKv : comp.props)
            {
                const std::string& prop = propKv.first;
                const SceneDoc::PropValue& pv = propKv.second;
                if (!scanValueRefs(pv, "blueprint instance '" + b.id + "' " + comp.name + "." + prop,
                                   BadValueSyntax::DeferToApply))
                    return false;
            }
        }
    }
    for (const auto& embKv : doc.embeds)
    {
        const auto& emb = embKv.second;
        for (const auto& propKv : emb.properties)
        {
            const std::string& key = propKv.first;
            const SceneDoc::PropValue& pv = propKv.second;
            if (!scanValueRefs(pv, "embed '" + emb.id + "' " + key, BadValueSyntax::Fatal))
                return false;
        }
    }

    return true;
}

static Components::SceneEntityTag MakeTag(const std::string& s)
{
    Components::SceneEntityTag t{};
    std::memset(t.value, 0, sizeof(t.value));
    const size_t maxCopy = sizeof(t.value) - 1;
    const size_t toCopy = std::min(maxCopy, s.size());
    std::memcpy(t.value, s.data(), toCopy);
    t.value[toCopy] = '\0';
    return t;
}

static std::string GetTagString(const Components::SceneEntityTag& t)
{
    return std::string(t.View());
}

// Matches legacy EnsureTags ids: "e_" + decimal entity id (e.g. e_1048576). Used to avoid copying these
// into Name on load — they read as raw internal ids in the hierarchy.
static bool LooksLikeAutoGeneratedNumericSceneId(std::string_view id)
{
    if (id.size() < 3)
        return false;
    if (id[0] != 'e' || id[1] != '_')
        return false;
    for (size_t i = 2; i < id.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(id[i])))
            return false;
    }
    return true;
}

static std::string SlugifyDisplayNameForSceneEntityId(std::string_view raw)
{
    std::string s;
    s.reserve(raw.size());
    bool lastWasUnderscore = false;
    for (char ch : raw)
    {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u))
        {
            s += static_cast<char>(std::tolower(u));
            lastWasUnderscore = false;
        }
        else if (ch == ' ' || ch == '-' || ch == '.' || ch == '/')
        {
            if (!s.empty() && !lastWasUnderscore)
            {
                s += '_';
                lastWasUnderscore = true;
            }
        }
    }
    while (!s.empty() && s.back() == '_')
        s.pop_back();
    return s;
}

static std::string AllocateUniqueSceneEntityId(const std::string& preferred, std::unordered_set<std::string>& used)
{
    std::string base = preferred.empty() ? std::string("entity") : preferred;
    if (base.empty())
        base = "entity";
    std::string candidate = base;
    for (int suffix = 2; used.count(candidate) != 0; ++suffix)
        candidate = base + "_" + std::to_string(suffix);
    used.insert(candidate);
    return candidate;
}

static void EnsureTags(ECS::World& world, const std::vector<ECS::EntityHandle>& entities)
{
    // First pass: the first entity to claim a given id keeps it. `used` ends up holding every id
    // that is taken, so ids generated below never collide with an existing tag that appears later.
    // An entity with no tag — or whose tag duplicates one an earlier entity already claimed (e.g. a
    // Duplicate/Clone that copied the original's SceneEntityTag) — is queued for reassignment, so
    // saved scenes never contain duplicate entity ids (which the loader rejects).
    std::unordered_set<std::string> used;
    used.reserve(entities.size() * 2u);
    std::unordered_set<uint32_t> needsId;
    needsId.reserve(entities.size());
    for (auto h : entities)
    {
        // Runtime-only entities are never written, so they need no scene id — and
        // minting one is an archetype move per entity per save, on a population the
        // generators churn.
        if (world.HasComponent<Components::RuntimeOnlyEntity>(h))
            continue;
        const auto* tag = world.GetComponent<Components::SceneEntityTag>(h);
        const std::string s = tag ? GetTagString(*tag) : std::string();
        if (!s.empty() && used.insert(s).second)
            continue;
        needsId.insert(h.index);
    }

    for (auto h : entities)
    {
        if (needsId.find(h.index) == needsId.end())
            continue;

        std::string newId;
        if (const auto* name = world.GetComponent<Components::Name>(h))
        {
            if (name->value[0] != '\0')
            {
                const std::string slug = SlugifyDisplayNameForSceneEntityId(name->View());
                if (!slug.empty())
                    newId = AllocateUniqueSceneEntityId(slug, used);
            }
        }
        if (newId.empty())
        {
            newId = "e_" + std::to_string(h.id);
            if (used.count(newId))
                newId = AllocateUniqueSceneEntityId(newId, used);
            else
                used.insert(newId);
        }

        // Overwrite a duplicate tag in place; add one when the entity had none.
        if (auto* tag = world.GetComponentForWrite<Components::SceneEntityTag>(h))
            *tag = MakeTag(newId);
        else
            world.AddComponentImmediate(h, MakeTag(newId));
    }
}

// Editor hierarchy sorts siblings by HierarchyOrder then entity id. Re-dense orders per parent group
// (0,10,20,...) before writing .scene so sibling order survives round-trip; SaveSceneToFile emits
// entity blocks depth-first sorted by HierarchyOrder (see SortEntitiesDepthFirstForSave).
static void NormalizeHierarchySiblingOrderForSave(ECS::World& world, const std::vector<ECS::EntityHandle>& entities)
{
    std::unordered_set<uint32_t> alive;
    alive.reserve(entities.size() * 2u);
    for (auto h : entities)
        alive.insert(h.index);

    std::unordered_map<uint32_t, std::vector<ECS::EntityHandle>> childrenByParent;
    childrenByParent.reserve(entities.size());
    std::vector<ECS::EntityHandle> roots;
    roots.reserve(entities.size() / 4u + 1u);

    for (auto h : entities)
    {
        // Runtime-only entities are not written, so they must not consume ordinals:
        // they would make an authored sibling's saved order depend on how many
        // pieces a generator happened to emit, rewriting the file on a change the
        // user never made and cannot see.
        if (world.HasComponent<Components::RuntimeOnlyEntity>(h))
            continue;
        if (const auto* par = world.GetComponent<Components::Parent>(h))
        {
            if (par->parent.IsValid() && world.IsValid(par->parent) && alive.count(par->parent.index) != 0)
            {
                childrenByParent[par->parent.index].push_back(h);
                continue;
            }
        }
        roots.push_back(h);
    }

    auto orderLess = [&](ECS::EntityHandle a, ECS::EntityHandle b) -> bool {
        std::int32_t oa = 0;
        std::int32_t ob = 0;
        if (const auto* o = world.GetComponent<Components::HierarchyOrder>(a))
            oa = o->order;
        if (const auto* o = world.GetComponent<Components::HierarchyOrder>(b))
            ob = o->order;
        if (oa != ob)
            return oa < ob;
        return a.id < b.id;
    };

    auto sortGroup = [&](std::vector<ECS::EntityHandle>& v) { std::stable_sort(v.begin(), v.end(), orderLess); };

    sortGroup(roots);
    for (auto& kv : childrenByParent)
        sortGroup(kv.second);

    auto assignDense = [&](const std::vector<ECS::EntityHandle>& group) {
        for (size_t i = 0; i < group.size(); ++i)
        {
            Components::HierarchyOrder ho{};
            ho.order = static_cast<std::int32_t>(static_cast<int>(i) * 10);
            world.AddComponentImmediate(group[i], ho);
        }
    };

    assignDense(roots);
    for (auto& kv : childrenByParent)
        assignDense(kv.second);
}

// Reorder entity handles depth-first pre-order (parent before descendants, siblings sorted by HierarchyOrder/id).
// Used after NormalizeHierarchySiblingOrderForSave so the written .scene reflects the Hierarchy panel ordering
// rather than lexical tag sorting.
static void DepthFirstPreorderAppend(
    ECS::EntityHandle h,
    const std::unordered_map<uint32_t, std::vector<ECS::EntityHandle>>& childrenByParent,
    std::vector<ECS::EntityHandle>& out)
{
    out.push_back(h);
    auto it = childrenByParent.find(h.index);
    if (it == childrenByParent.end())
        return;
    for (ECS::EntityHandle child : it->second)
        DepthFirstPreorderAppend(child, childrenByParent, out);
}

static void SortEntitiesDepthFirstForSave(ECS::World& world, std::vector<ECS::EntityHandle>& entities)
{
    if (entities.size() <= 1)
        return;

    std::unordered_set<uint32_t> alive;
    alive.reserve(entities.size() * 2u);
    for (auto h : entities)
        alive.insert(h.index);

    std::unordered_map<uint32_t, std::vector<ECS::EntityHandle>> childrenByParent;
    childrenByParent.reserve(entities.size());
    std::vector<ECS::EntityHandle> roots;
    roots.reserve(entities.size() / 4u + 1u);

    for (auto h : entities)
    {
        if (const auto* par = world.GetComponent<Components::Parent>(h))
        {
            if (par->parent.IsValid() && world.IsValid(par->parent) && alive.count(par->parent.index) != 0)
            {
                childrenByParent[par->parent.index].push_back(h);
                continue;
            }
        }
        roots.push_back(h);
    }

    auto orderLess = [&](ECS::EntityHandle a, ECS::EntityHandle b) -> bool {
        std::int32_t oa = 0;
        std::int32_t ob = 0;
        if (const auto* o = world.GetComponent<Components::HierarchyOrder>(a))
            oa = o->order;
        if (const auto* o = world.GetComponent<Components::HierarchyOrder>(b))
            ob = o->order;
        if (oa != ob)
            return oa < ob;
        return a.id < b.id;
    };
    auto sortGroup = [&](std::vector<ECS::EntityHandle>& v) {
        std::stable_sort(v.begin(), v.end(), orderLess);
    };

    sortGroup(roots);
    for (auto& kv : childrenByParent)
        sortGroup(kv.second);

    std::vector<ECS::EntityHandle> out;
    out.reserve(entities.size());
    for (ECS::EntityHandle r : roots)
        DepthFirstPreorderAppend(r, childrenByParent, out);

    if (out.size() != entities.size())
    {
        // Unusual graphs (cycles, inconsistent parenting); append leftovers deterministically.
        std::unordered_set<uint32_t> seenIdx;
        seenIdx.reserve(out.size());
        for (auto h : out)
            seenIdx.insert(h.index);

        std::vector<ECS::EntityHandle> missing;
        missing.reserve(entities.size() - out.size());
        for (auto h : entities)
            if (!seenIdx.count(h.index))
                missing.push_back(h);
        std::stable_sort(missing.begin(), missing.end(),
                           [](ECS::EntityHandle a, ECS::EntityHandle b) {
                               return a.id < b.id;
                           });
        out.insert(out.end(), missing.begin(), missing.end());
    }

    entities = std::move(out);
}

static std::filesystem::path ResolveResourcePath(const SceneLoadContext& ctx,
                                                 const std::filesystem::path& ownerFile,
                                                 const SceneDoc::Resource& r)
{
    std::filesystem::path p;
    if (!r.guid.empty())
    {
        if (ctx.Resolver)
        {
            try
            {
                GUID g(r.guid);
                g = ctx.Resolver->ResolveGuid(g);
                std::filesystem::path resolvedPath;
                AssetType resolvedType = AssetType::Unknown;
                if (ctx.Resolver->TryGetPathAndType(g, resolvedPath, resolvedType))
                    p = resolvedPath;
            }
            catch (...)
            {
            }
        }
    }
    if (p.empty() && !r.path.empty())
    {
        // Resource paths are relative to a mount root, searched in mount order.
        p = ResolveAuthoredAssetPath(ctx, ownerFile, std::filesystem::path(r.path));
    }
    return p;
}

struct Instantiated
{
    std::vector<ECS::EntityHandle> created;
    std::unordered_map<std::string, ECS::EntityHandle> idToEntity;
};

// A blueprint or subscene instance whose [resource] file the load cannot read. The error belongs to
// `ownerFile` at the instance's line, which is where the load stopped. The message says which of the
// three causes it is, names the path the loader searched (relative to `projectRoot` when it lies
// inside it), names both lines involved (the instance and the [resource] that declares the path),
// and says what to change. The resolved absolute path goes to the Debug log only.
static void ReportUnreadableResource(std::string_view kind, const SceneDoc::Resource& r,
                                     const std::filesystem::path& resolvedPath,
                                     const std::filesystem::path& projectRoot,
                                     const std::filesystem::path& ownerFile, int instanceLine,
                                     std::string* outError, std::filesystem::path* outErrorFile,
                                     int* outErrorLine)
{
    const std::filesystem::path relative = RelativePathUnderRoot(resolvedPath, projectRoot);
    const std::string searched = (relative.empty() ? resolvedPath : relative).generic_string();
    const std::string instance = ToLowerAscii(kind) + " on line " + std::to_string(instanceLine);
    const std::string declaringLine = "line " + std::to_string(r.headerLine);
    const std::string uses = " The " + instance + " uses resource \"" + r.id + "\", which " +
                             declaringLine + " declares. ";
    const std::string orRemove = ", or remove the " + instance + ", then open the scene again.";
    // A resource that resolved to no file at all: name it as the scene spells it, the path or the
    // guid of a guid-only resource.
    const std::string named = r.path.empty() ? "guid " + r.guid : r.path;
    std::error_code ec;
    std::string message;
    if (resolvedPath.empty())
        message = std::string(kind) + " file not found: " + named + " matches no file in the project." + uses +
                  "Change " + declaringLine + " to name an existing " + ToLowerAscii(kind) + orRemove;
    else if (!std::filesystem::exists(resolvedPath, ec))
        message = std::string(kind) + " file not found at " + searched + "." + uses + "Restore the file, change " +
                  declaringLine + orRemove;
    else
        message = std::string(kind) + " file could not be read at " + searched + "." + uses +
                  "Make the file readable, change " + declaringLine + orRemove;

    // Resolution detail only: the message travels in the error record, and the
    // caller that owns the failed load logs it once.
    Logger::Log::Debug("Scene: the {} in '{}' resolved resource \"{}\" to '{}'.", instance, ownerFile.string(),
                       r.id, resolvedPath.string());
    if (outError)
        *outError = std::move(message);
    if (outErrorFile)
        *outErrorFile = ownerFile;
    if (outErrorLine)
        *outErrorLine = instanceLine;
}

static bool ExpandIncludes(const std::filesystem::path& ownerFile,
                           SceneDoc& doc,
                           std::unordered_set<std::string>& visited,
                           std::string* outError,
                           std::filesystem::path* outErrorFile = nullptr,
                           int* outErrorLine = nullptr)
{
    const std::string key = ownerFile.lexically_normal().string();
    if (visited.count(key) > 0)
    {
        if (outError)
            *outError = "Include cycle detected at '" + ownerFile.string() + "'";
        if (outErrorFile)
            *outErrorFile = ownerFile;
        if (outErrorLine)
            *outErrorLine = 0;
        return false;
    }
    visited.insert(key);

    for (const auto& inc : doc.includes)
    {
        std::string text;
        if (!ReadTextFile(inc.path, text))
        {
            if (outError)
                *outError = "Failed to read include '" + inc.path.string() + "' (referenced at line " + std::to_string(inc.line) + ")";
            if (outErrorFile)
                *outErrorFile = inc.path;
            if (outErrorLine)
                *outErrorLine = inc.line;
            return false;
        }

        SceneDoc included{};
        std::string err;
        int parseLine = 0;
        if (!ParseDocument(inc.path, text, included, &err, &parseLine))
        {
            if (outError)
                *outError = "Failed to parse include '" + inc.path.string() + "': " + err;
            if (outErrorFile)
                *outErrorFile = inc.path;
            if (outErrorLine)
                *outErrorLine = parseLine != 0 ? parseLine : inc.line;
            return false;
        }

        // Spec: includes only merge resources/embeds (no entities/blueprints/subscenes).
        if (!included.entities.empty() || !included.blueprintInstances.empty() || !included.subscenes.empty())
        {
            const int ln = !included.entities.empty() ? included.entities.front().headerLine :
                           !included.blueprintInstances.empty() ? included.blueprintInstances.front().headerLine :
                                                                  included.subscenes.front().headerLine;
            if (outError)
                *outError = "Included file '" + inc.path.string() + "' contains entity content; [include] only merges [resource] and [embed]";
            if (outErrorFile)
                *outErrorFile = inc.path;
            if (outErrorLine)
                *outErrorLine = ln;
            return false;
        }

        if (!ExpandIncludes(inc.path, included, visited, &err, outErrorFile, outErrorLine))
        {
            if (outError)
                *outError = err;
            return false;
        }

        // Merge resources
        for (auto& kv : included.resources)
        {
            const std::string& id = kv.first;
            if (doc.resources.find(id) != doc.resources.end())
            {
                if (outError)
                    *outError = "Duplicate resource id '" + id + "' via include '" + inc.path.string() + "'";
                if (outErrorFile)
                    *outErrorFile = inc.path;
                if (outErrorLine)
                    *outErrorLine = inc.line;
                return false;
            }
            doc.resources.emplace(id, std::move(kv.second));
        }

        // Merge embeds
        for (auto& kv : included.embeds)
        {
            const std::string& id = kv.first;
            if (doc.embeds.find(id) != doc.embeds.end())
            {
                if (outError)
                    *outError = "Duplicate embed id '" + id + "' via include '" + inc.path.string() + "'";
                if (outErrorFile)
                    *outErrorFile = inc.path;
                if (outErrorLine)
                    *outErrorLine = inc.line;
                return false;
            }
            doc.embeds.emplace(id, std::move(kv.second));
        }
    }

    return true;
}

// Whether a save would emit a line for this field at all. False when the component emits none (it
// failed to materialize, or the serializer omits the value), which is the loader's signal that there
// is nothing for a save to substitute the authored text into — so the text cannot be preserved.
static bool FieldHasSerializedLine(const ECS::World& world, ECS::EntityHandle h,
                                   const ISceneComponentSchema& schema,
                                   const std::string& loweredProp)
{
    SceneSaveContext stubCtx{};
    std::vector<std::string> lines;
    schema.Serialize(world, h, stubCtx, lines);
    for (const std::string& l : lines)
    {
        std::string comp, prop, value;
        if (ParseComponentLine(l, comp, prop, value) && prop == loweredProp)
            return true;
    }
    return false;
}

// Where a reflected field lives inside its component, matched case-insensitively against the
// authored property key. Size 0 means the component has no reflected field table entry for it (a
// hand-written schema), which sends the override to the serialized-string tier instead.
static bool FindReflectedFieldSpan(ECS::ComponentTypeId typeId, const std::string& loweredProp,
                                   std::uint32_t& outOffset, std::uint32_t& outSize)
{
    outOffset = 0;
    outSize = 0;
    if (typeId == 0)
        return false;
    for (const ECS::FieldInfo& f : ECS::ComponentFieldRegistry::Get(typeId))
    {
        if (ToLowerAscii(std::string(f.Name)) != loweredProp)
            continue;
        outOffset = f.Offset;
        outSize = f.Size;
        return f.Size != 0;
    }
    return false;
}

// Apply one component's authored properties, tolerating individual failures.
//
// The grouped apply runs first and is the only path a clean scene takes, so nothing here costs a
// healthy load anything. When it fails, the component is re-applied property by property: every
// property the schema accepts lands, each one it rejects is recorded in `degradation` and its
// authored text preserved so a save re-emits it verbatim, and the load continues.
//
// Re-applying individually is what makes the good data survive: the grouped apply parses into a
// scratch byte buffer and discards it on the first bad value, so a single unknown enumerator would
// otherwise cost the component EVERY one of its fields, not just the offending one.
static void ApplyComponentPropertiesTolerantly(ECS::World& world, const SceneLoadContext& ctx,
                                               const ISceneComponentSchema& schema, ECS::EntityHandle h,
                                               const SceneDoc::ComponentProps& comp,
                                               const std::string& entityId,
                                               const std::filesystem::path& ownerFile,
                                               SceneLoadDegradation* degradation)
{
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    pairs.reserve(comp.props.size());
    for (const auto& propKv : comp.props)
        pairs.emplace_back(propKv.first, propKv.second.value);

    std::string err;
    std::size_t failedIndex = 0;
    if (schema.ApplyProperties(world, h, ctx, pairs, &err, &failedIndex))
        return;

    const std::string& authoredComp = !comp.originalName.empty() ? comp.originalName : comp.name;
    for (const auto& propKv : comp.props)
    {
        std::string propErr;
        if (schema.ApplyProperty(world, h, ctx, propKv.first, propKv.second.value, &propErr))
            continue;

        const std::string authoredProp =
            !propKv.second.originalKey.empty() ? propKv.second.originalKey : propKv.first;

        ECS::PreservedField pf;
        pf.Component = authoredComp;
        pf.Field = authoredProp;
        pf.RawText = propKv.second.value;
        pf.TypeId = ECS::ComponentFieldRegistry::FindByName(comp.name);

        // The fallback is sampled HERE, immediately after the failed apply, because this is the
        // moment the field holds what the apply fell back to. It is a byte range of this field
        // alone, so nothing that happens to the entity or its neighbours afterwards disturbs it.
        // A component with no reflected field table leaves FieldSize 0 and falls to the string
        // tier, whose sample can only be taken later, from a real save context.
        if (FindReflectedFieldSpan(pf.TypeId, propKv.first, pf.FieldOffset, pf.FieldSize))
        {
            std::vector<std::uint8_t> fallback;
            if (ECS::CapturePreservedFieldBytes(world, h, pf, fallback))
                pf.FallbackBytes = std::move(fallback);
            else
                pf.FieldSize = 0; // component absent or shorter than its table claims
        }

        // No line to substitute into means the text cannot survive a save; say so in the census
        // rather than implying a round-trip that will not happen.
        const bool preserved = pf.TypeId != 0 && FieldHasSerializedLine(world, h, schema, propKv.first);
        if (preserved)
            world.GetUnresolvedComponents().AddField(h, std::move(pf));

        if (degradation)
            degradation->skips.push_back(SceneLoadSkip{entityId, authoredComp, authoredProp, propErr,
                                                       ownerFile, propKv.second.line, preserved, h});
    }
}

// A component block whose name resolves to no schema. Three cases, told apart through the field
// registry: a name resolving to no registered type is genuinely unknown (a user module not loaded
// yet, or a foreign component) -> PRESERVE verbatim so a save round-trips it and the editor
// re-applies it once the type registers; a registered [DoNotSerialize] type (worldtransform,
// meshgpudata) -> DROP; a reflected type a hand-written schema writes under another token (a
// SplineComponent block beside the Spline block, both written by a build whose reflection fallback
// did not know the token owned the type) -> DROP with a warning, since the token's block carries the
// data and the next save no longer emits this one.
static void PreserveOrDropUnhandledComponent(ECS::World& world, ECS::EntityHandle h,
                                             const SceneDoc::ComponentProps& comp,
                                             const std::string& entityId,
                                             const std::filesystem::path& ownerFile)
{
    const std::string& compName = comp.name;
    // Preservation and messages echo the AUTHORED spelling: the lowered form is a matching key,
    // and re-emitting it would case-normalize the user's file.
    const std::string& authoredName = !comp.originalName.empty() ? comp.originalName : compName;
    const ECS::ComponentTypeId tid = ECS::ComponentFieldRegistry::FindByName(compName);
    if (tid == 0)
    {
        ECS::PreservedComponent pc;
        pc.Name = authoredName;
        pc.Props.reserve(comp.props.size());
        for (const auto& propKv : comp.props)
            pc.Props.emplace_back(!propKv.second.originalKey.empty() ? propKv.second.originalKey
                                                                     : propKv.first,
                                  propKv.second.value);
        Logger::Log::Warning("Scene: no registered type is named '{}' (on '{}'): its values are kept and "
                             "applied when a module that registers it loads; until then it does nothing, and if "
                             "no module does, remove it from the scene",
                             pc.Name, entityId);
        world.GetUnresolvedComponents().Add(h, std::move(pc));
        return;
    }
    if (ECS::ComponentFieldRegistry::IsComponentDoNotSerialize(tid))
    {
        Logger::Log::Debug("Scene: dropping non-serializable component '{}' in '{}'", compName,
                           ownerFile.string());
        return;
    }
    const ISceneComponentSchema* owner = SceneSchemaRegistry::FindForComponentType(tid);
    Logger::Log::Warning("Scene: dropping '{}' on '{}' in '{}': this build saves the component as '{}', "
                         "whose block carries its data; resave the scene to clear the stale block",
                         authoredName, entityId, ownerFile.string(),
                         owner ? owner->GetComponentName() : std::string_view{});
}

// The key of the line that carries a component's switch, "Token.enabled = false". The parser
// lowercases keys, so "Token.Enabled" reads the same.
constexpr std::string_view kComponentSwitchKey = "enabled";

// Applies one authored component block. A component that switches through its
// ECS::ComponentDisabled tag (ComponentRegistry::SwitchesThroughDisabledTag) has its switch line
// taken out before the schema sees the block and applied once the component exists, so every
// schema reads the switch the same way and none has to know about it. A switch line whose value
// is not a bool stays in the block for the schema to refuse and the census to record. A block
// with no properties, or none left once the switch line is out, adds the component's defaults.
static void ApplyComponentBlock(ECS::World& world, const SceneLoadContext& ctx,
                                const ISceneComponentSchema& schema, ECS::EntityHandle h,
                                const SceneDoc::ComponentProps& authored, const std::string& entityId,
                                const std::filesystem::path& ownerFile, int headerLine,
                                SceneLoadDegradation* degradation)
{
    const ECS::ComponentTypeId typeId = SceneSchemaRegistry::ComponentTypeForSchema(schema);
    std::optional<bool> enabled;
    SceneDoc::ComponentProps withoutSwitch;
    const SceneDoc::ComponentProps* block = &authored;
    if (ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
    {
        for (std::size_t i = 0; i < authored.props.size(); ++i)
        {
            if (authored.props[i].first != kComponentSwitchKey)
                continue;
            SceneValue parsed;
            if (ParseValue(authored.props[i].second.value, parsed) && parsed.Kind == SceneValueKind::Bool)
            {
                enabled = parsed.BoolValue;
                withoutSwitch = authored;
                withoutSwitch.props.erase(withoutSwitch.props.begin() + static_cast<std::ptrdiff_t>(i));
                block = &withoutSwitch;
            }
            break;
        }
    }

    // A block with no properties adds the component's defaults, and so does a block whose switch
    // line was taken: the switch is the component's own state, so the component exists even when
    // every other line of its block is refused, and a refused line has a component to be
    // preserved against. Only a block nothing else will materialize records the failure as a
    // drop — a component that cannot even be default-constructed has no text to preserve.
    if (block->props.empty() || (enabled && !world.HasComponent(h, typeId)))
    {
        std::string addErr;
        if (!schema.AddDefault(world, h, &addErr) && block->props.empty() && degradation)
            degradation->skips.push_back(SceneLoadSkip{
                entityId, !authored.originalName.empty() ? authored.originalName : authored.name, {},
                addErr, ownerFile, headerLine, /*preserved=*/false, h});
    }
    if (!block->props.empty())
        ApplyComponentPropertiesTolerantly(world, ctx, schema, h, *block, entityId, ownerFile, degradation);

    if (enabled && !*enabled && world.HasComponent(h, typeId))
        world.SetComponentEnabledImmediate(h, typeId, false);
}

// Applies every component authored on one entity. Cannot fail: a component or property this binary
// cannot apply is recorded in `degradation` and skipped, never propagated as a load failure.
static void ApplyEntityComponents(ECS::World& world,
                                 const SceneLoadContext& ctx,
                                 const std::filesystem::path& ownerFile,
                                 ECS::EntityHandle h,
                                 const SceneDoc::Entity& e,
                                 const std::string& entityId,
                                 SceneLoadDegradation* degradation)
{
    world.AddComponentImmediate(h, MakeTag(entityId));

    for (const auto& comp : e.components)
    {
        const std::string& compName = comp.name;
        const auto* schema = SceneSchemaRegistry::Find(compName);
        if (!schema)
        {
            PreserveOrDropUnhandledComponent(world, h, comp, entityId, ownerFile);
            continue;
        }

        ApplyComponentBlock(world, ctx, *schema, h, comp, entityId, ownerFile, e.headerLine, degradation);
    }

    // Hierarchy uses Name; scene files may omit it. Fill from the scene id
    // string for human-authored ids (e.g. main_camera). Skip auto-generated
    // technical ids (e_123) so we do not persist internal-looking labels —
    // the hierarchy falls back to "Entity <id>" instead.
    //
    // Routed through the Name schema so the WRITE goes through schema
    // validation rather than constructing the component shape inline.
    // IsPresent returns false for both absent and empty Name (the Name
    // schema's Serialize skips emission in either case), so a single check
    // captures the original "needs fallback" semantic.
    if (!LooksLikeAutoGeneratedNumericSceneId(entityId))
    {
        const auto* nameSchema = SceneSchemaRegistry::Find("Name");
        if (nameSchema && !nameSchema->IsPresent(world, h))
        {
            std::string err;
            const std::string quotedId = std::string("\"") + entityId + "\"";
            if (!nameSchema->ApplyProperty(world, h, ctx, "value", quotedId, &err))
            {
                // Soft-fail: a malformed id at this point would mean the
                // entity loads without a Name fallback. Log and continue.
                Logger::Log::Warning("Scene: failed to set fallback Name for '{}': {}",
                                     entityId, err);
            }
        }
    }
}

static bool InstantiateEntities(ECS::World& world,
                               const SceneLoadContext& ctx,
                               const std::filesystem::path& ownerFile,
                               const SceneDoc& doc,
                               const std::string& idPrefix,
                               Instantiated& out,
                               std::string* outError,
                               std::filesystem::path* outErrorFile,
                               int* outErrorLine,
                               SceneLoadDegradation* degradation)
{
    // Create entities first
    for (const auto& e : doc.entities)
    {
        const std::string fullId = idPrefix.empty() ? e.id : (idPrefix + e.id);
        ECS::EntityHandle h = world.CreateEntity();
        if (!h.IsValid())
        {
            if (outError) *outError = "Failed to create entity";
            if (outErrorFile)
                *outErrorFile = ownerFile;
            if (outErrorLine)
                *outErrorLine = e.headerLine;
            return false;
        }
        out.created.push_back(h);
        out.idToEntity[fullId] = h;
    }

    // Apply components. The full idToEntity map exists now, so EntityHandle fields
    // (e.g. SkyEnvironment.SunLight) resolve references — including forward ones —
    // directly against it.
    SceneLoadContext applyCtx = ctx;
    applyCtx.EntityIdMap = &out.idToEntity;
    applyCtx.EntityIdPrefix = idPrefix;
    for (size_t i = 0; i < doc.entities.size(); ++i)
    {
        const SceneDoc::Entity& e = doc.entities[i];
        const std::string fullId = idPrefix.empty() ? e.id : (idPrefix + e.id);
        ApplyEntityComponents(world, applyCtx, ownerFile, out.created[i], e, fullId, degradation);
    }

    // Parent links
    for (size_t i = 0; i < doc.entities.size(); ++i)
    {
        const SceneDoc::Entity& e = doc.entities[i];
        if (!e.parent)
        {
            world.RemoveComponentImmediate<Components::Parent>(out.created[i]);
            continue;
        }
        const std::string parentId = idPrefix + *e.parent;
        auto it = out.idToEntity.find(parentId);
        if (it == out.idToEntity.end())
        {
            if (outError) *outError = "Missing parent during instantiation";
            if (outErrorFile)
                *outErrorFile = ownerFile;
            if (outErrorLine)
                *outErrorLine = e.headerLine;
            return false;
        }
        Components::Parent p{};
        p.parent = it->second;
        world.AddComponentImmediate(out.created[i], p);
    }
    return true;
}

static void ApplyOffsetToRoots(ECS::World& world, const Instantiated& inst, float ox, float oy, float oz)
{
    std::unordered_set<ECS::EntityId> createdIds;
    createdIds.reserve(inst.created.size());
    for (auto h : inst.created)
        createdIds.insert(h.id);

    for (auto h : inst.created)
    {
        auto* parent = world.GetComponent<Components::Parent>(h);
        const bool hasLocalParent = parent && parent->parent.IsValid() && createdIds.count(parent->parent.id) > 0;
        if (hasLocalParent)
            continue;
        auto* t = world.GetComponentForWrite<Components::Transform>(h);
        if (!t)
            continue;
        t->Translate(ox, oy, oz);
    }
}

static void ParentRootsTo(ECS::World& world, const Instantiated& inst, ECS::EntityHandle parentEntity)
{
    std::unordered_set<ECS::EntityId> createdIds;
    createdIds.reserve(inst.created.size());
    for (auto h : inst.created)
        createdIds.insert(h.id);

    for (auto h : inst.created)
    {
        auto* parent = world.GetComponent<Components::Parent>(h);
        const bool hasLocalParent = parent && parent->parent.IsValid() && createdIds.count(parent->parent.id) > 0;
        if (hasLocalParent)
            continue;

        Components::Parent p{};
        p.parent = parentEntity;
        world.AddComponentImmediate(h, p);
    }
}

static bool LoadParsedIntoWorld(ECS::World& world,
                               const std::filesystem::path& sceneFilePath,
                               const SceneDoc& doc,
                               const LoadOptions& options,
                               const std::string& idPrefix,
                               Instantiated& out,
                               std::string* outError,
                               std::filesystem::path* outErrorFile,
                               int* outErrorLine,
                               SceneLoadDegradation* degradation)
{
    // Build the per-load context. Threaded through schema dispatch and
    // SceneIO helpers (TryResolveAssetReference et al) by const reference.
    SceneLoadContext localCtx{};
    localCtx.SceneFile = &sceneFilePath;
    localCtx.TargetWorld = &world;
    localCtx.Resolver = options.assetResolver;
    localCtx.AssetRoot = !options.assetRootOverride.empty()
                             ? options.assetRootOverride
                             : (localCtx.Resolver ? localCtx.Resolver->GetAssetRoot() : std::filesystem::path{});
    localCtx.EmbedMaterializer = options.embedMaterializer;
    localCtx.ResourceAbsPathById.reserve(doc.resources.size());
    localCtx.ResourceGuidById.reserve(doc.resources.size());
    for (const auto& kv : doc.resources)
    {
        localCtx.ResourceAbsPathById[kv.first] = ResolveResourcePath(localCtx, sceneFilePath, kv.second);
        localCtx.ResourceGuidById[kv.first] = kv.second.guid;
    }
    localCtx.EmbedsById.reserve(doc.embeds.size());
    for (const auto& kv : doc.embeds)
    {
        SceneLoadContext::EmbedDef e{};
        e.Type = kv.second.type;
        e.Guid = kv.second.guid;
        e.Properties.reserve(kv.second.properties.size());
        for (const auto& pkv : kv.second.properties)
        {
            e.Properties[pkv.first] = pkv.second.value;
        }
        localCtx.EmbedsById.emplace(kv.first, std::move(e));
    }

    // Base entities
    if (!InstantiateEntities(world, localCtx, sceneFilePath, doc, idPrefix, out, outError, outErrorFile, outErrorLine, degradation))
        return false;

    // Blueprint instances (scene-only)
    if (!doc.isBlueprint)
    {
        for (const auto& bi : doc.blueprintInstances)
        {
            auto rit = doc.resources.find(bi.sourceResourceId);
            if (rit == doc.resources.end())
                continue;

            const std::filesystem::path bpPath = ResolveResourcePath(localCtx, sceneFilePath, rit->second);
            std::string bpText;
            if (bpPath.empty() || !ReadTextFile(bpPath, bpText))
            {
                ReportUnreadableResource("Blueprint", rit->second, bpPath, options.projectRoot, sceneFilePath,
                                         bi.headerLine, outError, outErrorFile, outErrorLine);
                return false;
            }
            SceneDoc bpDoc{};
            {
                int bpParseLine = 0;
                if (!ParseDocument(bpPath, bpText, bpDoc, outError, &bpParseLine))
                {
                    if (outErrorFile)
                        *outErrorFile = bpPath;
                    if (outErrorLine)
                        *outErrorLine = bpParseLine;
                    return false;
                }
            }
            {
                int bpValLine = 0;
                if (!ValidateDocument(bpDoc, outError, &bpValLine))
                {
                    if (outErrorFile)
                        *outErrorFile = bpPath;
                    if (outErrorLine)
                        *outErrorLine = bpValLine;
                    return false;
                }
            }

            // Instantiate blueprint entities with prefix "instanceId."
            const std::string instPrefix = idPrefix + bi.id + ".";
            Instantiated bpInst{};
            {
                SceneLoadContext bpCtx{};
                bpCtx.SceneFile = &bpPath;
                bpCtx.TargetWorld = &world;
                bpCtx.Resolver = options.assetResolver;
                bpCtx.AssetRoot = localCtx.AssetRoot;
                bpCtx.EmbedMaterializer = options.embedMaterializer;
                bpCtx.ResourceAbsPathById.reserve(bpDoc.resources.size());
                bpCtx.ResourceGuidById.reserve(bpDoc.resources.size());
                for (const auto& kv : bpDoc.resources)
                {
                    bpCtx.ResourceAbsPathById[kv.first] = ResolveResourcePath(bpCtx, bpPath, kv.second);
                    bpCtx.ResourceGuidById[kv.first] = kv.second.guid;
                }
                bpCtx.EmbedsById.reserve(bpDoc.embeds.size());
                for (const auto& kv : bpDoc.embeds)
                {
                    SceneLoadContext::EmbedDef e{};
                    e.Type = kv.second.type;
                    e.Guid = kv.second.guid;
                    e.Properties.reserve(kv.second.properties.size());
                    for (const auto& pkv : kv.second.properties)
                    {
                        e.Properties[pkv.first] = pkv.second.value;
                    }
                    bpCtx.EmbedsById.emplace(kv.first, std::move(e));
                }

                if (!InstantiateEntities(world, bpCtx, bpPath, bpDoc, instPrefix, bpInst, outError, outErrorFile, outErrorLine, degradation))
                    return false;
            }

            // Create the root instance entity (bi.id) as the blueprint root mapping:
            // We model this by renaming the blueprint's "root" entity tag to bi.id if it exists,
            // otherwise we create a new root and parent all blueprint roots under it.
            ECS::EntityHandle rootEntity{};
            auto itRoot = bpInst.idToEntity.find(instPrefix + "root");
            if (itRoot != bpInst.idToEntity.end())
            {
                rootEntity = itRoot->second;
                // overwrite tag to instance id
                world.AddComponentImmediate(rootEntity, MakeTag(idPrefix + bi.id));
                bpInst.idToEntity[idPrefix + bi.id] = rootEntity;
            }
            else
            {
                rootEntity = world.CreateEntity();
                if (!rootEntity.IsValid())
                {
                    if (outError)
                        *outError = "Failed to create blueprint instance root entity";
                    if (outErrorFile)
                        *outErrorFile = sceneFilePath;
                    if (outErrorLine)
                        *outErrorLine = bi.headerLine;
                    return false;
                }
                world.AddComponentImmediate(rootEntity, MakeTag(idPrefix + bi.id));
                bpInst.idToEntity[idPrefix + bi.id] = rootEntity;
            }

            // Tag instance root for re-serialization.
            {
                Components::SceneBlueprintInstance inst{};
                std::memset(inst.sourcePath, 0, sizeof(inst.sourcePath));
                std::memset(inst.sourceGuid, 0, sizeof(inst.sourceGuid));
                const auto& res = rit->second;
                const size_t maxPath = sizeof(inst.sourcePath) - 1;
                const size_t maxGuid = sizeof(inst.sourceGuid) - 1;
                if (!res.path.empty())
                {
                    const size_t n = std::min(maxPath, res.path.size());
                    std::memcpy(inst.sourcePath, res.path.data(), n);
                    inst.sourcePath[n] = '\0';
                }
                if (!res.guid.empty())
                {
                    const size_t n = std::min(maxGuid, res.guid.size());
                    std::memcpy(inst.sourceGuid, res.guid.data(), n);
                    inst.sourceGuid[n] = '\0';
                }
                world.AddComponentImmediate(rootEntity, inst);
            }

            // Apply overrides to root entity
            for (const auto& compName : bi.removedComponents)
            {
                const auto* schema = SceneSchemaRegistry::Find(compName);
                if (!schema)
                {
                    if (outError) *outError = "Unknown component '" + compName + "' in remove directive";
                    if (outErrorFile)
                        *outErrorFile = sceneFilePath;
                    if (outErrorLine)
                        *outErrorLine = bi.headerLine;
                    return false;
                }
                if (!schema->Remove(world, rootEntity))
                {
                    if (outError)
                        *outError = "Component '" + compName + "' does not support removal";
                    if (outErrorFile)
                        *outErrorFile = sceneFilePath;
                    if (outErrorLine)
                        *outErrorLine = bi.headerLine;
                    return false;
                }
            }

            for (const auto& comp : bi.components)
            {
                const std::string& compName = comp.name;
                const auto* schema = SceneSchemaRegistry::Find(compName);
                if (!schema)
                {
                    PreserveOrDropUnhandledComponent(world, rootEntity, comp, bi.id, sceneFilePath);
                    continue;
                }

                // Addition semantics: a component listed with no properties means "add defaults".
                ApplyComponentBlock(world, localCtx, *schema, rootEntity, comp, bi.id, sceneFilePath,
                                    bi.headerLine, degradation);
            }

            // Ensure all blueprint roots are parented under the instance root.
            for (auto h : bpInst.created)
            {
                if (!world.IsValid(h) || h.id == rootEntity.id)
                    continue;
                auto* parent = world.GetComponent<Components::Parent>(h);
                const bool hasAnyParent = parent && parent->parent.IsValid();
                if (!hasAnyParent)
                {
                    Components::Parent p{};
                    p.parent = rootEntity;
                    world.AddComponentImmediate(h, p);
                }
            }

            // Parent instance root under specified parent (scene-level)
            if (bi.parent.has_value())
            {
                auto it = out.idToEntity.find(idPrefix + *bi.parent);
                if (it == out.idToEntity.end())
                {
                    if (outError) *outError = "Missing parent for blueprint instance";
                    if (outErrorFile)
                        *outErrorFile = sceneFilePath;
                    if (outErrorLine)
                        *outErrorLine = bi.headerLine;
                    return false;
                }
                Components::Parent p{};
                p.parent = it->second;
                world.AddComponentImmediate(rootEntity, p);
            }

            out.created.insert(out.created.end(), bpInst.created.begin(), bpInst.created.end());
            out.idToEntity.insert(bpInst.idToEntity.begin(), bpInst.idToEntity.end());
        }

        // Subscenes
        for (const auto& ss : doc.subscenes)
        {
            auto rit = doc.resources.find(ss.sourceResourceId);
            if (rit == doc.resources.end())
                continue;

            // Represent the subscene instance itself as a root entity (useful for editor hierarchy + re-serialization).
            ECS::EntityHandle subsceneRoot = world.CreateEntity();
            if (!subsceneRoot.IsValid())
            {
                if (outError)
                    *outError = "Failed to create subscene root entity";
                if (outErrorFile)
                    *outErrorFile = sceneFilePath;
                if (outErrorLine)
                    *outErrorLine = ss.headerLine;
                return false;
            }
            world.AddComponentImmediate(subsceneRoot, MakeTag(idPrefix + ss.id));
            {
                Components::SceneSubsceneInstance inst{};
                std::memset(inst.sourcePath, 0, sizeof(inst.sourcePath));
                std::memset(inst.sourceGuid, 0, sizeof(inst.sourceGuid));
                const auto& res = rit->second;
                const size_t maxPath = sizeof(inst.sourcePath) - 1;
                const size_t maxGuid = sizeof(inst.sourceGuid) - 1;
                if (!res.path.empty())
                {
                    const size_t n = std::min(maxPath, res.path.size());
                    std::memcpy(inst.sourcePath, res.path.data(), n);
                    inst.sourcePath[n] = '\0';
                }
                if (!res.guid.empty())
                {
                    const size_t n = std::min(maxGuid, res.guid.size());
                    std::memcpy(inst.sourceGuid, res.guid.data(), n);
                    inst.sourceGuid[n] = '\0';
                }
                inst.hasOffset = ss.hasOffset;
                inst.offsetX = ss.offset.x;
                inst.offsetY = ss.offset.y;
                inst.offsetZ = ss.offset.z;
                world.AddComponentImmediate(subsceneRoot, inst);
                if (!ss.enabled)
                    ECS::Entity(&world, subsceneRoot).SetEnabled<Components::SceneSubsceneInstance>(false);
            }
            out.created.push_back(subsceneRoot);
            out.idToEntity[idPrefix + ss.id] = subsceneRoot;

            if (!ss.enabled)
                continue;

            const std::filesystem::path subPath = ResolveResourcePath(localCtx, sceneFilePath, rit->second);
            std::string subText;
            if (subPath.empty() || !ReadTextFile(subPath, subText))
            {
                ReportUnreadableResource("Subscene", rit->second, subPath, options.projectRoot, sceneFilePath,
                                         ss.headerLine, outError, outErrorFile, outErrorLine);
                return false;
            }
            SceneDoc subDoc{};
            {
                int subParseLine = 0;
                if (!ParseDocument(subPath, subText, subDoc, outError, &subParseLine))
                {
                    if (outErrorFile)
                        *outErrorFile = subPath;
                    if (outErrorLine)
                        *outErrorLine = subParseLine;
                    return false;
                }
            }
            {
                int subValLine = 0;
                if (!ValidateDocument(subDoc, outError, &subValLine))
                {
                    if (outErrorFile)
                        *outErrorFile = subPath;
                    if (outErrorLine)
                        *outErrorLine = subValLine;
                    return false;
                }
            }

            Instantiated subInst{};
            const std::string subPrefix = idPrefix + ss.id + ".";
            LoadOptions subOpts = options;
            subOpts.mode = LoadMode::Additive;
            if (!LoadParsedIntoWorld(world, subPath, subDoc, subOpts, subPrefix, subInst, outError, outErrorFile, outErrorLine, degradation))
                return false;
            if (ss.hasOffset)
                ApplyOffsetToRoots(world, subInst, ss.offset.x, ss.offset.y, ss.offset.z);
            ParentRootsTo(world, subInst, subsceneRoot);

            out.created.insert(out.created.end(), subInst.created.begin(), subInst.created.end());
            out.idToEntity.insert(subInst.idToEntity.begin(), subInst.idToEntity.end());
        }
    }

    (void)options;
    return true;
}

} // namespace

bool LoadSceneFromFile(ECS::World& world,
                       const std::filesystem::path& sceneFilePath,
                       const LoadOptions& options)
{
    if (sceneFilePath.empty())
        return false;

    EnsureBuiltInSchemasRegistered();

    g_LastError = SceneIOError{};

    using PhaseClock = std::chrono::steady_clock;
    auto phaseStart = PhaseClock::now();
    // Closes the phase that just ran and opens the next one.
    auto markPhase = [&phaseStart](double* out) {
        const auto now = PhaseClock::now();
        if (out)
            *out = std::chrono::duration<double, std::milli>(now - phaseStart).count();
        phaseStart = now;
    };
    SceneLoadTimings* timings = options.outTimings;
    if (timings)
        *timings = SceneLoadTimings{};

    std::string text;
    if (!ReadTextFile(sceneFilePath, text))
    {
        SetError(sceneFilePath, 0, "Failed to read file");
        Logger::Log::Debug("Scene: '{}' stopped at read.", sceneFilePath.string());
        return false;
    }
    markPhase(timings ? &timings->ReadMs : nullptr);

    SceneDoc doc{};
    std::string err;
    int errLine = 0;
    if (!ParseDocument(sceneFilePath, text, doc, &err, &errLine))
    {
        SetError(sceneFilePath, errLine, err);
        Logger::Log::Debug("Scene: '{}' stopped at parse, line {}.", sceneFilePath.string(), errLine);
        return false;
    }

    {
        std::unordered_set<std::string> visited;
        std::filesystem::path errFile;
        int incLine = 0;
        if (!ExpandIncludes(sceneFilePath, doc, visited, &err, &errFile, &incLine))
        {
            SetError(errFile.empty() ? sceneFilePath : errFile, incLine, err);
            Logger::Log::Debug("Scene: '{}' stopped at include expansion, {}:{}.", sceneFilePath.string(),
                               g_LastError.file.string(), incLine);
            return false;
        }
    }
    markPhase(timings ? &timings->ParseMs : nullptr);

    int valLine = 0;
    if (!ValidateDocument(doc, &err, &valLine))
    {
        SetError(sceneFilePath, valLine, err);
        Logger::Log::Debug("Scene: '{}' stopped at validation, line {}.", sceneFilePath.string(), valLine);
        return false;
    }
    markPhase(timings ? &timings->ValidateMs : nullptr);

    // Everything above this point can fail without touching the world. Nothing
    // below it can — the clear is the point of no return, and the only failure
    // past it is LoadParsedIntoWorld's, which strands a partial scene.
    bool worldCleared = false;
    if (options.mode == LoadMode::Replace)
    {
        world.Clear();
        worldCleared = true;
        markPhase(timings ? &timings->ClearMs : nullptr);
    }

    Instantiated inst{};
    std::string idPrefix;
    if (options.mode == LoadMode::Additive)
    {
        const uint64_t n = ++g_AdditiveImportCounter;
        idPrefix = "import_" + std::to_string(n) + ".";
    }

    std::filesystem::path loadErrFile;
    int loadErrLine = 0;
    SceneLoadDegradation degradation;
    if (!LoadParsedIntoWorld(world, sceneFilePath, doc, options, idPrefix, inst, &err, &loadErrFile,
                             &loadErrLine, &degradation))
    {
        SetError(loadErrFile.empty() ? sceneFilePath : loadErrFile, loadErrLine, err);
        g_LastError.worldCleared = worldCleared;
        Logger::Log::Debug("Scene: '{}' stopped at instantiation, {}:{}. The world was {}cleared and holds {}.",
                           sceneFilePath.string(), g_LastError.file.string(), loadErrLine,
                           worldCleared ? "" : "not ",
                           FormatCount(world.GetEntityCount(), "entity", "entities"));

        if (options.outDegradation)
            *options.outDegradation = std::move(degradation);
        return false;
    }

    // The load succeeded, so every caller is about to treat this world as the scene. When part of the
    // file could not be applied, that is only true with an asterisk — say so once, loudly, with the
    // count and enough specifics to act on. The full census goes to outDegradation; this log line is
    // what a user who never reads a census actually sees.
    if (degradation.IsDegraded())
    {
        constexpr std::size_t kMaxLoggedSkips = 10;
        const std::size_t dropped = degradation.DroppedCount();
        Logger::Log::Error("Scene: '{}' loaded DEGRADED — {} assignment(s) in the file could not be "
                           "applied by this build ({} preserved for re-save, {} would be LOST on save). "
                           "The scene is open and everything else loaded.",
                           sceneFilePath.string(), degradation.skips.size(),
                           degradation.skips.size() - dropped, dropped);
        const std::size_t shown = std::min(kMaxLoggedSkips, degradation.skips.size());
        for (std::size_t i = 0; i < shown; ++i)
        {
            const SceneLoadSkip& s = degradation.skips[i];
            Logger::Log::Error("  [{}/{}] entity '{}': {}{}{} at {}:{} — {}{}", i + 1,
                               degradation.skips.size(), s.entityId, s.component,
                               s.field.empty() ? "" : ".", s.field, s.file.string(), s.line, s.message,
                               s.preserved ? " (authored text preserved)" : " (WILL BE LOST ON SAVE)");
        }
        if (degradation.skips.size() > shown)
            Logger::Log::Error("  ... and {} more", degradation.skips.size() - shown);
    }

    if (options.outDegradation)
        *options.outDegradation = std::move(degradation);

    if (options.outHierarchyUiFromFile)
    {
        options.outHierarchyUiFromFile->hadSection = doc.hasHierarchyUi;
        if (doc.hasHierarchyUi)
            options.outHierarchyUiFromFile->ui = doc.hierarchyUi;
        else
            options.outHierarchyUiFromFile->ui = SceneHierarchyUi{};
    }

    if (options.outEditorCameraFromFile)
    {
        options.outEditorCameraFromFile->hadSection = doc.hasEditorCamera;
        if (doc.hasEditorCamera)
            options.outEditorCameraFromFile->camera = doc.editorCamera;
        else
            options.outEditorCameraFromFile->camera = SceneEditorCamera{};
    }

    world.ProcessCommands();

    markPhase(timings ? &timings->InstantiateMs : nullptr);
    return true;
}

// Append serialized lines for every reflected component on `entity` that has no hand-written schema
// (the reflection fallback). Engine + user components lacking a built-in schema flow through here;
// components a built-in schema already wrote are skipped by ReflectionSchemaForUnhandledType.
static void SerializeReflectedComponents(const ECS::World& world, ECS::EntityHandle entity,
                                         const SceneSaveContext& ctx, std::vector<std::string>& lines)
{
    auto* arch = world.GetEntityArchetype(entity);
    if (!arch)
        return;
    for (ECS::ComponentTypeId tid : arch->GetSignature().GetComponents())
    {
        if (const ISceneComponentSchema* s = SceneSchemaRegistry::ReflectionSchemaForUnhandledType(tid))
            s->Serialize(world, entity, ctx, lines);
    }
}

// Whether a saved line belongs to a component token ("Token.prop = value").
static bool LineBelongsToComponent(const std::string& line, std::string_view token)
{
    if (line.size() <= token.size() || line[token.size()] != '.')
        return false;
    for (std::size_t i = 0; i < token.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(token[i])))
            return false;
    return true;
}

// Every component of `entity` switched off through its ECS::ComponentDisabled tag saves as
// "Token.enabled = false" ahead of its own lines, whatever schema writes those; a component that is
// on writes no switch line. A type with no tag (ComponentRegistry::SwitchesThroughDisabledTag is
// false) writes none either: one that keeps its own Enabled field carries its value in that field's
// line, and one declared NotToggleable has no on/off state.
static void SerializeSwitchedOffComponents(const ECS::World& world, ECS::EntityHandle entity,
                                           std::vector<std::string>& lines)
{
    auto* arch = world.GetEntityArchetype(entity);
    if (!arch)
        return;
    const auto& signature = arch->GetSignature();
    for (ECS::ComponentTypeId typeId : signature.GetComponents())
    {
        const ECS::ComponentRegistry::ComponentInfo* info = ECS::ComponentRegistry::GetComponentInfo(typeId);
        if (!info || !signature.Contains(ECS::ComponentDisabledTypeId(info->Name)))
            continue;
        if (!ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId))
            continue;
        const std::string_view token = SceneSchemaRegistry::SchemaTokenForComponentType(typeId);
        if (token.empty())
            continue;
        std::string line(token);
        line += ".enabled = false";
        const auto first = std::find_if(lines.begin(), lines.end(), [token](const std::string& l) {
            return LineBelongsToComponent(l, token);
        });
        lines.insert(first, std::move(line));
    }
}

// Re-emit components that were unknown (type not registered) at load time and have not yet been
// re-applied. They live in the World's side-table, not in any archetype, so the reflected-save
// path above can never see them — this verbatim re-emit is what prevents data loss when a user
// module is still compiling. Format mirrors ReflectionSceneSchema: one "Name.prop = value" line
// per property, or a bare "Name" line for a property-less addition.
static void SerializePreservedComponents(const ECS::World& world, ECS::EntityHandle entity,
                                         std::vector<std::string>& lines)
{
    const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    if (!store)
        return;
    auto it = store->Map().find(entity);
    if (it == store->Map().end())
        return;
    for (const ECS::PreservedComponent& pc : it->second)
    {
        if (pc.Props.empty())
        {
            lines.push_back(pc.Name);
        }
        else
        {
            for (const auto& [prop, value] : pc.Props)
            {
                std::string& line = lines.emplace_back();
                line.reserve(pc.Name.size() + prop.size() + value.size() + 4);
                line += pc.Name; line += '.'; line += prop; line += " = "; line += value;
            }
        }
    }
}

// Put back the authored text of fields this binary could not parse at load time.
//
// The field itself is known and its component serialized a line for it — carrying the DEFAULT the
// field fell back to. Writing that default is exactly the silent data loss a degraded load must not
// cause, so the authored text is substituted back into its own line, in place. Substituting rather
// than appending is required, not cosmetic: the parser coalesces repeated keys last-write-wins, so an
// appended line would shadow the real one and a stale value would win forever.
//
// An override still stands only while the FIELD still holds what it fell back to. For a reflected
// field that is a byte comparison over the field's own span; for a hand-written schema, which has no
// field table to give a span, it is a comparison of what the field serializes to.
//
// Field-scoped is the whole point. Coarser signals — a component column's change version, a
// per-component dirty flag — are tripped by things that did not touch this field at all: adding any
// component to the entity relocates its row, and editing a SIBLING field writes the same component.
// Both then retire a live override and the next save writes the fallback over the user's unreadable
// data, which is the loss this table exists to prevent.
static void ApplyPreservedFieldOverrides(ECS::World& world, ECS::EntityHandle entity,
                                         std::vector<std::string>& lines)
{
    if (!world.TryGetUnresolvedComponents() || world.TryGetUnresolvedComponents()->FieldsEmpty())
        return;
    std::vector<ECS::PreservedField>* fields =
        world.GetUnresolvedComponents().FieldsForMutable(entity);
    if (!fields)
        return;

    for (ECS::PreservedField& pf : *fields)
    {
        const std::string wantComp = ToLowerAscii(pf.Component);
        const std::string wantField = ToLowerAscii(pf.Field);
        for (std::string& l : lines)
        {
            std::string comp, prop, value;
            if (!ParseComponentLine(l, comp, prop, value) || comp != wantComp || prop != wantField)
                continue;

            if (pf.FieldSize != 0)
            {
                // Reflected: the field's own bytes, and nothing else, decide this.
                if (ECS::IsPreservedFieldSuperseded(world, entity, pf))
                    break; // written since the load — that value is the truth now
            }
            else if (!pf.FallbackTextCaptured)
            {
                // String tier, first save: this is the earliest a real save context exists, so it is
                // the earliest the fallback's serialized form can be known at all.
                pf.FallbackText = value;
                pf.FallbackTextCaptured = true;
            }
            else if (value != pf.FallbackText)
            {
                break; // written since the first save
            }

            l.assign(pf.Component).append(1, '.').append(pf.Field).append(" = ").append(pf.RawText);
            break;
        }
    }
}

std::string FormatEntityReferenceForSave(const SceneSaveContext& ctx, ECS::EntityHandle handle)
{
    if (!handle.IsValid() || !ctx.EntityTagById)
        return {};
    auto it = ctx.EntityTagById->find(handle.id);
    if (it == ctx.EntityTagById->end())
        return {}; // referenced entity is not part of this save -> drop the link
    return FormatQuoted(it->second);
}

bool TryResolveEntityReference(const SceneLoadContext& ctx, std::string_view value, ECS::EntityHandle& out)
{
    out = ECS::EntityHandle{};
    std::string id;
    if (!ParseQuotedString(value, id))
        return false; // malformed (expected a quoted entity-id string)
    if (id.empty() || !ctx.EntityIdMap)
        return true; // empty/clear reference -> leave invalid
    auto it = ctx.EntityIdMap->find(ctx.EntityIdPrefix + id);
    if (it != ctx.EntityIdMap->end())
        out = it->second;
    return true; // unknown id -> leave invalid (a dangling link just unlinks)
}

bool SaveSceneToFile(ECS::World& world,
                     const std::filesystem::path& sceneFilePath,
                     const SaveOptions& options)
{
    if (sceneFilePath.empty())
        return false;

    EnsureBuiltInSchemasRegistered();

    g_LastError = SceneIOError{};

    // Build the save context, threaded by const reference through schema
    // Serialize calls and SceneIO helpers (FormatAssetReferenceForSave et al).
    SceneSaveContext saveCtx{};
    saveCtx.SceneFile = &sceneFilePath;
    saveCtx.AssetRoot = !options.assetRootOverride.empty()
                            ? options.assetRootOverride
                            : (options.assetResolver ? options.assetResolver->GetAssetRoot() : std::filesystem::path{});
    saveCtx.Resolver = options.assetResolver;
    saveCtx.Unresolved = world.TryGetUnresolvedComponents();

    // Collect entities
    std::vector<ECS::EntityHandle> entities;
    {
        auto arches = world.GetAllArchetypes();
        for (auto* a : arches)
        {
            if (!a)
                continue;
            for (const auto& h : a->CollectEntities())
            {
                if (h.IsValid() && world.IsValid(h))
                    entities.push_back(h);
            }
        }
    }

    if (options.ensureSceneEntityTags)
    {
        EnsureTags(world, entities);
    }

    NormalizeHierarchySiblingOrderForSave(world, entities);
    SortEntitiesDepthFirstForSave(world, entities);

    // Build tag map
    std::unordered_map<ECS::EntityId, std::string> tagByEntity;
    tagByEntity.reserve(entities.size());
    for (auto h : entities)
    {
        if (auto* tag = world.GetComponent<Components::SceneEntityTag>(h))
            tagByEntity[h.id] = GetTagString(*tag);
    }
    // Let EntityHandle fields serialize a stable reference to any of these entities.
    saveCtx.EntityTagById = &tagByEntity;

    std::ostringstream out;

    // Preserve comments (best-effort) by carrying over contiguous ';' comment blocks immediately preceding
    // section headers from the existing file. This is not a perfect AST-preserving rewrite, but it keeps
    // most human-authored commentary stable across saves.
    std::unordered_map<std::string, std::vector<std::string>> commentsBeforeSection;
    {
        std::string existing;
        if (ReadTextFile(sceneFilePath, existing))
        {
            std::istringstream in(existing);
            std::string line;
            std::vector<std::string> pending;
            std::vector<std::string> leading;
            bool beforeFirstSection = true;

            auto makeKey = [](const std::string& type, const std::unordered_map<std::string, std::string>& attrs) -> std::string
            {
                if (type == "resource")
                {
                    if (auto it = attrs.find("guid"); it != attrs.end() && !it->second.empty())
                        return "resource:guid:" + it->second;
                    if (auto it = attrs.find("path"); it != attrs.end() && !it->second.empty())
                        return "resource:path:" + it->second;
                }
                if (type == "include")
                {
                    if (auto it = attrs.find("path"); it != attrs.end() && !it->second.empty())
                        return "include:path:" + it->second;
                }
                if ((type == "scene" || type == "blueprint") && attrs.find("version") != attrs.end())
                    return type + ":__header__";
                auto it = attrs.find("id");
                if (it != attrs.end() && !it->second.empty())
                    return type + ":" + it->second;
                return type + ":__noid__";
            };

            while (std::getline(in, line))
            {
                const std::string t = Trim(line);
                if (t.empty() || (!t.empty() && t.front() == ';'))
                {
                    pending.push_back(line);
                    if (beforeFirstSection)
                        leading.push_back(line);
                    continue;
                }
                if (t.front() == '[' && t.back() == ']')
                {
                    beforeFirstSection = false;
                    std::string type;
                    std::unordered_map<std::string, std::string> attrs;
                    if (ParseSectionHeader(t, type, attrs))
                    {
                        const std::string key = makeKey(type, attrs);
                        if (!pending.empty() && commentsBeforeSection.find(key) == commentsBeforeSection.end())
                            commentsBeforeSection.emplace(key, pending);
                    }
                    pending.clear();
                    continue;
                }

                // Any other content breaks the pending "immediately preceding header" block.
                beforeFirstSection = false;
                pending.clear();
            }

            // Emit file preamble (comments before first header) if present.
            if (!leading.empty())
            {
                for (const auto& l : leading)
                    out << l << "\n";
                out << "\n";
            }
        }
    }

    // Preserve include/resource/embed sections that are not represented in the ECS world.
    // These are copied from the existing on-disk file (if any) to keep authoring data stable.
    struct PreservedResourceSection
    {
        std::string key;  // "guid:..." or "path:..."
        std::string id;
        std::string path;
        std::string guid;
    };
    struct PreservedEmbedBlock
    {
        std::string id;
        std::string guid; // as authored; empty means the block predates persisted identity
        std::string headerLine;
        std::vector<std::string> bodyLines; // raw lines until next section header
    };

    std::vector<std::string> preservedIncludes;
    std::vector<PreservedResourceSection> preservedResources;
    std::unordered_map<std::string, std::string> preservedResourceIdByKey;
    std::unordered_set<std::string> preservedResourceIds;
    std::vector<PreservedEmbedBlock> preservedEmbeds;

    {
        std::string existing;
        if (ReadTextFile(sceneFilePath, existing))
        {
            std::vector<std::string> lines;
            {
                std::istringstream in(existing);
                std::string line;
                while (std::getline(in, line))
                    lines.push_back(line);
            }

            auto isHeaderLine = [&](const std::string& l) -> bool
            {
                const std::string t = Trim(l);
                return !t.empty() && t.front() == '[' && t.back() == ']';
            };

            auto makeResourceKey = [](const std::unordered_map<std::string, std::string>& attrs) -> std::string
            {
                auto itGuid = attrs.find("guid");
                if (itGuid != attrs.end() && !itGuid->second.empty())
                    return "guid:" + itGuid->second;
                auto itPath = attrs.find("path");
                if (itPath != attrs.end() && !itPath->second.empty())
                    return "path:" + itPath->second;
                return "path:";
            };

            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (!isHeaderLine(lines[i]))
                    continue;

                std::string type;
                std::unordered_map<std::string, std::string> attrs;
                if (!ParseSectionHeader(Trim(lines[i]), type, attrs))
                    continue;

                if (type == "include")
                {
                    preservedIncludes.push_back(Trim(lines[i]));
                    continue;
                }
                if (type == "resource")
                {
                    const std::string key = makeResourceKey(attrs);
                    auto itId = attrs.find("id");
                    auto itPath = attrs.find("path");
                    auto itGuid = attrs.find("guid");
                    if (itId != attrs.end() && !itId->second.empty())
                    {
                        PreservedResourceSection r{};
                        r.key = key;
                        r.id = itId->second;
                        r.path = (itPath != attrs.end()) ? itPath->second : std::string{};
                        r.guid = (itGuid != attrs.end()) ? itGuid->second : std::string{};
                        preservedResources.push_back(r);
                        if (preservedResourceIdByKey.find(key) == preservedResourceIdByKey.end())
                            preservedResourceIdByKey[key] = r.id;
                        preservedResourceIds.insert(r.id);
                    }
                    continue;
                }
                if (type == "embed")
                {
                    PreservedEmbedBlock b{};
                    auto itId = attrs.find("id");
                    b.id = (itId != attrs.end()) ? itId->second : std::string{};
                    if (auto itGuid = attrs.find("guid"); itGuid != attrs.end())
                        b.guid = itGuid->second;
                    b.headerLine = Trim(lines[i]);
                    size_t j = i + 1;
                    for (; j < lines.size(); ++j)
                    {
                        if (isHeaderLine(lines[j]))
                            break;
                        b.bodyLines.push_back(lines[j]);
                    }
                    preservedEmbeds.push_back(std::move(b));
                    i = j - 1;
                    continue;
                }

                // Skip bodies of other section types.
                size_t j = i + 1;
                for (; j < lines.size(); ++j)
                {
                    if (isHeaderLine(lines[j]))
                        break;
                }
                i = j - 1;
            }
        }
    }

    out << "[scene name=\"" << sceneFilePath.stem().string() << "\" version=1]\n\n";
    const auto schemas = SceneSchemaRegistry::GetAllSorted();

    // If requested, re-serialize blueprint instances back to [blueprint] sections (plus [resource] lines),
    // rather than flattening all instantiated entities.
    struct BlueprintRoot
    {
        ECS::EntityHandle h{};
        std::string id;        // SceneEntityTag of root (instance id)
        std::string sourcePath; // as-authored resource path
        std::string sourceGuid; // optional
        std::string parentId;   // SceneEntityTag of parent (if any and tagged)
    };

    std::vector<BlueprintRoot> blueprintRoots;
    struct SubsceneRoot
    {
        ECS::EntityHandle h{};
        std::string id;         // SceneEntityTag of subscene root (instance id)
        std::string sourcePath; // as-authored resource path
        std::string sourceGuid; // optional
        bool hasOffset = false;
        float ox = 0, oy = 0, oz = 0;
        bool enabled = true;
    };
    std::vector<SubsceneRoot> subsceneRoots;
    std::unordered_set<ECS::EntityId> excluded; // blueprint/subscene roots + prefixed children, plus runtime-only entities

    // Runtime-only entities (HLOD proxies, spline placement pieces, terrain collider
    // tiles) are engine-created and reconstructed at runtime; they must never be written
    // to the scene, mirroring the blueprint/subscene root exclusion below. Both emission
    // arms below consult `excluded`, so this holds whatever the preserve-* options say.
    for (auto h : entities)
    {
        if (world.HasComponent<Components::RuntimeOnlyEntity>(h))
            excluded.insert(h.id);
    }

    // An entity parented to a runtime-only one would emit a parent= naming no block
    // in the file, and the loader hard-rejects a missing parent — a scene that fails
    // to open, silently. Refuse the save and name the fix, as the instanced-parent
    // cases do. Keyed on the tag rather than on `excluded`, which also holds
    // blueprint and subscene roots: those ARE written, as their own sections, and
    // parenting under one is legal.
    const auto RuntimeOnlyParentOf = [&world](ECS::EntityHandle h) -> ECS::EntityHandle
    {
        const auto* p = world.GetComponent<Components::Parent>(h);
        if (p && p->parent.IsValid() && world.IsValid(p->parent) &&
            world.HasComponent<Components::RuntimeOnlyEntity>(p->parent))
        {
            return p->parent;
        }
        return {};
    };
    const auto DescribeEntity = [&world](ECS::EntityHandle h) -> std::string
    {
        if (const auto* name = world.GetComponent<Components::Name>(h); name && name->value[0] != '\0')
            return std::string(name->View());
        return "entity " + std::to_string(h.id);
    };

    if (options.preserveBlueprintInstances || options.preserveSubscenes)
    {
        if (options.preserveBlueprintInstances)
        {
            // Identify blueprint instance roots.
            for (auto h : entities)
            {
                const auto* bp = world.GetComponent<Components::SceneBlueprintInstance>(h);
                if (!bp)
                    continue;
                const std::string id = tagByEntity.count(h.id) ? tagByEntity[h.id] : ("e_" + std::to_string(h.id));
                BlueprintRoot r{};
                r.h = h;
                r.id = id;
                r.sourcePath = std::string(bp->SourcePath());
                r.sourceGuid = std::string(bp->SourceGuid());
                if (auto* p = world.GetComponent<Components::Parent>(h))
                {
                    auto it = tagByEntity.find(p->parent.id);
                    if (p->parent.IsValid() && it != tagByEntity.end())
                        r.parentId = it->second;
                }
                blueprintRoots.push_back(std::move(r));
            }
        }

        if (options.preserveSubscenes)
        {
            for (auto h : entities)
            {
                const auto* ss = world.GetComponent<Components::SceneSubsceneInstance>(h);
                if (!ss)
                    continue;
                const std::string id = tagByEntity.count(h.id) ? tagByEntity[h.id] : ("e_" + std::to_string(h.id));
                SubsceneRoot r{};
                r.h = h;
                r.id = id;
                r.sourcePath = std::string(ss->SourcePath());
                r.sourceGuid = std::string(ss->SourceGuid());
                r.hasOffset = ss->hasOffset;
                r.ox = ss->offsetX;
                r.oy = ss->offsetY;
                r.oz = ss->offsetZ;
                r.enabled = !world.HasComponent<ECS::ComponentDisabled<Components::SceneSubsceneInstance>>(h);
                subsceneRoots.push_back(std::move(r));
            }
        }

        // Exclude instance roots and their prefixed children from normal [entity] blocks.
        for (const auto& br : blueprintRoots)
        {
            excluded.insert(br.h.id);
            const std::string prefix = br.id + ".";
            for (auto h : entities)
            {
                auto it = tagByEntity.find(h.id);
                if (it == tagByEntity.end())
                    continue;
                if (StartsWith(it->second, prefix.c_str()))
                    excluded.insert(h.id);
            }
        }
        for (const auto& sr : subsceneRoots)
        {
            excluded.insert(sr.h.id);
            const std::string prefix = sr.id + ".";
            for (auto h : entities)
            {
                auto it = tagByEntity.find(h.id);
                if (it == tagByEntity.end())
                    continue;
                if (StartsWith(it->second, prefix.c_str()))
                    excluded.insert(h.id);
            }
        }

        // Emit resources for blueprint instances (deduped by guid/path).
        struct ResourceKey
        {
            std::string key;  // guid if present else path (with prefix)
            std::string path; // original path (as-authored)
            std::string guid; // optional
        };
        std::vector<ResourceKey> resourceKeys;
        resourceKeys.reserve(blueprintRoots.size() + subsceneRoots.size());
        {
            std::unordered_set<std::string> seen;
            for (const auto& br : blueprintRoots)
            {
                const std::string k = !br.sourceGuid.empty() ? ("guid:" + br.sourceGuid) : ("path:" + br.sourcePath);
                if (!seen.insert(k).second)
                    continue;
                resourceKeys.push_back(ResourceKey{k, br.sourcePath, br.sourceGuid});
            }
            for (const auto& sr : subsceneRoots)
            {
                const std::string k = !sr.sourceGuid.empty() ? ("guid:" + sr.sourceGuid) : ("path:" + sr.sourcePath);
                if (!seen.insert(k).second)
                    continue;
                resourceKeys.push_back(ResourceKey{k, sr.sourcePath, sr.sourceGuid});
            }
            std::sort(resourceKeys.begin(), resourceKeys.end(), [](const ResourceKey& a, const ResourceKey& b) { return a.key < b.key; });
        }

        // Determine resource ids, reusing any existing ids from the previous file (keyed by guid/path).
        std::unordered_map<std::string, std::string> resourceIdByKey;
        resourceIdByKey.reserve(resourceKeys.size());
        std::unordered_set<std::string> usedIds = preservedResourceIds;

        auto allocId = [&]() -> std::string
        {
            for (int n = 1;; ++n)
            {
                const std::string id = "res_" + std::to_string(n);
                if (usedIds.find(id) == usedIds.end())
                {
                    usedIds.insert(id);
                    return id;
                }
            }
        };

        for (const auto& rk : resourceKeys)
        {
            auto it = preservedResourceIdByKey.find(rk.key);
            if (it != preservedResourceIdByKey.end())
            {
                resourceIdByKey[rk.key] = it->second;
                usedIds.insert(it->second);
            }
            else
            {
                resourceIdByKey[rk.key] = allocId();
            }
        }

        // Emit preserved includes.
        for (const auto& inc : preservedIncludes)
        {
            std::string type;
            std::unordered_map<std::string, std::string> attrs;
            if (ParseSectionHeader(inc, type, attrs))
            {
                if (auto itPath = attrs.find("path"); itPath != attrs.end())
                {
                    const std::string ckey = "include:path:" + itPath->second;
                    if (auto it = commentsBeforeSection.find(ckey); it != commentsBeforeSection.end())
                    {
                        for (const auto& l : it->second)
                            out << l << "\n";
                    }
                }
            }
            out << inc << "\n";
        }
        if (!preservedIncludes.empty())
            out << "\n";

        // Emit resources: keep any existing resource lines, then add missing required ones.
        std::unordered_set<std::string> emittedKeys;
        emittedKeys.reserve(preservedResources.size() + resourceKeys.size());
        for (const auto& pr : preservedResources)
        {
            const std::string ckey = !pr.guid.empty() ? ("resource:guid:" + pr.guid) : ("resource:path:" + pr.path);
            if (auto it = commentsBeforeSection.find(ckey); it != commentsBeforeSection.end())
            {
                for (const auto& l : it->second)
                    out << l << "\n";
            }
            out << "[resource id=\"" << pr.id << "\"";
            if (!pr.path.empty())
                out << " path=\"" << pr.path << "\"";
            if (!pr.guid.empty())
                out << " guid=\"" << pr.guid << "\"";
            out << "]\n";
            emittedKeys.insert(pr.key);
        }
        for (const auto& rk : resourceKeys)
        {
            if (emittedKeys.find(rk.key) != emittedKeys.end())
                continue;
            const std::string ckey = !rk.guid.empty() ? ("resource:guid:" + rk.guid) : ("resource:path:" + rk.path);
            if (auto it = commentsBeforeSection.find(ckey); it != commentsBeforeSection.end())
            {
                for (const auto& l : it->second)
                    out << l << "\n";
            }

            // If we didn't already have a GUID in the authoring file, try to
            // resolve one via the registry. mintIfUnknown=true because the
            // [resource] section is a stable indirection target — every
            // entry should round-trip through reload, so we mint a fresh
            // guid for paths the registry hasn't observed yet (e.g. assets
            // imported in this session before scan completion).
            std::string guidOut = rk.guid;
            if (guidOut.empty())
                guidOut = HealGuidFromAuthoredPath(saveCtx, rk.path, /*mintIfUnknown=*/true);

            out << "[resource id=\"" << resourceIdByKey[rk.key] << "\"";
            if (!rk.path.empty())
                out << " path=\"" << rk.path << "\"";
            if (!guidOut.empty())
                out << " guid=\"" << guidOut << "\"";
            out << "]\n";
            emittedKeys.insert(rk.key);
        }
        if (!preservedResources.empty() || !resourceKeys.empty())
            out << "\n";

        // Emit embeds verbatim (they are not represented in the ECS world), except
        // that a block without a persisted identity gains one. The value written is
        // the identity this very file resolved to on load, so the first save after
        // this feature migrates the embed in place without moving it; from then on
        // the identity is a property of the block rather than of the scene's path.
        //
        // Only with a resolver bound: the derivation reproduces the identity a load
        // saw only if both agree on where the scene's GUID comes from. Stamping a
        // path-derived GUID into a file the editor loads registry-backed would
        // freeze the wrong identity, so a resolver-less save leaves the block
        // unmigrated (which is what it did before this attribute existed).
        for (const auto& eb : preservedEmbeds)
        {
            if (!eb.id.empty())
            {
                if (auto it = commentsBeforeSection.find("embed:" + eb.id); it != commentsBeforeSection.end())
                {
                    for (const auto& l : it->second)
                        out << l << "\n";
                }
            }
            if (eb.guid.empty() && !eb.id.empty() && saveCtx.Resolver)
            {
                const GUID legacyGuid = DeriveLegacyEmbedGuid(saveCtx.Resolver, sceneFilePath, eb.id);
                out << InsertHeaderAttribute(eb.headerLine, "guid", legacyGuid.ToString()) << "\n";
            }
            else
            {
                out << eb.headerLine << "\n";
            }
            for (const auto& l : eb.bodyLines)
                out << l << "\n";
            out << "\n";
        }

        // Sort blueprint instances so parent blueprint roots come first (to satisfy load-time ordering).
        std::unordered_map<std::string, size_t> idxById;
        idxById.reserve(blueprintRoots.size());
        for (size_t i = 0; i < blueprintRoots.size(); ++i)
            idxById[blueprintRoots[i].id] = i;

        std::vector<size_t> sorted;
        sorted.reserve(blueprintRoots.size());
        enum class Mark : uint8_t
        {
            None,
            Temp,
            Perm
        };
        std::vector<Mark> marks(blueprintRoots.size(), Mark::None);
        std::function<bool(size_t)> dfs = [&](size_t i) -> bool
        {
            if (marks[i] == Mark::Perm)
                return true;
            if (marks[i] == Mark::Temp)
                return false;
            marks[i] = Mark::Temp;
            const std::string& p = blueprintRoots[i].parentId;
            if (!p.empty())
            {
                auto it = idxById.find(p);
                if (it != idxById.end())
                {
                    if (!dfs(it->second))
                        return false;
                }
            }
            marks[i] = Mark::Perm;
            sorted.push_back(i);
            return true;
        };
        for (size_t i = 0; i < blueprintRoots.size(); ++i)
        {
            if (marks[i] == Mark::None)
            {
                if (!dfs(i))
                {
                    SetError(sceneFilePath, 0, "Cycle detected in blueprint instance parenting (cannot save)");
                    return false;
                }
            }
        }
        std::reverse(sorted.begin(), sorted.end());

        // Emit non-instanced entities first (depth-first hierarchy order matching the editor Hierarchy panel).
        std::vector<std::string> lines;
        for (auto h : entities)
        {
            lines.clear();
            if (excluded.count(h.id) > 0)
                continue;

            const std::string id = tagByEntity.count(h.id) ? tagByEntity[h.id] : ("e_" + std::to_string(h.id));

            if (const ECS::EntityHandle generatedParent = RuntimeOnlyParentOf(h); generatedParent.IsValid())
            {
                SetError(sceneFilePath, 0,
                         "Cannot save: entity '" + id + "' is parented under generated entity '" +
                             DescribeEntity(generatedParent) +
                             "', which is never written to the scene. Re-parent it under the entity that "
                             "generates those pieces.");
                return false;
            }

            // Parent
            std::string parentStr;
            if (auto* p = world.GetComponent<Components::Parent>(h))
            {
                auto it = tagByEntity.find(p->parent.id);
                if (p->parent.IsValid() && it != tagByEntity.end())
                    parentStr = it->second;
            }

            // Disallow parenting under an instanced child (cannot be represented in file).
            if (!parentStr.empty())
            {
                for (const auto& br : blueprintRoots)
                {
                    const std::string pref = br.id + ".";
                    if (StartsWith(parentStr, pref.c_str()))
                    {
                        SetError(sceneFilePath, 0, "Cannot save: entity '" + id + "' is parented under blueprint-instanced entity '" + parentStr + "'");
                        return false;
                    }
                }
                for (const auto& sr : subsceneRoots)
                {
                    const std::string pref = sr.id + ".";
                    if (StartsWith(parentStr, pref.c_str()))
                    {
                        SetError(sceneFilePath, 0, "Cannot save: entity '" + id + "' is parented under subscene-instanced entity '" + parentStr + "'");
                        return false;
                    }
                }
            }

            if (auto it = commentsBeforeSection.find("entity:" + id); it != commentsBeforeSection.end())
            {
                for (const auto& l : it->second)
                    out << l << "\n";
            }
            out << "[entity id=\"" << id << "\"";
            if (!parentStr.empty())
                out << " parent=\"" << parentStr << "\"";
            out << "]\n";

            for (const auto* schema : schemas)
            {
                if (schema)
                    schema->Serialize(world, h, saveCtx, lines);
            }
            SerializeReflectedComponents(world, h, saveCtx, lines);
            SerializeSwitchedOffComponents(world, h, lines);
            SerializePreservedComponents(world, h, lines);
            ApplyPreservedFieldOverrides(world, h, lines);
            for (const auto& l : lines)
                out << l << "\n";
            out << "\n";
        }

        // Cache blueprint root component sets by resource key.
        std::unordered_map<std::string, std::unordered_set<std::string>> bpRootComponentsByKey;
        bpRootComponentsByKey.reserve(resourceKeys.size());

        auto resolveBlueprintAbsolutePath = [&](const ResourceKey& rk) -> std::filesystem::path
        {
            std::filesystem::path p;
            if (!rk.guid.empty())
            {
                if (options.assetResolver)
                {
                    try
                    {
                        GUID g(rk.guid);
                        g = options.assetResolver->ResolveGuid(g);
                        std::filesystem::path resolvedPath;
                        AssetType resolvedType = AssetType::Unknown;
                        if (options.assetResolver->TryGetPathAndType(g, resolvedPath, resolvedType))
                            p = resolvedPath;
                    }
                    catch (...)
                    {
                    }
                }
            }
            if (p.empty() && !rk.path.empty())
            {
                p = rk.path;
                if (p.is_relative())
                {
                    const auto assetRoot = !options.assetRootOverride.empty()
                                               ? options.assetRootOverride
                                               : (options.assetResolver ? options.assetResolver->GetAssetRoot() : std::filesystem::path{});
                    if (!assetRoot.empty())
                        p = (assetRoot / p).lexically_normal();
                    else
                        p = (sceneFilePath.parent_path() / p).lexically_normal();
                }
            }
            return p;
        };

        auto getBlueprintRootComponentSet = [&](const ResourceKey& rk, std::string* outErr) -> const std::unordered_set<std::string>*
        {
            auto it = bpRootComponentsByKey.find(rk.key);
            if (it != bpRootComponentsByKey.end())
                return &it->second;

            const std::filesystem::path bpPath = resolveBlueprintAbsolutePath(rk);
            std::string bpText;
            if (bpPath.empty() || !ReadTextFile(bpPath, bpText))
            {
                if (outErr)
                    *outErr = "Failed to read blueprint '" + bpPath.string() + "'";
                return nullptr;
            }

            SceneDoc bpDoc{};
            int pLine = 0;
            std::string perr;
            if (!ParseDocument(bpPath, bpText, bpDoc, &perr, &pLine))
            {
                if (outErr)
                    *outErr = perr;
                return nullptr;
            }
            int vLine = 0;
            if (!ValidateDocument(bpDoc, &perr, &vLine))
            {
                if (outErr)
                    *outErr = perr;
                return nullptr;
            }

            std::unordered_set<std::string> comps;
            for (const auto& e : bpDoc.entities)
            {
                if (e.id == "root")
                {
                    for (const auto& comp : e.components)
                        comps.insert(comp.name);
                    break;
                }
            }

            auto [ins, ok] = bpRootComponentsByKey.emplace(rk.key, std::move(comps));
            (void)ok;
            return &ins->second;
        };

        // Emit blueprint instance sections. Reuses the `lines` buffer declared
        // above for the non-instanced entity loop (same scope), so its capacity
        // carries over and neither loop reallocates per entity.
        for (size_t si : sorted)
        {
            lines.clear();
            const BlueprintRoot& br = blueprintRoots[si];
            const std::string rkKey = !br.sourceGuid.empty() ? ("guid:" + br.sourceGuid) : ("path:" + br.sourcePath);
            const std::string sourceId = resourceIdByKey.count(rkKey) ? resourceIdByKey[rkKey] : std::string{};

            if (auto it = commentsBeforeSection.find("blueprint:" + br.id); it != commentsBeforeSection.end())
            {
                for (const auto& l : it->second)
                    out << l << "\n";
            }
            out << "[blueprint id=\"" << br.id << "\"";
            if (!sourceId.empty())
                out << " source=\"" << sourceId << "\"";
            if (!br.parentId.empty())
                out << " parent=\"" << br.parentId << "\"";
            out << "]\n";

            // Emit removals for blueprint-root components that are not present on the instance root.
            {
                std::string bperr;
                ResourceKey rk{rkKey, br.sourcePath, br.sourceGuid};
                const auto* bpComps = getBlueprintRootComponentSet(rk, &bperr);
                if (!bpComps)
                {
                    SetError(sceneFilePath, 0, bperr);
                    return false;
                }
                for (const auto& compLower : *bpComps)
                {
                    const auto* schema = SceneSchemaRegistry::Find(compLower);
                    if (!schema)
                        continue;
                    if (!schema->IsPresent(world, br.h))
                        out << "-" << schema->GetComponentName() << "\n";
                }
            }

            // Serialize root overrides (safe even if redundant).
            for (const auto* schema : schemas)
            {
                if (schema)
                    schema->Serialize(world, br.h, saveCtx, lines);
            }
            SerializeReflectedComponents(world, br.h, saveCtx, lines);
            SerializeSwitchedOffComponents(world, br.h, lines);
            SerializePreservedComponents(world, br.h, lines);
            ApplyPreservedFieldOverrides(world, br.h, lines);
            for (const auto& l : lines)
                out << l << "\n";
            out << "\n";
        }

        // Emit subscene instance sections (deterministic by id).
        std::sort(subsceneRoots.begin(), subsceneRoots.end(), [](const SubsceneRoot& a, const SubsceneRoot& b) { return a.id < b.id; });
        for (const auto& sr : subsceneRoots)
        {
            const std::string rkKey = !sr.sourceGuid.empty() ? ("guid:" + sr.sourceGuid) : ("path:" + sr.sourcePath);
            const std::string sourceId = resourceIdByKey.count(rkKey) ? resourceIdByKey[rkKey] : std::string{};

            if (auto it = commentsBeforeSection.find("subscene:" + sr.id); it != commentsBeforeSection.end())
            {
                for (const auto& l : it->second)
                    out << l << "\n";
            }
            out << "[subscene id=\"" << sr.id << "\"";
            if (!sourceId.empty())
                out << " source=\"" << sourceId << "\"";
            out << "]\n";

            if (sr.hasOffset)
                out << "offset = " << FormatFloat3(sr.ox, sr.oy, sr.oz) << "\n";
            if (!sr.enabled)
                out << "enabled = false\n";
            out << "\n";
        }
    }
    else
    {
        std::vector<std::string> lines;
        for (auto h : entities)
        {
            lines.clear();
            if (excluded.count(h.id) > 0)
                continue;

            const std::string id = tagByEntity.count(h.id) ? tagByEntity[h.id] : ("e_" + std::to_string(h.id));

            if (const ECS::EntityHandle generatedParent = RuntimeOnlyParentOf(h); generatedParent.IsValid())
            {
                SetError(sceneFilePath, 0,
                         "Cannot save: entity '" + id + "' is parented under generated entity '" +
                             DescribeEntity(generatedParent) +
                             "', which is never written to the scene. Re-parent it under the entity that "
                             "generates those pieces.");
                return false;
            }

            // Parent
            std::string parentStr;
            if (auto* p = world.GetComponent<Components::Parent>(h))
            {
                auto it = tagByEntity.find(p->parent.id);
                if (p->parent.IsValid() && it != tagByEntity.end())
                    parentStr = it->second;
            }

            out << "[entity id=\"" << id << "\"";
            if (!parentStr.empty())
                out << " parent=\"" << parentStr << "\"";
            out << "]\n";

            for (const auto* schema : schemas)
            {
                if (schema)
                    schema->Serialize(world, h, saveCtx, lines);
            }
            SerializeReflectedComponents(world, h, saveCtx, lines);
            SerializeSwitchedOffComponents(world, h, lines);
            SerializePreservedComponents(world, h, lines);
            ApplyPreservedFieldOverrides(world, h, lines);
            for (const auto& l : lines)
                out << l << "\n";
            out << "\n";
        }
    }

    if (options.hierarchyUi)
    {
        out << "\n[hierarchy_ui]\n";
        std::string expandedJoined;
        {
            std::vector<std::string> sorted = options.hierarchyUi->expandedSceneEntityTags;
            std::sort(sorted.begin(), sorted.end());
            for (size_t i = 0; i < sorted.size(); ++i)
            {
                if (i)
                    expandedJoined += ',';
                expandedJoined += sorted[i];
            }
        }
        out << "expanded = " << FormatQuoted(expandedJoined) << "\n";

        std::string selectionJoined;
        for (size_t i = 0; i < options.hierarchyUi->selectionSceneEntityTagsOrdered.size(); ++i)
        {
            if (i)
                selectionJoined += ';';
            selectionJoined += options.hierarchyUi->selectionSceneEntityTagsOrdered[i];
        }
        out << "selection = " << FormatQuoted(selectionJoined) << "\n";
    }

    if (options.editorCamera)
    {
        const SceneEditorCamera& c = *options.editorCamera;
        out << "\n[editor_camera]\n";
        out << "pos_x = " << c.PosX << "\n";
        out << "pos_y = " << c.PosY << "\n";
        out << "pos_z = " << c.PosZ << "\n";
        out << "yaw_deg = " << c.YawDeg << "\n";
        out << "pitch_deg = " << c.PitchDeg << "\n";
        out << "distance = " << c.Distance << "\n";
        out << "is_2d = " << (c.Is2D ? "true" : "false") << "\n";
    }

    const std::string text = out.str();
    if (!WriteTextFileAtomic(sceneFilePath, text))
    {
        SetError(sceneFilePath, 0, "Failed to write file");
        Logger::Log::Error("Scene: failed to write '{}'", sceneFilePath.string());
        return false;
    }
    return true;
}

} // namespace GameEngine::Scene
