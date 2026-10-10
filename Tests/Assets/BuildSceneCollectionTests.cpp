// What a build ships for a scene. The export walks the dependency graph from each
// built scene, so an asset the scene loads but the walk never reaches is missing from
// the package, and the packaged Player fails to load the scene (#2652). A scene names
// the blueprints its [blueprint ...] instances spawn in [resource] headers.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

void WriteTextFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

class BuildSceneCollection : public ::testing::Test
{
  protected:
    void SetUp() override { fs::create_directories(m_ProjectAssets); }

    void TearDown() override { m_AssetManager.Shutdown(); }

    void Mount()
    {
        ASSERT_TRUE(m_AssetManager.Initialize(m_ProjectAssets, nullptr,
                                              m_Root.Path() / "project" / "AssetDatabase.assetdb",
                                              m_Root.Path() / "project" / ".Cache" / "AssetDatabase"));
        m_AssetManager.WaitForStartupScan();
    }

    GUID GuidOf(const fs::path& file) { return m_AssetManager.GetRegistry().GetAssetGUID(file); }

    const TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("ge_build_scene_collect")};
    const fs::path m_ProjectAssets = m_Root.Path() / "project" / "Assets";
    AssetManager m_AssetManager;
};

std::string Blueprint(const std::string& name, const std::string& texturePath)
{
    return "[blueprint name=\"" + name + "\" version=1]\n"
           "[entity id=\"root\"]\n"
           "Name.value = \"" + name + "\"\n"
           "MeshRenderer.material = [path=\"" + texturePath + "\"]\n";
}

} // namespace

TEST_F(BuildSceneCollection, BlueprintsAScenesInstancesSpawnShipWithWhatTheyReference)
{
    // A path-only reference: the resource names its blueprint by path only, and the blueprint
    // names its own content. A second resource carries the GUID the editor's save heals in.
    WriteTextFile(m_ProjectAssets / "Textures" / "Well.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Textures" / "Rock.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Blueprints" / "Env" / "Well.blueprint", Blueprint("Well", "Textures/Well.png"));
    WriteTextFile(m_ProjectAssets / "Blueprints" / "Rock.blueprint", Blueprint("Rock", "Textures/Rock.png"));
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene", "[scene name=\"Lake\" version=1]\n");
    Mount();
    const GUID wellGuid = GuidOf(m_ProjectAssets / "Blueprints" / "Env" / "Well.blueprint");
    const GUID rockGuid = GuidOf(m_ProjectAssets / "Blueprints" / "Rock.blueprint");
    const GUID wellTextureGuid = GuidOf(m_ProjectAssets / "Textures" / "Well.png");
    const GUID rockTextureGuid = GuidOf(m_ProjectAssets / "Textures" / "Rock.png");
    ASSERT_FALSE(wellGuid.IsNull());
    ASSERT_FALSE(rockGuid.IsNull());
    ASSERT_FALSE(wellTextureGuid.IsNull());
    ASSERT_FALSE(rockTextureGuid.IsNull());
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[resource id=\"well\" path=\"Blueprints/Env/Well.blueprint\"]\n"
                  "[resource id=\"rock\" path=\"Blueprints/Rock.blueprint\" guid=\"" + rockGuid.ToString() + "\"]\n"
                  "\n"
                  "[blueprint id=\"well_0\" source=\"well\"]\n"
                  "Transform.position = (0, 0, 0)\n"
                  "\n"
                  "[blueprint id=\"rock_0\" source=\"rock\"]\n"
                  "Transform.position = (4, 0, 0)\n");

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});

    const AssetManifestEntry* well = manifest.FindByGuid(wellGuid);
    ASSERT_NE(well, nullptr) << "the blueprint a resource names by path did not ship";
    EXPECT_EQ(well->outputPath.generic_string(), "Assets/Blueprints/Env/Well.blueprint");
    const AssetManifestEntry* rock = manifest.FindByGuid(rockGuid);
    ASSERT_NE(rock, nullptr) << "the blueprint a resource names by GUID did not ship";
    EXPECT_EQ(rock->outputPath.generic_string(), "Assets/Blueprints/Rock.blueprint");
    EXPECT_NE(manifest.FindByGuid(wellTextureGuid), nullptr) << "what the spawned blueprint names did not ship";
    EXPECT_NE(manifest.FindByGuid(rockTextureGuid), nullptr) << "what the spawned blueprint names did not ship";
    EXPECT_TRUE(manifest.unresolvedDependencies.empty());
}

TEST_F(BuildSceneCollection, ResourceAScenesFieldNamesByIdShips)
{
    // `#id` field values resolve through the [resource] table, so the header is the
    // only place the scene names the asset.
    WriteTextFile(m_ProjectAssets / "Textures" / "Ground.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Field.scene",
                  "[scene name=\"Field\" version=1]\n"
                  "[resource id=\"ground\" path=\"Textures/Ground.png\"]\n"
                  "\n"
                  "[entity id=\"e_1\"]\n"
                  "MeshRenderer.material = #ground\n");
    Mount();
    const GUID groundGuid = GuidOf(m_ProjectAssets / "Textures" / "Ground.png");
    ASSERT_FALSE(groundGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Field.scene"});
    EXPECT_NE(manifest.FindByGuid(groundGuid), nullptr) << "the asset a #id field names did not ship";
}

TEST_F(BuildSceneCollection, ResourceWithAStaleGuidShipsByItsPath)
{
    // The load tries a resource's GUID first and its path when the registry does not
    // know the GUID (the asset was deleted and recreated, or the database rebuilt), so
    // the editor and the packaged Player load the blueprint by its path. It has to ship.
    WriteTextFile(m_ProjectAssets / "Textures" / "Well.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Blueprints" / "Well.blueprint", Blueprint("Well", "Textures/Well.png"));
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene", "[scene name=\"Lake\" version=1]\n");
    Mount();
    const GUID wellGuid = GuidOf(m_ProjectAssets / "Blueprints" / "Well.blueprint");
    const GUID wellTextureGuid = GuidOf(m_ProjectAssets / "Textures" / "Well.png");
    ASSERT_FALSE(wellGuid.IsNull());
    ASSERT_FALSE(wellTextureGuid.IsNull());
    const GUID staleGuid("0badc0de-0000-4000-8000-000000000001");
    ASSERT_NE(staleGuid, wellGuid);
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[resource id=\"well\" path=\"Blueprints/Well.blueprint\" guid=\"" + staleGuid.ToString() + "\"]\n"
                  "\n"
                  "[blueprint id=\"well_0\" source=\"well\"]\n"
                  "Transform.position = (0, 0, 0)\n");

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});

    EXPECT_NE(manifest.FindByGuid(wellGuid), nullptr) << "the blueprint the load finds by path did not ship";
    EXPECT_NE(manifest.FindByGuid(wellTextureGuid), nullptr) << "what the spawned blueprint names did not ship";
    // The stale GUID itself names nothing, and the build still reports it.
    ASSERT_EQ(manifest.unresolvedDependencies.size(), 1u);
    EXPECT_EQ(manifest.unresolvedDependencies[0], staleGuid);
}

TEST_F(BuildSceneCollection, FieldWithAStaleGuidShipsByItsPath)
{
    // The same fallback for a `[path=... guid=...]` field value (TryResolveAssetReference).
    WriteTextFile(m_ProjectAssets / "Textures" / "Ground.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Scenes" / "Field.scene", "[scene name=\"Field\" version=1]\n");
    Mount();
    const GUID groundGuid = GuidOf(m_ProjectAssets / "Textures" / "Ground.png");
    ASSERT_FALSE(groundGuid.IsNull());
    WriteTextFile(m_ProjectAssets / "Scenes" / "Field.scene",
                  "[scene name=\"Field\" version=1]\n"
                  "\n"
                  "[entity id=\"e_1\"]\n"
                  "MeshRenderer.material = [path=\"Textures/Ground.png\" guid=\"0badc0de-0000-4000-8000-000000000002\"]\n");

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Field.scene"});
    EXPECT_NE(manifest.FindByGuid(groundGuid), nullptr) << "the asset the load finds by path did not ship";
}

TEST_F(BuildSceneCollection, InstanceOverridesBeforeAnyEntityShip)
{
    // A scene made only of blueprint instances: an override on an instance's root names
    // an asset the blueprint does not, and nothing else in the scene names it.
    WriteTextFile(m_ProjectAssets / "Textures" / "Well.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Textures" / "Override.png", "PNGDATA");
    WriteTextFile(m_ProjectAssets / "Blueprints" / "Well.blueprint", Blueprint("Well", "Textures/Well.png"));
    WriteTextFile(m_ProjectAssets / "Scenes" / "Lake.scene",
                  "[scene name=\"Lake\" version=1]\n"
                  "[resource id=\"well\" path=\"Blueprints/Well.blueprint\"]\n"
                  "\n"
                  "[blueprint id=\"well_0\" source=\"well\"]\n"
                  "MeshRenderer.material = [path=\"Textures/Override.png\"]\n");
    Mount();
    const GUID overrideGuid = GuidOf(m_ProjectAssets / "Textures" / "Override.png");
    ASSERT_FALSE(overrideGuid.IsNull());

    AssetCollector collector(m_AssetManager);
    const AssetManifest manifest = collector.CollectFromScenes({"Scenes/Lake.scene"});
    EXPECT_NE(manifest.FindByGuid(overrideGuid), nullptr) << "the asset an instance override names did not ship";
}
