// Package Manager panel actions (Add / Remove / Re-pin / Relocate) against
// real temp manifests + lock files: exact JSON out for the mutation helpers,
// flow-level validation (name matching, duplicate guards, git-vs-file
// classification), and one env-gated LIVE git end-to-end against the real
// GameEngine-Packages repo.

#include "Assets/Packages/PackageGitFetcher.h"
#include "Assets/Packages/PackageManagementActions.h"
#include "AssetCore/PathNormalization.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/Packages/PackagesLockFile.h"
#include "Assets/Packages/ProjectPackagesManifest.h"
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

std::string ReadTextFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

void WritePackageJson(const fs::path& dir, const std::string& name, const std::string& version)
{
    WriteTextFile(dir / "package.json",
                  "{ \"name\": \"" + name + "\", \"version\": \"" + version + "\" }");
}

fs::path ManifestOf(const fs::path& projectRoot) { return projectRoot / "Packages" / "manifest.json"; }
fs::path LockOf(const fs::path& projectRoot)
{
    return projectRoot / "Packages" / kPackagesLockFileName;
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

class ScopedPackageCacheDir
{
  public:
    explicit ScopedPackageCacheDir(const fs::path& dir)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        SetEnvVar("GE_PACKAGE_CACHE_DIR", dir.string().c_str());
    }
    ~ScopedPackageCacheDir() { SetEnvVar("GE_PACKAGE_CACHE_DIR", nullptr); }
};

} // namespace

// ---------------------------------------------------------------------------
// Add / Remove input classification
// ---------------------------------------------------------------------------

TEST(PackageAddInput, ClassifiesGitVsLocal)
{
    EXPECT_TRUE(IsGitAddInput("git+https://github.com/a/b.git#v1"));
    EXPECT_TRUE(IsGitAddInput("https://github.com/a/b.git#v1"));
    EXPECT_FALSE(IsGitAddInput("C:/Packages/MyPack"));
    EXPECT_FALSE(IsGitAddInput("../SharedPackages/OceanPack"));
    EXPECT_FALSE(IsGitAddInput("file:../SharedPackages/OceanPack"));
}

// ---------------------------------------------------------------------------
// Add local
// ---------------------------------------------------------------------------

TEST(PackageAddLocal, CreatesManifestWithExactJsonForEmbeddedDir)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgadd_embedded");
    const fs::path project = root / "project";
    WritePackageJson(project / "Packages" / "pkg-a", "pkg-a", "1.0.0");

    PackageAddResult result;
    std::string error;
    // Absolute path to <project>/Packages/<name> canonicalizes to "embedded".
    ASSERT_TRUE(AddLocalPackageToProject(project, (project / "Packages" / "pkg-a").string(),
                                         result, error))
        << error;
    EXPECT_EQ(result.Name, "pkg-a");
    EXPECT_EQ(result.Spec, "embedded");

    EXPECT_EQ(ReadTextFile(ManifestOf(project)),
              "{\n  \"dependencies\": {\n    \"pkg-a\": \"embedded\"\n  }\n}\n");

    // Duplicate add fails loudly.
    EXPECT_FALSE(AddLocalPackageToProject(project, (project / "Packages" / "pkg-a").string(),
                                          result, error));
    EXPECT_NE(error.find("already declared"), std::string::npos) << error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageAddLocal, RelativePathResolvesAgainstPackagesDirAndStaysRelative)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgadd_rel");
    const fs::path project = root / "project";
    WritePackageJson(root / "SharedPackages" / "OceanPack", "ocean-pack", "2.0.0");
    fs::create_directories(project / "Packages");

    PackageAddResult result;
    std::string error;
    ASSERT_TRUE(AddLocalPackageToProject(project, "../../SharedPackages/OceanPack", result, error))
        << error;
    EXPECT_EQ(result.Name, "ocean-pack");
    EXPECT_EQ(result.Spec, "file:../../SharedPackages/OceanPack");

    ProjectPackagesManifest manifest;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.at("ocean-pack"), "file:../../SharedPackages/OceanPack");

    // The written spec must round-trip through the resolver.
    const PackageResolution res = PackageResolver::Resolve(project, root / "no-engine-packages");
    ASSERT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors.front());
    ASSERT_EQ(res.MountOrder.size(), 1u);
    EXPECT_EQ(res.MountOrder.front().Manifest.Name, "ocean-pack");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PackageAddLocal, MissingPackageJsonAndFilePrefixHandling)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgadd_bad");
    const fs::path project = root / "project";
    fs::create_directories(project / "Packages");
    fs::create_directories(root / "NotAPackage");

    PackageAddResult result;
    std::string error;
    EXPECT_FALSE(AddLocalPackageToProject(project, (root / "NotAPackage").string(), result, error));
    EXPECT_NE(error.find("no package.json"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(ManifestOf(project))) << "failed add must not create a manifest";

    // A pasted "file:<path>" spec is accepted and normalized.
    WritePackageJson(root / "Ext" / "ToolPack", "tool-pack", "0.3.0");
    ASSERT_TRUE(AddLocalPackageToProject(
        project, "file:" + (root / "Ext" / "ToolPack").generic_string(), result, error))
        << error;
    EXPECT_EQ(result.Name, "tool-pack");
    EXPECT_EQ(result.Spec, "file:" + (root / "Ext" / "ToolPack").generic_string());

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

TEST(PackageRemove, DropsDependencyDisabledEntryAndLockPin)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgremove");
    const fs::path project = root / "project";
    WriteTextFile(ManifestOf(project),
                  "{\n"
                  "  \"dependencies\": {\n"
                  "    \"git-pack\": \"git+https://host/repo.git#v1.0.0\",\n"
                  "    \"local-pack\": \"embedded\"\n"
                  "  },\n"
                  "  \"disabled\": [\"git-pack\"],\n"
                  "  \"customField\": 42\n"
                  "}\n");
    PackagesLock lock;
    PackagesLockEntry entry;
    entry.Spec = "git+https://host/repo.git#v1.0.0";
    entry.Url = "https://host/repo.git";
    entry.Ref = "v1.0.0";
    entry.Commit = "0123456789abcdef0123456789abcdef01234567";
    entry.Version = "1.0.0";
    entry.Integrity = "fnv1a64-0000000000000000";
    lock.Packages.emplace("git-pack", entry);
    ASSERT_TRUE(SavePackagesLock(LockOf(project), lock));

    std::string error;
    ASSERT_TRUE(RemovePackageFromProject(project, "git-pack", error)) << error;

    ProjectPackagesManifest manifest;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.count("git-pack"), 0u);
    EXPECT_EQ(manifest.Dependencies.count("local-pack"), 1u);
    EXPECT_TRUE(manifest.Disabled.empty());
    EXPECT_NE(ReadTextFile(ManifestOf(project)).find("customField"), std::string::npos)
        << "unrelated manifest fields must survive the rewrite";

    // The dead pin is dropped NOW — resolver lock hygiene only runs when
    // another git dependency loads the lock, which may never happen.
    PackagesLock after;
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), after, error)) << error;
    EXPECT_TRUE(after.Packages.empty());

    // Removing the last dependency drops the whole dependencies object.
    ASSERT_TRUE(RemovePackageFromProject(project, "local-pack", error)) << error;
    EXPECT_EQ(ReadTextFile(ManifestOf(project)).find("\"dependencies\""), std::string::npos);

    // Unknown name = loud failure.
    EXPECT_FALSE(RemovePackageFromProject(project, "nope", error));
    EXPECT_NE(error.find("not declared"), std::string::npos) << error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Re-pin
// ---------------------------------------------------------------------------

TEST(PackageRepin, SwapsRefPreservesSubdirAndDropsPin)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgrepin");
    const fs::path project = root / "project";
    const std::string spec =
        "git+https://github.com/Lunarsong/GameEngine-Packages.git#v0.1.0&path=packages/sample-water";
    WriteTextFile(ManifestOf(project),
                  "{ \"dependencies\": { \"sample-water\": \"" + spec + "\" } }");
    PackagesLock lock;
    PackagesLockEntry entry;
    entry.Spec = spec;
    entry.Url = "https://github.com/Lunarsong/GameEngine-Packages.git";
    entry.Ref = "v0.1.0";
    entry.Commit = "0123456789abcdef0123456789abcdef01234567";
    entry.Version = "0.1.0";
    entry.Integrity = "fnv1a64-0000000000000000";
    lock.Packages.emplace("sample-water", entry);
    ASSERT_TRUE(SavePackagesLock(LockOf(project), lock));

    std::string error;
    ASSERT_TRUE(RepinGitPackageToRef(project, "sample-water", "v0.2.1", error)) << error;

    ProjectPackagesManifest manifest;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.at("sample-water"),
              "git+https://github.com/Lunarsong/GameEngine-Packages.git#v0.2.1"
              "&path=packages/sample-water");

    PackagesLock after;
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), after, error)) << error;
    EXPECT_TRUE(after.Packages.empty()) << "re-pin must drop the stale pin";

    // Re-pinning to the SAME ref is "update to latest on ref": the spec text
    // is unchanged but the pin must still be dropped.
    ASSERT_TRUE(UpsertPackagesLockEntry(LockOf(project), "sample-water", entry, error)) << error;
    ASSERT_TRUE(RepinGitPackageToRef(project, "sample-water", "v0.2.1", error)) << error;
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), after, error)) << error;
    EXPECT_TRUE(after.Packages.empty());

    // Guard rails: bad refs and non-git dependencies.
    EXPECT_FALSE(RepinGitPackageToRef(project, "sample-water", "v1#oops", error));
    EXPECT_FALSE(RepinGitPackageToRef(project, "sample-water", "", error));
    WriteTextFile(ManifestOf(project), "{ \"dependencies\": { \"local\": \"embedded\" } }");
    EXPECT_FALSE(RepinGitPackageToRef(project, "local", "v1", error));
    EXPECT_NE(error.find("not a git dependency"), std::string::npos) << error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Relocate
// ---------------------------------------------------------------------------

TEST(PackageRelocate, ValidatesNameAndRewritesPath)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgrelocate");
    const fs::path project = root / "project";
    WriteTextFile(ManifestOf(project),
                  "{ \"dependencies\": { \"tool-pack\": \"file:../Gone/ToolPack\" } }");

    // Wrong package name => refused, manifest untouched.
    WritePackageJson(root / "Elsewhere" / "WrongPack", "other-pack", "1.0.0");
    std::string error;
    EXPECT_FALSE(RelocatePackageInProject(project, "tool-pack", root / "Elsewhere" / "WrongPack",
                                          error));
    EXPECT_NE(error.find("names 'other-pack'"), std::string::npos) << error;
    ProjectPackagesManifest manifest;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.at("tool-pack"), "file:../Gone/ToolPack");

    // Outside the project => absolute path, in the spelling the caller gave
    // (the mount-root rule: no symlink resolution), so a manifest never bakes
    // in a resolved volume path the user did not type.
    WritePackageJson(root / "Elsewhere" / "ToolPack", "tool-pack", "1.0.0");
    ASSERT_TRUE(RelocatePackageInProject(project, "tool-pack", root / "Elsewhere" / "ToolPack",
                                         error))
        << error;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    {
        const fs::path expected = AssetPaths::NormalizeMountRoot(root / "Elsewhere" / "ToolPack");
        EXPECT_EQ(manifest.Dependencies.at("tool-pack"), "file:" + expected.generic_string());
    }

    // Inside the project => relative to Packages/.
    WritePackageJson(project / "Vendor" / "ToolPack", "tool-pack", "1.0.0");
    ASSERT_TRUE(RelocatePackageInProject(project, "tool-pack", project / "Vendor" / "ToolPack",
                                         error))
        << error;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.at("tool-pack"), "file:../Vendor/ToolPack");

    // The embedded location canonicalizes to "embedded".
    WritePackageJson(project / "Packages" / "tool-pack", "tool-pack", "1.0.0");
    ASSERT_TRUE(RelocatePackageInProject(project, "tool-pack", project / "Packages" / "tool-pack",
                                         error))
        << error;
    ASSERT_TRUE(TryLoadProjectPackagesManifest(ManifestOf(project), manifest, error)) << error;
    EXPECT_EQ(manifest.Dependencies.at("tool-pack"), "embedded");

    // Git dependencies cannot be relocated.
    WriteTextFile(ManifestOf(project),
                  "{ \"dependencies\": { \"g\": \"git+https://host/r.git#v1\" } }");
    EXPECT_FALSE(RelocatePackageInProject(project, "g", root / "Elsewhere" / "ToolPack", error));
    EXPECT_NE(error.find("re-pin"), std::string::npos) << error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Lock entry helpers: exact JSON out
// ---------------------------------------------------------------------------

TEST(PackagesLockEntryEdit, UpsertAndRemoveRoundTrip)
{
    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkglock_edit");
    const fs::path lockFile = root / "Packages" / kPackagesLockFileName;

    PackagesLockEntry entry;
    entry.Spec = "git+https://host/repo.git#v1";
    entry.Url = "https://host/repo.git";
    entry.Ref = "v1";
    entry.Commit = "0123456789abcdef0123456789abcdef01234567";
    entry.Version = "1.0.0";
    entry.Integrity = "fnv1a64-0000000000000000";

    std::string error;
    ASSERT_TRUE(UpsertPackagesLockEntry(lockFile, "p", entry, error)) << error;
    EXPECT_EQ(ReadTextFile(lockFile),
              "{\n"
              "  \"packages\": {\n"
              "    \"p\": {\n"
              "      \"commit\": \"0123456789abcdef0123456789abcdef01234567\",\n"
              "      \"integrity\": \"fnv1a64-0000000000000000\",\n"
              "      \"ref\": \"v1\",\n"
              "      \"spec\": \"git+https://host/repo.git#v1\",\n"
              "      \"url\": \"https://host/repo.git\",\n"
              "      \"version\": \"1.0.0\"\n"
              "    }\n"
              "  },\n"
              "  \"version\": 1\n"
              "}\n");

    // Removing an absent entry (or from a missing file) is quiet success.
    ASSERT_TRUE(RemovePackagesLockEntry(lockFile, "not-there", error)) << error;
    ASSERT_TRUE(RemovePackagesLockEntry(root / "nope" / "lock.json", "p", error)) << error;

    ASSERT_TRUE(RemovePackagesLockEntry(lockFile, "p", error)) << error;
    PackagesLock after;
    ASSERT_TRUE(TryLoadPackagesLock(lockFile, after, error)) << error;
    EXPECT_TRUE(after.Packages.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Remote ref query: the sha fast-path needs no git and no network
// ---------------------------------------------------------------------------

TEST(GitRemoteRefQuery, FullShaIsItsOwnPin)
{
    std::string commit;
    std::string error;
    const std::string sha = "0123456789abcdef0123456789abcdef01234567";
    ASSERT_TRUE(QueryGitRemoteRefCommit("git", "https://example.invalid/repo.git", sha, commit,
                                        error))
        << error;
    EXPECT_EQ(commit, sha);
}

// ---------------------------------------------------------------------------
// LIVE git end-to-end (opt-in: set GE_GIT_E2E=1; requires git + network):
// add by tag -> lock pinned + offline re-resolve -> check-updates says up to
// date on the tag -> re-pin to an older tag -> resolve moves the pin.
// ---------------------------------------------------------------------------

TEST(PackageManagementLive, AddRepinCheckAgainstGameEnginePackagesRepo)
{
    const char* enabled = std::getenv("GE_GIT_E2E");
    if (!enabled || std::string(enabled) != "1")
        GTEST_SKIP() << "set GE_GIT_E2E=1 to run the live git end-to-end test";

    const fs::path root = TestUtils::MakeUniqueTempDirectory("pkgmgmt_live");
    const fs::path cacheDir = root / "cache";
    const fs::path project = root / "project";
    ScopedPackageCacheDir scoped(cacheDir);
    fs::create_directories(project / "Packages");

    const std::string repo = "https://github.com/Lunarsong/GameEngine-Packages.git";
    const std::string spec = "git+" + repo + "#v0.2.1&path=packages/sample-water";

    // Add learns the name from the fetched package.json and pins the lock.
    PackageAddResult added;
    std::string error;
    ASSERT_TRUE(AddGitPackageToProject(project, spec, added, error)) << error;
    EXPECT_EQ(added.Name, "sample-water");

    PackagesLock lock;
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), lock, error)) << error;
    ASSERT_EQ(lock.Packages.count("sample-water"), 1u);
    const std::string v021Commit = lock.Packages.at("sample-water").Commit;
    EXPECT_GE(v021Commit.size(), 40u);

    // The follow-up resolve honors the fresh pin offline (warm cache).
    const PackageResolution res = PackageResolver::Resolve(project, root / "no-engine-packages");
    ASSERT_TRUE(res.Errors.empty()) << (res.Errors.empty() ? "" : res.Errors.front());
    ASSERT_EQ(res.MountOrder.size(), 1u);
    EXPECT_EQ(res.MountOrder.front().GitCommit, v021Commit);

    // Check-updates: a tag does not move, so the pin is up to date.
    std::string status;
    ASSERT_TRUE(CheckGitPackageForUpdates(project, "sample-water", status, error)) << error;
    EXPECT_NE(status.find("up to date"), std::string::npos) << status;

    // Re-pin to the older tag: spec ref swaps, pin drops, resolve re-acquires.
    ASSERT_TRUE(RepinGitPackageToRef(project, "sample-water", "v0.1.0", error)) << error;
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), lock, error)) << error;
    EXPECT_EQ(lock.Packages.count("sample-water"), 0u);

    const PackageResolution repinned = PackageResolver::Resolve(project, root / "no-engine-packages");
    ASSERT_TRUE(repinned.Errors.empty())
        << (repinned.Errors.empty() ? "" : repinned.Errors.front());
    ASSERT_EQ(repinned.MountOrder.size(), 1u);
    EXPECT_NE(repinned.MountOrder.front().GitCommit, v021Commit)
        << "v0.1.0 must resolve to a different commit than v0.2.1";
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), lock, error)) << error;
    ASSERT_EQ(lock.Packages.count("sample-water"), 1u);
    EXPECT_EQ(lock.Packages.at("sample-water").Ref, "v0.1.0");

    // Remove ejects manifest + pin.
    ASSERT_TRUE(RemovePackageFromProject(project, "sample-water", error)) << error;
    ASSERT_TRUE(TryLoadPackagesLock(LockOf(project), lock, error)) << error;
    EXPECT_TRUE(lock.Packages.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}
