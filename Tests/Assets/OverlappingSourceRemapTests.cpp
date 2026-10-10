// S9-E5: overlapping-source policy.
//
// 1. RejectOverlappingRoots: package mounts refuse to register when their
//    root nests with an existing source root (either direction).
// 2. GuidRemapped propagation: when an allowed overlap (project/editor
//    legacy) remaps a path's GUID in place — RegisterAsset(path, alias) with
//    a derived-identity source over an already-registered project path — the
//    AssetManager atomically re-keys its loaded-asset and in-flight maps, so
//    a load started under the old GUID is still reachable under the new one.
//    This is the shape of the historical "overlapping-source load flake".
// 3. Alias retirement: once a remapped-away GUID is claimed again by a new
//    file at the old path, loads of that GUID land under it — never under
//    the migrated asset's key. The registry is the single owner of the
//    session alias, and a registration retires it.
//
// The remap hammer loops register-while-loading enough times to have caught
// the pre-fix behavior deterministically (the old code lost the loaded map
// entry on every iteration; one iteration suffices to fail, N adds margin).

#include "AssetDatabase/AssetDatabasePaths.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ParserRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>

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

// A parser failure keeps this test about completion bookkeeping: no loaded-map
// registration can obscure which in-flight generation the completion releases.
class HeldRemapParser : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::Unknown; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".bin"}; }
    int GetPriority() const override { return 1000; }
    std::string GetName() const override { return "HeldRemapParser"; }
    AssetParseResult Parse(const AssetMetadata&, AssetManager&) override
    {
        const unsigned index = Next.fetch_add(1);
        if (index >= Entered.size())
            return {false, "Unexpected extra decode"};
        Entered[index].set_value();
        Gates[index].wait();
        return {false, "Intentional held decode failure"};
    }
    std::array<std::promise<void>, 2> Entered;
    std::array<std::shared_future<void>, 2> Gates;
    std::atomic<unsigned> Next{0};
};

} // namespace

TEST(OverlappingSourceRemap, PackageMountRejectsNestedRoots)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_s9_overlap_reject");
    const fs::path projectRoot = tmp / "project";
    const fs::path nestedRoot = projectRoot / "packages" / "veg";
    WriteTextFile(projectRoot / "a.txt", "a");
    WriteTextFile(nestedRoot / "b.txt", "b");
    WriteTextFile(nestedRoot / ".assetmanifest", "{\"format\":\"assetdb\",\"version\":2}\n");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));

    AssetSourceDesc project{};
    project.Alias = "project";
    project.Root = projectRoot;
    project.DerivedIdentity = true;
    project.Priority = 100;
    ASSERT_TRUE(reg.RegisterSource(project));

    // Package root nested INSIDE an existing source root: rejected.
    EXPECT_FALSE(reg.RegisterSource(MakePackageMount("veg", nestedRoot)));

    // Package root that CONTAINS an existing source root: rejected too.
    WriteTextFile(tmp / ".assetmanifest", "{\"format\":\"assetdb\",\"version\":2}\n");
    EXPECT_FALSE(reg.RegisterSource(MakePackageMount("outer", tmp)));

    // Disjoint package root: accepted.
    const fs::path disjoint = tmp / "disjoint-pkg";
    WriteTextFile(disjoint / ".assetmanifest", "{\"format\":\"assetdb\",\"version\":2}\n");
    EXPECT_TRUE(reg.RegisterSource(MakePackageMount("okpkg", disjoint)));

    reg.Shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST(OverlappingSourceRemap, LegacyOverlapStillRegisters)
{
    // The default no---project editor run mounts project and editor over the
    // same root. That legacy overlap must keep working (RejectOverlappingRoots
    // defaults to false for plain sources).
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_s9_overlap_legacy");
    const fs::path root = tmp / "shared";
    WriteTextFile(root / "style.css", ".a{}");

    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));

    AssetSourceDesc project{};
    project.Alias = "project";
    project.Root = root;
    project.DerivedIdentity = true;
    project.Priority = 100;
    ASSERT_TRUE(reg.RegisterSource(project));

    AssetSourceDesc editor{};
    editor.Alias = "editor";
    editor.Root = root;
    editor.DerivedIdentity = true;
    editor.Priority = 50;
    EXPECT_TRUE(reg.RegisterSource(editor));

    reg.Shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST(OverlappingSourceRemap, RegisterWhileLoadingRekeysLoadedAndInFlight)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_s9_remap_hammer");
    const fs::path root = tmp / "shared";

    JobSystem::WorkStealingThreadPool pool(4);
    AssetManager manager;
    ASSERT_TRUE(manager.Initialize(&pool));

    AssetSourceDesc project{};
    project.Alias = "project";
    project.Root = root;
    project.DerivedIdentity = true;
    project.Priority = 100;
    ASSERT_TRUE(manager.RegisterSource(project));

    AssetSourceDesc editor{};
    editor.Alias = "editorish";
    editor.Root = root;
    editor.DerivedIdentity = true;
    editor.Priority = 50;
    ASSERT_TRUE(manager.RegisterSource(editor));

    AssetRegistry& reg = manager.GetRegistry();

    constexpr int kIterations = 50;
    for (int i = 0; i < kIterations; ++i)
    {
        const fs::path file = root / ("hammer" + std::to_string(i) + ".xml");
        WriteTextFile(file, "<UI><Panel/></UI>");

        // Register under the project first (project-namespace GUID)...
        ASSERT_TRUE(reg.RegisterAsset(file));
        const GUID projectGuid = reg.GetAssetGUID(file);
        ASSERT_FALSE(projectGuid.IsNull());

        // ...start a load under that GUID...
        auto future = manager.LoadAssetAsync(projectGuid, AssetLoadPriority::High);

        // ...then remap the path to the overlapping source's derived identity
        // while the load is (potentially) in flight. This is the flake shape.
        ASSERT_TRUE(reg.RegisterAsset(file, "editorish"));
        const GUID remappedGuid = reg.GetAssetGUID(file);
        ASSERT_FALSE(remappedGuid.IsNull());
        ASSERT_NE(remappedGuid, projectGuid) << "remap should assign the derived GUID";

        // The original load must resolve (success or failure, not hang)...
        SharedPtr<Asset> loaded = future.get();

        // ...and when it loaded an asset, that asset must be reachable under
        // the REMAPPED GUID — the manager re-keys m_LoadedAssets atomically
        // on GuidRemapped, so the registry and the loaded-map agree.
        if (loaded)
        {
            EXPECT_TRUE(manager.GetAsset(remappedGuid) != nullptr)
                << "iteration " << i << ": loaded asset lost by remap (old flake)";
            EXPECT_TRUE(manager.IsAssetLoaded(remappedGuid));
        }
    }

    // No in-flight entries may leak: every load resolved and erased itself
    // even if a remap changed the entry's key.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (manager.GetInFlightLoadCount() != 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(manager.GetInFlightLoadCount(), 0u) << "in-flight entries leaked across remaps";

    manager.Shutdown();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

TEST(OverlappingSourceRemap, ReclaimedGuidDoesNotStrandTheRemappedLoadGeneration)
{
    TestUtils::ScopedTempDir temporary(TestUtils::MakeUniqueTempDirectory("ge_remap_generation"));
    const fs::path root = temporary.Path() / "project";
    const fs::path originalPath = root / "original.bin";
    const fs::path renamedPath = root / "renamed.bin";
    WriteTextFile(originalPath, "first generation");

    JobSystem::WorkStealingThreadPool pool(4);
    AssetManager manager;
    ASSERT_TRUE(manager.Initialize(&pool));
    AssetSourceDesc project{};
    project.Alias = "project";
    project.Root = root;
    project.DerivedIdentity = true;
    project.RequiresScan = false; // This test explicitly owns both registrations.
    ASSERT_TRUE(manager.RegisterSource(project));
    auto& registry = manager.GetRegistry();
    ASSERT_TRUE(registry.RegisterAsset(originalPath));
    const GUID originalGuid = registry.GetAssetGUID(originalPath);
    ASSERT_FALSE(originalGuid.IsNull());

    // These promises die before manager on an early assertion exit. A broken
    // promise makes wait() ready too, so a held worker cannot strand teardown.
    std::array<std::promise<void>, 2> release;
    auto parser = std::make_shared<HeldRemapParser>();
    parser->Gates[0] = release[0].get_future().share();
    parser->Gates[1] = release[1].get_future().share();
    auto firstEntered = parser->Entered[0].get_future();
    auto secondEntered = parser->Entered[1].get_future();
    ASSERT_TRUE(manager.GetParserRegistry().RegisterParser(parser, parser->GetPriority()));
    auto first = manager.LoadAssetAsync(originalGuid, AssetLoadPriority::High);
    ASSERT_EQ(firstEntered.wait_for(std::chrono::seconds(10)), std::future_status::ready);

    std::error_code renameError;
    fs::rename(originalPath, renamedPath, renameError);
    ASSERT_FALSE(renameError) << renameError.message();
    ASSERT_TRUE(manager.RenameAssetPath(originalPath, renamedPath));
    ASSERT_TRUE(registry.RegisterAsset(renamedPath, "project"));
    const GUID remappedGuid = registry.GetAssetGUID(renamedPath);
    ASSERT_NE(remappedGuid, originalGuid);
    ASSERT_EQ(registry.ResolveSessionAlias(originalGuid), remappedGuid);
    auto joined = std::make_shared<std::promise<bool>>();
    auto callback = joined->get_future();
    manager.LoadAsset(remappedGuid, [joined](Result<SharedPtr<Asset>, AssetError> result)
    {
        joined->set_value(!result.IsOk());
    }, AssetLoadPriority::High);

    WriteTextFile(originalPath, "replacement generation");
    ASSERT_TRUE(registry.RegisterAsset(originalPath));
    ASSERT_EQ(registry.GetAssetGUID(originalPath), originalGuid);
    ASSERT_EQ(registry.ResolveSessionAlias(originalGuid), originalGuid);
    auto replacement = manager.LoadAssetAsync(originalGuid, AssetLoadPriority::High);
    ASSERT_EQ(secondEntered.wait_for(std::chrono::seconds(10)), std::future_status::ready);

    release[0].set_value();
    ASSERT_EQ(first.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(first.get(), nullptr);
    EXPECT_EQ(manager.GetInFlightLoadCount(), 1u) << "only the held replacement may remain";
    EXPECT_EQ(replacement.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    const auto callbackStatus = callback.wait_for(std::chrono::seconds(10));
    EXPECT_EQ(callbackStatus, std::future_status::ready) << "the remapped generation lost its callback";
    if (callbackStatus == std::future_status::ready)
        EXPECT_TRUE(callback.get());

    release[1].set_value();
    ASSERT_EQ(replacement.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(replacement.get(), nullptr);
    EXPECT_EQ(manager.GetInFlightLoadCount(), 0u);
    manager.Shutdown();
}

// Create -> Surface Shader, rename the pair to Rock, Create -> Surface Shader
// again. The rename keeps Rock.material's GUID at its new path; the watcher's
// aliased re-registration migrates it onto Rock.material's own path-derived
// GUID and aliases the old one to it. The new NewSurface.material then claims
// that old GUID as a primary. Each material must load as its own instance:
// resolving the reclaimed GUID through the stale alias stored the fresh
// document under Rock's key, so the inspector showed NewSurface for Rock and
// edits to either file surfaced on the other.
TEST(OverlappingSourceRemap, RecreatingARenamedAwayPathLoadsItsOwnInstance)
{
    const fs::path tmp = TestUtils::MakeUniqueTempDirectory("ge_s9_remap_recreate");
    const fs::path root = tmp / "project";
    const fs::path oldPath = root / "NewSurface.material";
    const fs::path newPath = root / "Rock.material";
    const auto materialText = [](const char* name) {
        return std::string("{\n  \"schemaVersion\": 3,\n  \"materialName\": \"") + name +
               "\",\n  \"properties\": {},\n  \"textures\": {}\n}\n";
    };
    WriteTextFile(oldPath, materialText("NewSurface"));

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager manager;
    ASSERT_TRUE(manager.Initialize(&pool));

    // A store-backed derived mount: the live rename journals its redirect
    // there, and the recreate retires it — the editor's project shape.
    const AssetDatabase::AssetDatabasePaths dbPaths =
        AssetDatabase::GetDefaultPathsForAssetRoot(root, {}, {});
    AssetSourceDesc project{};
    project.Alias = "project";
    project.Root = root;
    project.DerivedIdentity = true;
    project.AuthoritativeDbFile = dbPaths.authoritativeFile;
    project.CacheRoot = dbPaths.cacheRoot;
    project.Priority = 100;
    ASSERT_TRUE(manager.RegisterSource(project));
    AssetRegistry& reg = manager.GetRegistry();

    ASSERT_TRUE(reg.RegisterAsset(oldPath));
    const GUID oldGuid = reg.GetAssetGUID(oldPath);
    ASSERT_FALSE(oldGuid.IsNull());
    SharedPtr<Asset> renamed = manager.LoadAssetAsync(oldGuid, AssetLoadPriority::High).get();
    ASSERT_TRUE(renamed);

    // Inline rename (RenameAssetFileCommand) then the watcher leg's aliased
    // re-registration of the new path.
    std::error_code ec;
    fs::rename(oldPath, newPath, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(manager.RenameAssetPath(oldPath, newPath));
    ASSERT_TRUE(reg.RegisterAsset(newPath, "project"));
    const GUID renamedGuid = reg.GetAssetGUID(newPath);
    ASSERT_FALSE(renamedGuid.IsNull());
    ASSERT_NE(renamedGuid, oldGuid) << "the aliased re-registration must migrate to the derived GUID";
    EXPECT_EQ(manager.GetAsset(renamedGuid).get(), renamed.get()) << "the remap re-keys the loaded instance";
    EXPECT_EQ(reg.ResolveSessionAlias(oldGuid), renamedGuid);

    // Create -> Surface Shader again: a new file claims the old path, and
    // with it the old GUID.
    WriteTextFile(oldPath, materialText("NewSurface"));
    ASSERT_TRUE(reg.RegisterAsset(oldPath));
    ASSERT_EQ(reg.GetAssetGUID(oldPath), oldGuid);
    EXPECT_EQ(reg.ResolveSessionAlias(oldGuid), oldGuid) << "a registration retires the alias";

    SharedPtr<Asset> fresh = manager.LoadAssetAsync(oldGuid, AssetLoadPriority::High).get();
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh.get(), renamed.get()) << "the reclaimed GUID must load its own instance";
    EXPECT_EQ(manager.GetAsset(oldGuid).get(), fresh.get());
    EXPECT_EQ(manager.GetAsset(renamedGuid).get(), renamed.get())
        << "the fresh document must not replace the renamed asset's instance";
    // Each instance carries the registry's path for its own GUID (the
    // registry's normalized form, case-folded on case-insensitive filesystems).
    AssetMetadata freshMeta{};
    AssetMetadata renamedMeta{};
    ASSERT_TRUE(reg.TryGetAssetMetadata(oldGuid, freshMeta));
    ASSERT_TRUE(reg.TryGetAssetMetadata(renamedGuid, renamedMeta));
    EXPECT_EQ(fresh->GetPath(), freshMeta.Path);
    EXPECT_EQ(renamed->GetPath(), renamedMeta.Path);
    EXPECT_NE(fresh->GetPath(), renamed->GetPath());

    auto* freshMaterial = dynamic_cast<MaterialAsset*>(fresh.get());
    auto* renamedMaterial = dynamic_cast<MaterialAsset*>(renamed.get());
    ASSERT_TRUE(freshMaterial);
    ASSERT_TRUE(renamedMaterial);
    EXPECT_EQ(freshMaterial->GetDocument().materialName, "NewSurface");
    EXPECT_EQ(renamedMaterial->GetDocument().materialName, "NewSurface")
        << "the renamed asset keeps the document it was loaded with";

    // A disk edit of Rock.material reaches Rock only.
    WriteTextFile(newPath, materialText("Rock"));
    ASSERT_EQ(manager.ReloadAssetNow(renamedGuid), ReloadOutcome::Reloaded);
    EXPECT_EQ(renamedMaterial->GetDocument().materialName, "Rock");
    EXPECT_EQ(freshMaterial->GetDocument().materialName, "NewSurface");

    manager.Shutdown();
    fs::remove_all(tmp, ec);
}
