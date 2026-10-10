// Packages P3 (distribution): git spec parsing, global-cache path derivation,
// packages-lock.json round-trip, offline lock-honoring resolution against a
// pre-populated cache, and one env-gated LIVE git end-to-end test against the
// real GameEngine-Packages repo.

#include "Assets/AssetManager.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageGitFetcher.h"
#include "Assets/Packages/PackageGitSource.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/Packages/PackagesLockFile.h"
#include "Assets/Packages/ProjectPackagesManifest.h"
#include "NativeScripting/EngineBuildIdentity.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

namespace
{

namespace fs = std::filesystem;

void WriteTextFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

// A package's code modules are discovered from the files under its root, so a
// fixture module is a source file in the directory it is rooted at.
void WriteSourceFile(const fs::path& p)
{
    WriteTextFile(p, "// package module fixture");
}

std::string ReadTextFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

void SetEnvVar(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

// Redirects the global package cache into a per-test temp dir for the scope,
// then restores the process-wide scratch root the suite runs under.
class ScopedPackageCacheDir
{
  public:
    explicit ScopedPackageCacheDir(const fs::path& dir)
    {
        if (const char* previous = std::getenv("GE_PACKAGE_CACHE_DIR"))
            m_Previous = previous;
        std::error_code ec;
        fs::create_directories(dir, ec);
        SetEnvVar("GE_PACKAGE_CACHE_DIR", dir.string().c_str());
    }
    ~ScopedPackageCacheDir()
    {
        SetEnvVar("GE_PACKAGE_CACHE_DIR", m_Previous.empty() ? nullptr : m_Previous.c_str());
    }

  private:
    std::string m_Previous;
};

bool AnyContainsText(const std::vector<std::string>& messages, const std::string& needle)
{
    for (const auto& m : messages)
        if (m.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Git spec parsing
// ---------------------------------------------------------------------------

TEST(GitPackageSpecParse, RecognizesGitSpecs)
{
    EXPECT_TRUE(IsGitPackageSpec("git+https://github.com/a/b.git#v1"));
    EXPECT_TRUE(IsGitPackageSpec("https://github.com/a/b.git#v1"));
    EXPECT_TRUE(IsGitPackageSpec("http://internal/repo.git#main"));
    EXPECT_TRUE(IsGitPackageSpec("git+https://oops-no-ref.git")); // malformed, still git-shaped

    EXPECT_FALSE(IsGitPackageSpec("embedded"));
    EXPECT_FALSE(IsGitPackageSpec("file:../SharedPackages/OceanPack"));
    EXPECT_FALSE(IsGitPackageSpec("git@github.com:a/b.git")); // ssh scp-syntax unsupported
}

TEST(GitPackageSpecParse, ParsesUrlRefAndSubdir)
{
    GitPackageSpec spec;
    std::string error;

    ASSERT_TRUE(TryParseGitPackageSpec(
        "sample-water", "git+https://github.com/Lunarsong/GameEngine-Packages.git#v0.1.0", spec,
        error))
        << error;
    EXPECT_EQ(spec.Url, "https://github.com/Lunarsong/GameEngine-Packages.git");
    EXPECT_EQ(spec.Ref, "v0.1.0");
    EXPECT_TRUE(spec.Subdir.empty());

    ASSERT_TRUE(TryParseGitPackageSpec(
        "sample-water",
        "git+https://github.com/Lunarsong/GameEngine-Packages.git#v0.1.0&path=packages/sample-water",
        spec, error))
        << error;
    EXPECT_EQ(spec.Url, "https://github.com/Lunarsong/GameEngine-Packages.git");
    EXPECT_EQ(spec.Ref, "v0.1.0");
    EXPECT_EQ(spec.Subdir, "packages/sample-water");

    // Bare https (no git+ prefix) and sha refs parse too.
    ASSERT_TRUE(TryParseGitPackageSpec(
        "pkg", "https://host/repo.git#0123456789abcdef0123456789abcdef01234567", spec, error))
        << error;
    EXPECT_EQ(spec.Ref, "0123456789abcdef0123456789abcdef01234567");

    // Subdir slashes are trimmed to keep path joins exact.
    ASSERT_TRUE(TryParseGitPackageSpec("pkg", "git+https://host/r.git#main&path=/sub/dir/", spec,
                                       error))
        << error;
    EXPECT_EQ(spec.Subdir, "sub/dir");
}

TEST(GitPackageSpecParse, RejectsMalformedSpecs)
{
    GitPackageSpec spec;
    std::string error;

    // No ref — pinning is mandatory.
    EXPECT_FALSE(TryParseGitPackageSpec("p", "git+https://host/repo.git", spec, error));
    EXPECT_NE(error.find("no ref"), std::string::npos) << error;

    EXPECT_FALSE(TryParseGitPackageSpec("p", "git+https://host/repo.git#", spec, error));
    EXPECT_FALSE(TryParseGitPackageSpec("p", "git+ssh://host/repo.git#v1", spec, error));
    EXPECT_FALSE(TryParseGitPackageSpec("p", "git+https://#v1", spec, error));
    EXPECT_FALSE(TryParseGitPackageSpec("p", "git+https://host/r.git#main&path=", spec, error));
    EXPECT_FALSE(
        TryParseGitPackageSpec("p", "git+https://host/r.git#main&path=../escape", spec, error));
    EXPECT_NE(error.find(".."), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// Cache path derivation
// ---------------------------------------------------------------------------

TEST(PackageCachePaths, EntryNameIsContentAddressedAndSanitized)
{
    EXPECT_EQ(PackageCacheEntryName("sample-water", "0.1.0",
                                    "0123456789abcdef0123456789abcdef01234567"),
              "sample-water@0.1.0-0123456789ab");
    // Scoped names collapse to one path segment via the mount-alias mapping.
    EXPECT_EQ(PackageCacheEntryName("@studio/water.core", "2.3.4", "aabbccddeeff00112233"),
              "studio-water-core@2.3.4-aabbccddeeff");
    // Shorter-than-12 shas pass through un-truncated.
    EXPECT_EQ(PackageCacheEntryName("p", "1.0.0", "abc"), "p@1.0.0-abc");
}

TEST(PackageCachePaths, EnvOverrideWins)
{
    const fs::path dir = GameEngine::TestUtils::MakeUniqueTempDirectory("pkgcache_env");
    {
        ScopedPackageCacheDir scoped(dir);
        EXPECT_EQ(GlobalPackageCacheRoot(), dir);
    }
    // Without the override the root is machine-global, never empty.
    EXPECT_FALSE(GlobalPackageCacheRoot().empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Code modules build outside immutable git entries. The managed native cache uses
// short package/build identities; embedded packages keep owning their own .Cache.
TEST(PackageCachePaths, CodeModuleCacheDirLeavesGitEntriesImmutable)
{
    const fs::path dir = GameEngine::TestUtils::MakeUniqueTempDirectory("pkgcache_module");
    ScopedPackageCacheDir scoped(dir);

    ResolvedPackage gitPkg;
    gitPkg.Manifest.Name = "water-pack";
    gitPkg.Alias = "water-pack";
    gitPkg.SourceKind = PackageSourceKind::Git;
    gitPkg.RootDir = dir / "water-pack@1.0.0-abcabcabcabc";
    gitPkg.AssetsDir = gitPkg.RootDir / "Assets";

    ResolvedPackage embeddedPkg;
    embeddedPkg.Manifest.Name = "local-pack";
    embeddedPkg.Alias = "local-pack";
    embeddedPkg.SourceKind = PackageSourceKind::Embedded;
    embeddedPkg.RootDir = dir / "project" / "Packages" / "local-pack";
    embeddedPkg.AssetsDir = embeddedPkg.RootDir / "Assets";

    // One native runtime module each, discovered from the sources on disk.
    WriteSourceFile(gitPkg.RootDir / "Native" / "Module.cpp");
    WriteSourceFile(embeddedPkg.RootDir / "Native" / "Module.cpp");

    PackageResolution resolution;
    resolution.MountOrder = {gitPkg, embeddedPkg};

    const std::vector<PackageCodeModule> modules =
        CollectPackageCodeModules(resolution, /*editorContext=*/true);
    ASSERT_EQ(modules.size(), 2u);

    EXPECT_EQ(modules[0].CacheDir,
              PackageCacheDerivedNativeRoot(dir, "water-pack@1.0.0-abcabcabcabc",
                                            NativeScripting::EngineBuildIdentity()));
#if defined(__EMSCRIPTEN__)
    EXPECT_EQ(modules[0].CacheDir.parent_path(), dir / ".derived");
#else
#if defined(_WIN32)
    EXPECT_EQ(modules[0].CacheDir.parent_path().parent_path(),
              fs::temp_directory_path() / "GameEngine" / ".native");
#else
    EXPECT_EQ(modules[0].CacheDir.parent_path().parent_path(), dir / ".native");
#endif
    EXPECT_TRUE(modules[0].UsesManagedNativeCache);
    EXPECT_FALSE(modules[1].UsesManagedNativeCache);
#endif
    EXPECT_NE(modules[0].CacheDir.filename().string(), "water-pack@1.0.0-abcabcabcabc");
    EXPECT_EQ(modules[1].CacheDir, embeddedPkg.RootDir / ".Cache");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Lock file round-trip
// ---------------------------------------------------------------------------

TEST(PackagesLockFile, RoundTripsEntries)
{
    const fs::path dir = GameEngine::TestUtils::MakeUniqueTempDirectory("pkglock");
    const fs::path lockFile = dir / "Packages" / kPackagesLockFileName;

    PackagesLock lock;
    PackagesLockEntry entry;
    entry.Spec = "git+https://github.com/Lunarsong/GameEngine-Packages.git#v0.1.0&path=packages/sample-water";
    entry.Url = "https://github.com/Lunarsong/GameEngine-Packages.git";
    entry.Ref = "v0.1.0";
    entry.Commit = "0123456789abcdef0123456789abcdef01234567";
    entry.Version = "0.1.0";
    entry.Integrity = HashPackageManifestIntegrity("{\"name\":\"sample-water\"}");
    lock.Packages.emplace("sample-water", entry);

    ASSERT_TRUE(SavePackagesLock(lockFile, lock));

    PackagesLock loaded;
    std::string error;
    ASSERT_TRUE(TryLoadPackagesLock(lockFile, loaded, error)) << error;
    ASSERT_EQ(loaded.Packages.size(), 1u);
    const PackagesLockEntry& round = loaded.Packages.at("sample-water");
    EXPECT_EQ(round.Spec, entry.Spec);
    EXPECT_EQ(round.Url, entry.Url);
    EXPECT_EQ(round.Ref, entry.Ref);
    EXPECT_EQ(round.Commit, entry.Commit);
    EXPECT_EQ(round.Version, entry.Version);
    EXPECT_EQ(round.Integrity, entry.Integrity);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(PackagesLockFile, MissingFileIsEmptyAndMalformedFileErrors)
{
    const fs::path dir = GameEngine::TestUtils::MakeUniqueTempDirectory("pkglock_bad");

    PackagesLock lock;
    std::string error;
    EXPECT_TRUE(TryLoadPackagesLock(dir / "nope" / kPackagesLockFileName, lock, error));
    EXPECT_TRUE(lock.Packages.empty());

    const fs::path lockFile = dir / kPackagesLockFileName;
    WriteTextFile(lockFile, "this is not json");
    EXPECT_FALSE(TryLoadPackagesLock(lockFile, lock, error));
    EXPECT_FALSE(error.empty());

    WriteTextFile(lockFile, "{\"packages\": {\"p\": {\"spec\": \"x\"}}}"); // missing commit/version
    EXPECT_FALSE(TryLoadPackagesLock(lockFile, lock, error));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(PackagesLockFile, IntegrityHashIsStableAndContentSensitive)
{
    const std::string a = HashPackageManifestIntegrity("{\"name\":\"a\"}");
    EXPECT_EQ(a, HashPackageManifestIntegrity("{\"name\":\"a\"}"));
    EXPECT_NE(a, HashPackageManifestIntegrity("{\"name\":\"b\"}"));
    EXPECT_EQ(a.rfind("fnv1a64-", 0), 0u);
    EXPECT_EQ(a.size(), std::string("fnv1a64-").size() + 16);
}

// ---------------------------------------------------------------------------
// Resolver: lock-honoring offline path (warm cache, no git, no network)
// ---------------------------------------------------------------------------

TEST(PackageGitResolution, LockedSpecWithWarmCacheResolvesOffline)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkggit_offline");
    const fs::path cacheDir = root / "cache";
    const fs::path projectRoot = root / "project";
    ScopedPackageCacheDir scoped(cacheDir);

    const std::string commit = "fedcba9876543210fedcba9876543210fedcba98";
    const std::string spec =
        "git+https://example.invalid/does-not-exist.git#v9.9.9&path=packages/offline-pack";

    // Pre-populated immutable cache entry + matching lock = resolution must
    // never touch git (the URL host does not even exist).
    const std::string entryName = PackageCacheEntryName("offline-pack", "1.2.3", commit);
    WriteTextFile(cacheDir / entryName / "package.json",
                  "{\n  \"name\": \"offline-pack\",\n  \"version\": \"1.2.3\"\n}\n");
    WriteTextFile(cacheDir / entryName / "Assets" / "marker.txt", "offline");

    PackagesLock lock;
    PackagesLockEntry entry;
    entry.Spec = spec;
    entry.Url = "https://example.invalid/does-not-exist.git";
    entry.Ref = "v9.9.9";
    entry.Commit = commit;
    entry.Version = "1.2.3";
    entry.Integrity = "fnv1a64-0000000000000000";
    lock.Packages.emplace("offline-pack", entry);
    ASSERT_TRUE(SavePackagesLock(projectRoot / "Packages" / kPackagesLockFileName, lock));

    WriteTextFile(projectRoot / "Packages" / "manifest.json",
                  "{ \"dependencies\": { \"offline-pack\": \"" + spec + "\" } }");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    ASSERT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors.front());
    ASSERT_EQ(res.MountOrder.size(), 1u);
    const ResolvedPackage& pkg = res.MountOrder.front();
    EXPECT_EQ(pkg.Manifest.Name, "offline-pack");
    EXPECT_EQ(pkg.SourceKind, PackageSourceKind::Git);
    EXPECT_EQ(pkg.GitCommit, commit);
    EXPECT_EQ(pkg.RootDir.filename().string(), entryName);

    // The git mount shape for a manifest-less package: derived identity,
    // read-only, unwatched, metadata in memory without a derived database.
    const AssetSourceDesc desc = MakeGitPackageMount(pkg, root / "host-cache");
    EXPECT_TRUE(desc.DerivedIdentity);
    EXPECT_TRUE(desc.IsReadOnly);
    // Nothing is published, so there is no store an inferred tag could reach and
    // no baked artifact a cook would duplicate: both stay at the dev defaults.
    EXPECT_TRUE(desc.InfersTextureImportSettings);
    EXPECT_TRUE(desc.CooksDerivedArtifactsOnMiss);
    EXPECT_FALSE(desc.RegisterFileWatcher);
    EXPECT_TRUE(desc.RejectOverlappingRoots);
    EXPECT_TRUE(desc.AuthoritativeDbFile.empty());
    EXPECT_TRUE(desc.CacheRoot.empty());

    // The lock was honored, not rewritten.
    PackagesLock after;
    std::string error;
    ASSERT_TRUE(
        TryLoadPackagesLock(projectRoot / "Packages" / kPackagesLockFileName, after, error));
    EXPECT_EQ(after.Packages.at("offline-pack").Commit, commit);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageGitResolution, ShippedAssetManifestSelectsStoredIdentityMount)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkggit_manifest");
    ScopedPackageCacheDir scoped(root / "cache");

    ResolvedPackage pkg;
    pkg.Alias = "pub-pack";
    pkg.Priority = 31;
    pkg.SourceKind = PackageSourceKind::Git;
    pkg.RootDir = root / "cache" / "pub-pack@1.0.0-abcabcabcabc";
    pkg.AssetsDir = pkg.RootDir / "Assets";
    WriteTextFile(pkg.AssetsDir / ".assetmanifest", "");

    const AssetSourceDesc desc = MakeGitPackageMount(pkg, root / "host-cache");
    EXPECT_FALSE(desc.DerivedIdentity); // stored identity from the shipped manifest
    EXPECT_FALSE(desc.RequiresScan);
    EXPECT_TRUE(desc.IsImmutable);
    EXPECT_TRUE(desc.IsReadOnly);
    EXPECT_FALSE(desc.AcceptsMetadataWrites);
    // A dependency may ship stored identity without cooked textures, so this
    // process encodes the misses; its settings are the package author's and a
    // cache entry takes no writes, so nothing infers them.
    EXPECT_FALSE(desc.InfersTextureImportSettings);
    EXPECT_TRUE(desc.CooksDerivedArtifactsOnMiss);
    EXPECT_EQ(desc.AuthoritativeDbFile, pkg.AssetsDir / ".assetmanifest");

    // The same identity manifest is baked-only once staged for the Player: the
    // artifacts ship with it, so a miss is damage rather than work to do.
    const AssetSourceDesc staged = MakePackageMount(pkg.Alias, pkg.AssetsDir, pkg.Priority);
    EXPECT_FALSE(staged.InfersTextureImportSettings);
    EXPECT_FALSE(staged.CooksDerivedArtifactsOnMiss);
    EXPECT_TRUE(staged.IsImmutable);
    EXPECT_TRUE(staged.IsReadOnly);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageEngineMount, PublishedEnginePackageOwnsAWritableManifestStore)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkgengine_manifest");

    ResolvedPackage pkg;
    pkg.Alias = "eztree";
    pkg.Priority = 25;
    pkg.SourceKind = PackageSourceKind::Engine;
    pkg.RootDir = root / "Packages" / "eztree";
    pkg.AssetsDir = pkg.RootDir / "Assets";
    pkg.AuthoringRootDir = root / "Repository" / "Packages" / "eztree";
    WriteTextFile(pkg.AssetsDir / ".assetmanifest", "");
    WriteTextFile(pkg.AuthoringRootDir / "Assets" / ".assetmanifest", "");

    // An engine package is staged from the tree it is authored in, so it keeps
    // MakePackageMount's identity, scan and frozen-asset-set shape and differs
    // in exactly two fields: its import settings are writable, and they are
    // written to the manifest in the authoring tree rather than the staged copy
    // the next build overwrites. Every field is pinned, not a subset — a
    // descriptor test that lists most of a descriptor is where a side effect
    // hides.
    const AssetSourceDesc desc = MakeEnginePackageMount(pkg, root / "host-cache");
    EXPECT_FALSE(desc.DerivedIdentity);
    EXPECT_FALSE(desc.RequiresScan);
    EXPECT_EQ(desc.AuthoritativeDbFile, pkg.AuthoringRootDir / "Assets" / ".assetmanifest");
    EXPECT_EQ(desc.CacheRoot, root / "host-cache" / "Packages" / pkg.Alias / "AssetDatabase");
    EXPECT_TRUE(desc.IsImmutable);
    EXPECT_TRUE(desc.AcceptsMetadataWrites);
    EXPECT_FALSE(desc.IsReadOnly);
    EXPECT_FALSE(desc.TracksTombstones);
    EXPECT_FALSE(desc.RegisterFileWatcher);
    // Writable settings, but nothing INFERS them: the manifest is a committed
    // file, so the render path's bind-time tagger must never author it. Cooking
    // is the opposite answer — the editor owns the staged derived cache and is
    // the only thing that ever fills it.
    EXPECT_FALSE(desc.InfersTextureImportSettings);
    EXPECT_TRUE(desc.CooksDerivedArtifactsOnMiss);
    EXPECT_TRUE(desc.RejectOverlappingRoots);
    EXPECT_EQ(desc.Priority, pkg.Priority);
    EXPECT_EQ(desc.Alias, pkg.Alias);
    EXPECT_EQ(desc.Root, pkg.AssetsDir);
    EXPECT_TRUE(desc.IdentityManifestFile.empty());

    // The same staged tree with no authoring tree named — a shipped game, or an
    // engine build copied away from its checkout — is the read-only consumer
    // shape, storing into the staged manifest and taking no writes.
    ResolvedPackage shippedPkg = pkg;
    shippedPkg.AuthoringRootDir.clear();
    const AssetSourceDesc shipped = MakeEnginePackageMount(shippedPkg, root / "host-cache");
    EXPECT_TRUE(shipped.IsImmutable);
    EXPECT_FALSE(shipped.AcceptsMetadataWrites);
    EXPECT_TRUE(shipped.IsReadOnly);
    EXPECT_FALSE(shipped.InfersTextureImportSettings);
    // Still an engine package, still mounted only by a process built from the
    // engine's tree, so it cooks into the same host cache the writable shape
    // uses — what it cannot do is author the manifest it stages from.
    EXPECT_TRUE(shipped.CooksDerivedArtifactsOnMiss);
    EXPECT_EQ(shipped.CacheRoot, root / "host-cache" / "Packages" / pkg.Alias / "AssetDatabase");
    EXPECT_EQ(shipped.AuthoritativeDbFile, pkg.AssetsDir / ".assetmanifest");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageEngineMount, UnpublishedEnginePackageHasNoStoreToAuthorInto)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkgengine_nomanifest");
    ScopedPackageCacheDir scoped(root / "cache");

    ResolvedPackage pkg;
    pkg.Alias = "git-vcs";
    pkg.Priority = 25;
    pkg.SourceKind = PackageSourceKind::Engine;
    pkg.RootDir = root / "Packages" / "git-vcs";
    pkg.AssetsDir = pkg.RootDir / "Assets";
    WriteTextFile(pkg.AssetsDir / "Icons" / "SettingsGit.png", "PNGDATA");

    const AssetSourceDesc desc = MakeEnginePackageMount(pkg, root / "host-cache");
    EXPECT_TRUE(desc.DerivedIdentity);
    EXPECT_TRUE(desc.IsReadOnly);
    EXPECT_FALSE(desc.AcceptsMetadataWrites);
    EXPECT_TRUE(desc.AuthoritativeDbFile.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageGitResolution, MissingGitRefFailsLoudlyAndSkipsOnlyThatPackage)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkggit_badspec");
    ScopedPackageCacheDir scoped(root / "cache");
    const fs::path projectRoot = root / "project";

    WriteTextFile(projectRoot / "Packages" / "manifest.json",
                  "{ \"dependencies\": {\n"
                  "  \"bad-git\": \"git+https://host/repo.git\",\n"
                  "  \"good-local\": \"embedded\"\n"
                  "} }");
    WriteTextFile(projectRoot / "Packages" / "good-local" / "package.json",
                  "{ \"name\": \"good-local\", \"version\": \"1.0.0\" }");

    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    EXPECT_TRUE(AnyContainsText(res.Errors, "no ref"));
    ASSERT_EQ(res.MountOrder.size(), 1u); // degraded, not all-or-nothing
    EXPECT_EQ(res.MountOrder.front().Manifest.Name, "good-local");
    EXPECT_EQ(res.MountOrder.front().SourceKind, PackageSourceKind::Embedded);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Package Manager toggle: manifest disabled[] editing (parse-mutate-rewrite)
// ---------------------------------------------------------------------------

TEST(ProjectPackagesManifestEdit, DisableEnableRoundTripPreservesOtherFields)
{
    const fs::path dir = GameEngine::TestUtils::MakeUniqueTempDirectory("pkgmanifest_edit");
    const fs::path manifestFile = dir / "Packages" / "manifest.json";
    WriteTextFile(manifestFile,
                  "{\n"
                  "  \"dependencies\": { \"pkg-a\": \"embedded\", \"pkg-b\": \"embedded\" },\n"
                  "  \"disabled\": [\"pkg-b\"],\n"
                  "  \"customField\": 42\n"
                  "}\n");

    std::string error;
    ASSERT_TRUE(SetPackageDisabledInProjectManifest(manifestFile, "pkg-a", true, error)) << error;

    ProjectPackagesManifest manifest;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(manifestFile, manifest, error)) << error;
    EXPECT_EQ(manifest.Disabled.count("pkg-a"), 1u);
    EXPECT_EQ(manifest.Disabled.count("pkg-b"), 1u);
    EXPECT_EQ(manifest.Dependencies.size(), 2u);
    EXPECT_NE(ReadTextFile(manifestFile).find("customField"), std::string::npos)
        << "unrelated manifest fields must survive the rewrite";

    // Re-enable both; the empty array is dropped entirely.
    ASSERT_TRUE(SetPackageDisabledInProjectManifest(manifestFile, "pkg-a", false, error));
    ASSERT_TRUE(SetPackageDisabledInProjectManifest(manifestFile, "pkg-b", false, error));
    ASSERT_TRUE(TryLoadProjectPackagesManifest(manifestFile, manifest, error)) << error;
    EXPECT_TRUE(manifest.Disabled.empty());
    EXPECT_EQ(ReadTextFile(manifestFile).find("\"disabled\""), std::string::npos);

    // Disabling into a missing manifest creates it — a project that never
    // opted out must still be able to disable a package from the panel.
    const fs::path createdManifest = dir / "Created" / "manifest.json";
    EXPECT_TRUE(SetPackageDisabledInProjectManifest(createdManifest, "pkg-a", true, error)) << error;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(createdManifest, manifest, error)) << error;
    EXPECT_EQ(manifest.Disabled.count("pkg-a"), 1u);

    // Enabling with no manifest is already the desired state; no file appears.
    const fs::path untouchedManifest = dir / "Untouched" / "manifest.json";
    EXPECT_TRUE(SetPackageDisabledInProjectManifest(untouchedManifest, "pkg-a", false, error)) << error;
    EXPECT_FALSE(fs::exists(untouchedManifest));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// LIVE git end-to-end (opt-in: set GE_GIT_E2E=1; requires git + network).
// Clones the real sample packages from the GameEngine-Packages repo by tag,
// populates the global cache, writes the lock, and re-resolves offline.
// ---------------------------------------------------------------------------

TEST(PackageGitResolution, LiveEndToEndAgainstGameEnginePackagesRepo)
{
    const char* enabled = std::getenv("GE_GIT_E2E");
    if (!enabled || std::string(enabled) != "1")
        GTEST_SKIP() << "set GE_GIT_E2E=1 to run the live git end-to-end test";

    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkggit_live");
    const fs::path cacheDir = root / "cache";
    const fs::path projectRoot = root / "project";
    ScopedPackageCacheDir scoped(cacheDir);

    const std::string repo = "https://github.com/Lunarsong/GameEngine-Packages.git";
    WriteTextFile(projectRoot / "Packages" / "manifest.json",
                  "{ \"dependencies\": {\n"
                  "  \"sample-water\": \"git+" + repo + "#v0.1.0&path=packages/sample-water\",\n"
                  "  \"sample-tools\": \"git+" + repo + "#v0.1.0&path=packages/sample-tools\"\n"
                  "} }");

    // First resolve: clones, populates the cache, writes the lock.
    const PackageResolution res = PackageResolver::Resolve(projectRoot);
    ASSERT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors.front());
    ASSERT_EQ(res.MountOrder.size(), 2u);
    for (const ResolvedPackage& pkg : res.MountOrder)
    {
        EXPECT_EQ(pkg.SourceKind, PackageSourceKind::Git);
        EXPECT_GE(pkg.GitCommit.size(), 40u);
        EXPECT_TRUE(fs::exists(pkg.RootDir / "package.json"));
        EXPECT_TRUE(fs::exists(pkg.AssetsDir));
        EXPECT_TRUE(fs::exists(cacheDir / PackageCacheEntryName(
                                              pkg.Manifest.Name,
                                              pkg.Manifest.Version.ToString(), pkg.GitCommit)));
    }

    PackagesLock lock;
    std::string error;
    ASSERT_TRUE(TryLoadPackagesLock(projectRoot / "Packages" / kPackagesLockFileName, lock, error));
    ASSERT_EQ(lock.Packages.size(), 2u);
    const std::string firstCommit = lock.Packages.at("sample-water").Commit;
    EXPECT_EQ(lock.Packages.at("sample-water").Version, "0.1.0");

    // Second resolve: must honor the lock offline (warm cache) — byte-identical
    // pins and the sample-water module/defines visible to the code-module pass.
    const PackageResolution again = PackageResolver::Resolve(projectRoot);
    ASSERT_TRUE(again.Errors.empty()) << (again.Errors.empty() ? "" : again.Errors.front());
    ASSERT_EQ(again.MountOrder.size(), 2u);
    // sample-tools depends on sample-water: topo order puts water first.
    EXPECT_EQ(again.MountOrder[0].Manifest.Name, "sample-water");
    EXPECT_EQ(again.MountOrder[1].Manifest.Name, "sample-tools");
    EXPECT_EQ(again.MountOrder[0].GitCommit, firstCommit);

    // Code modules: water = CSharp Runtime, tools = CSharp Editor (editor
    // context only), and the SAMPLE_WATER define propagates.
    const std::vector<PackageCodeModule> modules =
        CollectPackageCodeModules(again, /*editorContext=*/true);
    ASSERT_EQ(modules.size(), 2u);
    EXPECT_EQ(modules[0].AssemblyName, "SampleWater");
    EXPECT_EQ(modules[0].Kind, PackageModuleRecord::ModuleKind::Runtime);
    EXPECT_EQ(modules[1].AssemblyName, "SampleTools.Editor");
    EXPECT_EQ(modules[1].Kind, PackageModuleRecord::ModuleKind::Editor);
    const std::vector<std::string> defines = CollectAllPackageDefines(again);
    ASSERT_EQ(defines.size(), 1u);
    EXPECT_EQ(defines[0], "SAMPLE_WATER");
    EXPECT_TRUE(CollectPackageCodeModules(again, /*editorContext=*/false).size() == 1u)
        << "Editor-kind sample-tools module must not exist in the Player context";

    // Mount into a live AssetManager straight from the immutable cache and
    // assert the packages' assets register under their aliases.
    std::error_code ec;
    fs::create_directories(projectRoot / "Assets", ec);
    {
        AssetManager am;
        ASSERT_TRUE(am.Initialize(projectRoot / "Assets", nullptr,
                                  projectRoot / "AssetDatabase.assetdb",
                                  projectRoot / ".Cache" / "AssetDatabase"));
        const std::vector<std::string> mounted = MountResolvedPackages(am, again, root / "host-cache");
        ASSERT_EQ(mounted.size(), 2u);
        am.WaitForStartupScan("sample-water");
        am.WaitForStartupScan("sample-tools");

        const GUID waterMarker =
            am.GetRegistry().GetAssetGUID(again.MountOrder[0].AssetsDir / "WaterMarker.txt");
        EXPECT_FALSE(waterMarker.IsNull()) << "sample-water asset did not register from the cache mount";
        const GUID toolsMarker =
            am.GetRegistry().GetAssetGUID(again.MountOrder[1].AssetsDir / "ToolsMarker.txt");
        EXPECT_FALSE(toolsMarker.IsNull()) << "sample-tools asset did not register from the cache mount";
        am.Shutdown();
    }

    fs::remove_all(root, ec);
}

TEST(PackageGitResolution, RegisteredMountUsesHostCacheOnlyWithManifest)
{
    const fs::path root = GameEngine::TestUtils::MakeUniqueTempDirectory("pkggit_host_cache");
    ScopedPackageCacheDir scoped(root / "global-cache");
    ResolvedPackage pkg;
    pkg.Alias = "host-pack";
    pkg.SourceKind = PackageSourceKind::Git;
    pkg.RootDir = root / "payload";
    pkg.AssetsDir = pkg.RootDir / "Assets";
    fs::create_directories(pkg.AssetsDir);
    const auto hostCache = root / "host-cache";
    for (const bool hasManifest : {false, true})
    {
        if (hasManifest) WriteTextFile(pkg.AssetsDir / ".assetmanifest", "");
        AssetRegistry registry;
        ASSERT_TRUE(registry.Initialize());
        ASSERT_TRUE(registry.RegisterSource(MakeGitPackageMount(pkg, hostCache)));
        const auto cache = registry.TryGetCacheRoot(pkg.AssetsDir / "marker.png");
        if (hasManifest)
        {
            ASSERT_TRUE(cache.has_value());
            EXPECT_EQ(cache->lexically_normal(), (hostCache / "Packages" / "host-pack").lexically_normal());
        }
        else EXPECT_FALSE(cache.has_value());
        registry.Shutdown();
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
