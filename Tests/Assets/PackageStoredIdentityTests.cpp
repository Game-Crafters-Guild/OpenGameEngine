// SEAM 4: stored-identity publishing for extracted packages.
//
// Covers: PublishPackageAssetManifest (current GUIDs verbatim, extraction
// remap, idempotence, loud collisions), the mutable stored-identity embedded
// mount (manifest merged at setup, published identity wins), the extraction
// acceptance shape (original project GUID resolves against the package mount
// after the file physically moved), and the out-of-editor add flow (derived
// GUID + package-DB routing for mutable mounts, derived FALLBACK for
// manifest-missing files in published mutable mounts, rejection on immutable
// mounts), and per-asset import metadata on a package mount (an engine
// package's manifest is its writable store; a cache-entry mount refuses).
// Identity decision table: Engine/Include/Assets/Packages/PackageMounts.h.

#include "AssetDatabase/AssetDbCache_Sqlite.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/PackagePublish.h"
#include "Assets/Packages/PackageResolver.h"
#include "Assets/TextureCook.h"
#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Registry-resolved paths come back normalized (lowercased on Windows);
// compare case-insensitively against test-constructed paths.
std::string NormalizeForCompare(const fs::path& p)
{
    std::string s = p.lexically_normal().generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Project fixture matching the PackageMountE2E shape: AssetManager-init
// project source (derived identity) + temp tree cleanup.
struct StoredIdentityProject
{
    fs::path Root;
    AssetManager Manager;

    explicit StoredIdentityProject(const char* prefix)
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

    // Writes an embedded package skeleton and returns its ResolvedPackage
    // (bypasses the resolver: these tests exercise the mount + publish
    // machinery, not resolution).
    ResolvedPackage MakePackage(const std::string& name, int32_t priority = 25)
    {
        const fs::path pkgRoot = Root / "Packages" / name;
        WriteTextFile(pkgRoot / "package.json",
                      "{ \"name\": \"" + name + "\", \"version\": \"1.0.0\" }\n");
        std::error_code ec;
        fs::create_directories(pkgRoot / "Assets", ec);
        ResolvedPackage pkg;
        pkg.RootDir = pkgRoot;
        pkg.AssetsDir = pkgRoot / "Assets";
        pkg.Alias = name;
        pkg.Priority = priority;
        pkg.SourceKind = PackageSourceKind::Embedded;
        return pkg;
    }

    bool MountEmbedded(const ResolvedPackage& pkg)
    {
        return Manager.RegisterSource(MakeEmbeddedPackageMount(pkg));
    }

    ~StoredIdentityProject()
    {
        Manager.Shutdown();
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Item 1: publish step — current GUIDs verbatim, idempotent.
// ---------------------------------------------------------------------------

TEST(PackagePublish, WritesCurrentGuidsVerbatimAndIsIdempotent)
{
    StoredIdentityProject project("ge_pkg_publish");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    ResolvedPackage pkg = project.MakePackage("pkg-art");
    WriteTextFile(pkg.AssetsDir / "textures" / "wood.png", "PNGDATA");
    WriteTextFile(pkg.AssetsDir / "marker.txt", "pkg-art");
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-art");

    const GUID woodGuid = reg.GetAssetGUID(pkg.AssetsDir / "textures" / "wood.png");
    ASSERT_FALSE(woodGuid.IsNull());
    EXPECT_EQ(woodGuid, AssetRegistry::DeriveGuidForSourcePath("pkg-art", "textures/wood.png"));

    const PackagePublishResult published = PublishPackageAssetManifest(reg, "pkg-art");
    ASSERT_TRUE(published.Success) << (published.Errors.empty() ? "" : published.Errors[0]);
    EXPECT_EQ(published.EntryCount, 2u);
    EXPECT_EQ(published.RemappedCount, 0u);
    EXPECT_EQ(NormalizeForCompare(published.ManifestPath),
              NormalizeForCompare(pkg.AssetsDir / ".assetmanifest"));
    ASSERT_TRUE(fs::exists(published.ManifestPath));

    // The manifest carries the registry's CURRENT GUIDs verbatim.
    AssetDatabase::AssetStore_TextJsonl manifest;
    ASSERT_TRUE(manifest.LoadFromFile(published.ManifestPath, nullptr));
    EXPECT_EQ(manifest.CountAssets(), 2u);
    const auto woodInManifest = manifest.LookupGuidByPath("textures/wood.png");
    ASSERT_TRUE(woodInManifest.has_value());
    EXPECT_EQ(*woodInManifest, woodGuid);

    // Republish with unchanged state → byte-identical file (idempotent).
    const std::string firstBytes = ReadTextFile(published.ManifestPath);
    const PackagePublishResult republished = PublishPackageAssetManifest(reg, "pkg-art");
    ASSERT_TRUE(republished.Success);
    EXPECT_EQ(ReadTextFile(published.ManifestPath), firstBytes);
}

// ---------------------------------------------------------------------------
// Item 2: extraction acceptance — a scene reference recorded against the
// ORIGINAL project GUID resolves against the package mount after the file
// physically moved into the package and the manifest was published with the
// original identity.
// ---------------------------------------------------------------------------

TEST(PackageStoredIdentity, ExtractionAcceptance_OriginalProjectGuidResolvesAfterMove)
{
    StoredIdentityProject project("ge_pkg_extract");
    const fs::path projTexAbs = project.Root / "Assets" / "textures" / "eztree" / "leaf.png";
    WriteTextFile(projTexAbs, "PNGDATA");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();
    am.WaitForStartupScan("project");

    // 1. Register under the project alias, record the ORIGINAL GUID, and
    //    author a scene-like reference against it.
    const GUID guidOrig = reg.GetAssetGUID(projTexAbs);
    ASSERT_FALSE(guidOrig.IsNull());
    EXPECT_EQ(guidOrig, AssetRegistry::DeriveGuidForSourcePath("project", "textures/eztree/leaf.png"));
    WriteTextFile(project.Root / "Assets" / "scene.ref", guidOrig.ToString());

    // 2. Simulated extraction: physically move the file into a package layout
    //    and drop the project registration (as the file watcher would).
    ResolvedPackage pkg = project.MakePackage("eztree");
    const fs::path pkgTexAbs = pkg.AssetsDir / "textures" / "leaf.png";
    std::error_code ec;
    fs::create_directories(pkgTexAbs.parent_path(), ec);
    fs::rename(projTexAbs, pkgTexAbs, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(reg.TryUnregisterAssetByPath(projTexAbs));

    // 3. Mount the package (no manifest yet → plain derived mount) and show
    //    the move actually broke identity: the package derives a DIFFERENT
    //    GUID than the project did. This is the gap SEAM 4 closes.
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("eztree");
    const GUID pkgDerived = reg.GetAssetGUID(pkgTexAbs);
    ASSERT_FALSE(pkgDerived.IsNull());
    EXPECT_NE(pkgDerived, guidOrig);

    // 4. Publish the manifest with the extraction remap: the entry for the
    //    file's NEW package path carries the ORIGINAL project-derived GUID.
    PackagePublishRemap remap;
    remap["textures/leaf.png"] = {"project", "textures/eztree/leaf.png"};
    const PackagePublishResult published = PublishPackageAssetManifest(reg, "eztree", remap);
    ASSERT_TRUE(published.Success) << (published.Errors.empty() ? "" : published.Errors[0]);
    EXPECT_EQ(published.RemappedCount, 1u);
    EXPECT_TRUE(published.Warnings.empty());
    {
        AssetDatabase::AssetStore_TextJsonl manifest;
        ASSERT_TRUE(manifest.LoadFromFile(published.ManifestPath, nullptr));
        const auto inManifest = manifest.LookupGuidByPath("textures/leaf.png");
        ASSERT_TRUE(inManifest.has_value());
        EXPECT_EQ(*inManifest, guidOrig);
    }

    // 5. Remount: the embedded mount now detects the manifest and mounts with
    //    MUTABLE STORED identity — the original GUID resolves.
    ASSERT_TRUE(am.BeginUnregisterSource("eztree"));
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("eztree");
    EXPECT_EQ(reg.GetAssetGUID(pkgTexAbs), guidOrig);

    // The scene's recorded reference resolves to the file at its package home.
    const GUID sceneRef{String(ReadTextFile(project.Root / "Assets" / "scene.ref"))};
    AssetMetadata meta;
    ASSERT_TRUE(reg.TryGetAssetMetadata(sceneRef, meta));
    EXPECT_NE(meta.Path.generic_string().find("textures/leaf.png"), std::string::npos);

    // The manifest itself must never register as an asset.
    EXPECT_TRUE(reg.GetAssetGUID(pkg.AssetsDir / ".assetmanifest").IsNull());

    // 6. Same acceptance through the existing IMMUTABLE MakePackageMount path
    //    (the shipped/git mount shape).
    ASSERT_TRUE(am.BeginUnregisterSource("eztree"));
    ASSERT_TRUE(am.RegisterSource(MakePackageMount("eztree", pkg.AssetsDir)));
    EXPECT_EQ(reg.GetAssetGUID(pkgTexAbs), guidOrig);
    AssetMetadata metaImmutable;
    ASSERT_TRUE(reg.TryGetAssetMetadata(guidOrig, metaImmutable));
    EXPECT_NE(metaImmutable.Path.generic_string().find("textures/leaf.png"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Item 3a: out-of-editor add to a MUTABLE package — derived GUID, metadata
// routed to the PACKAGE's own .assetdb, never the project DB (S5 routing).
// ---------------------------------------------------------------------------

TEST(PackageStoredIdentity, ExplorerAddToMutablePackageRoutesToPackageDb)
{
    StoredIdentityProject project("ge_pkg_add");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    ResolvedPackage pkg = project.MakePackage("pkg-mut");
    WriteTextFile(pkg.AssetsDir / "seed.txt", "seed");
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-mut");

    // Simulated Explorer copy after the startup scan, registered the way the
    // file watcher would register it.
    const fs::path addedAbs = pkg.AssetsDir / "textures" / "new.png";
    WriteTextFile(addedAbs, "PNGDATA");
    const auto registered = reg.RegisterAssetByPath(addedAbs);
    ASSERT_TRUE(registered.IsOk());
    const GUID derived = AssetRegistry::DeriveGuidForSourcePath("pkg-mut", "textures/new.png");
    EXPECT_EQ(registered.Value(), derived);

    // Routed to the package's own store (visible pre-flush)...
    bool foundInPackageStore = false;
    EXPECT_TRUE(reg.VisitSourceStoreRecords(
        "pkg-mut",
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.path == "textures/new.png" && rec.guid == derived)
                foundInPackageStore = true;
        }));
    EXPECT_TRUE(foundInPackageStore);

    // ...never into the project DB.
    bool leakedIntoProject = false;
    EXPECT_TRUE(reg.VisitSourceStoreRecords(
        "project",
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.path.find("new.png") != std::string::npos)
                leakedIntoProject = true;
        }));
    EXPECT_FALSE(leakedIntoProject);

    // Unmount flushes the package store; the record survives on disk in the
    // package's own .assetdb.
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-mut"));
    AssetDatabase::AssetStore_TextJsonl store;
    ASSERT_TRUE(store.LoadFromFile(pkg.RootDir / "AssetDatabase.assetdb", nullptr));
    const auto persisted = store.LookupGuidByPath("textures/new.png");
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(*persisted, derived);
}

// ---------------------------------------------------------------------------
// Item 3b: a file added to a PUBLISHED (stored-identity) package without a
// manifest entry gets the deterministic DERIVED FALLBACK GUID on the mutable
// mount; the manifest file itself is not touched; immutable mounts reject.
// ---------------------------------------------------------------------------

TEST(PackageStoredIdentity, ManifestMissingFileGetsDerivedFallbackOnMutableMountOnly)
{
    StoredIdentityProject project("ge_pkg_fallback");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    // Build a published package: derived mount → publish → remount stored.
    ResolvedPackage pkg = project.MakePackage("pkg-pub");
    WriteTextFile(pkg.AssetsDir / "seed.txt", "seed");
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-pub");
    const GUID seedGuid = reg.GetAssetGUID(pkg.AssetsDir / "seed.txt");
    ASSERT_TRUE(PublishPackageAssetManifest(reg, "pkg-pub").Success);
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-pub"));
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-pub");
    EXPECT_EQ(reg.GetAssetGUID(pkg.AssetsDir / "seed.txt"), seedGuid);

    const std::string manifestBytesBefore = ReadTextFile(pkg.AssetsDir / ".assetmanifest");

    // Add a file the manifest doesn't know about.
    const fs::path addedAbs = pkg.AssetsDir / "models" / "extra.glb";
    WriteTextFile(addedAbs, "GLBDATA");
    const auto registered = reg.RegisterAssetByPath(addedAbs);
    ASSERT_TRUE(registered.IsOk());

    // Derived FALLBACK identity: deterministic (== what a plain derived mount
    // of this alias would mint), not a random GUID::Generate.
    const GUID fallback = AssetRegistry::DeriveGuidForSourcePath("pkg-pub", "models/extra.glb");
    EXPECT_EQ(registered.Value(), fallback);

    // The fallback row lands in the package's working DB...
    bool inWorkingDb = false;
    EXPECT_TRUE(reg.VisitSourceStoreRecords(
        "pkg-pub",
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.path == "models/extra.glb" && rec.guid == fallback)
                inWorkingDb = true;
        }));
    EXPECT_TRUE(inWorkingDb);

    // ...while the published manifest is untouched (republish is explicit).
    EXPECT_EQ(ReadTextFile(pkg.AssetsDir / ".assetmanifest"), manifestBytesBefore);

    // Immutable mount of the same published package: the add is REJECTED.
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-pub"));
    ASSERT_TRUE(am.RegisterSource(MakePackageMount("pkg-pub", pkg.AssetsDir)));
    const fs::path rejectedAbs = pkg.AssetsDir / "models" / "rejected.glb";
    WriteTextFile(rejectedAbs, "GLBDATA");
    const auto rejected = reg.RegisterAssetByPath(rejectedAbs);
    ASSERT_TRUE(rejected.IsErr());
    EXPECT_EQ(rejected.Error(), AssetError::MountUnavailable);
    EXPECT_TRUE(reg.GetAssetGUID(rejectedAbs).IsNull());
}

// ---------------------------------------------------------------------------
// Item 3c: republish folds fallback-registered additions into the manifest
// with their GUIDs unchanged, and keeps remapped original identities without
// needing the remap again.
// ---------------------------------------------------------------------------

TEST(PackageStoredIdentity, RepublishFoldsAdditionsAndKeepsOriginalIdentity)
{
    StoredIdentityProject project("ge_pkg_republish");
    const fs::path projTexAbs = project.Root / "Assets" / "art" / "rock.png";
    WriteTextFile(projTexAbs, "PNGDATA");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();
    am.WaitForStartupScan("project");
    const GUID guidOrig = reg.GetAssetGUID(projTexAbs);
    ASSERT_FALSE(guidOrig.IsNull());

    // Extract into a package and publish with the remap once.
    ResolvedPackage pkg = project.MakePackage("pkg-re");
    const fs::path pkgTexAbs = pkg.AssetsDir / "art" / "rock.png";
    std::error_code ec;
    fs::create_directories(pkgTexAbs.parent_path(), ec);
    fs::rename(projTexAbs, pkgTexAbs, ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(reg.TryUnregisterAssetByPath(projTexAbs));
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-re");
    PackagePublishRemap remap;
    remap["art/rock.png"] = {"project", "art/rock.png"};
    ASSERT_TRUE(PublishPackageAssetManifest(reg, "pkg-re", remap).Success);

    // Remount stored, add a manifest-missing file (fallback identity).
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-re"));
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-re");
    ASSERT_EQ(reg.GetAssetGUID(pkgTexAbs), guidOrig);
    const fs::path addedAbs = pkg.AssetsDir / "art" / "moss.png";
    WriteTextFile(addedAbs, "PNGDATA2");
    const auto added = reg.RegisterAssetByPath(addedAbs);
    ASSERT_TRUE(added.IsOk());
    const GUID fallback = added.Value();
    EXPECT_EQ(fallback, AssetRegistry::DeriveGuidForSourcePath("pkg-re", "art/moss.png"));

    // Republish WITHOUT any remap: the merged working store already carries
    // the published identity, and the fallback addition folds in verbatim.
    const PackagePublishResult republished = PublishPackageAssetManifest(reg, "pkg-re");
    ASSERT_TRUE(republished.Success) << (republished.Errors.empty() ? "" : republished.Errors[0]);
    AssetDatabase::AssetStore_TextJsonl manifest;
    ASSERT_TRUE(manifest.LoadFromFile(republished.ManifestPath, nullptr));
    const auto rockInManifest = manifest.LookupGuidByPath("art/rock.png");
    const auto mossInManifest = manifest.LookupGuidByPath("art/moss.png");
    ASSERT_TRUE(rockInManifest.has_value());
    ASSERT_TRUE(mossInManifest.has_value());
    EXPECT_EQ(*rockInManifest, guidOrig);   // original identity survives republish
    EXPECT_EQ(*mossInManifest, fallback);   // addition folded in, GUID unchanged

    // After a remount, both files resolve to the same GUIDs they had before
    // the republish — no identity churn across publish cycles.
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-re"));
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-re");
    EXPECT_EQ(reg.GetAssetGUID(pkgTexAbs), guidOrig);
    EXPECT_EQ(reg.GetAssetGUID(addedAbs), fallback);
}

// ---------------------------------------------------------------------------
// Loud failure shapes: GUID collisions abort the publish; immutable and
// unknown mounts are rejected.
// ---------------------------------------------------------------------------

TEST(PackagePublish, CollisionsAndInvalidMountsFailLoudly)
{
    StoredIdentityProject project("ge_pkg_publish_err");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    ResolvedPackage pkg = project.MakePackage("pkg-bad");
    WriteTextFile(pkg.AssetsDir / "a.txt", "a");
    WriteTextFile(pkg.AssetsDir / "b.txt", "b");
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-bad");

    // Two package files remapped to the SAME original identity → the same
    // published GUID → loud collision, no manifest written.
    PackagePublishRemap collision;
    collision["a.txt"] = {"project", "shared/one.txt"};
    collision["b.txt"] = {"project", "shared/one.txt"};
    const PackagePublishResult failed = PublishPackageAssetManifest(reg, "pkg-bad", collision);
    EXPECT_FALSE(failed.Success);
    ASSERT_FALSE(failed.Errors.empty());
    EXPECT_NE(failed.Errors[0].find("collision"), std::string::npos);
    EXPECT_FALSE(fs::exists(pkg.AssetsDir / ".assetmanifest"));

    // A remap entry that matches nothing is surfaced as a warning.
    PackagePublishRemap typo;
    typo["does/not/exist.png"] = {"project", "whatever.png"};
    const PackagePublishResult warned = PublishPackageAssetManifest(reg, "pkg-bad", typo);
    EXPECT_TRUE(warned.Success);
    ASSERT_FALSE(warned.Warnings.empty());
    EXPECT_NE(warned.Warnings[0].find("does/not/exist.png"), std::string::npos);

    // Unknown alias fails.
    EXPECT_FALSE(PublishPackageAssetManifest(reg, "no-such-package").Success);

    // Immutable mounts are publish TARGETS, not publish sources.
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-bad"));
    ASSERT_TRUE(am.RegisterSource(MakePackageMount("pkg-bad", pkg.AssetsDir)));
    const PackagePublishResult immutable = PublishPackageAssetManifest(reg, "pkg-bad");
    EXPECT_FALSE(immutable.Success);
    ASSERT_FALSE(immutable.Errors.empty());
    EXPECT_NE(immutable.Errors[0].find("immutable"), std::string::npos);
}

// ---------------------------------------------------------------------------
// #1006: a derived mount's tombstone rests in the per-machine cache, so the
// journal flag reads false for a ghost and a filter that trusts it alone lets
// the vanished record into a manifest that is committed and shipped.
// ---------------------------------------------------------------------------

TEST(PackagePublish, VanishedRecordIsNotPublishedIntoTheManifest)
{
    StoredIdentityProject project("ge_pkg_publish_ghost");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    ResolvedPackage pkg = project.MakePackage("pkg-ghost");
    const fs::path keptAbs = pkg.AssetsDir / "textures" / "kept.png";
    const fs::path goneAbs = pkg.AssetsDir / "textures" / "gone.png";
    WriteTextFile(keptAbs, "PNGKEPT");
    WriteTextFile(goneAbs, "PNGGONE");

    // A derived mount's cache opens only once its working DB exists on disk
    // (§5-A's named residual), so a package on its very first mount has no
    // cache and nowhere to rest a tombstone. Give this one the DB a
    // previously-opened package would have, so the ghost gets its real
    // residence and the arrangement below is the filed one.
    {
        AssetDatabase::AssetStore_TextJsonl seed;
        ASSERT_TRUE(seed.SaveToFile(pkg.RootDir / "AssetDatabase.assetdb", nullptr));
    }
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-ghost");

    const GUID keptGuid = reg.GetAssetGUID(keptAbs);
    const GUID goneGuid = reg.GetAssetGUID(goneAbs);
    ASSERT_FALSE(keptGuid.IsNull());
    ASSERT_FALSE(goneGuid.IsNull());

    // The delete the watcher would see: file gone, identity kept.
    std::error_code ec;
    ASSERT_TRUE(fs::remove(goneAbs, ec));
    ASSERT_TRUE(reg.TryUnregisterAssetByPath(goneAbs));

    // Arrangement, hard-asserted: this test proves nothing unless the record
    // survives in the journal with the flag the old filter read still FALSE,
    // and the file is genuinely gone.
    bool sawGoneRecord = false;
    bool goneRecordFlaggedMissing = true;
    ASSERT_TRUE(reg.VisitSourceStoreRecords(
        "pkg-ghost",
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.path != "textures/gone.png")
                return;
            sawGoneRecord = true;
            goneRecordFlaggedMissing = rec.missing;
        }));
    ASSERT_TRUE(sawGoneRecord) << "the delete removed the journal record, so no ghost reaches publish";
    ASSERT_FALSE(goneRecordFlaggedMissing)
        << "the journal carries the tombstone, so the old journal-only filter already caught it";
    ASSERT_FALSE(fs::exists(goneAbs, ec));

    const PackagePublishResult published = PublishPackageAssetManifest(reg, "pkg-ghost");
    ASSERT_TRUE(published.Success) << (published.Errors.empty() ? "" : published.Errors[0]);
    EXPECT_EQ(published.EntryCount, 1u);
    EXPECT_EQ(published.VanishedCount, 1u);

    AssetDatabase::AssetStore_TextJsonl manifest;
    ASSERT_TRUE(manifest.LoadFromFile(published.ManifestPath, nullptr));
    EXPECT_TRUE(manifest.LookupGuidByPath("textures/kept.png").has_value())
        << "publish dropped a file that is on disk";
    EXPECT_FALSE(manifest.LookupGuidByPath("textures/gone.png").has_value())
        << "a vanished asset was published into the committed identity contract";

    // Dropping a record is never silent: the publisher has to know which
    // identities left the contract.
    bool warnedAboutGone = false;
    for (const std::string& w : published.Warnings)
    {
        if (w.find("textures/gone.png") != std::string::npos)
            warnedAboutGone = true;
    }
    EXPECT_TRUE(warnedAboutGone) << "the vanished record was dropped without saying so";

    // Residence pin, taken after the mount released the cache: the tombstone
    // lives cache-side, which is exactly why the journal flag above was false.
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-ghost"));
    AssetDatabase::AssetDbCache_Sqlite cache;
    ASSERT_TRUE(cache.Open(pkg.RootDir / ".Cache" / "AssetDatabase" / "AssetDbCache.sqlite"));
    AssetDatabase::AssetRecord cacheRec{};
    EXPECT_TRUE(cache.TryGetAsset(goneGuid, cacheRec) && cacheRec.missing)
        << "the derived mount's tombstone is not in the cache either — the ghost is flagged nowhere";
    cache.Close();
}

// A package on its first mount has no derived cache at all (§5-A's named
// residual: the cache opens only once the working DB exists), so a delete has
// NO residence to rest in — the record is flagged nowhere. Publishing has to
// drop it anyway, which is why disk, not a residence query, is the authority
// here: a both-residence surface reads false in both places for this ghost.
TEST(PackagePublish, VanishedRecordWithNoResidenceIsAlsoNotPublished)
{
    StoredIdentityProject project("ge_pkg_publish_noresidence");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    ResolvedPackage pkg = project.MakePackage("pkg-nores");
    const fs::path keptAbs = pkg.AssetsDir / "kept.txt";
    const fs::path goneAbs = pkg.AssetsDir / "gone.txt";
    WriteTextFile(keptAbs, "kept");
    WriteTextFile(goneAbs, "gone");
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-nores");

    std::error_code ec;
    ASSERT_TRUE(fs::remove(goneAbs, ec));
    ASSERT_TRUE(reg.TryUnregisterAssetByPath(goneAbs));

    // Arrangement: the record survives, unflagged, and there is no cache file
    // for a tombstone to be in.
    bool sawGoneRecord = false;
    bool goneRecordFlaggedMissing = true;
    ASSERT_TRUE(reg.VisitSourceStoreRecords(
        "pkg-nores",
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.path != "gone.txt")
                return;
            sawGoneRecord = true;
            goneRecordFlaggedMissing = rec.missing;
        }));
    ASSERT_TRUE(sawGoneRecord);
    ASSERT_FALSE(goneRecordFlaggedMissing);
    ASSERT_FALSE(fs::exists(pkg.RootDir / ".Cache" / "AssetDatabase" / "AssetDbCache.sqlite", ec))
        << "this package opened a cache, so the no-residence arrangement was not reproduced";

    const PackagePublishResult published = PublishPackageAssetManifest(reg, "pkg-nores");
    ASSERT_TRUE(published.Success) << (published.Errors.empty() ? "" : published.Errors[0]);
    EXPECT_EQ(published.EntryCount, 1u);
    EXPECT_EQ(published.VanishedCount, 1u);

    AssetDatabase::AssetStore_TextJsonl manifest;
    ASSERT_TRUE(manifest.LoadFromFile(published.ManifestPath, nullptr));
    EXPECT_TRUE(manifest.LookupGuidByPath("kept.txt").has_value());
    EXPECT_FALSE(manifest.LookupGuidByPath("gone.txt").has_value())
        << "a record no residence flags was published; only disk could have caught it";
}

// ---------------------------------------------------------------------------
// #1006's open sub-question, answered as a characterization: what happens
// downstream when a dangling entry DOES reach a manifest. Nothing complains.
// The mount-time merge skips only entries the manifest itself flags missing
// and never asks the filesystem, so a dangling entry is silently promoted to
// STORED identity in the working DB. That is why publish is the gate.
// ---------------------------------------------------------------------------

TEST(PackageStoredIdentity, DanglingManifestEntryIsMergedAtMountWithoutComplaint)
{
    StoredIdentityProject project("ge_pkg_dangling_manifest");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    ResolvedPackage pkg = project.MakePackage("pkg-dangle");
    WriteTextFile(pkg.AssetsDir / "real.txt", "real");
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-dangle");
    ASSERT_TRUE(PublishPackageAssetManifest(reg, "pkg-dangle").Success);
    ASSERT_TRUE(am.BeginUnregisterSource("pkg-dangle"));

    // Hand-write the entry publish must never emit: a record whose file is
    // not in the package tree.
    const fs::path manifestPath = pkg.AssetsDir / ".assetmanifest";
    const GUID danglingGuid("00000000-0000-4000-8000-000000001006");
    {
        AssetDatabase::AssetStore_TextJsonl manifest;
        ASSERT_TRUE(manifest.LoadFromFile(manifestPath, nullptr));
        AssetDatabase::AssetRecord dangling{};
        dangling.guid = danglingGuid;
        dangling.path = "textures/never-existed.png";
        dangling.type = AssetType::Texture;
        ASSERT_TRUE(manifest.UpsertAsset(dangling, nullptr));
        ASSERT_TRUE(manifest.SaveToFile(manifestPath, nullptr));
    }
    std::error_code ec;
    ASSERT_FALSE(fs::exists(pkg.AssetsDir / "textures" / "never-existed.png", ec));

    // The mount succeeds and the dangling identity lands in the working DB —
    // no error, no warning, no existence check anywhere on this path.
    ASSERT_TRUE(project.MountEmbedded(pkg));
    am.WaitForStartupScan("pkg-dangle");

    bool merged = false;
    ASSERT_TRUE(reg.VisitSourceStoreRecords(
        "pkg-dangle",
        [&](const AssetDatabase::AssetRecord& rec)
        {
            if (rec.guid == danglingGuid && rec.path == "textures/never-existed.png")
                merged = true;
        }));
    EXPECT_TRUE(merged)
        << "if the merge started rejecting dangling entries, publish is no longer the only gate — "
           "re-read #1006's decision before changing this expectation";
}

// ---------------------------------------------------------------------------
// Import metadata for a mounted package asset: an engine package owns its
// manifest and persists the inspector's edits into it; a cache entry and
// shipped content refuse the same write.
// ---------------------------------------------------------------------------

namespace
{

// The project's finished startup-scan future is consumed on one tick and the
// flush lands on a later one; with no job system the flush is synchronous.
void FlushDirtyStores(AssetRegistry& registry)
{
    for (int tick = 0; tick < 3; ++tick)
        registry.TickPersistence();
}

// A published single-texture package, manifest hand-written so the mount under
// test is the first one its tree ever had. Its own directory doubles as the
// tree it is authored in; the staged-copy-versus-authoring-tree split has its
// own test below.
ResolvedPackage MakePublishedTexturePackage(StoredIdentityProject& project,
                                            const std::string& alias,
                                            const std::string& relPath,
                                            GUID& outGuid)
{
    ResolvedPackage pkg = project.MakePackage(alias);
    pkg.SourceKind = PackageSourceKind::Engine;
    pkg.AuthoringRootDir = pkg.RootDir;
    WriteTextFile(pkg.AssetsDir / relPath, "PNGDATA");

    outGuid = AssetRegistry::DeriveGuidForSourcePath(alias, relPath);
    AssetDatabase::AssetStore_TextJsonl manifest;
    AssetDatabase::AssetRecord record{};
    record.guid = outGuid;
    record.path = relPath;
    record.type = AssetType::Texture;
    manifest.UpsertAsset(record, nullptr);
    manifest.SaveToFile(pkg.AssetsDir / ".assetmanifest", nullptr);
    return pkg;
}

} // namespace

TEST(PackageImportMetadata, EnginePackageMountPersistsImportSettingsIntoItsManifest)
{
    StoredIdentityProject project("ge_pkg_engine_kv");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    GUID textureGuid;
    const ResolvedPackage pkg = MakePublishedTexturePackage(
        project, "bark-pack", "textures/bark_color.png", textureGuid);
    ASSERT_FALSE(textureGuid.IsNull());
    const fs::path texture = pkg.AssetsDir / "textures" / "bark_color.png";

    ASSERT_TRUE(am.RegisterSource(MakeEnginePackageMount(pkg, project.Root / "host-cache")));
    am.WaitForStartupScan("project");

    const AssetImportSettingsOrigin origin = reg.GetImportSettingsOrigin(texture);
    EXPECT_TRUE(origin.Writable);
    // The inspector names this source in its read-only notice.
    EXPECT_EQ(origin.SourceAlias, "bark-pack");
    ASSERT_TRUE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "color"));

    std::string readBack;
    ASSERT_TRUE(reg.TryGetMetaValue(texture, kTextureUsageMetaKey, readBack));
    EXPECT_EQ(readBack, "color");

    // The package's own file on disk carries it — that file ships with the
    // package and is what the build reads back at export.
    FlushDirtyStores(reg);
    AssetDatabase::AssetStore_TextJsonl persisted;
    ASSERT_TRUE(persisted.LoadFromFile(pkg.AssetsDir / ".assetmanifest", nullptr));
    AssetDatabase::AssetRecord record{};
    ASSERT_TRUE(persisted.TryGetAsset(textureGuid, record));
    ASSERT_EQ(record.kv.count(kTextureUsageMetaKey), 1u);
    EXPECT_EQ(record.kv.at(kTextureUsageMetaKey), "color");
}

TEST(PackageImportMetadata, ImmutablePackageMountRefusesEveryMutationAndLeavesItsManifestAlone)
{
    StoredIdentityProject project("ge_pkg_immutable_kv");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    GUID textureGuid;
    const ResolvedPackage pkg = MakePublishedTexturePackage(
        project, "cached-pack", "textures/leaf_color.png", textureGuid);
    const fs::path texture = pkg.AssetsDir / "textures" / "leaf_color.png";
    const fs::path manifestPath = pkg.AssetsDir / ".assetmanifest";
    const std::string before = ReadTextFile(manifestPath);
    const fs::path added = pkg.AssetsDir / "textures" / "leaf_extra.png";
    WriteTextFile(added, "PNGDATA");

    // The shape a git cache entry and shipped Player content both mount with.
    ASSERT_TRUE(am.RegisterSource(MakePackageMount("cached-pack", pkg.AssetsDir, 25)));
    am.WaitForStartupScan("project");

    const AssetImportSettingsOrigin origin = reg.GetImportSettingsOrigin(texture);
    EXPECT_FALSE(origin.Writable);
    // Not authorable here: the inspector shows what the loaded payload holds,
    // and names this package as the place to change it.
    EXPECT_EQ(origin.SourceAlias, "cached-pack");
    EXPECT_FALSE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "color"));
    EXPECT_FALSE(reg.TryUnregisterAssetByPath(texture));
    EXPECT_FALSE(reg.TryRenameAssetPath(texture, pkg.AssetsDir / "textures" / "renamed.png"));
    EXPECT_FALSE(reg.RegisterAsset(added));

    FlushDirtyStores(reg);
    EXPECT_EQ(ReadTextFile(manifestPath), before);
}

// The staged tree a developer runs from is a copy the build overwrites from the
// repository on every build, so a write that lands there is lost and never
// reaches the package. Both trees exist here, as they do on a developer machine.
TEST(PackageImportMetadata, EnginePackageMountWritesThroughToTheAuthoringTree)
{
    StoredIdentityProject project("ge_pkg_engine_writethrough");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    GUID textureGuid;
    const ResolvedPackage authoring = MakePublishedTexturePackage(
        project, "authored-pack", "textures/bark_color.png", textureGuid);
    ASSERT_FALSE(textureGuid.IsNull());

    // What the build stages next to the executable, and what actually mounts.
    const fs::path stagedRoot = project.Root / "Staged" / "authored-pack";
    std::error_code ec;
    fs::create_directories(stagedRoot.parent_path(), ec);
    fs::copy(authoring.RootDir, stagedRoot, fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << ec.message();

    ResolvedPackage staged = authoring;
    staged.RootDir = stagedRoot;
    staged.AssetsDir = stagedRoot / "Assets";
    staged.AuthoringRootDir = authoring.RootDir;

    const fs::path stagedManifest = staged.AssetsDir / ".assetmanifest";
    const std::string stagedBefore = ReadTextFile(stagedManifest);
    ASSERT_FALSE(stagedBefore.empty());

    const AssetSourceDesc desc = MakeEnginePackageMount(staged, project.Root / "host-cache");
    EXPECT_EQ(desc.Root, staged.AssetsDir) << "the staged tree is what runs";
    EXPECT_EQ(desc.AuthoritativeDbFile, authoring.AssetsDir / ".assetmanifest");
    ASSERT_TRUE(am.RegisterSource(desc));
    am.WaitForStartupScan("project");

    const fs::path texture = staged.AssetsDir / "textures" / "bark_color.png";
    EXPECT_TRUE(reg.GetImportSettingsOrigin(texture).Writable);
    ASSERT_TRUE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "color"));
    FlushDirtyStores(reg);

    AssetDatabase::AssetStore_TextJsonl authored;
    ASSERT_TRUE(authored.LoadFromFile(authoring.AssetsDir / ".assetmanifest", nullptr));
    AssetDatabase::AssetRecord record{};
    ASSERT_TRUE(authored.TryGetAsset(textureGuid, record));
    ASSERT_EQ(record.kv.count(kTextureUsageMetaKey), 1u);
    EXPECT_EQ(record.kv.at(kTextureUsageMetaKey), "color");

    // The staged copy is build output: the next build refreshes it from the
    // tree above, so nothing writes it here.
    EXPECT_EQ(ReadTextFile(stagedManifest), stagedBefore);
}

// A staged tree with no authoring tree behind it — a shipped game, or an engine
// build copied away from the checkout that produced it — keeps shipped content's
// shape and refuses the write with the reason.
TEST(PackageImportMetadata, StagedEnginePackageWithNoAuthoringTreeIsReadOnly)
{
    StoredIdentityProject project("ge_pkg_engine_shipped");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    GUID textureGuid;
    ResolvedPackage pkg = MakePublishedTexturePackage(
        project, "shipped-pack", "textures/bark_color.png", textureGuid);
    pkg.AuthoringRootDir.clear();

    const fs::path manifestPath = pkg.AssetsDir / ".assetmanifest";
    const std::string before = ReadTextFile(manifestPath);

    const AssetSourceDesc desc = MakeEnginePackageMount(pkg, project.Root / "host-cache");
    EXPECT_FALSE(desc.AcceptsMetadataWrites);
    EXPECT_TRUE(desc.IsReadOnly);
    EXPECT_EQ(desc.AuthoritativeDbFile, manifestPath);
    ASSERT_TRUE(am.RegisterSource(desc));
    am.WaitForStartupScan("project");

    const fs::path texture = pkg.AssetsDir / "textures" / "bark_color.png";
    EXPECT_FALSE(reg.GetImportSettingsOrigin(texture).Writable);
    EXPECT_FALSE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "color"));
    FlushDirtyStores(reg);
    EXPECT_EQ(ReadTextFile(manifestPath), before);
}

// The grant an engine package gets is for import settings ONLY. Its asset set
// is fixed by the manifest, and a stored-identity row is unrecreatable
// (AssetRegistry.cpp calls it authoritative for exactly that reason), so every
// structural mutation the Project browser can reach — delete
// (AssetsBrowserController -> DeleteAssetPathsCommand -> TryUnregisterAssetByPath,
// which on a mount that tracks no tombstones would REMOVE the row rather than
// mark it missing), rename, and add — must still be refused.
TEST(PackageImportMetadata, EnginePackageMountRefusesStructuralMutationsAndKeepsItsRows)
{
    StoredIdentityProject project("ge_pkg_engine_structure");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    GUID textureGuid;
    const ResolvedPackage pkg = MakePublishedTexturePackage(
        project, "bark-pack", "textures/bark_color.png", textureGuid);
    const fs::path texture = pkg.AssetsDir / "textures" / "bark_color.png";
    const fs::path added = pkg.AssetsDir / "textures" / "bark_extra.png";
    WriteTextFile(added, "PNGDATA");

    ASSERT_TRUE(am.RegisterSource(MakeEnginePackageMount(pkg, project.Root / "host-cache")));
    am.WaitForStartupScan("project");

    // The one mutation the shape grants.
    ASSERT_TRUE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "color"));

    EXPECT_FALSE(reg.TryUnregisterAssetByPath(texture));
    EXPECT_FALSE(reg.TryRenameAssetPath(texture, pkg.AssetsDir / "textures" / "renamed.png"));
    EXPECT_FALSE(reg.RegisterAsset(added));
    EXPECT_EQ(reg.GetAssetGUID(texture), textureGuid);

    // The published row is still on disk, still at its published path, and the
    // refused add minted nothing into the package's identity contract.
    FlushDirtyStores(reg);
    AssetDatabase::AssetStore_TextJsonl persisted;
    ASSERT_TRUE(persisted.LoadFromFile(pkg.AssetsDir / ".assetmanifest", nullptr));
    AssetDatabase::AssetRecord record{};
    ASSERT_TRUE(persisted.TryGetAsset(textureGuid, record));
    EXPECT_EQ(record.path, "textures/bark_color.png");
    EXPECT_EQ(persisted.CountAssets(), 1u);
}

// The grant is for AUTHORED settings, not inferred ones. The render path tags a
// data texture's colour space and widens its cook usage when a material binds it
// (TextureService -> EnsureTextureColorSpaceTagged / EnsureTextureCookUsageTagged
// -> SetMetaValue), gated only on the mount's InfersTextureImportSettings. A
// package manifest is committed with the package, so opening a scene that draws
// one of its meshes — or running a developer Player — must not edit it: the gate
// is closed, and the rows a bind would infer are authored rows already.
TEST(PackageImportMetadata, EnginePackageMountInfersNothingSoAMaterialBindLeavesItAlone)
{
    StoredIdentityProject project("ge_pkg_engine_nobindwrite");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();

    GUID textureGuid;
    const ResolvedPackage pkg = MakePublishedTexturePackage(
        project, "bark-pack", "textures/bark_normal.png", textureGuid);
    const fs::path texture = pkg.AssetsDir / "textures" / "bark_normal.png";
    const fs::path manifestPath = pkg.AssetsDir / ".assetmanifest";

    ASSERT_TRUE(am.RegisterSource(MakeEnginePackageMount(pkg, project.Root / "host-cache")));
    am.WaitForStartupScan("project");
    const std::string before = ReadTextFile(manifestPath);
    ASSERT_FALSE(before.empty());

    // Exactly what a bind does: ask the gate, then write only if it opens.
    const bool infers = reg.GetDerivedArtifactPolicy(textureGuid).InfersTextureImportSettings;
    EXPECT_FALSE(infers) << "a material bind would author the package's committed manifest";
    if (infers)
    {
        EXPECT_TRUE(reg.SetMetaValue(texture, kTextureColorSpaceMetaKey, "linear"));
        EXPECT_TRUE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "normal"));
    }
    FlushDirtyStores(reg);
    EXPECT_EQ(ReadTextFile(manifestPath), before);

    // The inspector's write still lands: the mount is writable, it just infers
    // nothing — that is the distinction this shape rests on.
    EXPECT_TRUE(reg.GetImportSettingsOrigin(texture).Writable);
    ASSERT_TRUE(reg.SetMetaValue(texture, kTextureUsageMetaKey, "normal"));
    FlushDirtyStores(reg);
    EXPECT_NE(ReadTextFile(manifestPath), before);
}

// ---------------------------------------------------------------------------
// Acceptance (Arc 3a): the REAL extracted eztree engine package. Its committed
// .assetmanifest must carry the pre-extraction editor-mount GUIDs so existing
// scene references keep resolving, and must stay in sync with the package tree
// (a republish over an unchanged tree is byte-identical).
// The package is staged next to the test executable (ge_stage_packages)
// — tests consume staged content, never the source tree.
// ---------------------------------------------------------------------------

namespace
{

fs::path StagedEZTreePackageDir()
{
    // TestData/, not the exe-dir Packages/ the resolver implicitly
    // scans — the shared Tests output dir must stay out of the default scan.
    return PathUtils::GetExecutableDirectory() / "TestData" / "Packages" / "eztree";
}

} // namespace

TEST(PackageStoredIdentity, EZTreeEnginePackage_OriginalEditorGuidsResolve)
{
    const fs::path staged = StagedEZTreePackageDir();
    ASSERT_TRUE(fs::is_directory(staged)) << staged << " missing — Packages not staged";
    const fs::path assetsDir = staged / "Assets";
    ASSERT_TRUE(fs::exists(assetsDir / ".assetmanifest"));

    StoredIdentityProject project("ge_eztree_accept");
    ASSERT_TRUE(project.Initialize());
    auto& am = project.Manager;
    auto& reg = am.GetRegistry();
    ASSERT_TRUE(am.RegisterSource(MakePackageMount("eztree", assetsDir)));

    // Every moved asset keeps its pre-extraction editor-mount identity:
    // GUID = derived("editor", <original editor-relative path>). The relative
    // paths did not change in the move, so original == package-relative.
    const std::pair<const char*, fs::path> continuity[] = {
        {"Materials/EZTree/EZTree_Bark.material",
         assetsDir / "Materials" / "EZTree" / "EZTree_Bark.material"},
        {"Materials/EZTree/EZTree_Leaves.material",
         assetsDir / "Materials" / "EZTree" / "EZTree_Leaves.material"},
        {"Materials/EZTree/EZTree_Trellis.material",
         assetsDir / "Materials" / "EZTree" / "EZTree_Trellis.material"},
        {"Textures/EZTree/leaves/oak_color.png",
         assetsDir / "Textures" / "EZTree" / "leaves" / "oak_color.png"},
        {"Textures/EZTree/bark/oak_color_1k.jpg",
         assetsDir / "Textures" / "EZTree" / "bark" / "oak_color_1k.jpg"},
        {"Shaders/VertexModifiers/ez_tree_wind.glsl",
         assetsDir / "Shaders" / "VertexModifiers" / "ez_tree_wind.glsl"},
        {"Shaders/Surfaces/ez_tree_leaves.glsl",
         assetsDir / "Shaders" / "Surfaces" / "ez_tree_leaves.glsl"},
    };
    for (const auto& [originalRel, packagePath] : continuity)
    {
        const GUID original = AssetRegistry::DeriveGuidForSourcePath("editor", originalRel);
        ASSERT_FALSE(original.IsNull());
        EXPECT_EQ(reg.GetAssetGUID(packagePath), original)
            << originalRel << " lost its pre-extraction identity";
        AssetMetadata meta;
        ASSERT_TRUE(reg.TryGetAssetMetadata(original, meta))
            << originalRel << " does not resolve by its original GUID";
    }
}

TEST(PackageStoredIdentity, EZTreeEnginePackage_RepublishIsByteIdentical)
{
    const fs::path staged = StagedEZTreePackageDir();
    ASSERT_TRUE(fs::is_directory(staged)) << staged << " missing — Packages not staged";

    // Republish from a scratch copy: the manifest must reflect the CURRENT
    // package tree exactly. A file added/removed/moved without republishing
    // (or identity drift of any kind) breaks byte equality here.
    StoredIdentityProject project("ge_eztree_repub");
    ASSERT_TRUE(project.Initialize());
    const fs::path pkgRoot = project.Root / "Packages" / "eztree";
    std::error_code ec;
    fs::create_directories(pkgRoot.parent_path(), ec);
    fs::copy(staged, pkgRoot, fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << ec.message();

    ResolvedPackage pkg;
    pkg.RootDir = pkgRoot;
    pkg.AssetsDir = pkgRoot / "Assets";
    pkg.Alias = "eztree";
    pkg.Priority = 25;
    pkg.SourceKind = PackageSourceKind::Embedded;

    auto& am = project.Manager;
    ASSERT_TRUE(project.MountEmbedded(pkg)); // manifest present -> mutable stored identity
    am.WaitForStartupScan("eztree");

    const std::string committedBytes = ReadTextFile(pkg.AssetsDir / ".assetmanifest");
    ASSERT_FALSE(committedBytes.empty());

    const PackagePublishResult republished =
        PublishPackageAssetManifest(am.GetRegistry(), "eztree");
    ASSERT_TRUE(republished.Success)
        << (republished.Errors.empty() ? "" : republished.Errors[0]);
    const std::string republishedBytes = ReadTextFile(republished.ManifestPath);
    if (republishedBytes != committedBytes)
    {
        // Drop the correct bytes next to the staged manifest so a content move
        // can adopt them without hand-editing (hand edits are what drift).
        const fs::path regenerated = staged / "Assets" / ".assetmanifest.regenerated";
        std::ofstream out(regenerated, std::ios::binary | std::ios::trunc);
        out << republishedBytes;
        ADD_FAILURE() << "Packages/eztree/Assets/.assetmanifest is stale — adopt the "
                         "regenerated copy written to "
                      << regenerated << " and commit it";
    }
}
