#include <gtest/gtest.h>
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"
#include <filesystem>
#include <fstream>

using namespace GameEngine;

// A `.meta` file is never an asset: the registry refuses it, so no sidecar of a sidecar can appear.
class MetaFileRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir = TestUtils::MakeUniqueTempDirectory("meta_file_registry_test");
        std::filesystem::remove_all(testDir);
        std::filesystem::create_directories(testDir);
        jobSystem = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        assetManager = std::make_unique<AssetManager>();
        ASSERT_TRUE(assetManager->Initialize(testDir, jobSystem.get())) << "Failed to initialize AssetManager";
    }

    void TearDown() override {
        if (assetManager) {
            assetManager->Shutdown();
            assetManager.reset();
        }
        jobSystem.reset();
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    void CreateTestFile(const std::filesystem::path& filePath, const std::string& content) {
        std::filesystem::create_directories(filePath.parent_path());
        std::ofstream file(filePath);
        ASSERT_TRUE(file.is_open()) << "Failed to create test file: " << filePath;
        file << content;
    }

    std::filesystem::path testDir;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> jobSystem;
    std::unique_ptr<AssetManager> assetManager;
};

TEST_F(MetaFileRegistryTest, RegisterAssetRefusesAMetaFileAndAcceptsAnAsset) {
    auto& registry = assetManager->GetRegistry();

    auto normalAssetPath = testDir / "normal_asset.txt";
    CreateTestFile(normalAssetPath, "Normal asset content");
    auto metaFilePath = testDir / "test_file.meta";
    CreateTestFile(metaFilePath, "guid: test-guid\ntype: Unknown\nname: test_file");

    EXPECT_TRUE(registry.RegisterAsset(normalAssetPath)) << "Normal asset should be successfully registered";
    EXPECT_FALSE(registry.RegisterAsset(metaFilePath)) << "Meta file should be rejected from registration";

    EXPECT_TRUE(registry.GetAssetGUID(metaFilePath).IsNull()) << "Meta file should not have a GUID in registry";
    EXPECT_FALSE(registry.GetAssetGUID(normalAssetPath).IsNull()) << "Normal asset should have a GUID in registry";
}
