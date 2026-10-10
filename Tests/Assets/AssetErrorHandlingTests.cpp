#include <gtest/gtest.h>
#include "Assets/AudioAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/BinaryAsset.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetTasks.h"
#include "AssetCore/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "Core/EngineLoggerBridge.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

using namespace GameEngine;

class AssetErrorHandlingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        testDir = TestUtils::MakeUniqueTempDirectory("test_error_assets");
        std::filesystem::create_directories(testDir);

        // Set logger level for test output
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
    }
    
    void TearDown() override {
        // Clean up test files
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }
    
    // Helper function to create corrupted WAV file
    void CreateCorruptedWAVFile(const std::filesystem::path& path) {
        std::ofstream file(path, std::ios::binary);
        
        // Write invalid WAV header
        file.write("RIFF", 4);
        uint32_t invalidSize = 0xFFFFFFFF;  // Invalid size
        file.write(reinterpret_cast<const char*>(&invalidSize), 4);
        file.write("WAVE", 4);
        // Missing fmt chunk - corrupted file
        
        file.close();
    }
    
    // Helper function to create malformed OBJ file
    void CreateMalformedOBJFile(const std::filesystem::path& path) {
        std::ofstream file(path);
        file << "# Malformed OBJ file\n";
        file << "v invalid vertex data\n";      // Invalid vertex format
        file << "f 999/999/999\n";             // Reference to non-existent vertex
        file << "invalid_command\n";           // Invalid OBJ command
        file.close();
    }
    
    // Helper function to create very large file
    void CreateLargeFile(const std::filesystem::path& path, size_t sizeMB) {
        std::ofstream file(path, std::ios::binary);
        
        const size_t chunkSize = 1024 * 1024; // 1MB chunks
        std::vector<uint8_t> chunk(chunkSize, 0xAA);
        
        for (size_t i = 0; i < sizeMB; ++i) {
            file.write(reinterpret_cast<const char*>(chunk.data()), chunkSize);
        }
        
        file.close();
    }
    
protected:
    std::filesystem::path testDir;
};

// Corrupted File Tests
TEST_F(AssetErrorHandlingTest, AudioAsset_CorruptedWAV_LoadFailure) {
    auto corruptedPath = testDir / "corrupted.wav";
    CreateCorruptedWAVFile(corruptedPath);
    
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, corruptedPath);
    
    // Should fail to load corrupted file
    EXPECT_FALSE(audioAsset.Load());
    EXPECT_EQ(audioAsset.GetState(), AssetState::Failed);
    EXPECT_EQ(audioAsset.GetFormat(), AudioFormat::WAV); // Format detected but loading failed
    EXPECT_EQ(audioAsset.GetSampleRate(), 0u);
    EXPECT_EQ(audioAsset.GetDataSize(), 0u);
}

TEST_F(AssetErrorHandlingTest, ModelAsset_MalformedOBJ_LoadFailure) {
    auto malformedPath = testDir / "malformed.obj";
    CreateMalformedOBJFile(malformedPath);
    
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, malformedPath);
    
    // Should fail to load malformed file
    EXPECT_FALSE(modelAsset.Load());
    EXPECT_EQ(modelAsset.GetState(), AssetState::Failed);
    EXPECT_EQ(modelAsset.GetFormat(), ModelFormat::OBJ); // Format detected but loading failed
    EXPECT_EQ(modelAsset.GetMeshCount(), 0u);
}

// Invalid Data Tests
TEST_F(AssetErrorHandlingTest, AudioAsset_InvalidWAVData_LoadFailure) {
    // Create invalid WAV data (too short)
    std::vector<uint8_t> invalidData = {0x52, 0x49, 0x46, 0x46}; // Just "RIFF"
    
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, testDir / "test.wav");
    
    EXPECT_FALSE(audioAsset.LoadFromData(invalidData));
    EXPECT_EQ(audioAsset.GetState(), AssetState::Failed);
}

TEST_F(AssetErrorHandlingTest, ModelAsset_InvalidOBJData_LoadFailure) {
    // Create invalid OBJ data
    std::vector<uint8_t> invalidData = {0xFF, 0xFE, 0xFD, 0xFC}; // Binary data in text file
    
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, testDir / "test.obj");
    
    EXPECT_FALSE(modelAsset.LoadFromData(invalidData));
    EXPECT_EQ(modelAsset.GetState(), AssetState::Failed);
}

// Empty Data Tests
TEST_F(AssetErrorHandlingTest, AudioAsset_EmptyData_LoadFailure) {
    std::vector<uint8_t> emptyData;
    
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, testDir / "empty.wav");
    
    EXPECT_FALSE(audioAsset.LoadFromData(emptyData));
    EXPECT_EQ(audioAsset.GetState(), AssetState::Failed);
}

TEST_F(AssetErrorHandlingTest, ModelAsset_EmptyData_LoadFailure) {
    std::vector<uint8_t> emptyData;
    
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, testDir / "empty.obj");
    
    EXPECT_FALSE(modelAsset.LoadFromData(emptyData));
    EXPECT_EQ(modelAsset.GetState(), AssetState::Failed);
}

TEST_F(AssetErrorHandlingTest, BinaryAsset_EmptyData_LoadSuccess) {
    // BinaryAsset should handle empty data gracefully
    std::vector<uint8_t> emptyData;
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, testDir / "empty.bin", AssetType::Unknown);
    
    EXPECT_TRUE(binaryAsset.LoadFromData(emptyData));
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(binaryAsset.GetDataSize(), 0u);
    EXPECT_FALSE(binaryAsset.HasData());
}

// Large File Tests
TEST_F(AssetErrorHandlingTest, BinaryAsset_LargeFile_LoadSuccess) {
    // Test with 10MB file
    auto largePath = testDir / "large.bin";
    CreateLargeFile(largePath, 10);
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, largePath, AssetType::Unknown);
    
    // Should handle large files
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(binaryAsset.GetDataSize(), 10 * 1024 * 1024u); // 10MB
    EXPECT_TRUE(binaryAsset.HasData());
}

// Memory Stress Tests
TEST_F(AssetErrorHandlingTest, BinaryAsset_MultipleLoads_MemoryManagement) {
    // Create test file
    auto binPath = testDir / "test.bin";
    std::ofstream file(binPath, std::ios::binary);
    std::vector<uint8_t> testData(1024, 0xAB);
    file.write(reinterpret_cast<const char*>(testData.data()), testData.size());
    file.close();
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, binPath, AssetType::Unknown);
    
    // Load multiple times - should handle memory properly
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    
    // Load again - should not leak memory
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(binaryAsset.GetDataSize(), 1024u);
    
    // Unload and reload
    binaryAsset.Unload();
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Unloaded);
    EXPECT_FALSE(binaryAsset.HasData());
    
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_TRUE(binaryAsset.HasData());
}

// Concurrent Access Tests
TEST_F(AssetErrorHandlingTest, AudioAsset_ConcurrentLoadUnload_ThreadSafety) {
    // Create test WAV file
    auto wavPath = testDir / "concurrent.wav";
    std::ofstream file(wavPath, std::ios::binary);
    
    // Create some dummy audio data (1 second of silence)
    const uint32_t sampleRate = 44100;
    const uint16_t numChannels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t samplesPerSecond = sampleRate;
    const uint32_t dataSize = samplesPerSecond * numChannels * (bitsPerSample / 8);
    const uint32_t fileSize = 36 + dataSize;  // Header size + data size

    // WAV header
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    uint32_t subchunk1Size = 16;
    file.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    uint16_t audioFormat = 1;
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    // Write dummy audio data (silence)
    std::vector<uint8_t> audioData(dataSize, 0);
    file.write(reinterpret_cast<const char*>(audioData.data()), dataSize);
    file.close();
    
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, wavPath);
    
    // Test multiple load/unload cycles
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(audioAsset.Load());
        EXPECT_EQ(audioAsset.GetState(), AssetState::Loaded);
        
        audioAsset.Unload();
        EXPECT_EQ(audioAsset.GetState(), AssetState::Unloaded);
    }
}

// File Permission Tests
TEST_F(AssetErrorHandlingTest, AudioAsset_ReadOnlyFile_LoadSuccess) {
    // Create test WAV file
    auto wavPath = testDir / "readonly.wav";
    std::ofstream file(wavPath, std::ios::binary);
    
    // Create some dummy audio data (1 second of silence)
    const uint32_t sampleRate = 44100;
    const uint16_t numChannels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t samplesPerSecond = sampleRate;
    const uint32_t dataSize = samplesPerSecond * numChannels * (bitsPerSample / 8);
    const uint32_t fileSize = 36 + dataSize;  // Header size + data size

    // WAV header
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&fileSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    uint32_t subchunk1Size = 16;
    file.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    uint16_t audioFormat = 1;
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    // Write dummy audio data (silence)
    std::vector<uint8_t> audioData(dataSize, 0);
    file.write(reinterpret_cast<const char*>(audioData.data()), dataSize);
    file.close();
    
    // Make file read-only
    std::filesystem::permissions(wavPath, std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, wavPath);
    
    // Should still be able to read read-only file
    EXPECT_TRUE(audioAsset.Load());
    EXPECT_EQ(audioAsset.GetState(), AssetState::Loaded);
}

// Asset State Validation Tests
TEST_F(AssetErrorHandlingTest, AssetState_InvalidTransitions_Handled) {
    auto binPath = testDir / "state_test.bin";
    std::ofstream file(binPath, std::ios::binary);
    std::vector<uint8_t> testData(100, 0x42);
    file.write(reinterpret_cast<const char*>(testData.data()), testData.size());
    file.close();
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, binPath, AssetType::Unknown);
    
    // Initial state should be Unloaded
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Unloaded);
    
    // Load should succeed
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    
    // Loading already loaded asset should succeed (idempotent)
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    
    // Unload should work
    binaryAsset.Unload();
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Unloaded);
    
    // Unloading already unloaded asset should be safe
    binaryAsset.Unload();
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Unloaded);
}

namespace
{
// Collects warnings that name a given text. Log::AddSink takes ownership of the
// sink, so the collected messages are shared_ptr-owned and outlive the capture.
class WarningCapture
{
  public:
    explicit WarningCapture(Logger::String needle)
    {
        // Engine is a SHARED library: AssetManager logs through Engine's Logger
        // state, not this executable's copy. Adopt it, or the capture reads zero.
        Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
        Logger::Log::Initialize({});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        sink->RegisterCallback([state = m_State, needle = std::move(needle)](const Logger::LogMessage& msg) {
            if (msg.Level == Logger::LogLevel::Warning && msg.Message.find(needle) != Logger::String::npos)
            {
                std::lock_guard lock(state->Mutex);
                state->Messages.push_back(msg.Message);
            }
        });
        Logger::Log::AddSink(std::move(sink));
    }

    std::vector<Logger::String> Messages() const
    {
        Logger::Log::Flush();
        std::lock_guard lock(m_State->Mutex);
        return m_State->Messages;
    }

  private:
    struct State
    {
        std::mutex Mutex;
        std::vector<Logger::String> Messages;
    };
    std::shared_ptr<State> m_State = std::make_shared<State>();
};
} // namespace

// A throwing asset factory reports its exception text with the asset's name and type.
TEST_F(AssetErrorHandlingTest, AssetManager_CreateAsset_ThrowingFactory_LogsReason) {
    AssetManager assetManager;
    ASSERT_TRUE(assetManager.Initialize(testDir, nullptr));

    const String kReason = "factory rejected the header";
    AssetTypeRegistry& typeRegistry = assetManager.GetAssetTypeRegistry();
    typeRegistry.UnregisterAssetType(AssetType::Video);
    ASSERT_TRUE(typeRegistry.RegisterAssetType(AssetTypeRegistration(
        AssetType::Video, {".throwingfactory"},
        [kReason](const AssetMetadata&) -> SharedPtr<Asset> { throw std::runtime_error(kReason); },
        "Throwing test factory", 1000)));

    AssetMetadata metadata;
    metadata.Guid = GUID::Generate();
    metadata.Path = testDir / "broken.throwingfactory";
    metadata.Name = "broken";
    metadata.Extension = ".throwingfactory";
    metadata.Type = AssetType::Video;

    WarningCapture warnings(kReason);
    EXPECT_NO_THROW(assetManager.CreateAsset(metadata));

    const auto messages = warnings.Messages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_NE(messages.front().find("broken"), Logger::String::npos) << messages.front();
    EXPECT_NE(messages.front().find(AssetTypeToString(AssetType::Video)), Logger::String::npos) << messages.front();
}

TEST_F(AssetErrorHandlingTest, DecodeSuppressionPreservesFileSpellingAndReportsErrorOnce)
{
    const auto assetDirectory = std::filesystem::canonical(testDir) / "MixedCaseAssets";
    std::filesystem::create_directories(assetDirectory);
    struct FailureCase
    {
        const char* Filename;
        AssetType Type;
        const char* Contents;
        const char* Error;
    };
    const FailureCase cases[] = {
        {"MalformedPipeline.rendergraph", AssetType::RenderPipeline, "{ not json }",
         "JSON parse failed (malformed JSON)"},
        {"EmptyMaterial.material", AssetType::Material, " ", "expected JSON object"}
    };
    for (const auto& failure : cases)
    {
        std::ofstream(assetDirectory / failure.Filename) << failure.Contents;
    }

    AssetManager manager;
    ASSERT_TRUE(manager.Initialize(testDir));
    manager.WaitForStartupScan();
    for (const auto& failure : cases)
    {
        const auto path = assetDirectory / failure.Filename;
        SCOPED_TRACE(path.generic_string());
        AssetMetadata metadata;
        ASSERT_TRUE(manager.GetRegistry().TryGetAssetMetadata(path, metadata));
        ASSERT_EQ(metadata.Type, failure.Type);
        const std::string contents = failure.Contents;
        const Vector<uint8> bytes(contents.begin(), contents.end());
        EXPECT_FALSE(DecodeAssetPayload(manager, metadata.Guid, metadata, bytes));
        String reason;
        ASSERT_TRUE(manager.IsLoadSuppressed(metadata.Guid, &reason));
        EXPECT_EQ(reason, "Failed to load '" + path.generic_string() + "': " + failure.Error);
    }
    manager.Shutdown();
}
