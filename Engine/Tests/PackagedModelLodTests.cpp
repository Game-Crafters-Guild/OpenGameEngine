#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/MeshLODCache.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "Assets/Packages/PackageMounts.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{
using InstallTree = std::map<fs::path, std::pair<fs::file_time_type, std::string>>;

InstallTree ReadInstallTree(const fs::path& root)
{
    InstallTree result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        std::string bytes;
        if (entry.is_regular_file())
        {
            std::ifstream file(entry.path(), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        // Directory timestamps can lag completed child creation on Windows;
        // directory membership and each file's bytes and timestamp define the snapshot.
        const auto modified = entry.is_regular_file() ? entry.last_write_time() : fs::file_time_type{};
        result.emplace(entry.path().lexically_relative(root),
                       std::make_pair(modified, std::move(bytes)));
    }
    return result;
}

class PackagedModelLodTest : public testing::Test
{
protected:
    void SetUp() override
    {
        PreviousSettings = GetLODImportSettings();
        LODImportSettings settings;
        settings.AutoGenerateOnImport = true;
        SetLODImportSettings(settings);
        fs::create_directories(InstallRoot / "Assets");
        std::ofstream(ModelPath) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        std::ofstream(InstallRoot / "Assets/.assetmanifest")
            << R"({"format":"assetmanifest","version":2})" << '\n'
            << "{\"guid\":\"" << ModelGuid.ToString()
            << R"(","path":"triangle.obj","type":"Model"})" << '\n';
    }

    void TearDown() override
    {
        SetLODImportSettings(PreviousSettings);
    }

    TestUtils::ScopedTempDir TemporaryDirectory{TestUtils::MakeUniqueTempDirectory("packaged_model_lod")};
    const fs::path InstallRoot = TemporaryDirectory.Path() / "Install";
    const fs::path ModelPath = InstallRoot / "Assets/triangle.obj";
    const GUID ModelGuid{"22a0544a-5a86-43ea-b8dc-7fbe90aa4592"};
    LODImportSettings PreviousSettings;
};
} // namespace

TEST_F(PackagedModelLodTest, ShippedCookIsLoadedWithoutWritingIntoInstall)
{
    ModelAsset source(ModelGuid, ModelPath);
    ASSERT_TRUE(source.Load());
    Vector<Mesh> cookedMeshes = source.GetMeshes();
    ASSERT_EQ(cookedMeshes.size(), 1u);
    cookedMeshes.front().ExtraLODs = {{0, 1, 2}};
    cookedMeshes.front().ExtraLODErrors = {0.125f};
    cookedMeshes.front().ExtraLODSloppy = {0};
    const fs::path cookedPath = InstallRoot / "Assets/.lod" / (ModelGuid.ToString() + ".gelod");
    ASSERT_TRUE(WriteLodCache(cookedPath, {17, 29}, ComputeGeneratedLodHash(cookedMeshes), cookedMeshes));
    const auto shipped = ReadInstallTree(InstallRoot);

    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(InstallRoot / "Assets", nullptr,
                                 TemporaryDirectory.Path() / "UserData/AssetDatabase.assetdb",
                                 TemporaryDirectory.Path() / "UserData/.Cache/AssetDatabase"));
    ASSERT_EQ(assets.GetRegistry().GetAssetGUID(ModelPath), ModelGuid);
    ASSERT_FALSE(assets.GetRegistry().AcceptsDerivedRecords(ModelPath));
    // A manifest still exposes a cache-root path; write policy decides its use.
    ASSERT_TRUE(assets.GetRegistry().TryGetCacheRoot(ModelPath).has_value());
    {
        AssetManager::ScopedThreadAssetManager context(&assets);
        ModelAsset loaded(ModelGuid, ModelPath);
        ASSERT_TRUE(loaded.Load());
        loaded.PostLoad();
        ASSERT_EQ(loaded.GetMeshes().size(), 1u);
        EXPECT_EQ(loaded.GetMeshes().front().ExtraLODs, cookedMeshes.front().ExtraLODs);
        EXPECT_EQ(loaded.GetMeshes().front().ExtraLODErrors, cookedMeshes.front().ExtraLODErrors);
    }
    assets.Shutdown();
    EXPECT_EQ(ReadInstallTree(InstallRoot), shipped);
}

TEST_F(PackagedModelLodTest, MissingCookDoesNotCreateAnInstallCache)
{
    const auto shipped = ReadInstallTree(InstallRoot);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(InstallRoot / "Assets", nullptr,
                                 TemporaryDirectory.Path() / "UserData/AssetDatabase.assetdb",
                                 TemporaryDirectory.Path() / "UserData/.Cache/AssetDatabase"));
    {
        AssetManager::ScopedThreadAssetManager context(&assets);
        ModelAsset loaded(ModelGuid, ModelPath);
        ASSERT_TRUE(loaded.Load());
        loaded.PostLoad();
    }
    assets.Shutdown();
    EXPECT_EQ(ReadInstallTree(InstallRoot), shipped);
}

TEST_F(PackagedModelLodTest, EditorProjectWritesAndReusesItsDerivedCache)
{
    ASSERT_TRUE(fs::remove(InstallRoot / "Assets/.assetmanifest"));
    const auto sourceTree = ReadInstallTree(InstallRoot);
    const fs::path hostCache = TemporaryDirectory.Path() / "HostCache";
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(InstallRoot / "Assets", nullptr,
                                 TemporaryDirectory.Path() / "UserData/AssetDatabase.assetdb",
                                 hostCache / "AssetDatabase"));
    const GUID modelGuid = assets.GetRegistry().GetAssetGUID(ModelPath);
    ASSERT_FALSE(modelGuid.IsNull());
    AssetManager::ScopedThreadAssetManager context(&assets);
    {
        ModelAsset loaded(modelGuid, ModelPath);
        ASSERT_TRUE(loaded.Load());
        loaded.PostLoad();
    }
    ASSERT_TRUE(fs::exists(hostCache / "Lod"));
    const auto generated = ReadInstallTree(hostCache / "Lod");
    ASSERT_EQ(generated.size(), 1u);
    const fs::path cacheFile = hostCache / "Lod" / generated.begin()->first;
    ASSERT_EQ(cacheFile.extension(), ".gelod");
    // A distinct timestamp makes a rewrite observable even on a coarse file clock.
    fs::last_write_time(cacheFile, fs::file_time_type::clock::now() - std::chrono::hours(24));
    const auto cached = ReadInstallTree(hostCache / "Lod");
    {
        ModelAsset loaded(modelGuid, ModelPath);
        ASSERT_TRUE(loaded.Load());
        loaded.PostLoad();
    }
    EXPECT_EQ(ReadInstallTree(hostCache / "Lod"), cached);
    assets.Shutdown();
    EXPECT_EQ(ReadInstallTree(InstallRoot), sourceTree);
}

TEST_F(PackagedModelLodTest, GitPackageWritesAndReusesItsHostOwnedCache)
{
    const auto packageTree = ReadInstallTree(InstallRoot);
    const fs::path hostCache = TemporaryDirectory.Path() / "HostCache";
    const fs::path projectAssets = TemporaryDirectory.Path() / "Project/Assets";
    fs::create_directories(projectAssets);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(projectAssets, nullptr,
                                 TemporaryDirectory.Path() / "UserData/AssetDatabase.assetdb",
                                 hostCache / "AssetDatabase"));
    ResolvedPackage package;
    package.Alias = "model-package";
    package.SourceKind = PackageSourceKind::Git;
    package.RootDir = InstallRoot;
    package.AssetsDir = InstallRoot / "Assets";
    ASSERT_TRUE(assets.RegisterSource(MakeGitPackageMount(package, hostCache)));
    ASSERT_EQ(assets.GetRegistry().GetAssetGUID(ModelPath), ModelGuid);
    ASSERT_FALSE(assets.GetRegistry().AcceptsDerivedRecords(ModelPath));
    AssetManager::ScopedThreadAssetManager context(&assets);
    {
        ModelAsset loaded(ModelGuid, ModelPath);
        ASSERT_TRUE(loaded.Load());
        loaded.PostLoad();
    }
    const fs::path lodDirectory = hostCache / "Packages/model-package/Lod";
    ASSERT_TRUE(fs::exists(lodDirectory));
    const auto generated = ReadInstallTree(lodDirectory);
    ASSERT_EQ(generated.size(), 1u);
    const fs::path cacheFile = lodDirectory / generated.begin()->first;
    ASSERT_EQ(cacheFile.extension(), ".gelod");
    fs::last_write_time(cacheFile, fs::file_time_type::clock::now() - std::chrono::hours(24));
    const auto cached = ReadInstallTree(lodDirectory);
    {
        ModelAsset loaded(ModelGuid, ModelPath);
        ASSERT_TRUE(loaded.Load());
        loaded.PostLoad();
    }
    EXPECT_EQ(ReadInstallTree(lodDirectory), cached);
    assets.Shutdown();
    EXPECT_EQ(ReadInstallTree(InstallRoot), packageTree);
    EXPECT_TRUE(fs::is_empty(projectAssets));
}
