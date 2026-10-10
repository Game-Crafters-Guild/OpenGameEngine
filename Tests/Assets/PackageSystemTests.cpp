// Packages P0: manifest parsing, resolver semantics (topo order, priorities,
// degraded resolution), and end-to-end mounting through AssetManager.

#include "AssetCore/SubassetDeriveKeys.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackageCodeDiscovery.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageGitSource.h"
#include "Assets/Packages/PackageManifest.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/Packages/PackagesIndex.h"
#include "Assets/Packages/PackageVersion.h"
#include "Assets/Packages/ProjectPackagesManifest.h"
#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "Core/EngineLoggerBridge.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/PackageBuildStaging.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

using namespace GameEngine;

namespace
{

namespace fs = std::filesystem;

// CollectPackageCodeModules leases and prunes generations under the global
// package cache, so every test in this process resolves that cache to a
// throwaway root instead of the developer's real one.
class ScratchPackageCacheEnvironment : public testing::Environment
{
  public:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("ge_pkg_cache");
        std::error_code ec;
        fs::create_directories(m_Root, ec);
        Testing::SetEnvVar("GE_PACKAGE_CACHE_DIR", m_Root.string().c_str());
    }
    void TearDown() override
    {
        Testing::SetEnvVar("GE_PACKAGE_CACHE_DIR", "");
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

  private:
    fs::path m_Root;
};

const testing::Environment* const s_ScratchPackageCache =
    testing::AddGlobalTestEnvironment(new ScratchPackageCacheEnvironment);

void WriteTextFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

// Minimal package.json body. deps as raw JSON object body ("\"b\": \"^1.0\"").
std::string PackageJson(const std::string& name,
                        const std::string& version,
                        const std::string& depsJsonBody = {})
{
    std::string json = "{\n  \"name\": \"" + name + "\",\n  \"version\": \"" + version + "\"";
    if (!depsJsonBody.empty())
        json += ",\n  \"dependencies\": { " + depsJsonBody + " }";
    json += "\n}\n";
    return json;
}

// Writes an embedded package under <projectRoot>/Packages/<name>/ with one
// marker asset so the mount has something to register.
void WriteEmbeddedPackage(const fs::path& projectRoot,
                          const std::string& name,
                          const std::string& version,
                          const std::string& depsJsonBody = {})
{
    const fs::path pkgDir = projectRoot / "Packages" / name;
    WriteTextFile(pkgDir / "package.json", PackageJson(name, version, depsJsonBody));
    WriteTextFile(pkgDir / "Assets" / "marker.txt", name + " " + version);
}

std::string JoinNames(const std::vector<ResolvedPackage>& packages)
{
    std::string out;
    for (const auto& pkg : packages)
    {
        if (!out.empty())
            out += ",";
        out += pkg.Manifest.Name;
    }
    return out;
}

bool AnyContains(const std::vector<std::string>& messages, const std::string& needle)
{
    return std::any_of(messages.begin(), messages.end(),
                       [&](const std::string& m) { return m.find(needle) != std::string::npos; });
}

// How many resolved packages MountResolvedPackages is expected to mount. Not
// every resolved package mounts: a code-only package (manifest "assets": "",
// e.g. unity-import) carries an empty AssetsDir and is skipped by design.
size_t CountMountablePackages(const PackageResolution& resolution)
{
    return static_cast<size_t>(
        std::count_if(resolution.MountOrder.begin(), resolution.MountOrder.end(),
                      [](const ResolvedPackage& pkg) { return !pkg.AssetsDir.empty(); }));
}

// Resolved paths come back registry-normalized (lowercased on Windows;
// macOS temp dirs often surface as /var vs /private/var). Compare the
// canonical spelling, case-insensitively.
std::string NormalizeForCompare(const fs::path& p)
{
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec)
        canon = p.lexically_normal();
    std::string s = canon.generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

PackageVersion V(const std::string& text)
{
    PackageVersion v;
    EXPECT_TRUE(PackageVersion::TryParse(text, v));
    return v;
}

bool RangeMatches(const std::string& range, const std::string& version)
{
    PackageVersionRange r;
    EXPECT_TRUE(PackageVersionRange::TryParse(range, r)) << range;
    return r.Matches(V(version));
}

} // namespace

// ---------------------------------------------------------------------------
// Semver
// ---------------------------------------------------------------------------

TEST(PackageVersionRanges, OperatorsMatchNpmSemantics)
{
    // exact (full + npm x-range partials)
    EXPECT_TRUE(RangeMatches("1.2.3", "1.2.3"));
    EXPECT_FALSE(RangeMatches("1.2.3", "1.2.4"));
    EXPECT_TRUE(RangeMatches("2.1", "2.1.5"));
    EXPECT_FALSE(RangeMatches("2.1", "2.2.0"));
    EXPECT_TRUE(RangeMatches("2", "2.9.1"));
    EXPECT_FALSE(RangeMatches("2", "3.0.0"));

    // caret
    EXPECT_TRUE(RangeMatches("^2.1", "2.1.0"));
    EXPECT_TRUE(RangeMatches("^2.1", "2.9.9"));
    EXPECT_FALSE(RangeMatches("^2.1", "3.0.0"));
    EXPECT_FALSE(RangeMatches("^2.1", "2.0.9"));
    EXPECT_TRUE(RangeMatches("^0.2.3", "0.2.9"));
    EXPECT_FALSE(RangeMatches("^0.2.3", "0.3.0"));

    // tilde
    EXPECT_TRUE(RangeMatches("~1.2.3", "1.2.9"));
    EXPECT_FALSE(RangeMatches("~1.2.3", "1.3.0"));
    EXPECT_TRUE(RangeMatches("~1", "1.9.0"));

    // >=
    EXPECT_TRUE(RangeMatches(">=1.2.0", "2.0.0"));
    EXPECT_FALSE(RangeMatches(">=1.2.0", "1.1.9"));

    // any
    EXPECT_TRUE(RangeMatches("*", "0.0.1"));

    // prerelease sorts below release
    EXPECT_TRUE(V("1.2.3-alpha").Compare(V("1.2.3")) < 0);

    PackageVersionRange r;
    EXPECT_FALSE(PackageVersionRange::TryParse("abc", r));
    EXPECT_FALSE(PackageVersionRange::TryParse("1.2.3.4", r));
}

// ---------------------------------------------------------------------------
// package.json parsing
// ---------------------------------------------------------------------------

TEST(PackageManifestParse, FullManifestRoundTrip)
{
    const std::string json = R"({
        "name": "@studio/ocean-pack",
        "version": "1.2.0",
        "displayName": "Ocean Pack",
        "dependencies": { "water-core": "^2.1" },
        "assets": "Content/",
        "modules": [
            { "name": "OceanPack", "kind": "Runtime", "lang": "CSharp", "root": "Scripts/" },
            { "name": "OceanPackNative", "kind": "Editor", "lang": "Cpp", "root": "Native/", "prebuilt": "Binaries/" }
        ],
        "runtimeAssets": { "OceanSurface": ["Textures/Foam/", "Textures/caustics.png"] },
        "platforms": ["Windows", "macOS"],
        "enabledByDefault": false,
        "someUnknownField": { "ignored": true }
    })";

    PackageManifest manifest;
    std::string error;
    ASSERT_TRUE(TryParsePackageManifest(json, "mem://package.json", manifest, error)) << error;
    EXPECT_EQ(manifest.Name, "@studio/ocean-pack");
    EXPECT_EQ(manifest.Version.ToString(), "1.2.0");
    EXPECT_EQ(manifest.DisplayName, "Ocean Pack");
    ASSERT_EQ(manifest.Dependencies.size(), 1u);
    EXPECT_EQ(manifest.Dependencies.begin()->first, "water-core");
    EXPECT_EQ(manifest.AssetsDir, "Content/");
    ASSERT_EQ(manifest.Modules.size(), 2u);
    EXPECT_EQ(manifest.Modules[0].Name, "OceanPack");
    EXPECT_EQ(manifest.Modules[0].Kind, PackageModuleRecord::ModuleKind::Runtime);
    EXPECT_EQ(manifest.Modules[0].Lang, PackageModuleRecord::ModuleLang::CSharp);
    EXPECT_EQ(manifest.Modules[1].Kind, PackageModuleRecord::ModuleKind::Editor);
    EXPECT_EQ(manifest.Modules[1].Lang, PackageModuleRecord::ModuleLang::Cpp);
    EXPECT_EQ(manifest.Modules[1].Prebuilt, "Binaries/");
    ASSERT_EQ(manifest.RuntimeAssets.size(), 1u);
    EXPECT_EQ(manifest.RuntimeAssets.at("OceanSurface"),
              (std::vector<std::string>{"Textures/Foam/", "Textures/caustics.png"}));
    EXPECT_EQ(manifest.Platforms.size(), 2u);
    EXPECT_FALSE(manifest.EnabledByDefault);
}

// A runtimeAssets path must stay inside the package's assets folder.
TEST(PackageManifestParse, RuntimeAssetsPathOutsideThePackageIsRefused)
{
    const std::string json = R"({
        "name": "ocean-pack",
        "version": "1.0.0",
        "runtimeAssets": { "OceanSurface": ["../Shared/foam.png"] }
    })";

    PackageManifest manifest;
    std::string error;
    EXPECT_FALSE(TryParsePackageManifest(json, "mem://package.json", manifest, error));
    EXPECT_NE(error.find("runtimeAssets.OceanSurface"), std::string::npos) << error;
}

// The two keys the schema dropped are refused by name, not ignored: a manifest
// that still carries one is asking for behaviour the engine no longer has, and
// silently shipping it under the replacement rule is the failure mode the
// refusal exists to prevent. Each message names the key and its replacement.
TEST(PackageManifestParse, RemovedDefinesKeyIsRefused)
{
    const std::string json = R"({
        "name": "ocean-pack",
        "version": "1.0.0",
        "defines": ["OCEAN_PACK"]
    })";

    PackageManifest manifest;
    std::string error;
    EXPECT_FALSE(TryParsePackageManifest(json, "mem://package.json", manifest, error));
    EXPECT_NE(error.find("'defines'"), std::string::npos) << error;
    EXPECT_NE(error.find("removed"), std::string::npos) << error;
    EXPECT_NE(error.find("GE_PACKAGE_OCEAN_PACK"), std::string::npos) << error;
}

TEST(PackageManifestParse, RemovedAlwaysShipKeyIsRefused)
{
    const std::string json = R"({
        "name": "ocean-pack",
        "version": "1.0.0",
        "alwaysShip": false
    })";

    PackageManifest manifest;
    std::string error;
    EXPECT_FALSE(TryParsePackageManifest(json, "mem://package.json", manifest, error));
    EXPECT_NE(error.find("'alwaysShip'"), std::string::npos) << error;
    EXPECT_NE(error.find("removed"), std::string::npos) << error;
    EXPECT_NE(error.find("runtimeAssets"), std::string::npos) << error;
}

TEST(PackageManifestParse, DefaultsApply)
{
    PackageManifest manifest;
    std::string error;
    ASSERT_TRUE(TryParsePackageManifest(PackageJson("tiny", "0.1.0"), "mem://package.json", manifest, error))
        << error;
    EXPECT_EQ(manifest.DisplayName, "tiny");
    EXPECT_EQ(manifest.AssetsDir, "Assets/");
    EXPECT_TRUE(manifest.EnabledByDefault);
    EXPECT_TRUE(manifest.Modules.empty());
}

// A code-only package (unity-import: an Editor module, no mountable content)
// declares "assets": "" explicitly. An ABSENT key still means "Assets/" — the
// probe default — so the empty string is the only way to say "no mount", and
// it must not trip the path-escape guard that rejects "../".
TEST(PackageManifestParse, EmptyAssetsDeclaresCodeOnlyPackage)
{
    PackageManifest manifest;
    std::string error;
    ASSERT_TRUE(TryParsePackageManifest(R"({"name":"code-only","version":"1.0.0","assets":""})",
                                        "mem://package.json", manifest, error))
        << error;
    EXPECT_TRUE(manifest.AssetsDir.empty());
}

TEST(PackageManifestParse, MalformedManifestsProducePreciseErrors)
{
    PackageManifest manifest;
    std::string error;
    const fs::path where = "X:/pkg/package.json";

    EXPECT_FALSE(TryParsePackageManifest("not json {", where, manifest, error));
    EXPECT_NE(error.find("X:/pkg/package.json"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(R"({"version":"1.0.0"})", where, manifest, error));
    EXPECT_NE(error.find("'name'"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(R"({"name":"BadUpper","version":"1.0.0"})", where, manifest, error));
    EXPECT_NE(error.find("'name'"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(R"({"name":"ok"})", where, manifest, error));
    EXPECT_NE(error.find("'version'"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(R"({"name":"ok","version":"1.2"})", where, manifest, error));
    EXPECT_NE(error.find("'version'"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(
        R"({"name":"ok","version":"1.0.0","dependencies":{"dep":"nonsense range"}})", where, manifest, error));
    EXPECT_NE(error.find("dependencies.dep"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(
        R"({"name":"ok","version":"1.0.0","modules":[{"name":"M","kind":"Weird","lang":"CSharp","root":"Src/"}]})",
        where, manifest, error));
    EXPECT_NE(error.find("modules[0].kind"), std::string::npos);

    EXPECT_FALSE(TryParsePackageManifest(
        R"({"name":"ok","version":"1.0.0","assets":"../escape/"})", where, manifest, error));
    EXPECT_NE(error.find("'assets'"), std::string::npos);
}

TEST(PackageManifestParse, NameValidationAndAliasSanitization)
{
    EXPECT_TRUE(IsValidPackageName("ocean-pack"));
    EXPECT_TRUE(IsValidPackageName("@studio/water.core"));
    EXPECT_FALSE(IsValidPackageName("Ocean"));
    EXPECT_FALSE(IsValidPackageName(".hidden"));
    EXPECT_FALSE(IsValidPackageName("_private"));
    EXPECT_FALSE(IsValidPackageName("@/x"));
    EXPECT_FALSE(IsValidPackageName("a/b"));
    EXPECT_FALSE(IsValidPackageName(""));

    EXPECT_EQ(SanitizePackageAlias("ocean-pack"), "ocean-pack");
    EXPECT_EQ(SanitizePackageAlias("@studio/water.core"), "studio-water-core");
    // '-' is the alias's one separator, so an underscore in a legal npm name
    // folds into it and the alias keeps the charset the define is derived from.
    EXPECT_EQ(SanitizePackageAlias("grid_tools"), "grid-tools");
}

// ---------------------------------------------------------------------------
// Project manifest
// ---------------------------------------------------------------------------

TEST(ProjectPackagesManifestLoad, MissingFileIsQuietEmpty)
{
    ProjectPackagesManifest manifest;
    std::string error;
    EXPECT_TRUE(TryLoadProjectPackagesManifest("Z:/does/not/exist/manifest.json", manifest, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(manifest.Dependencies.empty());
    EXPECT_TRUE(manifest.Disabled.empty());
}

// Guards the precondition the package tests below depend on: the temp root
// is already canonical, so expectations built from it and the paths the
// engine hands back are one spelling (macOS: /var/folders/… is a symlink to
// /private/var/folders/…).
TEST(TestTempRoot, IsCanonicalSoEngineResolvedPathsCompareEqual)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_tmproot_canonical");
    ASSERT_TRUE(fs::create_directories(tmp));

    std::error_code ec;
    const fs::path canonical = fs::canonical(tmp, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(tmp, canonical)
        << "MakeUniqueTempDirectory must return an already-canonical path.\n"
        << "  returned:  " << tmp.string() << "\n"
        << "  canonical: " << canonical.string();

    fs::remove_all(tmp, ec);
}

TEST(ProjectPackagesManifestLoad, ParsesDependenciesAndDisabled)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_pkg_projmanifest");
    const fs::path file = tmp / "manifest.json";
    WriteTextFile(file, R"({
        "dependencies": { "a": "embedded", "b": "file:../Shared/B" },
        "disabled": ["a"]
    })");

    ProjectPackagesManifest manifest;
    std::string error;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(file, manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.size(), 2u);
    EXPECT_EQ(manifest.Dependencies.at("a"), "embedded");
    EXPECT_EQ(manifest.Dependencies.at("b"), "file:../Shared/B");
    EXPECT_EQ(manifest.Disabled.count("a"), 1u);

    WriteTextFile(file, "{ broken");
    EXPECT_FALSE(TryLoadProjectPackagesManifest(file, manifest, error));
    EXPECT_FALSE(error.empty());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// packages.index (P2 — the packaged game's mount list)
// ---------------------------------------------------------------------------

TEST(PackagesIndexIO, MissingFileIsQuietEmpty)
{
    PackagesIndex index;
    std::string error;
    EXPECT_TRUE(TryLoadPackagesIndex("Z:/does/not/exist/packages.index", index, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(index.Packages.empty());
}

TEST(PackagesIndexIO, RoundTripsEntriesInOrder)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_pkg_index");
    const fs::path file = tmp / "Packages" / kPackagesIndexFileName;

    PackagesIndex written;
    written.Packages.push_back({"water-core", "water-core", "2.1.0", 30, {}});
    written.Packages.push_back({"@studio/ocean-pack", "studio-ocean-pack", "1.2.0", 31,
                                {"StudioOceanPack"}});
    ASSERT_TRUE(SavePackagesIndex(file, written));

    PackagesIndex loaded;
    std::string error;
    ASSERT_TRUE(TryLoadPackagesIndex(file, loaded, error)) << error;
    ASSERT_EQ(loaded.Packages.size(), 2u);
    EXPECT_EQ(loaded.Packages[0].Name, "water-core");
    EXPECT_EQ(loaded.Packages[0].Alias, "water-core");
    EXPECT_EQ(loaded.Packages[0].Priority, 30);
    EXPECT_TRUE(loaded.Packages[0].NativeModules.empty());
    EXPECT_EQ(loaded.Packages[1].Name, "@studio/ocean-pack");
    EXPECT_EQ(loaded.Packages[1].Alias, "studio-ocean-pack");
    EXPECT_EQ(loaded.Packages[1].Version, "1.2.0");
    EXPECT_EQ(loaded.Packages[1].Priority, 31);
    ASSERT_EQ(loaded.Packages[1].NativeModules.size(), 1u);
    EXPECT_EQ(loaded.Packages[1].NativeModules[0], "StudioOceanPack");

    WriteTextFile(file, "{ broken");
    EXPECT_FALSE(TryLoadPackagesIndex(file, loaded, error));
    EXPECT_FALSE(error.empty());

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

TEST(PackageResolverTest, ChainResolvesTopoOrderWithDependentPriorities)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_chain");
    // a -> b -> c
    WriteEmbeddedPackage(projectRoot, "pkg-a", "1.0.0", "\"pkg-b\": \"^2.0\"");
    WriteEmbeddedPackage(projectRoot, "pkg-b", "2.3.1", "\"pkg-c\": \">=1.0.0\"");
    WriteEmbeddedPackage(projectRoot, "pkg-c", "1.5.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded", "pkg-c": "embedded" }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors[0]);
    ASSERT_EQ(res.MountOrder.size(), 3u);
    // Dependencies first...
    EXPECT_EQ(JoinNames(res.MountOrder), "pkg-c,pkg-b,pkg-a");
    // ...and dependents get the HIGHER priority so they shadow dependencies.
    EXPECT_EQ(res.MountOrder[0].Priority, kPackageMountPriorityBase);
    EXPECT_EQ(res.MountOrder[1].Priority, kPackageMountPriorityBase + 1);
    EXPECT_EQ(res.MountOrder[2].Priority, kPackageMountPriorityBase + 2);
    EXPECT_LT(res.MountOrder[2].Priority, 100); // below project band

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

TEST(PackageResolverTest, MissingDependencySkipsPackageOthersMount)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_missingdep");
    WriteEmbeddedPackage(projectRoot, "pkg-a", "1.0.0", "\"no-such-pkg\": \"^1.0\"");
    WriteEmbeddedPackage(projectRoot, "pkg-b", "1.0.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded" }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_EQ(JoinNames(res.MountOrder), "pkg-b");
    EXPECT_TRUE(AnyContains(res.Errors, "no-such-pkg"));

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

TEST(PackageResolverTest, VersionConflictSkipsPackageAndDependents)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_conflict");
    // a requires c@^2.0 but c is 1.5.0; b depends on a and must be skipped too.
    WriteEmbeddedPackage(projectRoot, "pkg-a", "1.0.0", "\"pkg-c\": \"^2.0\"");
    WriteEmbeddedPackage(projectRoot, "pkg-b", "1.0.0", "\"pkg-a\": \"*\"");
    WriteEmbeddedPackage(projectRoot, "pkg-c", "1.5.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded", "pkg-c": "embedded" }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_EQ(JoinNames(res.MountOrder), "pkg-c");
    EXPECT_TRUE(AnyContains(res.Errors, "dependency conflict"));
    EXPECT_TRUE(AnyContains(res.Errors, "pkg-b"));

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

TEST(PackageResolverTest, CycleIsNamedAndSkippedOthersMount)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_cycle");
    WriteEmbeddedPackage(projectRoot, "pkg-a", "1.0.0", "\"pkg-b\": \"*\"");
    WriteEmbeddedPackage(projectRoot, "pkg-b", "1.0.0", "\"pkg-a\": \"*\"");
    WriteEmbeddedPackage(projectRoot, "pkg-c", "1.0.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded", "pkg-c": "embedded" }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_EQ(JoinNames(res.MountOrder), "pkg-c");
    EXPECT_TRUE(AnyContains(res.Errors, "cycle"));
    EXPECT_TRUE(AnyContains(res.Errors, "pkg-a"));
    EXPECT_TRUE(AnyContains(res.Errors, "pkg-b"));

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

TEST(PackageResolverTest, DisabledPackageValidatedNotMountedDependentsSkipped)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_disabled");
    WriteEmbeddedPackage(projectRoot, "pkg-a", "1.0.0");
    WriteEmbeddedPackage(projectRoot, "pkg-b", "1.0.0", "\"pkg-c\": \"*\"");
    WriteEmbeddedPackage(projectRoot, "pkg-c", "1.0.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded", "pkg-c": "embedded" },
        "disabled": ["pkg-c"]
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_EQ(JoinNames(res.MountOrder), "pkg-a");
    ASSERT_EQ(res.Disabled.size(), 1u);
    EXPECT_EQ(res.Disabled[0].Manifest.Name, "pkg-c");
    EXPECT_TRUE(AnyContains(res.Warnings, "pkg-b"));
    EXPECT_TRUE(AnyContains(res.Warnings, "disabled"));

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

TEST(PackageResolverTest, NameMismatchAndAliasCollisionAreLoudErrors)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_naming");
    // Key says "renamed" but the manifest says "actual-name".
    WriteTextFile(projectRoot / "Packages" / "renamed" / "package.json",
                  PackageJson("actual-name", "1.0.0"));
    // "@studio/tools" and "studio-tools" sanitize to the same alias.
    WriteTextFile(projectRoot / "Packages" / "@studio" / "tools" / "package.json",
                  PackageJson("@studio/tools", "1.0.0"));
    WriteEmbeddedPackage(projectRoot, "studio-tools", "1.0.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": {
            "renamed": "embedded",
            "@studio/tools": "embedded",
            "studio-tools": "embedded"
        }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_TRUE(AnyContains(res.Errors, "does not match manifest name"));
    EXPECT_TRUE(AnyContains(res.Errors, "collides"));
    // Exactly one of the colliding pair survives (map order: '@' sorts first).
    EXPECT_EQ(JoinNames(res.MountOrder), "@studio/tools");

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// Two names differing only by '-' versus '_' are ONE alias, so they are also
// one GE_PACKAGE_ macro. The pair is refused by the alias check — the only
// check there is — naming the alias and the fix.
TEST(PackageResolverTest, NamesDifferingOnlyBySeparatorAreOneAliasAndRejected)
{
    const fs::path projectRoot = TestUtils::MakeUniqueTempDirectory("ge_pkg_separator_collide");
    WriteEmbeddedPackage(projectRoot, "grid-tools", "1.0.0");
    WriteEmbeddedPackage(projectRoot, "grid_tools", "1.0.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": {
            "grid-tools": "embedded",
            "grid_tools": "embedded"
        }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_TRUE(AnyContains(res.Errors, "mount alias 'grid-tools' collides"));
    EXPECT_TRUE(AnyContains(res.Errors, "rename one of them"));
    // Exactly one survives (map order: '-' sorts before '_').
    EXPECT_EQ(JoinNames(res.MountOrder), "grid-tools");

    std::error_code ec;
    fs::remove_all(projectRoot, ec);
}

// ---------------------------------------------------------------------------
// Implicit engine packages (<exe dir>/Packages, injected root in tests)
// ---------------------------------------------------------------------------

TEST(PackageResolverTest, EnginePackagesResolveImplicitlyWithoutProjectManifest)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_pkg_engine");
    const fs::path projectRoot = root / "project"; // no Packages/manifest.json at all
    fs::create_directories(projectRoot);
    const fs::path engineRoot = root / "Packages";
    WriteTextFile(engineRoot / "treegen" / "package.json", PackageJson("treegen", "1.1.0"));
    WriteTextFile(engineRoot / "treegen" / "Assets" / "marker.txt", "treegen");
    WriteTextFile(engineRoot / "not-a-package" / "readme.txt", "no manifest here");

    const PackageResolution res = PackageResolver::Resolve(projectRoot, engineRoot);
    EXPECT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors[0]);
    ASSERT_EQ(res.MountOrder.size(), 1u);
    EXPECT_EQ(res.MountOrder.front().Manifest.Name, "treegen");
    EXPECT_EQ(res.MountOrder.front().SourceKind, PackageSourceKind::Engine);
    EXPECT_EQ(res.MountOrder.front().Alias, "treegen");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageResolverTest, ProjectDeclarationShadowsEnginePackageOfSameName)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_pkg_engine_shadow");
    const fs::path projectRoot = root / "project";
    const fs::path engineRoot = root / "Packages";
    WriteTextFile(engineRoot / "treegen" / "package.json", PackageJson("treegen", "1.1.0"));
    // Package-dev flow: the project checks out its own copy of the same package.
    WriteEmbeddedPackage(projectRoot, "treegen", "2.0.0");
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": { "treegen": "embedded" }
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot, engineRoot);
    EXPECT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors[0]);
    ASSERT_EQ(res.MountOrder.size(), 1u);
    EXPECT_EQ(res.MountOrder.front().Manifest.Version.ToString(), "2.0.0");
    EXPECT_EQ(res.MountOrder.front().SourceKind, PackageSourceKind::Embedded);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageResolverTest, DisabledListOptsOutOfEnginePackageWithoutWarning)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_pkg_engine_disable");
    const fs::path projectRoot = root / "project";
    const fs::path engineRoot = root / "Packages";
    WriteTextFile(engineRoot / "treegen" / "package.json", PackageJson("treegen", "1.1.0"));
    WriteTextFile(projectRoot / "Packages" / "manifest.json", R"({
        "dependencies": {},
        "disabled": ["treegen"]
    })");

    const PackageResolution res = PackageResolver::Resolve(projectRoot, engineRoot);
    EXPECT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors[0]);
    EXPECT_TRUE(res.Warnings.empty()) << (res.Warnings.empty() ? "" : res.Warnings[0]);
    EXPECT_TRUE(res.MountOrder.empty());
    ASSERT_EQ(res.Disabled.size(), 1u);
    EXPECT_EQ(res.Disabled.front().Manifest.Name, "treegen");

    std::error_code ec;
    fs::remove_all(root, ec);
}

// The unity-import shape: an engine package that ships only a native Editor
// module. It resolves and loads its module like any other package, but keeps
// an EMPTY AssetsDir so the mount pass skips it silently instead of warning
// "no assets dir" on every project open.
TEST(PackageResolverTest, CodeOnlyEnginePackageResolvesWithEmptyAssetsDir)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_pkg_code_only");
    const fs::path projectRoot = root / "project";
    fs::create_directories(projectRoot);
    const fs::path engineRoot = root / "Packages";
    WriteTextFile(engineRoot / "unity-import" / "package.json",
                  R"({"name":"unity-import","version":"1.0.0","assets":""})");

    const PackageResolution res = PackageResolver::Resolve(projectRoot, engineRoot);
    EXPECT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors[0]);
    EXPECT_TRUE(res.Warnings.empty()) << (res.Warnings.empty() ? "" : res.Warnings[0]);
    ASSERT_EQ(res.MountOrder.size(), 1u);
    EXPECT_EQ(res.MountOrder.front().Manifest.Name, "unity-import");
    EXPECT_TRUE(res.MountOrder.front().AssetsDir.empty())
        << res.MountOrder.front().AssetsDir.string();
    // RootDir still points at the package so the module loader finds Binaries/.
    EXPECT_EQ(NormalizeForCompare(res.MountOrder.front().RootDir),
              NormalizeForCompare(engineRoot / "unity-import"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// End-to-end: resolve + mount into a live AssetManager
// ---------------------------------------------------------------------------

namespace
{

struct E2EProject
{
    fs::path Root;
    AssetManager Manager;

    explicit E2EProject(const char* prefix)
        : Root(TestUtils::MakeUniqueTempDirectory(prefix))
    {
    }

    bool Initialize()
    {
        fs::create_directories(Root / "Assets");
        return Manager.Initialize(Root / "Assets", nullptr,
                                  Root / "AssetDatabase.assetdb",
                                  Root / ".Cache" / "AssetDatabase");
    }

    ~E2EProject()
    {
        Manager.Shutdown();
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

// Captures formatted log lines so a test can assert that a resolution path
// stayed SILENT.
class CapturingLogSink final : public Logger::LogSink
{
  public:
    explicit CapturingLogSink(std::vector<std::string>* out) : m_Out(out) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Out->push_back(message.Message);
    }

    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    Logger::String GetName() const override { return "CapturingLogSink"; }

  private:
    std::vector<std::string>* m_Out;
    std::mutex m_Mutex;
};

} // namespace

TEST(PackageMountE2E, TwoPackagesMountCrossPackageGuidAndAliasUrlsResolve)
{
    E2EProject project("ge_pkg_e2e");

    // pkg-a ships a texture; pkg-b depends on pkg-a and has a scene-like
    // record that references pkg-a's texture by GUID.
    WriteEmbeddedPackage(project.Root, "pkg-a", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Textures" / "foo.png", "PNGDATA");
    WriteEmbeddedPackage(project.Root, "pkg-b", "1.0.0", "\"pkg-a\": \"^1.0\"");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded" }
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    const std::vector<std::string> mounted = MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase");
    ASSERT_EQ(mounted.size(), 2u);
    am.WaitForStartupScan("pkg-a");
    am.WaitForStartupScan("pkg-b");

    // pkg-a's texture is registered under the package mount.
    const fs::path texAbs =
        fs::weakly_canonical(project.Root / "Packages" / "pkg-a" / "Assets" / "Textures" / "foo.png");
    const GUID texGuid = reg.GetAssetGUID(texAbs);
    ASSERT_FALSE(texGuid.IsNull());

    // The unmounted derive helper produces the SAME GUID a mounted derived-identity
    // source registers — build validation relies on this to attribute a dangling
    // reference to a disabled (never-mounted) package's file.
    EXPECT_EQ(AssetRegistry::DeriveGuidForSourcePath("pkg-a", "Textures/foo.png"), texGuid);

    // Write a scene-like record in pkg-b referencing the GUID, then resolve
    // it back through the registry (cross-package GUID reference).
    WriteTextFile(project.Root / "Packages" / "pkg-b" / "Assets" / "scene.ref",
                  texGuid.ToString());
    std::ifstream in(project.Root / "Packages" / "pkg-b" / "Assets" / "scene.ref");
    std::string guidText;
    std::getline(in, guidText);
    const GUID referenced{String(guidText)};
    AssetMetadata meta;
    ASSERT_TRUE(reg.TryGetAssetMetadata(referenced, meta));
    EXPECT_NE(NormalizeForCompare(meta.Path).find("textures/foo.png"), std::string::npos);

    // <alias>: URL resolution via AssetManager::ResolveAssetPath and the
    // registry's alias-scoped GUID resolution.
    const fs::path viaAlias = am.ResolveAssetPath(fs::path("pkg-a:Textures/foo.png"));
    EXPECT_EQ(NormalizeForCompare(viaAlias), NormalizeForCompare(texAbs));
    EXPECT_EQ(reg.ResolveByAlias("pkg-a:Textures/foo.png"), texGuid);

    // Sources present with the resolver's priorities.
    const auto sources = am.GetRegisteredSources();
    const auto findSource = [&](const std::string& alias) -> const AssetSourceDesc* {
        for (const auto& s : sources)
            if (s.Alias == alias)
                return &s;
        return nullptr;
    };
    const AssetSourceDesc* pkgA = findSource("pkg-a");
    const AssetSourceDesc* pkgB = findSource("pkg-b");
    ASSERT_NE(pkgA, nullptr);
    ASSERT_NE(pkgB, nullptr);
    EXPECT_GT(pkgB->Priority, pkgA->Priority); // dependent shadows dependency
    EXPECT_TRUE(pkgA->RejectOverlappingRoots);

    // Per-package DB + cache roots, never the project's.
    EXPECT_EQ(NormalizeForCompare(pkgA->AuthoritativeDbFile),
              NormalizeForCompare(project.Root / "Packages" / "pkg-a" / "AssetDatabase.assetdb"));
    EXPECT_EQ(NormalizeForCompare(pkgA->CacheRoot),
              NormalizeForCompare(project.Root / "Packages" / "pkg-a" / ".Cache" / "AssetDatabase"));

    // Unmount cleanly (project-switch path).
    UnmountPackageSources(am, mounted);
    EXPECT_TRUE(reg.GetAssetsForSource("pkg-a").empty());
    EXPECT_EQ(am.GetSourceRoot("pkg-a"), fs::path());
}

TEST(PackageMountE2E, DisabledPackageNotMountedAndOverlappingRootRejected)
{
    E2EProject project("ge_pkg_e2e_guard");

    WriteEmbeddedPackage(project.Root, "pkg-ok", "1.0.0");
    WriteEmbeddedPackage(project.Root, "pkg-off", "1.0.0");
    // A package whose assets dir nests inside the project's Assets/ root:
    // RejectOverlappingRoots must refuse it at mount time.
    const fs::path overlapDir = project.Root / "OverlapPkg";
    WriteTextFile(overlapDir / "package.json",
                  "{ \"name\": \"pkg-overlap\", \"version\": \"1.0.0\", \"assets\": \"../Assets/\" }");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": {
            "pkg-ok": "embedded",
            "pkg-off": "embedded",
            "pkg-overlap": "file:../OverlapPkg"
        },
        "disabled": ["pkg-off"]
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    // "assets": "../Assets/" is rejected at manifest parse (path escape) —
    // that package errors out before ever reaching the mount.
    EXPECT_TRUE(AnyContains(res.Errors, "pkg-overlap") || AnyContains(res.Errors, "assets"));

    const std::vector<std::string> mounted = MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase");
    ASSERT_EQ(mounted.size(), 1u);
    EXPECT_EQ(mounted[0], "pkg-ok");
    EXPECT_EQ(am.GetSourceRoot("pkg-off"), fs::path());
    EXPECT_EQ(am.GetSourceRoot("pkg-overlap"), fs::path());
}

TEST(PackageMountE2E, OverlappingRootPackageRejectedLoudlyAtMount)
{
    E2EProject project("ge_pkg_e2e_overlap");

    // Package located INSIDE the project's Assets/ tree with a legal-looking
    // manifest: the resolver can't see the nesting (that's registry policy),
    // so RegisterSource's RejectOverlappingRoots must catch it.
    const fs::path nested = project.Root / "Assets" / "NestedPkg";
    WriteTextFile(nested / "package.json", PackageJson("pkg-nested", "1.0.0"));
    WriteTextFile(nested / "Assets" / "n.txt", "n");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-nested": "file:../Assets/NestedPkg" }
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_EQ(res.MountOrder.size(), 1u); // resolver is fine with it...
    const std::vector<std::string> mounted = MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase");
    EXPECT_TRUE(mounted.empty()); // ...the registry's overlap policy is not.
    EXPECT_EQ(am.GetSourceRoot("pkg-nested"), fs::path());
}

TEST(PackageMountE2E, OverlappingRootPackageReachedThroughSymlinkRejectedAtMount)
{
    E2EProject project("ge_pkg_e2e_overlap_link");

    // The same nested package, but the manifest reaches it through a symlink
    // that lives OUTSIDE the project: lexically the mount root shares no
    // prefix with <project>/Assets, physically it is the same directory.
    const fs::path nested = project.Root / "Assets" / "NestedPkg";
    WriteTextFile(nested / "package.json", PackageJson("pkg-nested", "1.0.0"));
    WriteTextFile(nested / "Assets" / "n.txt", "n");
    const TestUtils::ScopedTempDir elsewhere(TestUtils::MakeUniqueTempDirectory("ge_pkg_e2e_overlap_link_via"));
    const fs::path link = elsewhere.Path() / "link";
    std::error_code ec;
    fs::create_directory_symlink(nested, link, ec);
    if (ec)
        GTEST_SKIP() << "directory symlink not supported: " << ec.message();
    WriteTextFile(project.Root / "Packages" / "manifest.json",
                  "{ \"dependencies\": { \"pkg-nested\": \"file:" + link.generic_string() + "\" } }");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_EQ(res.MountOrder.size(), 1u);
    EXPECT_EQ(res.MountOrder[0].AssetsDir, link / "Assets"); // the spec's spelling, symlink unresolved
    const std::vector<std::string> mounted = MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase");
    EXPECT_TRUE(mounted.empty());
    EXPECT_EQ(am.GetSourceRoot("pkg-nested"), fs::path());
}

TEST(PackageMountE2E, ProjectAssetShadowsPackageAssetAtSameLogicalPath)
{
    E2EProject project("ge_pkg_e2e_shadow");

    WriteEmbeddedPackage(project.Root, "pkg-a", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Shared" / "logo.png", "PKG");
    WriteTextFile(project.Root / "Assets" / "Shared" / "logo.png", "PROJECT");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded" }
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    const std::vector<std::string> mounted = MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase");
    ASSERT_EQ(mounted.size(), 1u);
    am.WaitForStartupScan("pkg-a");
    am.WaitForStartupScan("project");

    // Implicit resolution: project (priority 100) wins over the package (30).
    const fs::path resolved = am.ResolveAssetPath(fs::path("Shared/logo.png"));
    EXPECT_EQ(NormalizeForCompare(resolved),
              NormalizeForCompare(project.Root / "Assets" / "Shared" / "logo.png"));

    // Alias-scoped resolution still reaches the shadowed package copy.
    const fs::path pkgCopy = am.ResolveAssetPath(fs::path("Shared/logo.png"), "pkg-a");
    EXPECT_EQ(NormalizeForCompare(pkgCopy),
              NormalizeForCompare(project.Root / "Packages" / "pkg-a" / "Assets" / "Shared" / "logo.png"));
}

// P2.1 GAP 5 negative leg: a scene referencing a DISABLED package's asset must
// FAIL the packaged-build validation — even when the scene's dependency edges
// were extracted BEFORE the reference was authored. Extraction is lazy and
// never invalidated on file edits, so the collector's scene walk must
// re-extract the built roots at build time instead of trusting the cache.
TEST(PackageMountE2E, BuildFailsWhenSceneReferencesDisabledPackageAsset)
{
    E2EProject project("ge_pkg_e2e_disabled_ref");

    WriteEmbeddedPackage(project.Root, "heavy-pack", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "heavy-pack" / "Assets" / "Textures" / "rock.png",
                  "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "heavy-pack": "embedded" },
        "disabled": ["heavy-pack"]
    })");

    // The scene exists at scan time WITHOUT the package reference.
    const char* kSceneNoRef =
        "[scene name=\"main\"]\n"
        "\n"
        "[entity id=\"e1\"]\n"
        "Transform.position = 0,0,0\n";
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene", kSceneNoRef);

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();
    am.WaitForStartupScan("project");

    const GUID sceneGuid = reg.GetAssetGUID(project.Root / "Assets" / "Scenes" / "main.scene");
    ASSERT_FALSE(sceneGuid.IsNull());

    // Prime the lazy extraction on the referenceless scene: the cached edge set
    // is now "extracted" and EMPTY — the live-run failure mode.
    EXPECT_TRUE(reg.GetDependencies(sceneGuid).empty());

    // Author the disabled-package reference AFTER extraction (lazy extraction
    // is never invalidated on edit, so the cache stays empty/stale).
    const GUID disabledGuid =
        AssetRegistry::DeriveGuidForSourcePath("heavy-pack", "Textures/rock.png");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  std::string(kSceneNoRef) +
                      "MeshRenderer.material = \"" + std::string(disabledGuid.ToString()) + "\"\n");

    const PackageResolution resolution = PackageResolver::Resolve(project.Root);
    ASSERT_EQ(resolution.Disabled.size(), 1u);
    ASSERT_TRUE(resolution.MountOrder.empty());

    // The collector's scene walk re-extracts the built root, so the dangling
    // reference surfaces as an unresolved dependency...
    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
    ASSERT_FALSE(manifest.unresolvedDependencies.empty());
    EXPECT_NE(std::find(manifest.unresolvedDependencies.begin(),
                        manifest.unresolvedDependencies.end(), disabledGuid),
              manifest.unresolvedDependencies.end());

    // ...and validation attributes it to the disabled package and FAILS the build.
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    EXPECT_FALSE(ValidatePackagedBuild(manifest, resolution, {}, {}, errors, warnings));
    EXPECT_TRUE(AnyContains(errors, "DISABLED package"));
    EXPECT_TRUE(AnyContains(errors, "heavy-pack"));
    EXPECT_TRUE(AnyContains(errors, "Textures/rock.png"));
}

// P2.2 BUG C: the LIVE-editor shape of the disabled-package reference. The
// package was enabled when the session started, so it is MOUNTED; the user then
// disables it in Packages/manifest.json without restarting. At build time the
// resolver reports it disabled, but its mount is still registered — every GUID
// resolves, the collector emits normal entries (nothing unresolved), and the
// unresolved-dependency net is blind. Ownership validation over the collected
// entries' source aliases must fail the build with attribution anyway.
TEST(PackageMountE2E, BuildFailsWhenDisabledPackageIsStillMounted)
{
    E2EProject project("ge_pkg_e2e_disabled_mounted");

    WriteEmbeddedPackage(project.Root, "heavy-pack", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "heavy-pack" / "Assets" / "Textures" / "rock.png",
                  "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "heavy-pack": "embedded" }
    })");

    const GUID packageGuid =
        AssetRegistry::DeriveGuidForSourcePath("heavy-pack", "Textures/rock.png");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n"
                  "\n"
                  "[entity id=\"e1\"]\n"
                  "MeshRenderer.material = \"" +
                      std::string(packageGuid.ToString()) + "\"\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    // Session start: the package is enabled and mounts normally.
    const PackageResolution enabled = PackageResolver::Resolve(project.Root);
    ASSERT_EQ(enabled.MountOrder.size(), 1u);
    ASSERT_EQ(MountResolvedPackages(am, enabled, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("heavy-pack");
    am.WaitForStartupScan("project");

    // Mid-session: the user disables the package; the mount stays registered.
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "heavy-pack": "embedded" },
        "disabled": ["heavy-pack"]
    })");
    const PackageResolution resolution = PackageResolver::Resolve(project.Root);
    ASSERT_EQ(resolution.Disabled.size(), 1u);
    ASSERT_TRUE(resolution.MountOrder.empty());

    // The mounted GUID resolves, so the collector emits a normal entry — the
    // unresolved net never fires...
    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());
    const AssetManifestEntry* entry = manifest.FindByGuid(packageGuid);
    ASSERT_NE(entry, nullptr) << "mounted disabled-package asset must have been collected";
    EXPECT_EQ(entry->sourceAlias, "heavy-pack");

    // ...but ownership validation attributes it and FAILS the build. Session
    // mounts deliberately withheld: this pins the per-entry ownership net on
    // its own (the session-mount net is pinned by
    // MidSessionDisabledPackageFailsBuildValidation below).
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    EXPECT_FALSE(ValidatePackagedBuild(manifest, resolution, {}, {}, errors, warnings));
    EXPECT_TRUE(AnyContains(errors, "DISABLED package"));
    EXPECT_TRUE(AnyContains(errors, "heavy-pack"));
    EXPECT_TRUE(AnyContains(errors, "Textures/rock.png"));
    EXPECT_TRUE(AnyContains(errors, "still mounted"));
}

// P4b FINDING A regression — the live demo's exact failure shape. A scene whose
// MeshRenderer.material is saved in the canonical AssetRef form
// `[path="..." guid="..."]` references a PROJECT .material whose albedoMap
// references a PACKAGE texture by GUID. The collector must transitively stage
// scene -> material -> texture and attribute the texture to its owning package.
// Before the fix the scene parser treated the whole bracketed AssetRef as a
// garbage TargetPath, the walk found zero edges, and the shipped manifest
// contained ONLY the scene.
TEST(PackageMountE2E, CollectFromScenesStagesSceneMaterialPackageTextureChain)
{
    E2EProject project("ge_pkg_e2e_depchain");

    WriteEmbeddedPackage(project.Root, "tex-pack", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "tex-pack" / "Assets" / "Textures" / "grid.png",
                  "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "tex-pack": "embedded" }
    })");

    // The package texture GUID is derivable before mount (derived identity), so
    // the material can be authored up front, exactly like a checked-in project.
    const GUID texGuid = AssetRegistry::DeriveGuidForSourcePath("tex-pack", "Textures/grid.png");
    WriteTextFile(project.Root / "Assets" / "Materials" / "rock.material",
                  std::string("{\n"
                              "  \"schemaVersion\": 3,\n"
                              "  \"materialName\": \"rock\",\n"
                              "  \"textures\": {\n"
                              "    \"albedoMap\": { \"guid\": \"") +
                      texGuid.ToString() +
                      "\", \"path\": \"Textures/grid.png\" }\n"
                      "  }\n"
                      "}\n");
    // Placeholder scene so the scan registers it; the real content (which needs
    // the material's scanned GUID) is authored after startup, mirroring an
    // in-session save. The collector re-extracts built roots, so the fresh
    // content is what the walk sees.
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n\n[entity id=\"e1\"]\nTransform.position = 0,0,0\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty());
    ASSERT_EQ(MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("tex-pack");
    am.WaitForStartupScan("project");

    ASSERT_FALSE(reg.GetAssetGUID(fs::weakly_canonical(
                                      project.Root / "Packages" / "tex-pack" / "Assets" /
                                      "Textures" / "grid.png"))
                     .IsNull());

    const fs::path matAbs = project.Root / "Assets" / "Materials" / "rock.material";
    const GUID matGuid = reg.GetAssetGUID(matAbs);
    ASSERT_FALSE(matGuid.IsNull());

    const GUID sceneGuid = reg.GetAssetGUID(project.Root / "Assets" / "Scenes" / "main.scene");
    ASSERT_FALSE(sceneGuid.IsNull());

    // Author the material reference in the canonical save form (what
    // FormatAssetReferenceForSave writes for MeshRenderer.material).
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n"
                  "\n"
                  "[entity id=\"e1\"]\n"
                  "MeshRenderer.material = [path=\"Materials/rock.material\" guid=\"" +
                      std::string(matGuid.ToString()) + "\"]\n");

    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());

    EXPECT_NE(manifest.FindByGuid(sceneGuid), nullptr) << "scene missing from manifest";
    EXPECT_NE(manifest.FindByGuid(matGuid), nullptr)
        << "scene -> material edge (canonical AssetRef form) did not materialize";
    const AssetManifestEntry* texEntry = manifest.FindByGuid(texGuid);
    ASSERT_NE(texEntry, nullptr) << "material -> package texture edge did not materialize";
    EXPECT_EQ(texEntry->sourceAlias, "tex-pack");
    EXPECT_EQ(texEntry->outputPath.generic_string(), "Packages/tex-pack/Assets/Textures/grid.png");

    // Same-session edit freshness: rewriting the material and re-statting it
    // must invalidate the cached extraction so the next query sees fresh edges
    // (the demo needed a manual file-touch nudge here).
    (void)reg.GetDependencies(matGuid); // prime the lazy extraction
    WriteTextFile(matAbs,
                  "{\n  \"schemaVersion\": 3,\n  \"materialName\": \"rock\",\n  \"textures\": {}\n}\n");
    ASSERT_TRUE(reg.TryUpdateFilesystemMetadata(matAbs));
    EXPECT_TRUE(reg.GetDependencies(matGuid).empty())
        << "stale dependency edges served after a content edit";
}

// An Animator plays an embedded clip from a model other than the mesh it drives.
// The durable source pair is the only thing in the scene that names that model,
// so the dependency walk is what puts it in a shipped build.
TEST(PackageMountE2E, CollectFromScenesStagesTheAnimatorSourceModel)
{
    E2EProject project("ge_pkg_e2e_animsource");
    WriteTextFile(project.Root / "Assets" / "Models" / "character.fbx", "FBXDATA");
    WriteTextFile(project.Root / "Assets" / "Animations" / "run.fbx", "FBXDATA");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n\n[entity id=\"e1\"]\nTransform.position = 0,0,0\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();
    am.WaitForStartupScan("project");

    const GUID unreferenced = reg.GetAssetGUID(project.Root / "Assets" / "Models" / "character.fbx");
    const GUID sourceGuid = reg.GetAssetGUID(project.Root / "Assets" / "Animations" / "run.fbx");
    ASSERT_FALSE(unreferenced.IsNull());
    ASSERT_FALSE(sourceGuid.IsNull());

    // The canonical save form for an embedded clip: the container plus the index.
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n"
                  "\n"
                  "[entity id=\"e1\"]\n"
                  "Animator.clipSourceModelGuid = [path=\"Animations/run.fbx\" guid=\"" +
                      std::string(sourceGuid.ToString()) +
                      "\"]\n"
                      "Animator.clipSourceAnimationIndex = 0\n");

    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
    EXPECT_NE(manifest.FindByGuid(sourceGuid), nullptr)
        << "the Animator's embedded source model did not reach the manifest";
    EXPECT_EQ(manifest.FindByGuid(unreferenced), nullptr)
        << "control: an unreferenced project model must not ride along";

    // The pre-pair form named only the derived subasset identity, which has no
    // registry record of its own (AssetCore/SubassetDeriveKeys.h), so nothing in
    // the scene named the container.
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n"
                  "\n"
                  "[entity id=\"e1\"]\n"
                  "Animator.clipGuid = [guid=\"" +
                      std::string(GUID::Derive(sourceGuid, EmbeddedClipDeriveKey(0)).ToString()) +
                      "\"]\n");
    AssetManifest legacy = collector.CollectFromScenes({"Scenes/main.scene"});
    EXPECT_EQ(legacy.FindByGuid(sourceGuid), nullptr)
        << "control: a derived clip GUID cannot name its container";
}

// P2.3: the packaged re-run defect, through the REAL acquisition path. A
// package is enabled and MOUNTED at session start; the user then disables it
// in Packages/manifest.json without reopening the project. The build-time
// resolution (fresh from the edited manifest — the same acquisition
// BuildPipeline::ExecuteImpl performs) drops it from MountOrder, so the
// MountOrder-gated package collection stages nothing of it: the package
// asset silently vanishes from the manifest ("Copied 1 assets" instead of 2)
// with no collected entry and no unresolved GUID for the reference nets to
// attribute — the first failure used to be the native ABI define gate at [70].
// The session-mount net must fail validation at PreAssetCopy instead.
TEST(PackageMountE2E, MidSessionDisabledPackageFailsBuildValidation)
{
    E2EProject project("ge_pkg_e2e_disable_midsession");

    const fs::path pkgDir = project.Root / "Packages" / "heavy-pack";
    WriteTextFile(pkgDir / "package.json",
                  "{ \"name\": \"heavy-pack\", \"version\": \"1.0.0\" }\n");
    WriteTextFile(pkgDir / "Assets" / "Shaders" / "rock.glsl", "// rock surface\n");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "heavy-pack": "embedded" }
    })");
    // The scene does NOT reference the package asset — it only ever ships via
    // the package's Shaders/ folder, so there is no dependency edge for the
    // ownership net to see.
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n"
                  "\n"
                  "[entity id=\"e1\"]\n"
                  "Transform.position = 0,0,0\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    // Session start: enabled, mounted (the initial resolution the editor made).
    const PackageResolution sessionResolution = PackageResolver::Resolve(project.Root);
    ASSERT_EQ(sessionResolution.MountOrder.size(), 1u);
    const std::vector<std::string> sessionMounted =
        MountResolvedPackages(am, sessionResolution, project.Root / ".Cache" / "AssetDatabase");
    ASSERT_EQ(sessionMounted.size(), 1u);
    am.WaitForStartupScan("heavy-pack");
    am.WaitForStartupScan("project");

    const GUID packageAssetGuid =
        AssetRegistry::DeriveGuidForSourcePath("heavy-pack", "Shaders/rock.glsl");

    // Enabled-leg baseline: the package step collects the package's shader.
    {
        AssetCollector collector(am);
        AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
        for (const ResolvedPackage& package : sessionResolution.MountOrder)
            collector.CollectPackageRuntimeAssets(package, {}, manifest);
        ASSERT_NE(manifest.FindByGuid(packageAssetGuid), nullptr);
    }

    // Mid-session: disable on disk; the mount stays registered.
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "heavy-pack": "embedded" },
        "disabled": ["heavy-pack"]
    })");

    // Build-time acquisition — the function the pipeline itself uses.
    const PackageResolution resolution =
        ResolveBuildTimePackages(project.Root, sessionMounted);
    ASSERT_EQ(resolution.Disabled.size(), 1u);
    ASSERT_TRUE(resolution.MountOrder.empty());

    // Real collection: the package asset drops from the manifest with nothing
    // unresolved — the silent-drop shape.
    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
    for (const ResolvedPackage& package : resolution.MountOrder)
        collector.CollectPackageRuntimeAssets(package, {}, manifest);
    EXPECT_EQ(manifest.FindByGuid(packageAssetGuid), nullptr);
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());

    // Validation must fail it anyway, attributed to the mid-session disable.
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    EXPECT_FALSE(ValidatePackagedBuild(manifest, resolution, {}, sessionMounted, errors,
                                       warnings));
    EXPECT_TRUE(AnyContains(errors, "DISABLED"));
    EXPECT_TRUE(AnyContains(errors, "heavy-pack"));
    EXPECT_TRUE(AnyContains(errors, "still mounted"));
}

// ---------------------------------------------------------------------------
// P1 code modules: package-name -> assembly-name mapping and the flattening of
// a resolution into compile-graph module descriptors (defines propagation,
// dependency references, editor/runtime kinds).
// ---------------------------------------------------------------------------

namespace
{

// A code module a fixture package delivers. Discovery reads files, not
// declarations, so a fixture module is one source file of the right language in
// the directory the module roots at; its kind follows from an "Editor" segment
// in that directory, exactly as it does for a real package.
struct FixtureModule
{
    PackageModuleRecord::ModuleLang Lang = PackageModuleRecord::ModuleLang::CSharp;
    std::string Root; // package-relative source dir
};

// Build a ResolvedPackage whose sources exist on disk, for
// CollectPackageCodeModules to discover.
ResolvedPackage MakeResolvedPackage(const std::string& name,
                                    const fs::path& rootDir,
                                    const std::vector<FixtureModule>& modules,
                                    std::vector<std::string> deps = {})
{
    ResolvedPackage pkg;
    pkg.Manifest.Name = name;
    EXPECT_TRUE(PackageVersion::TryParse("1.0.0", pkg.Manifest.Version));
    for (const FixtureModule& module : modules)
    {
        const bool isCpp = module.Lang == PackageModuleRecord::ModuleLang::Cpp;
        WriteTextFile(rootDir / module.Root / (isCpp ? "Module.cpp" : "Module.cs"),
                      "// package module fixture");
    }
    for (const std::string& dep : deps)
    {
        PackageVersionRange range;
        EXPECT_TRUE(PackageVersionRange::TryParse("^1.0.0", range));
        pkg.Manifest.Dependencies.emplace(dep, range);
    }
    pkg.RootDir = rootDir;
    pkg.AssetsDir = rootDir / "Assets";
    pkg.Alias = SanitizePackageAlias(name);
    return pkg;
}

const PackageCodeModule* FindModule(const std::vector<PackageCodeModule>& modules,
                                    const std::string& assemblyName)
{
    for (const PackageCodeModule& m : modules)
        if (m.AssemblyName == assemblyName)
            return &m;
    return nullptr;
}

} // namespace

TEST(PackageCodeModules, AssemblyNameMappingIsDeterministic)
{
    EXPECT_EQ(PackageAssemblyName("sample-pack"), "SamplePack");
    EXPECT_EQ(PackageAssemblyName("@studio/water.core"), "StudioWaterCore");
    EXPECT_EQ(PackageAssemblyName("ocean"), "Ocean");
    EXPECT_EQ(PackageAssemblyName("a_b"), "A_b");
    EXPECT_EQ(PackageAssemblyName("2d-tools"), "2dTools");
}

// The one rule PackageDefine implements: "GE_PACKAGE_" + the sanitized mount
// alias, upper-cased, '-' written as '_'. Change any part of it (prefix,
// case, separator) and this goes red — game code's `#if` spellings and every
// shipped package's compile graph key off it.
TEST(PackageCodeModules, DefineMappingIsDeterministic)
{
    EXPECT_EQ(PackageDefine("ocean-pack"), "GE_PACKAGE_OCEAN_PACK");
    EXPECT_EQ(PackageDefine("@studio/water.core"), "GE_PACKAGE_STUDIO_WATER_CORE");
    EXPECT_EQ(PackageDefine("ocean"), "GE_PACKAGE_OCEAN");
    // Via the alias fold: "a_b" mounts as "a-b" and exports the same macro.
    EXPECT_EQ(PackageDefine("a_b"), "GE_PACKAGE_A_B");
    // A digit-leading name is only a legal macro because of the prefix.
    EXPECT_EQ(PackageDefine("2d-tools"), "GE_PACKAGE_2D_TOOLS");
}

// The property that makes a define-collision check unnecessary: the define is
// the mount alias rewritten one character for one character, so lower-casing
// it and writing '_' back as '-' returns the alias. Two packages cannot share
// a macro without sharing an alias, which the resolver already refuses
// (PackageResolverTest.NamesDifferingOnlyBySeparatorAreOneAliasAndRejected).
TEST(PackageCodeModules, DefineRoundTripsToTheMountAlias)
{
    constexpr std::string_view kPrefix = "GE_PACKAGE_";
    for (const char* name :
         {"ocean", "ocean-pack", "grid_tools", "@studio/water.core", "2d-tools", "a.b_c-d"})
    {
        const std::string define = PackageDefine(name);
        ASSERT_EQ(define.rfind(kPrefix, 0), 0u) << name;

        std::string alias = define.substr(kPrefix.size());
        for (char& c : alias)
        {
            if (c == '_')
                c = '-';
            else if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        EXPECT_EQ(alias, SanitizePackageAlias(name)) << name;
    }
}

// ---------------------------------------------------------------------------
// Code discovery: which files under a package root make which modules.
// ---------------------------------------------------------------------------

TEST(PackageCodeDiscovery, PartitionsSourcesByExtensionAndEditorSegment)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_discovery"));
    const fs::path root = tmp.Path() / "ocean-pack";

    // Four modules: C++ runtime and editor, C# runtime and editor.
    WriteTextFile(root / "Native" / "Ocean.cpp", "");
    WriteTextFile(root / "Native" / "Detail" / "Wave.cpp", "");
    WriteTextFile(root / "Native" / "Ocean.h", "");
    WriteTextFile(root / "Editor" / "OceanInspector.cpp", "");
    WriteTextFile(root / "Scripts" / "Ocean.cs", "");
    WriteTextFile(root / "Editor" / "Tools" / "OceanWindow.cs", "");

    // Content, another scripting language, a shipped binary, and two trees the
    // asset scan never indexes: none of them is a module or widens one.
    WriteTextFile(root / "Assets" / "Textures" / "foam.png", "");
    WriteTextFile(root / "Assets" / "Shaders" / "ocean.glsl", "");
    WriteTextFile(root / "Scripts" / "legacy.lua", "");
    WriteTextFile(root / "Binaries" / "win-x64" / "Ocean.dll", "");
    WriteTextFile(root / ".Cache" / "build" / "CMakeCXXCompilerId.cpp", "");
    WriteTextFile(root / "obj" / "Debug" / "AssemblyInfo.cs", "");

    const std::vector<DiscoveredPackageModule> modules = DiscoverPackageCodeModules(root, "ocean-pack");
    ASSERT_EQ(modules.size(), 4u);

    EXPECT_EQ(modules[0].Lang, ML::CSharp);
    EXPECT_EQ(modules[0].Kind, MK::Runtime);
    EXPECT_EQ(NormalizeForCompare(modules[0].RootDir), NormalizeForCompare(root / "Scripts"));

    EXPECT_EQ(modules[1].Lang, ML::CSharp);
    EXPECT_EQ(modules[1].Kind, MK::Editor);
    EXPECT_EQ(NormalizeForCompare(modules[1].RootDir),
              NormalizeForCompare(root / "Editor" / "Tools"));

    // The root is the deepest directory holding every file of the module, so
    // Native/Detail and the header both sit under it.
    EXPECT_EQ(modules[2].Lang, ML::Cpp);
    EXPECT_EQ(modules[2].Kind, MK::Runtime);
    EXPECT_EQ(NormalizeForCompare(modules[2].RootDir), NormalizeForCompare(root / "Native"));

    EXPECT_EQ(modules[3].Lang, ML::Cpp);
    EXPECT_EQ(modules[3].Kind, MK::Editor);
    EXPECT_EQ(NormalizeForCompare(modules[3].RootDir), NormalizeForCompare(root / "Editor"));
}

TEST(PackageCodeDiscovery, HeadersAloneBuildNothingButWidenTheRoot)
{
    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_discovery_hdr"));

    const fs::path headerOnly = tmp.Path() / "header-only";
    WriteTextFile(headerOnly / "Include" / "Ocean.h", "");
    EXPECT_TRUE(DiscoverPackageCodeModules(headerOnly, "header-only").empty());

    // With a translation unit the module exists, and its root reaches the
    // headers — the generated build puts the root on the include path.
    const fs::path withSource = tmp.Path() / "with-source";
    WriteTextFile(withSource / "Include" / "Ocean.h", "");
    WriteTextFile(withSource / "Source" / "Ocean.cpp", "");
    const std::vector<DiscoveredPackageModule> modules =
        DiscoverPackageCodeModules(withSource, "with-source");
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].Lang, PackageModuleRecord::ModuleLang::Cpp);
    EXPECT_EQ(NormalizeForCompare(modules[0].RootDir), NormalizeForCompare(withSource));
}

// A module compiles every source under its root, so a runtime root that
// contains the editor root would compile the editor half into the runtime
// module. Both go, and only for the language that nests.
TEST(PackageCodeDiscovery, NestedRuntimeAndEditorRootsAreRefusedPerLanguage)
{
    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_discovery_nest"));
    const fs::path root = tmp.Path() / "nesting-pack";

    WriteTextFile(root / "Ocean.cpp", "");                   // C++ runtime at the package root
    WriteTextFile(root / "Editor" / "OceanInspector.cpp", ""); // C++ editor under it
    WriteTextFile(root / "Scripts" / "Ocean.cs", "");          // C# is unaffected

    const std::vector<DiscoveredPackageModule> modules =
        DiscoverPackageCodeModules(root, "nesting-pack");
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].Lang, PackageModuleRecord::ModuleLang::CSharp);
    EXPECT_EQ(NormalizeForCompare(modules[0].RootDir), NormalizeForCompare(root / "Scripts"));
}

TEST(PackageCodeDiscovery, MissingRootYieldsNoModules)
{
    EXPECT_TRUE(DiscoverPackageCodeModules(fs::path("X:/no/such/package"), "ghost").empty());
    EXPECT_TRUE(DiscoverPackageCodeModules(fs::path(), "ghost").empty());
}

// The one module fact discovery cannot answer: a shipped binaries directory
// holds no source, so the manifest still declares it and it lands on the
// module whose language and kind it names.
TEST(PackageCodeModules, DeclaredPrebuiltDirLandsOnTheMatchingModule)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_prebuilt"));
    ResolvedPackage pkg = MakeResolvedPackage("eztree", tmp.Path() / "eztree",
                                              {{ML::Cpp, "Native/"}, {ML::Cpp, "Editor/"}});
    const auto declare = [&pkg](MK kind, const std::string& prebuilt) {
        PackageModuleRecord record;
        record.Kind = kind;
        record.Lang = ML::Cpp;
        record.Root = kind == MK::Editor ? "Editor/" : "Native/";
        record.Prebuilt = prebuilt;
        pkg.Manifest.Modules.push_back(std::move(record));
    };
    declare(MK::Runtime, "Binaries/Runtime/");
    declare(MK::Editor, "Binaries/Editor/");

    PackageResolution res;
    res.MountOrder.push_back(std::move(pkg));

    const std::vector<PackageCodeModule> modules = CollectPackageCodeModules(res, /*editorContext=*/true);
    ASSERT_EQ(modules.size(), 2u);
    const PackageCodeModule* runtime = FindModule(modules, "Eztree");
    const PackageCodeModule* editor = FindModule(modules, "Eztree.Editor");
    ASSERT_NE(runtime, nullptr);
    ASSERT_NE(editor, nullptr);
    // Trailing separators normalize away so downstream comparisons are exact.
    EXPECT_EQ(NormalizeForCompare(runtime->PrebuiltDir),
              NormalizeForCompare(tmp.Path() / "eztree" / "Binaries" / "Runtime"));
    EXPECT_EQ(NormalizeForCompare(editor->PrebuiltDir),
              NormalizeForCompare(tmp.Path() / "eztree" / "Binaries" / "Editor"));
}

// A package that ships binaries and no sources still delivers its native
// module: the scan cannot see a .dll, so the manifest's `prebuilt` alone names
// it, with no source root for the loaders to compile.
TEST(PackageCodeModules, PrebuiltOnlyNativeModuleIsDeliveredWithoutSources)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_prebuilt_only"));
    ResolvedPackage pkg = MakeResolvedPackage("closed-tree", tmp.Path() / "closed-tree", {});
    const auto declare = [&pkg](ML lang, MK kind, const std::string& prebuilt) {
        PackageModuleRecord record;
        record.Kind = kind;
        record.Lang = lang;
        record.Root = "Native/";
        record.Prebuilt = prebuilt;
        pkg.Manifest.Modules.push_back(std::move(record));
    };
    declare(ML::Cpp, MK::Runtime, "Binaries/Runtime/");
    declare(ML::Cpp, MK::Editor, "Binaries/Editor/");

    const std::vector<DiscoveredPackageModule> delivered = ModulesDeliveredBy(pkg);
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0].Lang, ML::Cpp);
    EXPECT_EQ(delivered[0].Kind, MK::Runtime);
    EXPECT_TRUE(delivered[0].RootDir.empty());
    EXPECT_EQ(delivered[1].Kind, MK::Editor);

    PackageResolution res;
    res.MountOrder.push_back(std::move(pkg));

    const std::vector<PackageCodeModule> editorModules = CollectPackageCodeModules(res, /*editorContext=*/true);
    ASSERT_EQ(editorModules.size(), 2u);
    const PackageCodeModule* runtime = FindModule(editorModules, "ClosedTree");
    ASSERT_NE(runtime, nullptr);
    EXPECT_TRUE(runtime->RootDir.empty());
    EXPECT_EQ(NormalizeForCompare(runtime->PrebuiltDir),
              NormalizeForCompare(tmp.Path() / "closed-tree" / "Binaries" / "Runtime"));
    ASSERT_NE(FindModule(editorModules, "ClosedTree.Editor"), nullptr);

    const std::vector<PackageCodeModule> playerModules = CollectPackageCodeModules(res, /*editorContext=*/false);
    ASSERT_EQ(playerModules.size(), 1u);
    EXPECT_EQ(playerModules[0].AssemblyName, "ClosedTree");
}

// The C# counterpart: a package that ships a managed assembly and no .cs
// delivers its module by the `prebuilt` declaration alone, with no source root;
// ScriptManager then uses the declared DLL instead of compiling.
TEST(PackageCodeModules, PrebuiltOnlyCSharpModuleIsDeliveredWithoutSources)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_prebuilt_cs"));
    ResolvedPackage pkg = MakeResolvedPackage("closed-pack", tmp.Path() / "closed-pack", {});
    PackageModuleRecord record;
    record.Kind = MK::Editor;
    record.Lang = ML::CSharp;
    record.Prebuilt = "Binaries/Managed/";
    pkg.Manifest.Modules.push_back(std::move(record));

    PackageResolution res;
    res.MountOrder.push_back(std::move(pkg));

    const std::vector<PackageCodeModule> editorModules = CollectPackageCodeModules(res, /*editorContext=*/true);
    ASSERT_EQ(editorModules.size(), 1u);
    EXPECT_EQ(editorModules[0].AssemblyName, "ClosedPack.Editor");
    EXPECT_EQ(editorModules[0].Lang, ML::CSharp);
    EXPECT_TRUE(editorModules[0].RootDir.empty());
    EXPECT_EQ(NormalizeForCompare(editorModules[0].PrebuiltDir),
              NormalizeForCompare(tmp.Path() / "closed-pack" / "Binaries" / "Managed"));
    EXPECT_TRUE(CollectPackageCodeModules(res, /*editorContext=*/false).empty());
}

// A .dll is not an asset type and discovery never reads one: a package whose
// mount holds assemblies but declares no `prebuilt` delivers no module.
TEST(PackageCodeModules, UndeclaredDllInTheMountDeliversNoModule)
{
    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_undeclared_dll"));
    ResolvedPackage pkg = MakeResolvedPackage("loose-pack", tmp.Path() / "loose-pack", {});
    WriteTextFile(pkg.RootDir / "LoosePack.dll", "not declared");
    WriteTextFile(pkg.RootDir / "Plugins" / "Helper.dll", "not declared");

    EXPECT_TRUE(ModulesDeliveredBy(pkg).empty());
}

// A package's top-level Tests folder, in any case, holds sources for a test
// executable the engine's own build defines; it is never a module of the
// package, so a package that ships tests beside its editor code delivers the
// editor module alone. The rule is the top level only: an Editor/Tests folder
// is editor code like the rest of Editor.
TEST(PackageCodeModules, TestsFolderDeliversNoModule)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_tests_folder"));
    for (const char* testsFolder : {"Tests", "tests"})
    {
        ResolvedPackage pkg = MakeResolvedPackage("helper-pack", tmp.Path() / testsFolder / "helper-pack",
                                                  {{ML::Cpp, "Editor/"}});
        WriteTextFile(pkg.RootDir / testsFolder / "HelperTests.cpp", "");
        WriteTextFile(pkg.RootDir / testsFolder / "Scripts" / "HelperTests.cs", "");

        const std::vector<DiscoveredPackageModule> delivered = ModulesDeliveredBy(pkg);
        ASSERT_EQ(delivered.size(), 1u) << testsFolder;
        EXPECT_EQ(delivered[0].Lang, ML::Cpp) << testsFolder;
        EXPECT_EQ(delivered[0].Kind, MK::Editor) << testsFolder;
        EXPECT_EQ(NormalizeForCompare(delivered[0].RootDir), NormalizeForCompare(pkg.RootDir / "Editor"))
            << testsFolder;
    }

    ResolvedPackage nested = MakeResolvedPackage("nested-pack", tmp.Path() / "nested-pack", {});
    WriteTextFile(nested.RootDir / "Editor" / "Tests" / "NestedTests.cpp", "");
    const std::vector<DiscoveredPackageModule> delivered = ModulesDeliveredBy(nested);
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(delivered[0].Kind, MK::Editor);
    EXPECT_EQ(NormalizeForCompare(delivered[0].RootDir), NormalizeForCompare(nested.RootDir / "Editor" / "Tests"));
}

TEST(PackageCodeModules, FlattensModulesInTopoOrderWithDefinesAndReferences)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_modules_topo"));
    PackageResolution res;
    res.MountOrder.push_back(MakeResolvedPackage("water-core", tmp.Path() / "water-core",
                                                 {{ML::CSharp, "Runtime/"}}));
    res.MountOrder.push_back(MakeResolvedPackage(
        "ocean-pack", tmp.Path() / "ocean-pack",
        {{ML::CSharp, "Runtime/"}, {ML::CSharp, "Editor/"}, {ML::Cpp, "Native/"}},
        {"water-core"}));

    const std::vector<PackageCodeModule> modules = CollectPackageCodeModules(res, /*editorContext=*/true);
    ASSERT_EQ(modules.size(), 4u);

    // Topo (mount) order preserved: dependency's module first.
    EXPECT_EQ(modules[0].AssemblyName, "WaterCore");
    EXPECT_EQ(modules[0].PackageName, "water-core");
    EXPECT_TRUE(modules[0].DependencyAssemblies.empty());
    ASSERT_EQ(modules[0].Defines.size(), 1u);
    EXPECT_EQ(modules[0].Defines[0], "GE_PACKAGE_WATER_CORE");
    EXPECT_EQ(NormalizeForCompare(modules[0].RootDir),
              NormalizeForCompare(tmp.Path() / "water-core" / "Runtime"));

    const PackageCodeModule* oceanRuntime = FindModule(modules, "OceanPack");
    ASSERT_NE(oceanRuntime, nullptr);
    EXPECT_EQ(oceanRuntime->Lang, ML::CSharp);
    // Defines propagate dependencies-first: a consumer can #if the dependency's
    // derived define as well as its own.
    ASSERT_EQ(oceanRuntime->Defines.size(), 2u);
    EXPECT_EQ(oceanRuntime->Defines[0], "GE_PACKAGE_WATER_CORE");
    EXPECT_EQ(oceanRuntime->Defines[1], "GE_PACKAGE_OCEAN_PACK");
    ASSERT_EQ(oceanRuntime->DependencyAssemblies.size(), 1u);
    EXPECT_EQ(oceanRuntime->DependencyAssemblies[0], "WaterCore");
    ASSERT_EQ(oceanRuntime->DependencyPackages.size(), 1u);
    EXPECT_EQ(oceanRuntime->DependencyPackages[0], "water-core");

    // Editor-kind C# module gets the .Editor suffix and the same references
    // (water-core has no editor assembly to add).
    const PackageCodeModule* oceanEditor = FindModule(modules, "OceanPack.Editor");
    ASSERT_NE(oceanEditor, nullptr);
    EXPECT_EQ(oceanEditor->Kind, MK::Editor);
    ASSERT_EQ(oceanEditor->DependencyAssemblies.size(), 1u);
    EXPECT_EQ(oceanEditor->DependencyAssemblies[0], "WaterCore");

    // Native runtime module rides the same descriptor with the package-name identity.
    const auto cppIt = std::find_if(modules.begin(), modules.end(), [](const PackageCodeModule& m) {
        return m.Lang == ML::Cpp;
    });
    ASSERT_NE(cppIt, modules.end());
    EXPECT_EQ(cppIt->AssemblyName, "OceanPack");
    ASSERT_EQ(cppIt->Defines.size(), 2u);
}

TEST(PackageCodeModules, PlayerContextExcludesEditorKindModules)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_modules_player"));
    PackageResolution res;
    res.MountOrder.push_back(MakeResolvedPackage(
        "ocean-pack", tmp.Path() / "ocean-pack",
        {{ML::CSharp, "Runtime/"}, {ML::CSharp, "Editor/"}, {ML::Cpp, "Editor/Native/"}}));

    const std::vector<PackageCodeModule> playerModules =
        CollectPackageCodeModules(res, /*editorContext=*/false);
    ASSERT_EQ(playerModules.size(), 1u);
    EXPECT_EQ(playerModules[0].AssemblyName, "OceanPack");
    EXPECT_EQ(playerModules[0].Kind, MK::Runtime);

    const std::vector<PackageCodeModule> editorModules =
        CollectPackageCodeModules(res, /*editorContext=*/true);
    EXPECT_EQ(editorModules.size(), 3u);
}

// EditorSDK phase 1: native Editor-kind modules are collected in editor
// context (they build against the staged EditorSDK) with the same ".Editor"
// naming C# editor assemblies use. Two C# source directories with no Editor
// segment are one runtime module rooted at their common ancestor.
TEST(PackageCodeModules, NativeEditorModulesCollectWithEditorSuffix)
{
    using MK = PackageModuleRecord::ModuleKind;
    using ML = PackageModuleRecord::ModuleLang;

    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_modules_native"));
    PackageResolution res;
    res.MountOrder.push_back(MakeResolvedPackage("toolkit", tmp.Path() / "toolkit",
                                                 {{ML::Cpp, "Editor/Native/"},
                                                  {ML::Cpp, "Native/"},
                                                  {ML::CSharp, "Scripts/A/"},
                                                  {ML::CSharp, "Scripts/B/"}}));

    const std::vector<PackageCodeModule> modules = CollectPackageCodeModules(res, /*editorContext=*/true);
    ASSERT_EQ(modules.size(), 3u);

    const PackageCodeModule* nativeEditor = FindModule(modules, "Toolkit.Editor");
    ASSERT_NE(nativeEditor, nullptr);
    EXPECT_EQ(nativeEditor->Kind, MK::Editor);
    EXPECT_EQ(nativeEditor->Lang, ML::Cpp);
    EXPECT_EQ(NormalizeForCompare(nativeEditor->RootDir),
              NormalizeForCompare(tmp.Path() / "toolkit" / "Editor" / "Native"));

    // Scripts/A and Scripts/B are one C# runtime module rooted at Scripts.
    const PackageCodeModule* csharpRuntime = FindModule(modules, "Toolkit");
    ASSERT_NE(csharpRuntime, nullptr);
    EXPECT_EQ(csharpRuntime->Lang, ML::CSharp);
    EXPECT_EQ(NormalizeForCompare(csharpRuntime->RootDir),
              NormalizeForCompare(tmp.Path() / "toolkit" / "Scripts"));

    // The native runtime module keeps the unsuffixed name — the two halves
    // can never collide in assembly names or cache dirs.
    const PackageCodeModule* nativeRuntime = std::addressof(
        *std::find_if(modules.begin(), modules.end(), [](const PackageCodeModule& m) {
            return m.Lang == ML::Cpp && m.Kind == MK::Runtime;
        }));
    EXPECT_EQ(nativeRuntime->AssemblyName, "Toolkit");

    // Player context drops the native editor module entirely.
    const std::vector<PackageCodeModule> playerModules =
        CollectPackageCodeModules(res, /*editorContext=*/false);
    EXPECT_EQ(playerModules.size(), 2u);
    EXPECT_EQ(FindModule(playerModules, "Toolkit.Editor"), nullptr);
}

TEST(PackageCodeModules, CollectAllPackageDefinesUnionsEnabledPackages)
{
    PackageResolution res;
    res.MountOrder.push_back(MakeResolvedPackage("water-core", fs::path("X:/w"), {}));
    res.MountOrder.push_back(
        MakeResolvedPackage("ocean-pack", fs::path("X:/o"), {}, {"water-core"}));

    const std::vector<std::string> defines = CollectAllPackageDefines(res);
    ASSERT_EQ(defines.size(), 2u);
    EXPECT_EQ(defines[0], "GE_PACKAGE_WATER_CORE");
    EXPECT_EQ(defines[1], "GE_PACKAGE_OCEAN_PACK");
}

// MAX_PATH mitigation: MSVC's FileTracker (FTK1011/C1083) cannot use \\?\ long
// paths, so the native build root under the package cache must keep its entry
// segment short no matter how long the content-addressed entry name gets. The
// asset-DB derived root deliberately keeps the readable long form.
TEST(PackageCodeModules, GitPackageNativeCacheDirIsHashShortened)
{
    // Deliberately deep/long entry name (scoped name + long prerelease + sha12).
    const std::string entry =
        PackageCacheEntryName("@very-long-studio-namespace/extremely-verbose-package-name",
                              "12.34.56-prerelease.build.9999", "0123456789abcdef");
    ASSERT_GT(entry.size(), 60u);

    const fs::path cacheRoot = fs::path("X:/cache/PackageCache");
    const std::string& engineId = NativeScripting::EngineBuildIdentity();
    const fs::path nativeRoot = PackageCacheDerivedNativeRoot(cacheRoot, entry, engineId);

    // Bounded package-scope segment: prefix8 + '-' + 12 hex. Desktop nests a
    // 12-hex engine-build generation under it: .native/<scope>/<generation>.
#if defined(__EMSCRIPTEN__)
    EXPECT_EQ(NormalizeForCompare(nativeRoot.parent_path()),
              NormalizeForCompare(cacheRoot / ".derived"));
    const std::string segment = nativeRoot.filename().string();
#else
#if defined(_WIN32)
    EXPECT_EQ(NormalizeForCompare(nativeRoot.parent_path().parent_path()),
              NormalizeForCompare(fs::temp_directory_path() / "GameEngine" / ".native"));
#else
    EXPECT_EQ(NormalizeForCompare(nativeRoot.parent_path().parent_path()),
              NormalizeForCompare(cacheRoot / ".native"));
#endif
    EXPECT_EQ(nativeRoot.filename().string().size(), 12u) << nativeRoot.filename().string();
    const std::string segment = nativeRoot.parent_path().filename().string();
#endif
    EXPECT_EQ(segment.size(), 8u + 1u + 12u) << segment;
    EXPECT_EQ(segment.substr(0, 8), entry.substr(0, 8));

    // Deterministic, and distinct entries stay distinct (different commit).
    EXPECT_EQ(PackageCacheDerivedNativeRoot(cacheRoot, entry, engineId), nativeRoot);
    const std::string entry2 =
        PackageCacheEntryName("@very-long-studio-namespace/extremely-verbose-package-name",
                              "12.34.56-prerelease.build.9999", "fedcba98765432");
    EXPECT_NE(PackageCacheDerivedNativeRoot(cacheRoot, entry2, engineId), nativeRoot);

    // CollectPackageCodeModules routes GIT modules through the shortened root;
    // embedded packages keep their in-package .Cache.
    using ML = PackageModuleRecord::ModuleLang;
    const TestUtils::ScopedTempDir tmp(TestUtils::MakeUniqueTempDirectory("ge_pkg_modules_gitcache"));
    PackageResolution res;
    // The cache root the CacheDir derives from is process-global; only the
    // package root's LAST segment (the cache entry name) feeds that path, so
    // the fixture's sources can live in a temp dir of the same name.
    ResolvedPackage gitPkg = MakeResolvedPackage(
        "@very-long-studio-namespace/extremely-verbose-package-name", tmp.Path() / entry,
        {{ML::Cpp, "Native/"}});
    gitPkg.SourceKind = PackageSourceKind::Git;
    res.MountOrder.push_back(std::move(gitPkg));

    const std::vector<PackageCodeModule> modules = CollectPackageCodeModules(res, /*editorContext=*/true);
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(NormalizeForCompare(modules[0].CacheDir),
              NormalizeForCompare(PackageCacheDerivedNativeRoot(GlobalPackageCacheRoot(), entry, engineId)));
    EXPECT_LE(modules[0].CacheDir.filename().string().size(), 21u);
}

// The native cache root is machine-global, so two engine builds sharing one slot
// means the second one finds DLLs linked against the first one's Engine.dll. The
// engine id must therefore separate slots — WITHOUT lengthening the segment, which
// is the MAX_PATH constraint the test above pins.
TEST(PackageCodeModules, GitPackageNativeCacheDirIsKeyedByEngineBuild)
{
    const fs::path cacheRoot = fs::path("X:/cache/PackageCache");
    const std::string entry = PackageCacheEntryName("water-pack", "1.0.0", "0123456789abcdef");

    const fs::path engineA = PackageCacheDerivedNativeRoot(cacheRoot, entry, "engine-build-aaaa");
    const fs::path engineB = PackageCacheDerivedNativeRoot(cacheRoot, entry, "engine-build-bbbb");
    EXPECT_NE(engineA, engineB) << "one slot shared by two engine builds is the whole defect";
    EXPECT_EQ(engineA.parent_path(), engineB.parent_path())
        << "every engine build of one entry shares one package scope";
    EXPECT_EQ(engineA.filename().string().size(), engineB.filename().string().size());
    EXPECT_EQ(engineA.filename().string().size(), 12u)
        << "the engine build is its own 12-hex generation segment";

    // The separator makes the two components unambiguous: a bare concatenation
    // would collide ("ab"+"c" == "a"+"bc").
    EXPECT_NE(PackageCacheDerivedNativeRoot(cacheRoot, "ab", "c"),
              PackageCacheDerivedNativeRoot(cacheRoot, "a", "bc"));

    // Retention is per package entry/cache namespace, independent of the Engine
    // build. Windows includes the original cache root because builds use temp.
    std::string packageIdentity = entry;
#if defined(_WIN32)
    packageIdentity = cacheRoot.generic_string() + '/' + entry;
#endif
    std::uint64_t expected = 14695981039346656037ULL;
    for (const char c : packageIdentity)
    {
        expected ^= static_cast<unsigned char>(c);
        expected *= 1099511628211ULL;
    }
    char hex[13];
    std::snprintf(hex, sizeof(hex), "%012llx",
                  static_cast<unsigned long long>(expected & 0xFFFFFFFFFFFFULL));
    EXPECT_EQ(engineA.parent_path().filename().string(), entry.substr(0, 8) + "-" + hex);
    EXPECT_NE(PackageCacheDerivedNativeRoot(cacheRoot, entry, ""), engineA)
        << "an empty engine id is still its own generation";
}

// ---------------------------------------------------------------------------
// Package shader roots (ARC 3a / seam 3)
// ---------------------------------------------------------------------------

// The material pipeline's package shader roots are DERIVED from the mount
// table (CollectPackageShaderDirs) — priority order, Shaders/-dir convention,
// editor mount excluded (it is the fixed AdapterShaderDir fallback), and the
// source-set version lets consumers re-derive exactly when mounts move.
TEST(PackageShaderRoots, DirsDerivedFromMountsInPriorityOrderEditorExcluded)
{
    E2EProject project("ge_pkg_shader_dirs");

    WriteEmbeddedPackage(project.Root, "pkg-a", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Shaders" / "Surfaces" / "a.glsl",
                  "// a\n");
    // pkg-b depends on pkg-a -> higher mount priority -> earlier shader root.
    WriteEmbeddedPackage(project.Root, "pkg-b", "1.0.0", "\"pkg-a\": \"^1.0\"");
    WriteTextFile(project.Root / "Packages" / "pkg-b" / "Assets" / "Shaders" / "Surfaces" / "b.glsl",
                  "// b\n");
    // A package WITHOUT a Shaders/ dir contributes no root.
    WriteEmbeddedPackage(project.Root, "pkg-noshaders", "1.0.0");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded", "pkg-b": "embedded", "pkg-noshaders": "embedded" }
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    const std::vector<std::string> mounted = MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase");
    ASSERT_EQ(mounted.size(), 3u);

    // An 'editor' source with its own Shaders/ dir must be excluded — its
    // Shaders dir is the AdapterShaderDir, the explicit final fallback.
    const fs::path editorRoot = project.Root / "EditorAssets";
    WriteTextFile(editorRoot / "Shaders" / "Adapters" / "adapter_probe.glsl", "// adapter\n");
    AssetSourceDesc editorSrc;
    editorSrc.Alias = std::string(kAssetSourceAliasEditor);
    editorSrc.Root = editorRoot;
    editorSrc.Priority = 0;
    ASSERT_TRUE(am.RegisterSource(editorSrc));

    const std::vector<fs::path> dirs = CollectPackageShaderDirs(am);
    ASSERT_EQ(dirs.size(), 2u);
    EXPECT_EQ(NormalizeForCompare(dirs[0]),
              NormalizeForCompare(project.Root / "Packages" / "pkg-b" / "Assets" / "Shaders"));
    EXPECT_EQ(NormalizeForCompare(dirs[1]),
              NormalizeForCompare(project.Root / "Packages" / "pkg-a" / "Assets" / "Shaders"));

    // Unmounting is a source-set change: the version moves and the derived
    // roots drop with it (this is what re-arms the material build context).
    const uint64_t versionBefore = am.GetSourceSetVersion();
    UnmountPackageSources(am, mounted);
    EXPECT_NE(am.GetSourceSetVersion(), versionBefore);
    EXPECT_TRUE(CollectPackageShaderDirs(am).empty());
}

// AdapterShaderDir must survive the PACKAGED mount shape. A shipped game
// registers no 'editor' source (BuildPlayerEditorSourceDesc returns nullopt for
// a manifest layout), and the build pipeline stages the adapter templates into
// the project mount at <content>/Assets/Shaders. Resolving the adapter dir
// through the 'editor' alias alone yields an empty path there, and an empty
// AdapterShaderDir makes every material compile bail before it starts — a
// shipped game that renders no material at all.
TEST(PackageShaderRoots, AdapterDirResolvesFromProjectMountWhenNoEditorSource)
{
    E2EProject project("ge_pkg_adapter_dir_packaged");

    // The packaged layout the build pipeline produces: a manifest beside the
    // fused content, and the engine shader tree under the project mount.
    WriteTextFile(project.Root / "Assets" / ".assetmanifest", "{}");
    WriteTextFile(project.Root / "Assets" / "Shaders" / "Adapters" / "adapter_probe.glsl",
                  "// adapter\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    // No 'editor' source is registered — exactly what the Player does for a
    // packaged game.
    ASSERT_TRUE(am.GetSourceRoot(kAssetSourceAliasEditor).empty());
    EXPECT_TRUE(am.ResolveAssetPath("Shaders", kAssetSourceAliasEditor).empty());

    EXPECT_EQ(NormalizeForCompare(ResolveAdapterShaderDir(am)),
              NormalizeForCompare(project.Root / "Assets" / "Shaders"));
}

// The fallback must not steal the editor case: whenever an 'editor' source IS
// registered, its Shaders/ tree stays the adapter dir even though the project
// mount also has one. Pinning to the editor mount is what keeps a worktree
// editor off a sibling project's stale shaders.
TEST(PackageShaderRoots, AdapterDirPrefersEditorMountOverProjectShaders)
{
    E2EProject project("ge_pkg_adapter_dir_editor");

    WriteTextFile(project.Root / "Assets" / "Shaders" / "Adapters" / "project_probe.glsl", "// p\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const fs::path editorRoot = project.Root / "EditorAssets";
    WriteTextFile(editorRoot / "Shaders" / "Adapters" / "adapter_probe.glsl", "// adapter\n");
    AssetSourceDesc editorSrc;
    editorSrc.Alias = std::string(kAssetSourceAliasEditor);
    editorSrc.Root = editorRoot;
    editorSrc.Priority = 0;
    ASSERT_TRUE(am.RegisterSource(editorSrc));

    EXPECT_EQ(NormalizeForCompare(ResolveAdapterShaderDir(am)),
              NormalizeForCompare(editorRoot / "Shaders"));
}

// The two properties MaterialSystem's re-derive depends on. The implicit
// resolve returns the project candidate whether or not it exists, so the
// adapter dir is NEVER empty: a consumer that treats "empty" as "not resolved
// yet" would latch this placeholder forever, and one that treats a non-empty
// path as ready would hand a wrong-but-existing root to the compiler and let
// the shader cache memoize the failures. Readiness must therefore be existence,
// and the value must be re-derived when the source set moves.
TEST(PackageShaderRoots, AdapterDirIsNeverEmptyAndNeedNotExist)
{
    E2EProject project("ge_pkg_adapter_dir_placeholder");

    // Neither an 'editor' source nor a project Shaders/ tree exists yet.
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    ASSERT_TRUE(am.GetSourceRoot(kAssetSourceAliasEditor).empty());

    const fs::path resolved = ResolveAdapterShaderDir(am);
    EXPECT_FALSE(resolved.empty()) << "emptiness is not a usable not-ready signal";
    EXPECT_EQ(NormalizeForCompare(resolved),
              NormalizeForCompare(project.Root / "Assets" / "Shaders"));

    std::error_code ec;
    EXPECT_FALSE(fs::exists(resolved, ec)) << "placeholder must not masquerade as ready";
}

// The late-editor-mount window, at the resolver: the editor registers its
// project source well before its editor source, so a resolve taken in between
// returns a different (placeholder) root than the same call after. A cached
// first value is therefore wrong, which is why EnsureMaterialBuildContextReady
// re-derives AdapterShaderDir on a source-set version change and drops what was
// compiled against the old root.
TEST(PackageShaderRoots, AdapterDirMovesWhenEditorSourceRegistersLate)
{
    E2EProject project("ge_pkg_adapter_dir_late_mount");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const fs::path beforeMount = ResolveAdapterShaderDir(am);
    const uint64_t versionBefore = am.GetSourceSetVersion();

    const fs::path editorRoot = project.Root / "EditorAssets";
    WriteTextFile(editorRoot / "Shaders" / "Adapters" / "adapter_probe.glsl", "// adapter\n");
    AssetSourceDesc editorSrc;
    editorSrc.Alias = std::string(kAssetSourceAliasEditor);
    editorSrc.Root = editorRoot;
    editorSrc.Priority = 0;
    ASSERT_TRUE(am.RegisterSource(editorSrc));

    // Registering a source moves the version — the signal the re-derive keys on.
    EXPECT_NE(am.GetSourceSetVersion(), versionBefore);

    const fs::path afterMount = ResolveAdapterShaderDir(am);
    EXPECT_NE(NormalizeForCompare(afterMount), NormalizeForCompare(beforeMount));
    EXPECT_EQ(NormalizeForCompare(afterMount), NormalizeForCompare(editorRoot / "Shaders"));
}

// A shipped game has no 'editor' mount, so asking for one must not read as an
// authoring error in its log. The engine-shader resolver and this adapter-dir
// derivation take the same route through AssetManager, so a silent derivation
// here is a silent packaged run.
TEST(PackageShaderRoots, AdapterDirDerivationIsSilentInPackagedLayout)
{
    E2EProject project("ge_pkg_adapter_dir_silent");

    WriteTextFile(project.Root / "Assets" / ".assetmanifest", "{}");
    WriteTextFile(project.Root / "Assets" / "Shaders" / "Adapters" / "adapter_probe.glsl", "// adapter\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    ASSERT_TRUE(am.GetSourceRoot(kAssetSourceAliasEditor).empty());

    std::vector<std::string> logLines;
    // Engine is a SHARED library: AssetManager logs through Engine.dll's Logger
    // state, not this exe's copy.
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::AddSink(std::make_unique<CapturingLogSink>(&logLines));

    // Re-derives happen on every source-set move, so one miss would repeat.
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(NormalizeForCompare(ResolveAdapterShaderDir(am)),
                  NormalizeForCompare(project.Root / "Assets" / "Shaders"));
    }

    Logger::Log::Flush();
    const auto offending = std::count_if(
        logLines.begin(), logLines.end(), [](const std::string& l)
        { return l.find("unknown asset source alias") != std::string::npos; });
    Logger::Log::ClearSinks();

    EXPECT_EQ(offending, 0) << "packaged games must not be told 'editor' is an unknown alias";
}

// What a package ships beyond the scenes' references: its Shaders/ folder, which
// materials name by path with no dependency edge, and nothing else it holds.
TEST(PackageRuntimeAssetStaging, ShadersFolderShipsAndUnreferencedAssetsDoNot)
{
    E2EProject project("ge_pkg_shaders_stage");

    WriteTextFile(project.Root / "Packages" / "pkg-a" / "package.json", PackageJson("pkg-a", "1.0.0"));
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Shaders" / "Surfaces" / "leaf.glsl",
                  "// leaf surface\n");
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Textures" / "bark.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded" }
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    ASSERT_EQ(MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("pkg-a");

    AssetCollector collector(am);
    AssetManifest manifest;
    EXPECT_TRUE(collector.CollectPackageRuntimeAssets(res.MountOrder[0], {}, manifest).empty());

    ASSERT_EQ(manifest.entries.size(), 1u) << "the shader source and nothing the build does not reference";
    EXPECT_EQ(manifest.entries[0].sourceAlias, "pkg-a");
    EXPECT_EQ(NormalizeForCompare(manifest.entries[0].outputPath),
              NormalizeForCompare(fs::path("Packages") / "pkg-a" / "Assets" / "Shaders" / "Surfaces" / "leaf.glsl"));
}

// A scene that reaches one package asset through a project material ships that
// asset and not its siblings: the package step adds nothing the scene does not use.
TEST(PackageRuntimeAssetStaging, SceneReachingOnePackageAssetShipsItAndNotItsSiblings)
{
    E2EProject project("ge_pkg_closure_stage");

    WriteTextFile(project.Root / "Packages" / "pkg-a" / "package.json", PackageJson("pkg-a", "1.0.0"));
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Textures" / "bark.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "pkg-a" / "Assets" / "Textures" / "stone.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "pkg-a": "embedded" }
    })");
    const GUID bark = AssetRegistry::DeriveGuidForSourcePath("pkg-a", "Textures/bark.png");
    const GUID stone = AssetRegistry::DeriveGuidForSourcePath("pkg-a", "Textures/stone.png");
    WriteTextFile(project.Root / "Assets" / "Materials" / "trunk.material",
                  "{ \"schemaVersion\": 3, \"textures\": { \"albedoMap\": \"" + std::string(bark.ToString()) +
                      "\" } }\n");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "main.scene",
                  "[scene name=\"main\"]\n\n[entity id=\"e1\"]\n"
                  "MeshRenderer.material = [path=\"Materials/trunk.material\"]\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    ASSERT_EQ(MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("pkg-a");
    am.WaitForStartupScan("project");

    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/main.scene"});
    const std::unordered_set<std::string> components = AssetCollector::CollectComponentNames(manifest);
    for (const ResolvedPackage& package : res.MountOrder)
        collector.CollectPackageRuntimeAssets(package, components, manifest);

    const AssetManifestEntry* shipped = manifest.FindByGuid(bark);
    ASSERT_NE(shipped, nullptr) << "the referenced package texture must ship";
    EXPECT_EQ(NormalizeForCompare(shipped->outputPath),
              NormalizeForCompare(fs::path("Packages") / "pkg-a" / "Assets" / "Textures" / "bark.png"));
    EXPECT_EQ(manifest.FindByGuid(stone), nullptr) << "an unreferenced sibling must not ship";
}

// A package lists under runtimeAssets what its code loads by path for a component.
// The listed folder ships when a built scene uses that component and not otherwise,
// and a listed path that matches nothing is reported back for the build to warn about.
TEST(PackageRuntimeAssetStaging, RuntimeAssetsShipOnlyForAComponentTheScenesUse)
{
    E2EProject project("ge_pkg_runtime_assets_stage");

    WriteTextFile(project.Root / "Packages" / "tree-pack" / "package.json", R"({
        "name": "tree-pack", "version": "1.0.0",
        "runtimeAssets": { "Tree": ["Textures/Bark/", "Textures/Missing.png"] }
    })");
    WriteTextFile(project.Root / "Packages" / "tree-pack" / "Assets" / "Textures" / "Bark" / "oak.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "tree-pack" / "Assets" / "Textures" / "rock.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "tree-pack": "embedded" }
    })");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "bare.scene",
                  "[scene name=\"bare\"]\n\n[entity id=\"e1\"]\nTransform.position = (0, 0, 0)\n");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "forest.scene",
                  "[scene name=\"forest\"]\n\n[entity id=\"e1\"]\nTree.seed = 7\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    ASSERT_EQ(MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("tree-pack");
    am.WaitForStartupScan("project");
    const GUID oak = AssetRegistry::DeriveGuidForSourcePath("tree-pack", "Textures/Bark/oak.png");
    const GUID rock = AssetRegistry::DeriveGuidForSourcePath("tree-pack", "Textures/rock.png");

    AssetCollector collector(am);
    AssetManifest bare = collector.CollectFromScenes({"Scenes/bare.scene"});
    collector.CollectPackageRuntimeAssets(res.MountOrder[0], AssetCollector::CollectComponentNames(bare), bare);
    EXPECT_EQ(bare.FindByGuid(oak), nullptr) << "no scene uses Tree, so its runtime assets stay out";

    AssetManifest forest = collector.CollectFromScenes({"Scenes/forest.scene"});
    const std::vector<std::string> unmatched = collector.CollectPackageRuntimeAssets(
        res.MountOrder[0], AssetCollector::CollectComponentNames(forest), forest);
    EXPECT_NE(forest.FindByGuid(oak), nullptr) << "a scene uses Tree, so the listed folder ships";
    EXPECT_EQ(forest.FindByGuid(rock), nullptr) << "an unlisted, unreferenced asset stays out";
    ASSERT_EQ(unmatched.size(), 1u);
    EXPECT_EQ(unmatched[0], "Tree: Textures/Missing.png");
}

// An editor-only package (editor icons at its asset root) ships nothing and is left
// out of the game's package mount list.
TEST(PackageRuntimeAssetStaging, EditorOnlyPackageShipsNothingAndIsNotMounted)
{
    E2EProject project("ge_pkg_editor_only_stage");

    WriteTextFile(project.Root / "Packages" / "vcs-pack" / "package.json", PackageJson("vcs-pack", "1.0.0"));
    WriteTextFile(project.Root / "Packages" / "vcs-pack" / "Assets" / "Icons" / "SettingsVcs.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "vcs-pack": "embedded" }
    })");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    ASSERT_EQ(MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("vcs-pack");

    AssetCollector collector(am);
    AssetManifest manifest;
    collector.CollectPackageRuntimeAssets(res.MountOrder[0], {}, manifest);
    EXPECT_TRUE(manifest.entries.empty()) << "no built scene references the editor icon";

    const fs::path contentRoot = project.Root / "staged-game";
    std::vector<std::string> errors;
    ASSERT_TRUE(StagePackagesIndex(contentRoot, manifest, res, {}, errors));
    EXPECT_FALSE(fs::exists(contentRoot / "Packages" / kPackagesIndexFileName))
        << "a package with no shipped footprint is not mounted by the game";
}

// A packaged game asked for an asset the export did not ship says which one and how
// to ship it, once per id, and the load still yields nothing.
TEST(PackageRuntimeAssetStaging, PackagedGameReportsAMissingAssetOnce)
{
    E2EProject project("ge_pkg_not_in_build");
    WriteTextFile(project.Root / "Assets" / ".assetmanifest", "{}");
    ASSERT_TRUE(project.Initialize());

    std::vector<std::string> logLines;
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::AddSink(std::make_unique<CapturingLogSink>(&logLines));

    const GUID missing = AssetRegistry::DeriveGuidForSourcePath("project", "Textures/never-shipped.png");
    EXPECT_EQ(project.Manager.LoadAssetAsync(missing).get(), nullptr);
    EXPECT_EQ(project.Manager.LoadAssetAsync(missing).get(), nullptr);
    const fs::path missingPath("Textures/also-never-shipped.png");
    for (int i = 0; i < 2; ++i)
        project.Manager.LoadAsset(missingPath, [](const Result<SharedPtr<Asset>, AssetError>& result) {
            EXPECT_FALSE(result.IsOk());
        });

    Logger::Log::Flush();
    const auto reportsOf = [&logLines](const std::string& id) {
        return std::count_if(logLines.begin(), logLines.end(), [&id](const std::string& line) {
            return line.find(id) != std::string::npos && line.find("is not in this build") != std::string::npos &&
                   line.find("runtimeAssets") != std::string::npos;
        });
    };
    const auto guidReports = reportsOf(std::string(missing.ToString()));
    const auto pathReports = reportsOf(missingPath.generic_string());
    Logger::Log::ClearSinks();
    EXPECT_EQ(guidReports, 1) << "a GUID load miss";
    EXPECT_EQ(pathReports, 1) << "a path load miss";
}

// Outside a packaged game a miss has other causes (a deleted or renamed asset), so
// the shipping advice is not logged there.
TEST(PackageRuntimeAssetStaging, ProjectOutsideAPackagedGameDoesNotReportMisses)
{
    E2EProject project("ge_pkg_not_in_build_dev");
    ASSERT_TRUE(project.Initialize());

    std::vector<std::string> logLines;
    Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    Logger::Log::Initialize({});
    Logger::Log::AddSink(std::make_unique<CapturingLogSink>(&logLines));

    const GUID missing = AssetRegistry::DeriveGuidForSourcePath("project", "Textures/deleted.png");
    EXPECT_EQ(project.Manager.LoadAssetAsync(missing).get(), nullptr);

    Logger::Log::Flush();
    const auto reports = std::count_if(logLines.begin(), logLines.end(), [](const std::string& line) {
        return line.find("is not in this build") != std::string::npos;
    });
    Logger::Log::ClearSinks();
    EXPECT_EQ(reports, 0);
}

// A component used only inside a blueprint the scene instances, with no override of
// its fields in the scene, still ships the package's runtimeAssets for it.
TEST(PackageRuntimeAssetStaging, ComponentUsedOnlyInABlueprintShipsItsRuntimeAssets)
{
    E2EProject project("ge_pkg_runtime_assets_blueprint");

    WriteTextFile(project.Root / "Packages" / "tree-pack" / "package.json", R"({
        "name": "tree-pack", "version": "1.0.0",
        "runtimeAssets": { "Tree": ["Textures/Bark/"] }
    })");
    WriteTextFile(project.Root / "Packages" / "tree-pack" / "Assets" / "Textures" / "Bark" / "oak.png", "PNGDATA");
    WriteTextFile(project.Root / "Packages" / "manifest.json", R"({
        "dependencies": { "tree-pack": "embedded" }
    })");
    WriteTextFile(project.Root / "Assets" / "Blueprints" / "Oak.blueprint",
                  "[blueprint name=\"Oak\" version=1]\n[entity id=\"root\"]\nTree.seed = 7\n");
    WriteTextFile(project.Root / "Assets" / "Scenes" / "grove.scene",
                  "[scene name=\"grove\" version=1]\n"
                  "[resource id=\"oak\" path=\"Blueprints/Oak.blueprint\"]\n\n"
                  "[blueprint id=\"oak_0\" source=\"oak\"]\n"
                  "Transform.position = (0, 0, 0)\n");

    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    const PackageResolution res = PackageResolver::Resolve(project.Root);
    ASSERT_TRUE(res.Errors.empty()) << res.Errors[0];
    ASSERT_EQ(MountResolvedPackages(am, res, project.Root / ".Cache" / "AssetDatabase").size(), 1u);
    am.WaitForStartupScan("tree-pack");
    am.WaitForStartupScan("project");

    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes({"Scenes/grove.scene"});
    collector.CollectPackageRuntimeAssets(res.MountOrder[0], AssetCollector::CollectComponentNames(manifest), manifest);
    EXPECT_NE(manifest.FindByGuid(AssetRegistry::DeriveGuidForSourcePath("tree-pack", "Textures/Bark/oak.png")),
              nullptr)
        << "the instanced blueprint uses Tree, so the listed folder ships";
}

// ---------------------------------------------------------------------------
// Arc 3a acceptance F: the REAL staged eztree engine package flows through the
// build pipeline's package steps — implicit resolution, the package collection
// for a game whose scene has a tree (Shaders/ subtree and the default textures
// eztree lists under runtimeAssets for EZTree) with Packages/<alias>/Assets/
// output layout, original editor GUIDs preserved, the per-entry staged copy, and the real
// packages.index writer. The pipeline's EngineCore-bound orchestration
// (BuildPipeline::Build) is exercised live post-reboot; every package-specific
// step below is the same code it calls.
// ---------------------------------------------------------------------------

TEST(PackageMountE2E, EZTreeEnginePackageStagesForPackagedGameWithShaders)
{
    const fs::path enginePackagesRoot =
        PathUtils::GetExecutableDirectory() / "TestData" / "Packages";
    ASSERT_TRUE(fs::is_directory(enginePackagesRoot / "eztree"))
        << enginePackagesRoot << " missing — Packages not staged";

    E2EProject project("ge_pkg_e2e_eztree_stage");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    // Implicit engine-package resolution: no Packages/manifest.json at all.
    // (The engine root also carries lore-vcs; this test only pins eztree.)
    const PackageResolution resolution =
        PackageResolver::Resolve(project.Root, enginePackagesRoot);
    const auto eztreeIt =
        std::find_if(resolution.MountOrder.begin(), resolution.MountOrder.end(),
                     [](const ResolvedPackage& pkg) { return pkg.Manifest.Name == "eztree"; });
    ASSERT_NE(eztreeIt, resolution.MountOrder.end());
    ASSERT_EQ(MountResolvedPackages(am, resolution, project.Root / ".Cache" / "AssetDatabase").size(), CountMountablePackages(resolution));

    // The pipeline's RunPackageAssetStep shape for a scene that uses the EZTree
    // component: the package's Shaders/ subtree and its runtimeAssets for EZTree.
    AssetCollector collector(am);
    AssetManifest manifest;
    EXPECT_TRUE(collector.CollectPackageRuntimeAssets(*eztreeIt, {"EZTree"}, manifest).empty())
        << "every path eztree lists under runtimeAssets matches its assets";
    ASSERT_FALSE(manifest.entries.empty());

    const auto findByOutput = [&](const char* needle) -> const AssetManifestEntry* {
        for (const auto& e : manifest.entries)
            if (NormalizeForCompare(e.outputPath).find(needle) != std::string::npos)
                return &e;
        return nullptr;
    };
    const AssetManifestEntry* wind = findByOutput("ez_tree_wind.glsl");
    const AssetManifestEntry* leaves = findByOutput("ez_tree_leaves.glsl");
    ASSERT_NE(wind, nullptr) << "Shaders/ subtree missing from the staged set";
    ASSERT_NE(leaves, nullptr);
    EXPECT_NE(findByOutput("oak_color_1k.jpg"), nullptr) << "the EZTree default textures must ship";
    EXPECT_EQ(findByOutput("eztree_bark.material"), nullptr) << "no scene references the bark material";
    EXPECT_EQ(NormalizeForCompare(wind->outputPath),
              NormalizeForCompare(fs::path("Packages") / "eztree" / "Assets" / "Shaders" /
                                  "VertexModifiers" / "ez_tree_wind.glsl"));
    // Staged identity is the pre-extraction editor GUID (seam 4 continuity).
    EXPECT_EQ(wind->guid, AssetRegistry::DeriveGuidForSourcePath(
                              "editor", "Shaders/VertexModifiers/ez_tree_wind.glsl"));

    // Per-entry staged copy (the CopyAssets mapping: contentRoot/outputPath)
    // and the REAL packages.index writer over the staged footprint.
    const fs::path contentRoot = project.Root / "staged-game";
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        std::error_code ec;
        const fs::path dst = contentRoot / entry.outputPath;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(entry.sourcePath, dst, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << entry.sourcePath << ": " << ec.message();
    }
    std::vector<std::string> errors;
    ASSERT_TRUE(StagePackagesIndex(contentRoot, manifest, resolution, {}, errors))
        << (errors.empty() ? "" : errors[0]);

    // On-disk staged layout: the package's asset tree incl. Shaders/, and the
    // index that makes the packaged Player mount it.
    EXPECT_TRUE(fs::exists(contentRoot / "Packages" / "eztree" / "Assets" / "Shaders" /
                           "VertexModifiers" / "ez_tree_wind.glsl"));
    EXPECT_TRUE(fs::exists(contentRoot / "Packages" / "eztree" / "Assets" / "Shaders" /
                           "Surfaces" / "ez_tree_leaves.glsl"));

    // Editor-only subtree convention (package plan §7.6): the package's
    // Assets/Editor/ tree (panel UXML/CSS, editor chrome) serves the EDITOR
    // mount only. The package step must not drag it into the packaged game — no
    // manifest entry may point into it and the staged tree must not carry it.
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        EXPECT_EQ(NormalizeForCompare(entry.outputPath)
                      .find(NormalizeForCompare(fs::path("Packages") / "eztree" / "Assets" /
                                                "Editor")),
                  std::string::npos)
            << "editor-only asset leaked into the packaged manifest: "
            << entry.outputPath.generic_string();
    }
    EXPECT_FALSE(fs::exists(contentRoot / "Packages" / "eztree" / "Assets" / "Editor"))
        << "packaged staging must exclude the package's Assets/Editor/ subtree";
    EXPECT_TRUE(fs::exists(contentRoot / "Packages" / "eztree" / "Assets" / "Textures" /
                           "EZTree" / "leaves" / "oak_color.png"));

    PackagesIndex index;
    std::string indexError;
    ASSERT_TRUE(TryLoadPackagesIndex(contentRoot / kPackagesStagingDirName / kPackagesIndexFileName,
                                     index, indexError))
        << indexError;
    ASSERT_EQ(index.Packages.size(), 1u);
    EXPECT_EQ(index.Packages.front().Name, "eztree");
    EXPECT_EQ(index.Packages.front().Alias, "eztree");
}

// EditorSDK phase 2: the eztree package ships its editor panel UI (UXML/CSS)
// and an editor-chrome stylesheet under Assets/UI. Both must resolve from the
// PACKAGE mount under its alias — exactly the (alias, path) pairs the Editor
// module's panel descriptor and stylesheet contribution carry — and the
// panel-bearing Editor-kind module must stay editor-only in code-module
// collection (both directions), so Player/packaged contexts never build it.
TEST(PackageMountE2E, EZTreePanelUiAssetsResolveFromPackageMountAndStayEditorOnly)
{
    const fs::path enginePackagesRoot =
        PathUtils::GetExecutableDirectory() / "TestData" / "Packages";
    ASSERT_TRUE(fs::is_directory(enginePackagesRoot / "eztree"))
        << enginePackagesRoot << " missing — Packages not staged";

    E2EProject project("ge_pkg_e2e_eztree_panel_ui");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution resolution =
        PackageResolver::Resolve(project.Root, enginePackagesRoot);
    ASSERT_GE(resolution.MountOrder.size(), 1u);
    ASSERT_EQ(MountResolvedPackages(am, resolution, project.Root / ".Cache" / "AssetDatabase").size(), CountMountablePackages(resolution));
    am.WaitForStartupScan("eztree");

    // The panel descriptor's asset paths, resolved against the package alias.
    const GUID layoutGuid =
        am.ResolveAssetGuid(fs::path("Editor/UI/panels/EZTreeStatsPanel.uxml"), "eztree");
    const GUID styleGuid =
        am.ResolveAssetGuid(fs::path("Editor/UI/panels/EZTreeStatsPanel.css"), "eztree");
    const GUID chromeGuid = am.ResolveAssetGuid(fs::path("Editor/UI/EZTreeEditorChrome.css"), "eztree");
    EXPECT_FALSE(layoutGuid.IsNull());
    EXPECT_FALSE(styleGuid.IsNull());
    EXPECT_FALSE(chromeGuid.IsNull());

    // Registered with the UI asset types the panel bind path expects.
    AssetMetadata meta;
    ASSERT_TRUE(am.GetRegistry().TryGetAssetMetadata(layoutGuid, meta));
    EXPECT_EQ(meta.Type, AssetType::UILayout);
    ASSERT_TRUE(am.GetRegistry().TryGetAssetMetadata(styleGuid, meta));
    EXPECT_EQ(meta.Type, AssetType::UIStyle);
    ASSERT_TRUE(am.GetRegistry().TryGetAssetMetadata(chromeGuid, meta));
    EXPECT_EQ(meta.Type, AssetType::UIStyle);

    // The panel UI ships with the PACKAGE — nothing leaks into the project mount.
    EXPECT_TRUE(am.ResolveAssetGuid(fs::path("Editor/UI/EZTreeEditorChrome.css"), "project").IsNull());

    // Module collection over the REAL manifest: the Editor-kind module (which
    // registers the panel) exists in editor context and never in player context.
    const std::vector<PackageCodeModule> editorModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/true);
    const auto findByName = [](const std::vector<PackageCodeModule>& modules, const char* name) {
        return std::any_of(modules.begin(), modules.end(), [name](const PackageCodeModule& m) {
            return m.AssemblyName == name;
        });
    };
    EXPECT_TRUE(findByName(editorModules, "Eztree"));
    EXPECT_TRUE(findByName(editorModules, "Eztree.Editor"));

    const std::vector<PackageCodeModule> playerModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/false);
    EXPECT_TRUE(findByName(playerModules, "Eztree"));
    EXPECT_FALSE(findByName(playerModules, "Eztree.Editor"));
}

// The svn-vcs package carries the extracted SVN provider as an Editor-only
// module (packaged games need no VCS). Same acceptance as lore-vcs: gating in
// BOTH directions, chrome stylesheet + settings icon resolving from the
// package mount under its alias.
TEST(PackageMountE2E, SvnVcsModuleStaysEditorOnlyAndChromeCssResolves)
{
    const fs::path enginePackagesRoot =
        PathUtils::GetExecutableDirectory() / "TestData" / "Packages";
    ASSERT_TRUE(fs::is_directory(enginePackagesRoot / "svn-vcs"))
        << enginePackagesRoot << " missing svn-vcs — Packages not staged";

    E2EProject project("ge_pkg_e2e_svn_vcs");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution resolution =
        PackageResolver::Resolve(project.Root, enginePackagesRoot);
    const bool svnResolved =
        std::any_of(resolution.MountOrder.begin(), resolution.MountOrder.end(),
                    [](const auto& pkg) { return pkg.Manifest.Name == "svn-vcs"; });
    ASSERT_TRUE(svnResolved) << "svn-vcs must resolve implicitly";

    ASSERT_FALSE(MountResolvedPackages(am, resolution, project.Root / ".Cache" / "AssetDatabase").empty());
    am.WaitForStartupScan("svn-vcs");

    // The chrome stylesheet + settings icon ship with the PACKAGE mount.
    const GUID chromeGuid = am.ResolveAssetGuid(fs::path("Editor/UI/SvnVcsChrome.css"), "svn-vcs");
    const GUID iconGuid = am.ResolveAssetGuid(fs::path("Icons/SettingsSVN.png"), "svn-vcs");
    EXPECT_FALSE(chromeGuid.IsNull());
    EXPECT_FALSE(iconGuid.IsNull());
    EXPECT_TRUE(am.ResolveAssetGuid(fs::path("Editor/UI/SvnVcsChrome.css"), "project").IsNull());

    // Both directions of the Editor-kind gate.
    const auto findByName = [](const std::vector<PackageCodeModule>& modules, const char* name) {
        return std::any_of(modules.begin(), modules.end(), [name](const PackageCodeModule& m) {
            return m.AssemblyName == name;
        });
    };
    const std::vector<PackageCodeModule> editorModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/true);
    EXPECT_TRUE(findByName(editorModules, "SvnVcs.Editor"))
        << "the extracted provider module must be collected for the editor";

    const std::vector<PackageCodeModule> playerModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/false);
    EXPECT_FALSE(findByName(playerModules, "SvnVcs.Editor"))
        << "packaged/player contexts must never see the VCS provider module";
    EXPECT_TRUE(std::none_of(playerModules.begin(), playerModules.end(),
                             [](const PackageCodeModule& m) {
                                 return m.PackageName == "svn-vcs";
                             }))
        << "svn-vcs carries no runtime modules at all";
}

// The diversion-vcs package carries the extracted Diversion provider as an
// Editor-only module (packaged games need no VCS). Same acceptance as
// lore-vcs: gating in BOTH directions, chrome stylesheet + settings icon
// resolving from the package mount under its alias.
TEST(PackageMountE2E, DiversionVcsModuleStaysEditorOnlyAndChromeCssResolves)
{
    const fs::path enginePackagesRoot =
        PathUtils::GetExecutableDirectory() / "TestData" / "Packages";
    ASSERT_TRUE(fs::is_directory(enginePackagesRoot / "diversion-vcs"))
        << enginePackagesRoot << " missing diversion-vcs — Packages not staged";

    E2EProject project("ge_pkg_e2e_diversion_vcs");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution resolution =
        PackageResolver::Resolve(project.Root, enginePackagesRoot);
    const bool diversionResolved =
        std::any_of(resolution.MountOrder.begin(), resolution.MountOrder.end(),
                    [](const auto& pkg) { return pkg.Manifest.Name == "diversion-vcs"; });
    ASSERT_TRUE(diversionResolved) << "diversion-vcs must resolve implicitly";

    ASSERT_FALSE(MountResolvedPackages(am, resolution, project.Root / ".Cache" / "AssetDatabase").empty());
    am.WaitForStartupScan("diversion-vcs");

    // The chrome stylesheet + settings icon ship with the PACKAGE mount.
    const GUID chromeGuid =
        am.ResolveAssetGuid(fs::path("Editor/UI/DiversionVcsChrome.css"), "diversion-vcs");
    const GUID iconGuid =
        am.ResolveAssetGuid(fs::path("Icons/SettingsDiversion.png"), "diversion-vcs");
    EXPECT_FALSE(chromeGuid.IsNull());
    EXPECT_FALSE(iconGuid.IsNull());
    EXPECT_TRUE(am.ResolveAssetGuid(fs::path("Editor/UI/DiversionVcsChrome.css"), "project").IsNull());

    // Both directions of the Editor-kind gate.
    const auto findByName = [](const std::vector<PackageCodeModule>& modules, const char* name) {
        return std::any_of(modules.begin(), modules.end(), [name](const PackageCodeModule& m) {
            return m.AssemblyName == name;
        });
    };
    const std::vector<PackageCodeModule> editorModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/true);
    EXPECT_TRUE(findByName(editorModules, "DiversionVcs.Editor"))
        << "the extracted provider module must be collected for the editor";

    const std::vector<PackageCodeModule> playerModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/false);
    EXPECT_FALSE(findByName(playerModules, "DiversionVcs.Editor"))
        << "packaged/player contexts must never see the VCS provider module";
    EXPECT_TRUE(std::none_of(playerModules.begin(), playerModules.end(),
                             [](const PackageCodeModule& m) {
                                 return m.PackageName == "diversion-vcs";
                             }))
        << "diversion-vcs carries no runtime modules at all";
}

// EditorSDK phase 3: the lore-vcs package carries the extracted Lore VCS
// provider as an Editor-only module (packaged games need no VCS). Gating must
// hold in BOTH directions — collected in editor context, absent in player/
// packaged context — and its chrome stylesheet must resolve from the package
// mount under its alias.
TEST(PackageMountE2E, LoreVcsModuleStaysEditorOnlyAndChromeCssResolves)
{
    const fs::path enginePackagesRoot =
        PathUtils::GetExecutableDirectory() / "TestData" / "Packages";
    ASSERT_TRUE(fs::is_directory(enginePackagesRoot / "lore-vcs"))
        << enginePackagesRoot << " missing lore-vcs — Packages not staged";

    E2EProject project("ge_pkg_e2e_lore_vcs");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;

    const PackageResolution resolution =
        PackageResolver::Resolve(project.Root, enginePackagesRoot);
    const bool loreResolved =
        std::any_of(resolution.MountOrder.begin(), resolution.MountOrder.end(),
                    [](const auto& pkg) { return pkg.Manifest.Name == "lore-vcs"; });
    ASSERT_TRUE(loreResolved) << "lore-vcs must resolve implicitly";

    ASSERT_FALSE(MountResolvedPackages(am, resolution, project.Root / ".Cache" / "AssetDatabase").empty());
    am.WaitForStartupScan("lore-vcs");

    // The chrome stylesheet + settings icon ship with the PACKAGE mount.
    const GUID chromeGuid = am.ResolveAssetGuid(fs::path("Editor/UI/LoreVcsChrome.css"), "lore-vcs");
    const GUID iconGuid = am.ResolveAssetGuid(fs::path("Icons/SettingsLore.png"), "lore-vcs");
    EXPECT_FALSE(chromeGuid.IsNull());
    EXPECT_FALSE(iconGuid.IsNull());
    EXPECT_TRUE(am.ResolveAssetGuid(fs::path("Editor/UI/LoreVcsChrome.css"), "project").IsNull());

    // Both directions of the Editor-kind gate.
    const auto findByName = [](const std::vector<PackageCodeModule>& modules, const char* name) {
        return std::any_of(modules.begin(), modules.end(), [name](const PackageCodeModule& m) {
            return m.AssemblyName == name;
        });
    };
    const std::vector<PackageCodeModule> editorModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/true);
    EXPECT_TRUE(findByName(editorModules, "LoreVcs.Editor"))
        << "the extracted provider module must be collected for the editor";

    const std::vector<PackageCodeModule> playerModules =
        CollectPackageCodeModules(resolution, /*editorContext=*/false);
    EXPECT_FALSE(findByName(playerModules, "LoreVcs.Editor"))
        << "packaged/player contexts must never see the VCS provider module";
    EXPECT_TRUE(std::none_of(playerModules.begin(), playerModules.end(),
                             [](const PackageCodeModule& m) {
                                 return m.PackageName == "lore-vcs";
                             }))
        << "lore-vcs carries no runtime modules at all";
}
