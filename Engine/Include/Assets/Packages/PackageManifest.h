#pragma once

#include "Assets/Packages/PackageVersion.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

// One entry of the manifest's "modules" array. A package's code modules are
// discovered from the files under its root (PackageCodeDiscovery), so the
// records are parsed and validated but their name, kind, lang and root are
// read by nothing. Only `prebuilt` is still consumed: a shipped binary is not
// an asset type, so a binaries directory is invisible to the scan.
// The kind and language enums below are the vocabulary discovery produces.
struct PackageModuleRecord
{
    enum class ModuleKind
    {
        Runtime,
        Editor,
    };
    enum class ModuleLang
    {
        CSharp,
        Cpp,
    };

    std::string Name;
    ModuleKind Kind = ModuleKind::Runtime;
    ModuleLang Lang = ModuleLang::CSharp;
    std::string Root;     // package-relative source dir
    std::string Prebuilt; // optional package-relative prebuilt-binaries dir
};

// Parsed package.json (decision 2026-07-15: plain npm-schema-compatible
// package.json; engine fields are top-level and npm tooling ignores them;
// unknown fields are ignored here for the same forward compatibility).
// A key the schema once had is NOT unknown: "defines" and "alwaysShip" are
// refused by name so a stale manifest fails loudly instead of shipping under
// a rule it no longer states (see TryParsePackageManifest).
struct PackageManifest
{
    // npm name rules: lowercase [a-z0-9-._], optional "@scope/name". The name
    // is the package's GUID-namespace identity and must never change once
    // assets ship (frozen GUID derivation contract).
    std::string Name;
    PackageVersion Version;
    std::string DisplayName; // defaults to Name
    // Dependency name -> raw semver range text (validated at parse time).
    std::map<std::string, PackageVersionRange> Dependencies;
    // Package-relative asset dir mounted into the asset database. Default "Assets/".
    std::string AssetsDir = "Assets/";
    std::vector<PackageModuleRecord> Modules;
    // "runtimeAssets": component name -> asset-dir-relative files or folders the
    // package's code loads by path for that component (no dependency edge names
    // them). A build ships them, with their own references, when a built scene or
    // blueprint uses the component.
    std::map<std::string, std::vector<std::string>> RuntimeAssets;
    std::vector<std::string> Platforms;
    bool EnabledByDefault = true;
};

// Validate an npm-style package name: optional "@scope/" prefix; each segment
// is lowercase [a-z0-9] plus interior '-', '.', '_'; segments must not start
// with '.' or '_'; max 214 chars total.
bool IsValidPackageName(std::string_view name);

// Parse a package.json text body. On failure returns false and fills
// outError with a precise message that names the manifest path, the field,
// and why it was rejected. Never throws. The removed keys "defines" and
// "alwaysShip" are rejected by name, each with the rule that replaced it.
bool TryParsePackageManifest(std::string_view jsonText,
                             const std::filesystem::path& manifestPathForErrors,
                             PackageManifest& out,
                             std::string& outError);

// Convenience: read <packageDir>/package.json from disk and parse it.
bool TryLoadPackageManifest(const std::filesystem::path& manifestFile,
                            PackageManifest& out,
                            std::string& outError);

} // namespace GameEngine
