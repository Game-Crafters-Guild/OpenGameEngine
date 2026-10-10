#include "Assets/Packages/PackageManifest.h"

#include "AssetCore/SharedFileRead.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

constexpr size_t kMaxPackageNameLength = 214;

std::string MakeError(const std::filesystem::path& manifestPath,
                      std::string_view field,
                      std::string_view why)
{
    std::string msg = "package.json (" + manifestPath.generic_string() + ")";
    if (!field.empty())
    {
        msg += ": field '";
        msg += field;
        msg += "'";
    }
    msg += ": ";
    msg += why;
    return msg;
}

bool IsValidNameSegment(std::string_view segment)
{
    if (segment.empty())
        return false;
    if (segment.front() == '.' || segment.front() == '_')
        return false;
    for (char c : segment)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '.' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

bool TryReadStringArray(const nlohmann::json& doc,
                        const char* field,
                        const std::filesystem::path& manifestPath,
                        std::vector<std::string>& out,
                        std::string& outError)
{
    if (!doc.contains(field))
        return true;
    const auto& node = doc[field];
    if (!node.is_array())
    {
        outError = MakeError(manifestPath, field, "must be an array of strings");
        return false;
    }
    for (const auto& item : node)
    {
        if (!item.is_string())
        {
            outError = MakeError(manifestPath, field, "must be an array of strings");
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

// Keys the schema no longer has. Silently ignoring one would let a manifest
// keep asking for behaviour the engine stopped implementing — a package whose
// "alwaysShip": true once meant "ship the whole tree" would quietly ship only
// what the build references instead. Each refusal names the rule that replaced
// the key.
bool RejectRemovedKeys(const nlohmann::json& doc,
                       const std::filesystem::path& manifestPath,
                       std::string& outError)
{
    struct RemovedKey
    {
        const char* Field;
        const char* Replacement;
    };
    static constexpr RemovedKey kRemovedKeys[] = {
        {"defines",
         "removed: every package exports exactly one compile define derived from its name "
         "(\"ocean-pack\" exports GE_PACKAGE_OCEAN_PACK). Delete the key"},
        {"alwaysShip",
         "removed: a build ships the package assets its scenes reference, the package's Shaders/ "
         "folder and the files \"runtimeAssets\" lists for the components the scenes use. Delete "
         "the key, and list in \"runtimeAssets\" what the package's code loads by path"},
    };

    for (const RemovedKey& removed : kRemovedKeys)
    {
        if (doc.contains(removed.Field))
        {
            outError = MakeError(manifestPath, removed.Field, removed.Replacement);
            return false;
        }
    }
    return true;
}

bool IsSafeRelativeDir(std::string_view dir)
{
    if (dir.empty())
        return false;
    const std::filesystem::path p(dir);
    if (p.is_absolute())
        return false;
    for (const auto& segment : p)
    {
        if (segment == "..")
            return false;
    }
    return true;
}

bool TryParseModuleRecord(const nlohmann::json& node,
                          size_t index,
                          const std::filesystem::path& manifestPath,
                          PackageModuleRecord& out,
                          std::string& outError)
{
    const std::string fieldPrefix = "modules[" + std::to_string(index) + "]";
    if (!node.is_object())
    {
        outError = MakeError(manifestPath, fieldPrefix, "must be an object");
        return false;
    }

    if (!node.contains("name") || !node["name"].is_string() || node["name"].get<std::string>().empty())
    {
        outError = MakeError(manifestPath, fieldPrefix + ".name", "required non-empty string");
        return false;
    }
    out.Name = node["name"].get<std::string>();

    if (!node.contains("kind") || !node["kind"].is_string())
    {
        outError = MakeError(manifestPath, fieldPrefix + ".kind", "required string: \"Runtime\" or \"Editor\"");
        return false;
    }
    const std::string kind = node["kind"].get<std::string>();
    if (kind == "Runtime")
        out.Kind = PackageModuleRecord::ModuleKind::Runtime;
    else if (kind == "Editor")
        out.Kind = PackageModuleRecord::ModuleKind::Editor;
    else
    {
        outError = MakeError(manifestPath, fieldPrefix + ".kind",
                             "unknown kind '" + kind + "' (expected \"Runtime\" or \"Editor\")");
        return false;
    }

    if (!node.contains("lang") || !node["lang"].is_string())
    {
        outError = MakeError(manifestPath, fieldPrefix + ".lang", "required string: \"CSharp\" or \"Cpp\"");
        return false;
    }
    const std::string lang = node["lang"].get<std::string>();
    if (lang == "CSharp")
        out.Lang = PackageModuleRecord::ModuleLang::CSharp;
    else if (lang == "Cpp")
        out.Lang = PackageModuleRecord::ModuleLang::Cpp;
    else
    {
        outError = MakeError(manifestPath, fieldPrefix + ".lang",
                             "unknown lang '" + lang + "' (expected \"CSharp\" or \"Cpp\")");
        return false;
    }

    if (!node.contains("root") || !node["root"].is_string())
    {
        outError = MakeError(manifestPath, fieldPrefix + ".root", "required package-relative directory string");
        return false;
    }
    out.Root = node["root"].get<std::string>();
    if (!IsSafeRelativeDir(out.Root))
    {
        outError = MakeError(manifestPath, fieldPrefix + ".root",
                             "'" + out.Root + "' must be a package-relative directory (no absolute paths, no '..')");
        return false;
    }

    if (node.contains("prebuilt"))
    {
        if (!node["prebuilt"].is_string())
        {
            outError = MakeError(manifestPath, fieldPrefix + ".prebuilt", "must be a package-relative directory string");
            return false;
        }
        out.Prebuilt = node["prebuilt"].get<std::string>();
        if (!IsSafeRelativeDir(out.Prebuilt))
        {
            outError = MakeError(manifestPath, fieldPrefix + ".prebuilt",
                                 "'" + out.Prebuilt + "' must be a package-relative directory (no absolute paths, no '..')");
            return false;
        }
    }
    return true;
}

// "runtimeAssets": { "<Component>": ["<asset-dir-relative file or folder>", ...] }.
bool TryParseRuntimeAssets(const nlohmann::json& doc,
                           const std::filesystem::path& manifestPath,
                           std::map<std::string, std::vector<std::string>>& out,
                           std::string& outError)
{
    if (!doc.contains("runtimeAssets"))
        return true;
    const auto& node = doc["runtimeAssets"];
    if (!node.is_object())
    {
        outError = MakeError(manifestPath, "runtimeAssets",
                             "must be an object of component name -> array of asset paths");
        return false;
    }
    for (const auto& [component, paths] : node.items())
    {
        const std::string field = "runtimeAssets." + component;
        if (component.empty() || !paths.is_array())
        {
            outError = MakeError(manifestPath, field, "must be a component name with an array of asset paths");
            return false;
        }
        std::vector<std::string>& list = out[component];
        for (const auto& path : paths)
        {
            if (!path.is_string() || !IsSafeRelativeDir(path.get<std::string>()))
            {
                outError = MakeError(manifestPath, field,
                                     "each entry must be a file or folder relative to the package's assets "
                                     "folder (no absolute paths, no '..')");
                return false;
            }
            list.push_back(path.get<std::string>());
        }
    }
    return true;
}

} // namespace

bool IsValidPackageName(std::string_view name)
{
    if (name.empty() || name.size() > kMaxPackageNameLength)
        return false;

    std::string_view scope;
    std::string_view bare = name;
    if (name.front() == '@')
    {
        const size_t slash = name.find('/');
        if (slash == std::string_view::npos || slash == 1 || slash + 1 >= name.size())
            return false;
        scope = name.substr(1, slash - 1);
        bare = name.substr(slash + 1);
        if (!IsValidNameSegment(scope))
            return false;
        if (bare.find('/') != std::string_view::npos)
            return false;
    }
    else if (name.find('/') != std::string_view::npos)
    {
        return false;
    }
    return IsValidNameSegment(bare);
}

bool TryParsePackageManifest(std::string_view jsonText,
                             const std::filesystem::path& manifestPathForErrors,
                             PackageManifest& out,
                             std::string& outError)
{
    out = PackageManifest{};
    outError.clear();

    const nlohmann::json doc =
        nlohmann::json::parse(jsonText.begin(), jsonText.end(), nullptr, /*allow_exceptions=*/false,
                              /*ignore_comments=*/true);
    if (doc.is_discarded())
    {
        outError = MakeError(manifestPathForErrors, {}, "not valid JSON");
        return false;
    }
    if (!doc.is_object())
    {
        outError = MakeError(manifestPathForErrors, {}, "root must be a JSON object");
        return false;
    }

    // name
    if (!doc.contains("name") || !doc["name"].is_string())
    {
        outError = MakeError(manifestPathForErrors, "name", "required string");
        return false;
    }
    out.Name = doc["name"].get<std::string>();
    if (!IsValidPackageName(out.Name))
    {
        outError = MakeError(manifestPathForErrors, "name",
                             "'" + out.Name +
                                 "' is not a valid package name (npm rules: lowercase [a-z0-9-._], "
                                 "optional @scope/name, segments must not start with '.' or '_')");
        return false;
    }

    // version — the package's own version must be a full three-part semver.
    if (!doc.contains("version") || !doc["version"].is_string())
    {
        outError = MakeError(manifestPathForErrors, "version", "required semver string");
        return false;
    }
    {
        std::string versionError;
        const std::string versionText = doc["version"].get<std::string>();
        if (!PackageVersion::TryParse(versionText, out.Version, &versionError) ||
            out.Version.SpecifiedParts != 3)
        {
            outError = MakeError(manifestPathForErrors, "version",
                                 "'" + versionText + "' is not a full semver (expected major.minor.patch)");
            return false;
        }
    }

    // displayName
    if (doc.contains("displayName"))
    {
        if (!doc["displayName"].is_string())
        {
            outError = MakeError(manifestPathForErrors, "displayName", "must be a string");
            return false;
        }
        out.DisplayName = doc["displayName"].get<std::string>();
    }
    if (out.DisplayName.empty())
        out.DisplayName = out.Name;

    // dependencies
    if (doc.contains("dependencies"))
    {
        const auto& deps = doc["dependencies"];
        if (!deps.is_object())
        {
            outError = MakeError(manifestPathForErrors, "dependencies", "must be an object of name -> semver range");
            return false;
        }
        for (const auto& [depName, rangeNode] : deps.items())
        {
            if (!IsValidPackageName(depName))
            {
                outError = MakeError(manifestPathForErrors, "dependencies",
                                     "'" + depName + "' is not a valid package name");
                return false;
            }
            if (!rangeNode.is_string())
            {
                outError = MakeError(manifestPathForErrors, "dependencies." + depName,
                                     "range must be a string");
                return false;
            }
            PackageVersionRange range;
            std::string rangeError;
            const std::string rangeText = rangeNode.get<std::string>();
            if (!PackageVersionRange::TryParse(rangeText, range, &rangeError))
            {
                outError = MakeError(manifestPathForErrors, "dependencies." + depName,
                                     "invalid range '" + rangeText + "': " + rangeError);
                return false;
            }
            out.Dependencies.emplace(depName, range);
        }
    }

    // assets — an explicit "" declares a code-only package (no asset mount,
    // e.g. unity-import); an absent key keeps the "Assets/" default probe.
    if (doc.contains("assets"))
    {
        if (!doc["assets"].is_string())
        {
            outError = MakeError(manifestPathForErrors, "assets", "must be a package-relative directory string");
            return false;
        }
        out.AssetsDir = doc["assets"].get<std::string>();
        if (!out.AssetsDir.empty() && !IsSafeRelativeDir(out.AssetsDir))
        {
            outError = MakeError(manifestPathForErrors, "assets",
                                 "'" + out.AssetsDir +
                                     "' must be a package-relative directory (no absolute paths, no '..')");
            return false;
        }
    }

    // modules — parsed and validated; only `prebuilt` is read (see
    // PackageModuleRecord). The records stay until the schema drops them.
    if (doc.contains("modules"))
    {
        const auto& modules = doc["modules"];
        if (!modules.is_array())
        {
            outError = MakeError(manifestPathForErrors, "modules", "must be an array of module records");
            return false;
        }
        for (size_t i = 0; i < modules.size(); ++i)
        {
            PackageModuleRecord record;
            if (!TryParseModuleRecord(modules[i], i, manifestPathForErrors, record, outError))
                return false;
            out.Modules.push_back(std::move(record));
        }
    }

    if (!TryParseRuntimeAssets(doc, manifestPathForErrors, out.RuntimeAssets, outError))
        return false;

    if (!RejectRemovedKeys(doc, manifestPathForErrors, outError))
        return false;

    if (!TryReadStringArray(doc, "platforms", manifestPathForErrors, out.Platforms, outError))
        return false;

    // enabledByDefault
    if (doc.contains("enabledByDefault"))
    {
        if (!doc["enabledByDefault"].is_boolean())
        {
            outError = MakeError(manifestPathForErrors, "enabledByDefault", "must be a boolean");
            return false;
        }
        out.EnabledByDefault = doc["enabledByDefault"].get<bool>();
    }

    return true;
}

bool TryLoadPackageManifest(const std::filesystem::path& manifestFile,
                            PackageManifest& out,
                            std::string& outError)
{
    String text;
    if (!ReadFileTextShared(manifestFile, text))
    {
        outError = "package.json (" + manifestFile.generic_string() + "): cannot open file";
        return false;
    }
    return TryParsePackageManifest(text, manifestFile, out, outError);
}

} // namespace GameEngine
