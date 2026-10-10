#include <gtest/gtest.h>
#include "Assets/AudioAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/BinaryAsset.h"
#include "AssetCore/GUID.h"
#include "Logger/Logger.h"
#include "TestTempDir.h"
#include <filesystem>
#include <fstream>
#include <vector>

using namespace GameEngine;

class AssetTypeLoadingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        testDir = TestUtils::MakeUniqueTempDirectory("test_assets");
        std::filesystem::create_directories(testDir);

        // Set logger level for test output
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
    }
    
    void TearDown() override {
        // Clean up test files
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }
    
    // Helper function to create test WAV file
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
    
    // Helper function to create test OBJ file
    void CreateTestOBJFile(const std::filesystem::path& path) {
        std::ofstream file(path);
        file << "# Test OBJ file\n";
        file << "v 0.0 0.0 0.0\n";      // Vertex 1
        file << "v 1.0 0.0 0.0\n";      // Vertex 2
        file << "v 0.0 1.0 0.0\n";      // Vertex 3
        file << "vn 0.0 0.0 1.0\n";     // Normal
        file << "vt 0.0 0.0\n";         // Texture coordinate 1
        file << "vt 1.0 0.0\n";         // Texture coordinate 2
        file << "vt 0.5 1.0\n";         // Texture coordinate 3
        file << "f 1/1/1 2/2/1 3/3/1\n"; // Face
        file.close();
    }
    
    // Helper function to create test binary file
    void CreateTestBinaryFile(const std::filesystem::path& path, size_t size = 1024) {
        std::ofstream file(path, std::ios::binary);
        std::vector<uint8_t> data(size);
        
        // Fill with test pattern
        for (size_t i = 0; i < size; ++i) {
            data[i] = static_cast<uint8_t>(i % 256);
        }
        
        file.write(reinterpret_cast<const char*>(data.data()), size);
        file.close();
    }
    
protected:
    std::filesystem::path testDir;
};

// AudioAsset Tests
TEST_F(AssetTypeLoadingTest, AudioAsset_WAV_LoadSuccess) {
    // Create test WAV file
    auto wavPath = testDir / "test.wav";
    CreateTestWAVFile(wavPath);
    
    // Create AudioAsset
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, wavPath);
    
    // Test loading
    EXPECT_TRUE(audioAsset.Load());
    EXPECT_EQ(audioAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(audioAsset.GetFormat(), AudioFormat::WAV);
    EXPECT_EQ(audioAsset.GetSampleRate(), 44100u);
    EXPECT_EQ(audioAsset.GetChannels(), AudioChannels::Stereo);
    // GetBitDepth reports the runtime PCM format (f32 = 32 bits), not the
    // source file's bit depth. Source is 16-bit PCM, decoded to f32.
    EXPECT_EQ(audioAsset.GetBitDepth(), 32u);
}

TEST_F(AssetTypeLoadingTest, AudioAsset_UnsupportedFormat_LoadFailure) {
    // AudioAsset uses miniaudio content-sniffing (not extension-based rejection),
    // so an unsupported format is data miniaudio cannot decode — not just a
    // wrong extension on valid WAV bytes.
    auto unsupportedPath = testDir / "test.xyz";
    {
        std::ofstream file(unsupportedPath, std::ios::binary);
        const char garbage[] = "this is not audio data, just plain text bytes\n";
        file.write(garbage, sizeof(garbage) - 1);
    }

    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, unsupportedPath);

    // Test loading should fail
    EXPECT_FALSE(audioAsset.Load());
    EXPECT_EQ(audioAsset.GetState(), AssetState::Failed);
    EXPECT_EQ(audioAsset.GetFormat(), AudioFormat::Unknown);
}

TEST_F(AssetTypeLoadingTest, AudioAsset_LoadFromData_Success) {
    // Create test WAV data
    std::vector<uint8_t> wavData;
    
    // Create minimal WAV header in memory
    auto wavPath = testDir / "temp.wav";
    CreateTestWAVFile(wavPath);
    
    // Read the file data
    std::ifstream file(wavPath, std::ios::binary);
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    wavData.resize(size);
    file.read(reinterpret_cast<char*>(wavData.data()), size);
    file.close();
    
    // Test LoadFromData
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, testDir / "test.wav");
    
    EXPECT_TRUE(audioAsset.LoadFromData(wavData));
    EXPECT_EQ(audioAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(audioAsset.GetFormat(), AudioFormat::WAV);
}

// ModelAsset Tests
TEST_F(AssetTypeLoadingTest, ModelAsset_OBJ_LoadSuccess) {
    // Create test OBJ file
    auto objPath = testDir / "test.obj";
    CreateTestOBJFile(objPath);
    
    // Create ModelAsset
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, objPath);
    
    // Test loading
    EXPECT_TRUE(modelAsset.Load());
    EXPECT_EQ(modelAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(modelAsset.GetFormat(), ModelFormat::OBJ);
    EXPECT_GT(modelAsset.GetMeshCount(), 0u);
    EXPECT_GT(modelAsset.GetTotalVertexCount(), 0u);
}

TEST_F(AssetTypeLoadingTest, ModelAsset_UnsupportedFormat_LoadFailure) {
    // Create test file with unsupported extension
    auto unsupportedPath = testDir / "test.xyz";
    CreateTestOBJFile(unsupportedPath);
    
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, unsupportedPath);
    
    // Test loading should fail
    EXPECT_FALSE(modelAsset.Load());
    EXPECT_EQ(modelAsset.GetState(), AssetState::Failed);
    EXPECT_EQ(modelAsset.GetFormat(), ModelFormat::Unknown);
}

TEST_F(AssetTypeLoadingTest, ModelAsset_LoadFromData_Success) {
    // Create test OBJ data
    auto objPath = testDir / "temp.obj";
    CreateTestOBJFile(objPath);
    
    // Read the file data
    std::ifstream file(objPath, std::ios::binary);
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> objData(size);
    file.read(reinterpret_cast<char*>(objData.data()), size);
    file.close();
    
    // Test LoadFromData
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, testDir / "test.obj");
    
    EXPECT_TRUE(modelAsset.LoadFromData(objData));
    EXPECT_EQ(modelAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(modelAsset.GetFormat(), ModelFormat::OBJ);
}

// A re-load reports the content it just parsed, never the parse before it.
// The glTF loader records clip identities inside its animation branch and
// can still return false afterwards (no meshes), which leaves the instance
// re-loadable with those identities already recorded.
TEST_F(AssetTypeLoadingTest, ModelAsset_ReloadWithoutAnimations_DropsPreviousClipGuids) {
    // One node, one animation, no meshes: the animation branch runs, then the
    // loader fails on the empty mesh list.
    const std::string withClips = R"({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [ 0 ] } ],
      "nodes": [ { "name": "Root" } ],
      "buffers": [ { "byteLength": 32,
        "uri": "data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAA=" } ],
      "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 8 },
                       { "buffer": 0, "byteOffset": 8, "byteLength": 24 } ],
      "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR" },
                     { "bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3" } ],
      "animations": [ { "name": "Walk",
        "samplers": [ { "input": 0, "output": 1, "interpolation": "LINEAR" } ],
        "channels": [ { "sampler": 0, "target": { "node": 0, "path": "translation" } } ] } ]
    })";

    // One node, one triangle, no animations.
    const std::string withoutClips = R"({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [ 0 ] } ],
      "nodes": [ { "name": "Root", "mesh": 0 } ],
      "meshes": [ { "name": "Tri", "primitives": [
        { "attributes": { "POSITION": 0 }, "indices": 1, "mode": 4 } ] } ],
      "buffers": [ { "byteLength": 42,
        "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA" } ],
      "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
                       { "buffer": 0, "byteOffset": 36, "byteLength": 6 } ],
      "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [ 0, 0, 0 ], "max": [ 1, 1, 0 ] },
                     { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" } ]
    })";

    const auto gltfPath = testDir / "reload.gltf";
    auto writeAndRead = [&](const std::string& body) {
        std::ofstream out(gltfPath, std::ios::binary | std::ios::trunc);
        out << body;
        out.close();
        return std::vector<uint8_t>(body.begin(), body.end());
    };

    ModelAsset modelAsset(GUID::Generate(), gltfPath);

    ASSERT_FALSE(modelAsset.LoadFromData(writeAndRead(withClips)));
    ASSERT_EQ(modelAsset.GetState(), AssetState::Failed);
    ASSERT_EQ(modelAsset.GetEmbeddedClipGuids().size(), 1u)
        << "precondition: the failed parse must have recorded the clip identity";
    ASSERT_TRUE(modelAsset.HasAnimations());

    EXPECT_TRUE(modelAsset.LoadFromData(writeAndRead(withoutClips)));
    EXPECT_EQ(modelAsset.GetState(), AssetState::Loaded);
    EXPECT_TRUE(modelAsset.GetEmbeddedClipGuids().empty())
        << "clip GUIDs from the previous parse survived a clipless re-load";
    EXPECT_TRUE(modelAsset.GetAnimationNames().empty());
    EXPECT_FALSE(modelAsset.HasAnimations());
}

// BinaryAsset Tests
TEST_F(AssetTypeLoadingTest, BinaryAsset_LoadSuccess) {
    // Create test binary file
    auto binPath = testDir / "test.bin";
    size_t testSize = 2048;
    CreateTestBinaryFile(binPath, testSize);
    
    // Create BinaryAsset
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, binPath, AssetType::Unknown);
    
    // Test loading
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(binaryAsset.GetDataSize(), testSize);
    EXPECT_TRUE(binaryAsset.HasData());
    EXPECT_NE(binaryAsset.GetData(), nullptr);
}

TEST_F(AssetTypeLoadingTest, BinaryAsset_EmptyFile_LoadSuccess) {
    // Create empty binary file
    auto binPath = testDir / "empty.bin";
    CreateTestBinaryFile(binPath, 0);
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, binPath, AssetType::Unknown);
    
    // Test loading empty file should succeed
    EXPECT_TRUE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(binaryAsset.GetDataSize(), 0u);
    EXPECT_FALSE(binaryAsset.HasData());
    EXPECT_EQ(binaryAsset.GetData(), nullptr);
}

TEST_F(AssetTypeLoadingTest, BinaryAsset_LoadFromData_Success) {
    // Create test binary data
    std::vector<uint8_t> testData = {0x01, 0x02, 0x03, 0x04, 0x05};
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, testDir / "test.bin", AssetType::Unknown);
    
    // Test LoadFromData
    EXPECT_TRUE(binaryAsset.LoadFromData(testData));
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Loaded);
    EXPECT_EQ(binaryAsset.GetDataSize(), testData.size());
    EXPECT_TRUE(binaryAsset.HasData());
    
    // Verify data integrity
    auto dataCopy = binaryAsset.GetDataCopy();
    EXPECT_EQ(dataCopy, testData);
}

// Error Handling Tests
TEST_F(AssetTypeLoadingTest, AudioAsset_NonExistentFile_LoadFailure) {
    auto nonExistentPath = testDir / "nonexistent.wav";
    
    GUID testGuid = GUID::Generate();
    AudioAsset audioAsset(testGuid, nonExistentPath);
    
    EXPECT_FALSE(audioAsset.Load());
    EXPECT_EQ(audioAsset.GetState(), AssetState::Failed);
}

TEST_F(AssetTypeLoadingTest, ModelAsset_NonExistentFile_LoadFailure) {
    auto nonExistentPath = testDir / "nonexistent.obj";
    
    GUID testGuid = GUID::Generate();
    ModelAsset modelAsset(testGuid, nonExistentPath);
    
    EXPECT_FALSE(modelAsset.Load());
    EXPECT_EQ(modelAsset.GetState(), AssetState::Failed);
}

TEST_F(AssetTypeLoadingTest, BinaryAsset_NonExistentFile_LoadFailure) {
    auto nonExistentPath = testDir / "nonexistent.bin";
    
    GUID testGuid = GUID::Generate();
    BinaryAsset binaryAsset(testGuid, nonExistentPath, AssetType::Unknown);
    
    EXPECT_FALSE(binaryAsset.Load());
    EXPECT_EQ(binaryAsset.GetState(), AssetState::Failed);
}
