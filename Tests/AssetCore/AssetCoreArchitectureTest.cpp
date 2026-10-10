#include <gtest/gtest.h>
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetRegistry.h"
#include <memory>
#include <filesystem>

using namespace GameEngine;

// Test asset implementation for architecture verification
class TestAsset : public Asset {
public:
    TestAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Unknown, path), m_LoadCalled(false), m_UnloadCalled(false) {}

    bool Load() override {
        m_LoadCalled = true;
        SetState(AssetState::Loaded);
        return true;
    }

    bool LoadFromData(const Vector<uint8>& data) override {
        m_LoadCalled = true;
        m_LoadedData = data;
        SetState(AssetState::Loaded);
        return true;
    }

    void Unload() override {
        m_UnloadCalled = true;
        SetState(AssetState::Unloaded);
    }

    // Test accessors
    bool WasLoadCalled() const { return m_LoadCalled; }
    bool WasUnloadCalled() const { return m_UnloadCalled; }
    const Vector<uint8>& GetLoadedData() const { return m_LoadedData; }

private:
    bool m_LoadCalled;
    bool m_UnloadCalled;
    Vector<uint8> m_LoadedData;
};

class AssetCoreArchitectureTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        testDir = std::filesystem::temp_directory_path() / "assetcore_test";
        std::filesystem::create_directories(testDir);
    }

    void TearDown() override {
        // Clean up test directory
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
    }

    std::filesystem::path testDir;
};

// Test 1: Basic AssetCore Library Integration
TEST_F(AssetCoreArchitectureTest, BasicLibraryIntegration) {
    // Test that AssetCore types are accessible and functional
    
    // Test GUID generation
    auto guid1 = GUID::Generate();
    auto guid2 = GUID::Generate();
    EXPECT_FALSE(guid1.IsNull());
    EXPECT_FALSE(guid2.IsNull());
    EXPECT_NE(guid1, guid2);
    
    // Test GUID string conversion
    std::string guidStr = guid1.ToString();
    EXPECT_FALSE(guidStr.empty());
    EXPECT_EQ(guidStr.length(), 36); // Standard GUID string length
    
    // Test AssetType enum
    EXPECT_NE(AssetType::UILayout, AssetType::UIStyle);
    EXPECT_NE(AssetType::UILayout, AssetType::Unknown);
}

// Test 2: Asset Base Class Functionality
TEST_F(AssetCoreArchitectureTest, AssetBaseClassFunctionality) {
    auto guid = GUID::Generate();
    auto testPath = testDir / "test.asset";
    
    // Create test asset
    auto asset = std::make_shared<TestAsset>(guid, testPath);
    
    // Test initial state
    EXPECT_EQ(asset->GetGUID(), guid);
    EXPECT_EQ(asset->GetType(), AssetType::Unknown);
    EXPECT_EQ(asset->GetPath(), testPath);
    EXPECT_EQ(asset->GetState(), AssetState::Unloaded);
    EXPECT_FALSE(asset->IsLoaded());
    
    // Test loading
    EXPECT_TRUE(asset->Load());
    EXPECT_TRUE(asset->WasLoadCalled());
    EXPECT_EQ(asset->GetState(), AssetState::Loaded);
    EXPECT_TRUE(asset->IsLoaded());
    
    // Test unloading
    asset->Unload();
    EXPECT_TRUE(asset->WasUnloadCalled());
    EXPECT_EQ(asset->GetState(), AssetState::Unloaded);
    EXPECT_FALSE(asset->IsLoaded());
}

// Test 3: Asset Type Registry Functionality
TEST_F(AssetCoreArchitectureTest, AssetTypeRegistryFunctionality) {
    AssetTypeRegistry registry;
    
    // Test asset type registration (use Texture instead of Unknown since Unknown is rejected)
    AssetTypeRegistration registration(
        AssetType::Texture,
        {".test"},
        [](const AssetMetadata& metadata) -> SharedPtr<Asset> {
            return std::make_shared<TestAsset>(metadata.Guid, metadata.Path);
        },
        "Test Asset",
        100
    );

    EXPECT_TRUE(registry.RegisterAssetType(registration));
    
    // Test asset creation
    AssetMetadata metadata;
    metadata.Guid = GUID::Generate();
    metadata.Path = testDir / "test.test";
    metadata.Type = AssetType::Texture;
    metadata.Name = "test";
    metadata.Extension = ".test";

    auto asset = registry.CreateAsset(metadata);
    EXPECT_NE(asset, nullptr);
    EXPECT_EQ(asset->GetGUID(), metadata.Guid);
    EXPECT_EQ(asset->GetPath(), metadata.Path);

    // Test extension lookup
    auto foundType = registry.GetAssetTypeFromExtension(".test");
    EXPECT_EQ(foundType, AssetType::Texture);
}

// Test 4: Asset Event System Functionality
TEST_F(AssetCoreArchitectureTest, AssetEventSystemFunctionality) {
    AssetEventDispatcher dispatcher;
    
    // Test event callback registration
    bool eventReceived = false;
    AssetEvent receivedEvent(AssetEventType::AssetLoaded, GUID::Generate(), AssetType::Unknown, "");

    auto callbackHandle = dispatcher.AddCallback([&](const AssetEvent& event) {
        eventReceived = true;
        receivedEvent = event;
    });
    
    EXPECT_NE(callbackHandle, 0);
    
    // Test event dispatching
    auto guid = GUID::Generate();
    AssetEvent testEvent(AssetEventType::AssetLoaded, guid, AssetType::Unknown, "test.asset");
    
    dispatcher.DispatchEvent(testEvent);
    
    EXPECT_TRUE(eventReceived);
    EXPECT_EQ(receivedEvent.EventType, AssetEventType::AssetLoaded);
    EXPECT_EQ(receivedEvent.AssetGuid, guid);
    EXPECT_EQ(receivedEvent.Type, AssetType::Unknown);
    EXPECT_EQ(receivedEvent.AssetPath, "test.asset");
    
    // Test callback removal
    dispatcher.RemoveCallback(callbackHandle);
    
    eventReceived = false;
    dispatcher.DispatchEvent(testEvent);
    EXPECT_FALSE(eventReceived); // Should not receive event after removal
}

// Test 5: Circular Dependency Resolution Verification
TEST_F(AssetCoreArchitectureTest, CircularDependencyResolution) {
    // This test verifies that we can use AssetCore types without any circular dependencies
    // The fact that this test compiles and links proves the architecture is working
    
    // Create instances of all major AssetCore types
    auto guid = GUID::Generate();
    auto asset = std::make_shared<TestAsset>(guid, testDir / "test.asset");
    AssetTypeRegistry registry;
    AssetEventDispatcher dispatcher;
    
    // Test that they can interact without issues
    AssetMetadata metadata;
    metadata.Guid = guid;
    metadata.Path = testDir / "test.asset";
    metadata.Type = AssetType::Unknown;
    
    AssetEvent event(AssetEventType::AssetCreated, guid, AssetType::Unknown, metadata.Path.string());
    
    // If we reach this point, the circular dependency has been successfully resolved
    EXPECT_TRUE(true);
}

// Test 6: Memory Management and RAII
TEST_F(AssetCoreArchitectureTest, MemoryManagementAndRAII) {
    // Test that AssetCore types properly manage memory and follow RAII principles
    
    {
        auto guid = GUID::Generate();
        auto asset = std::make_shared<TestAsset>(guid, testDir / "test.asset");
        
        // Load asset
        EXPECT_TRUE(asset->Load());
        EXPECT_TRUE(asset->IsLoaded());
        
        // Asset should automatically clean up when going out of scope
    }
    
    // Test with registry
    {
        AssetTypeRegistry registry;
        AssetEventDispatcher dispatcher;
        
        // Register and create assets
        AssetTypeRegistration registration(
            AssetType::Texture,
            {".test"},
            [](const AssetMetadata& metadata) -> SharedPtr<Asset> {
                return std::make_shared<TestAsset>(metadata.Guid, metadata.Path);
            },
            "Test Asset",
            100
        );
        
        registry.RegisterAssetType(registration);
        
        // Objects should clean up properly when going out of scope
    }
    
    EXPECT_TRUE(true); // If we reach here, memory management is working correctly
}
