#include <gtest/gtest.h>
#include "Core/Engine.h"
#include "Core/Application.h"
#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/BinaryAsset.h"
#include "Assets/TextureAsset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetRegistry.h"
#include "Logger/Logger.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <thread>

#ifdef UISYSTEM_AVAILABLE
#include "UISystem/UISystemInitializer.h"
#include "UISystem/UILayoutAsset.h"
#include "UISystem/UIStyleAsset.h"
#endif

using namespace GameEngine;

class Complete4StagePipelineIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Capture the original working directory — Engine::Initialize rewrites
        // it to WorkspaceDirectory, and we must restore it before remove_all
        // or Windows refuses to delete a directory that contains the CWD.
        originalCwd = std::filesystem::current_path();

        // Use a unique temp directory per test so a failed TearDown in one
        // test can't pollute subsequent tests (nested test-dir paths).
        testDir = TestUtils::MakeUniqueTempDirectory("integration_test_assets");
        std::filesystem::create_directories(testDir);

        // Set logger level for detailed output
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);

        // Initialize Engine (this will register AudioAsset and ModelAsset)
        engine = std::make_unique<EngineCore>();
        ApplicationConfig config;
        // IMPORTANT: isolate the engine's asset root + asset database for this test run so we
        // don't pollute the build output directory's AssetDatabase.assetdb (used by the Editor).
        config.WorkspaceDirectory = testDir.string();
        config.AssetDirectory = testDir.string();
        // Keep DB/cache inside the test directory so TearDown removes everything.
        config.AssetDatabaseFile = "AssetDatabase.assetdb";
        config.AssetDatabaseCacheDirectory = ".Cache/AssetDatabase";
        ASSERT_TRUE(engine->Initialize(config)) << "Failed to initialize Engine";

        // Get asset manager from engine
        assetManager = &engine->GetAssetManager();

        // Create comprehensive test assets
        CreateTestAssets();

        Logger::Log::Info("Complete4StagePipelineIntegrationTest: Setup complete with full Engine initialization");
    }

    void TearDown() override {
        // Shutdown engine
        if (engine) {
            engine->Shutdown();
            engine.reset();
        }

        // Restore CWD before removing testDir — the engine moved it into testDir.
        std::error_code ec;
        std::filesystem::current_path(originalCwd, ec);

        // Use error_code overload: if Windows is still holding a file handle
        // after engine shutdown, don't throw — leaving a temp dir behind is
        // a soft leak, not a test failure.
        std::filesystem::remove_all(testDir, ec);

        Logger::Log::Info("Complete4StagePipelineIntegrationTest: Teardown complete");
    }
    
    void CreateTestAssets() {
        // Create WAV audio file
        CreateTestWAVFile(testDir / "test_audio.wav");
        
        // Create OBJ model file
        CreateTestOBJFile(testDir / "test_model.obj");
        
        // Create binary file
        CreateTestBinaryFile(testDir / "test_data.bin", 2048);
        
        // Create texture files (PNG header)
        CreateTestPNGFile(testDir / "test_texture.png");
        CreateTestPNGFile(testDir / "base_texture.png"); // For dependency tests
        
#ifdef UISYSTEM_AVAILABLE
        // Create UI layout file
        CreateTestXMLFile(testDir / "test_layout.xml");
        
        // Create UI style file
        CreateTestCSSFile(testDir / "test_style.css");
#endif
        
        Logger::Log::Info("Complete4StagePipelineIntegrationTest: Created {} test assets", GetTestAssetCount());
    }
    
    void CreateTestWAVFile(const std::filesystem::path& path) {
        std::ofstream file(path, std::ios::binary);

        // Create some dummy audio data (1 second of silence)
        const uint32_t sampleRate = 44100;
        const uint16_t numChannels = 2;
        const uint16_t bitsPerSample = 16;
        const uint32_t samplesPerSecond = sampleRate;
        const uint32_t dataSize = samplesPerSecond * numChannels * (bitsPerSample / 8);
        const uint32_t fileSize = 36 + dataSize;  // Header size + data size

        // WAV header
        file.write("RIFF", 4);                    // ChunkID
        file.write(reinterpret_cast<const char*>(&fileSize), 4);
        file.write("WAVE", 4);                    // Format

        // fmt subchunk
        file.write("fmt ", 4);                    // Subchunk1ID
        uint32_t subchunk1Size = 16;              // Subchunk1Size
        file.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
        uint16_t audioFormat = 1;                 // PCM
        file.write(reinterpret_cast<const char*>(&audioFormat), 2);
        file.write(reinterpret_cast<const char*>(&numChannels), 2);
        file.write(reinterpret_cast<const char*>(&sampleRate), 4);
        uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        file.write(reinterpret_cast<const char*>(&byteRate), 4);
        uint16_t blockAlign = numChannels * (bitsPerSample / 8);
        file.write(reinterpret_cast<const char*>(&blockAlign), 2);
        file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

        // data subchunk
        file.write("data", 4);                    // Subchunk2ID
        file.write(reinterpret_cast<const char*>(&dataSize), 4);

        // Write dummy audio data (silence)
        std::vector<uint8_t> audioData(dataSize, 0);
        file.write(reinterpret_cast<const char*>(audioData.data()), dataSize);

        file.close();
    }
    
    void CreateTestOBJFile(const std::filesystem::path& path) {
        std::ofstream file(path);
        file << "# Test OBJ file for integration testing\n";
        file << "v 0.0 0.0 0.0\n";
        file << "v 1.0 0.0 0.0\n";
        file << "v 0.0 1.0 0.0\n";
        file << "v 0.0 0.0 1.0\n";
        file << "vn 0.0 0.0 1.0\n";
        file << "vn 0.0 1.0 0.0\n";
        file << "vt 0.0 0.0\n";
        file << "vt 1.0 0.0\n";
        file << "vt 0.5 1.0\n";
        file << "f 1/1/1 2/2/1 3/3/1\n";
        file << "f 1/1/2 3/3/2 4/1/2\n";
        file.close();
    }
    
    void CreateTestBinaryFile(const std::filesystem::path& path, size_t size) {
        std::ofstream file(path, std::ios::binary);
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i) {
            data[i] = static_cast<uint8_t>((i * 37) % 256); // Pattern for validation
        }
        file.write(reinterpret_cast<const char*>(data.data()), size);
        file.close();
    }
    
    void CreateTestPNGFile(const std::filesystem::path& path) {
        std::ofstream file(path, std::ios::binary);
        
        // PNG signature
        uint8_t pngSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        file.write(reinterpret_cast<const char*>(pngSignature), 8);
        
        // IHDR chunk (minimal 1x1 PNG)
        uint32_t ihdrLength = 13;
        file.write(reinterpret_cast<const char*>(&ihdrLength), 4);
        file.write("IHDR", 4);
        uint32_t width = 1, height = 1;
        file.write(reinterpret_cast<const char*>(&width), 4);
        file.write(reinterpret_cast<const char*>(&height), 4);
        uint8_t bitDepth = 8, colorType = 2, compression = 0, filter = 0, interlace = 0;
        file.write(reinterpret_cast<const char*>(&bitDepth), 1);
        file.write(reinterpret_cast<const char*>(&colorType), 1);
        file.write(reinterpret_cast<const char*>(&compression), 1);
        file.write(reinterpret_cast<const char*>(&filter), 1);
        file.write(reinterpret_cast<const char*>(&interlace), 1);
        uint32_t ihdrCrc = 0; // Simplified for test
        file.write(reinterpret_cast<const char*>(&ihdrCrc), 4);
        
        file.close();
    }
    
#ifdef UISYSTEM_AVAILABLE
    void CreateTestXMLFile(const std::filesystem::path& path) {
        std::ofstream file(path);
        file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        // Use a root element that matches the engine UI markup conventions so ParserRegistry sniffing
        // classifies this .xml as a UILayout (not generic XML).
        file << "<UI id=\"root\">\n";
        file << "  <UIElement id=\"main\" class=\"panel\">\n";
        file << "    <Button id=\"testButton\" text=\"Test Button\"/>\n";
        file << "    <Label id=\"testLabel\" text=\"Test Label\"/>\n";
        file << "  </UIElement>\n";
        file << "</UI>\n";
        file.close();
    }
    
    void CreateTestCSSFile(const std::filesystem::path& path) {
        std::ofstream file(path);
        file << "/* Test CSS file for integration testing */\n";
        file << ".button {\n";
        file << "  background-color: #4CAF50;\n";
        file << "  border: none;\n";
        file << "  color: white;\n";
        file << "  padding: 15px 32px;\n";
        file << "  text-align: center;\n";
        file << "  font-size: 16px;\n";
        file << "}\n";
        file << "\n";
        file << ".label {\n";
        file << "  font-family: Arial, sans-serif;\n";
        file << "  font-size: 14px;\n";
        file << "  color: #333333;\n";
        file << "}\n";
        file.close();
    }
#endif
    
    int GetTestAssetCount() const {
#ifdef UISYSTEM_AVAILABLE
        return 7; // WAV, OBJ, BIN, PNG, base_PNG, XML, CSS
#else
        return 5; // WAV, OBJ, BIN, PNG, base_PNG
#endif
    }
    
    // Helper function to measure execution time
    template<typename Func>
    double MeasureExecutionTime(Func&& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        return duration.count() / 1000000.0; // Convert to milliseconds
    }
    
protected:
    std::filesystem::path testDir;
    std::filesystem::path originalCwd;
    std::unique_ptr<EngineCore> engine;
    AssetManager* assetManager;
};

// Test complete 4-stage pipeline with all asset types
TEST_F(Complete4StagePipelineIntegrationTest, AllAssetTypes_Complete4StagePipeline) {
    // Verify asset type registrations
    auto& typeRegistry = assetManager->GetAssetTypeRegistry();
    
    EXPECT_TRUE(typeRegistry.IsAssetTypeRegistered(AssetType::Audio)) 
        << "AudioAsset should be registered";
    EXPECT_TRUE(typeRegistry.IsAssetTypeRegistered(AssetType::Model)) 
        << "ModelAsset should be registered";
    EXPECT_TRUE(typeRegistry.IsAssetTypeRegistered(AssetType::Texture)) 
        << "TextureAsset should be registered";
    
#ifdef UISYSTEM_AVAILABLE
    EXPECT_TRUE(typeRegistry.IsAssetTypeRegistered(AssetType::UILayout)) 
        << "UILayoutAsset should be registered";
    EXPECT_TRUE(typeRegistry.IsAssetTypeRegistered(AssetType::UIStyle)) 
        << "UIStyleAsset should be registered";
#endif
    
    // Test asset creation for each type
    std::vector<std::pair<std::filesystem::path, AssetType>> testAssets = {
        {testDir / "test_audio.wav", AssetType::Audio},
        {testDir / "test_model.obj", AssetType::Model},
        {testDir / "test_data.bin", AssetType::Unknown}, // BinaryAsset uses Unknown type
        {testDir / "test_texture.png", AssetType::Texture}
    };
    
#ifdef UISYSTEM_AVAILABLE
    testAssets.push_back({testDir / "test_layout.xml", AssetType::UILayout});
    testAssets.push_back({testDir / "test_style.css", AssetType::UIStyle});
#endif
    
    std::vector<GUID> assetGuids;
    std::vector<AssetLoadHandle> loadHandles;
    
    // Stage 1: Discovery and Asset Creation
    Logger::Log::Info("=== Stage 1: Discovery and Asset Creation ===");
    for (const auto& [path, expectedType] : testAssets) {
        GUID assetGuid = GUID::Generate();
        assetGuids.push_back(assetGuid);
        
        // Create asset metadata
        AssetMetadata metadata;
        metadata.Guid = assetGuid;
        metadata.Path = path;
        metadata.Name = path.stem().string();
        metadata.Extension = path.extension().string();
        metadata.Type = expectedType;
        metadata.FileSize = std::filesystem::file_size(path);
        
        // Register with asset registry
        assetManager->GetRegistry().RegisterAssetMetadata(metadata);
        
        // Test asset creation
        auto asset = assetManager->CreateAsset(metadata);
        if (expectedType == AssetType::Unknown) {
            // BinaryAsset should be created via parser registry
            EXPECT_TRUE(asset != nullptr) << "Failed to create BinaryAsset for " << path.string();
        } else {
            EXPECT_TRUE(asset != nullptr) << "Failed to create asset for " << path.string() << " (type: " << static_cast<int>(expectedType) << ")";
        }
        
        if (asset) {
            EXPECT_EQ(asset->GetType(), expectedType) << "Asset type mismatch for " << path.string();
            Logger::Log::Info("✓ Created {} asset: {}", AssetTypeToString(expectedType), metadata.Name);
        }
    }
    
    // Stage 2: Async Loading Pipeline
    Logger::Log::Info("=== Stage 2: Async Loading Pipeline ===");
    double pipelineTime = MeasureExecutionTime([&]() {
        for (const auto& guid : assetGuids) {
            AssetLoadRequest request;
            request.AssetGuid = guid;
            request.Priority = AssetLoadPriority::High;
            
            auto handle = assetManager->LoadAsset(request);
            loadHandles.push_back(std::move(handle));
        }
    });
    
    Logger::Log::Info("Pipeline submission completed in {:.3f}ms", pipelineTime);
    EXPECT_LT(pipelineTime, 50.0) << "Pipeline submission should be under 50ms";
    
    // Stage 3: Wait for completion and validate results
    Logger::Log::Info("=== Stage 3: Pipeline Completion Validation ===");
    
    int completedAssets = 0;
    int maxWaitIterations = 100; // 10 seconds max wait
    
    for (int iteration = 0; iteration < maxWaitIterations; ++iteration) {
        completedAssets = 0;
        
        for (auto& handle : loadHandles) {
            if (handle.IsComplete()) {
                completedAssets++;
            }
        }
        
        if (completedAssets == loadHandles.size()) {
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    Logger::Log::Info("Pipeline completion: {}/{} assets completed", completedAssets, loadHandles.size());
    
    // Stage 4: Validate loaded assets
    Logger::Log::Info("=== Stage 4: Asset Validation ===");
    
    int successfullyLoaded = 0;
    for (size_t i = 0; i < loadHandles.size(); ++i) {
        auto& handle = loadHandles[i];
        const auto& [path, expectedType] = testAssets[i];
        
        if (handle.IsComplete()) {
            SharedPtr<Asset> asset = handle.GetResult();
            if (asset) {
                EXPECT_EQ(asset->GetState(), AssetState::Loaded) 
                    << "Asset should be in loaded state: " << path.string();
                
                if (asset->GetState() == AssetState::Loaded) {
                    successfullyLoaded++;
                    Logger::Log::Info("✓ Successfully loaded: {} ({})", path.filename().string(), AssetTypeToString(asset->GetType()));
                    
                    // Type-specific validation
                    switch (asset->GetType()) {
                        case AssetType::Audio: {
                            auto audioAsset = std::static_pointer_cast<AudioAsset>(asset);
                            EXPECT_EQ(audioAsset->GetFormat(), AudioFormat::WAV);
                            EXPECT_EQ(audioAsset->GetSampleRate(), 44100u);
                            break;
                        }
                        case AssetType::Model: {
                            auto modelAsset = std::static_pointer_cast<ModelAsset>(asset);
                            EXPECT_EQ(modelAsset->GetFormat(), ModelFormat::OBJ);
                            EXPECT_GT(modelAsset->GetMeshCount(), 0u);
                            break;
                        }
                        case AssetType::Unknown: {
                            auto binaryAsset = std::static_pointer_cast<BinaryAsset>(asset);
                            EXPECT_TRUE(binaryAsset->HasData());
                            EXPECT_EQ(binaryAsset->GetDataSize(), 2048u);
                            break;
                        }
                        case AssetType::Texture: {
                            auto textureAsset = std::static_pointer_cast<TextureAsset>(asset);
                            EXPECT_GT(textureAsset->GetWidth(), 0u);
                            EXPECT_GT(textureAsset->GetHeight(), 0u);
                            break;
                        }
                        default:
                            // UI assets or other types
                            break;
                    }
                }
            } else {
                Logger::Log::Error("✗ Failed to get result for: {}", path.filename().string());
            }
        } else {
            Logger::Log::Error("✗ Asset loading not completed: {}", path.filename().string());
        }
    }
    
    Logger::Log::Info("=== Integration Test Results ===");
    Logger::Log::Info("Total assets: {}", testAssets.size());
    Logger::Log::Info("Successfully loaded: {}", successfullyLoaded);
    Logger::Log::Info("Pipeline submission time: {:.3f}ms", pipelineTime);
    
    // Final assertions
    EXPECT_GT(successfullyLoaded, 0) << "At least some assets should load successfully";
    
    // We expect at least the basic asset types to work (Audio, Model, Binary, Texture)
    int expectedMinimumSuccess = 4;
#ifdef UISYSTEM_AVAILABLE
    expectedMinimumSuccess = 6; // Include UI assets if available
#endif
    
    EXPECT_GE(successfullyLoaded, expectedMinimumSuccess * 0.75)
        << "At least 75% of assets should load successfully in integration test";
}
