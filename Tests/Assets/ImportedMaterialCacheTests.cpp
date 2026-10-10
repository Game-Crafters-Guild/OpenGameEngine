// A model's imported materials are derived data: the record lives under the
// project's derived-cache root, reloading an unchanged model writes nothing, and
// the authoritative (tracked) asset database is never touched by it.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ImportedMaterialCache.h"
#include "Assets/ModelAsset.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// One model load as the asset manager performs it: parse, then PostLoad under the
// manager's loading context.
void LoadModel(AssetManager& assets, const GUID& guid, const std::filesystem::path& path)
{
    ModelAsset model(guid, path);
    AssetManager::ScopedThreadAssetManager context(&assets);
    ASSERT_TRUE(model.Load());
    model.PostLoad();
}

// A one-triangle glTF whose single material is what the record must capture.
std::filesystem::path WriteTriangleModel(const std::filesystem::path& directory)
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t indices[] = {0, 1, 2, 0};
    std::ofstream binary(directory / "triangle.bin", std::ios::binary);
    binary.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    binary.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    binary.close();
    const std::filesystem::path path = directory / "triangle.gltf";
    std::ofstream(path) << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"triangle.bin","byteLength":44}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
"materials":[{"name":"cached_material","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.5,0.5,1],"metallicFactor":0,"roughnessFactor":0.7},"alphaMode":"MASK","alphaCutoff":0.4}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    return path;
}

ImportedMaterialData MakeImportedMaterial()
{
    ImportedMaterialData material{};
    material.Name = "fox_material";
    material.DiffuseTexture = "__embedded:0";
    material.NormalTexture = "__embedded:1";
    material.DiffuseTextureTransform = {2.0f, 0.0f, 0.25f, 0.0f, 0.0f, 2.0f, 0.5f, 0.0f};
    material.DiffuseColor[0] = 0.8f;
    material.DiffuseColor[1] = 0.6f;
    material.DiffuseColor[2] = 0.4f;
    material.DiffuseColor[3] = 1.0f;
    material.EmissiveColor[1] = 0.1f;
    material.Metallic = 0.0f;
    material.Roughness = 0.58f;
    material.DoubleSided = true;
    material.AlphaMode = AlphaMode::Mask;
    material.AlphaCutoff = 0.3f;
    material.AlphaModeInferred = true;
    material.IgnoresVertexColor = true;
    return material;
}

} // namespace

TEST(ImportedMaterialCache, RoundTripPreservesEveryField)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_imported_material_roundtrip");
    const fs::path file = root / "ModelMaterials" / "model.json";
    const ImportedMaterialData written = MakeImportedMaterial();

    ASSERT_EQ(WriteImportedMaterialCache(file, {written}), ImportedMaterialCacheWrite::Written);
    const auto read = ReadImportedMaterialCache(file);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->size(), 1u);
    const ImportedMaterialData& material = read->front();
    EXPECT_EQ(material.Name, written.Name);
    EXPECT_EQ(material.DiffuseTexture, written.DiffuseTexture);
    EXPECT_EQ(material.NormalTexture, written.NormalTexture);
    EXPECT_EQ(material.DiffuseTextureTransform, written.DiffuseTextureTransform);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(material.DiffuseColor[i], written.DiffuseColor[i]);
    EXPECT_EQ(material.EmissiveColor[1], written.EmissiveColor[1]);
    EXPECT_EQ(material.Roughness, written.Roughness);
    EXPECT_EQ(material.DoubleSided, written.DoubleSided);
    EXPECT_EQ(material.AlphaMode, written.AlphaMode);
    EXPECT_EQ(material.AlphaCutoff, written.AlphaCutoff);
    EXPECT_EQ(material.AlphaModeInferred, written.AlphaModeInferred);
    EXPECT_EQ(material.IgnoresVertexColor, written.IgnoresVertexColor);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(ImportedMaterialCache, UnreadableRecordIsAMiss)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_imported_material_corrupt");
    const fs::path file = root / "model.json";
    std::ofstream(file, std::ios::binary) << "{\"version\":3,\"materials\":[{\"name\":";
    EXPECT_FALSE(ReadImportedMaterialCache(file).has_value());
    EXPECT_FALSE(ReadImportedMaterialCache(root / "absent.json").has_value());

    std::error_code ec;
    fs::remove_all(root, ec);
}

// JSON has no NaN or infinity; a record holding them still reads back, so the
// model's materials are not a permanent miss.
TEST(ImportedMaterialCache, NonFiniteFieldsRoundTrip)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_imported_material_nonfinite");
    const fs::path file = root / "model.json";
    ImportedMaterialData written = MakeImportedMaterial();
    written.Roughness = std::numeric_limits<float>::quiet_NaN();
    written.DiffuseColor[3] = std::numeric_limits<float>::infinity();
    written.DiffuseTextureTransform[2] = -std::numeric_limits<float>::infinity();

    ASSERT_EQ(WriteImportedMaterialCache(file, {written}), ImportedMaterialCacheWrite::Written);
    const auto read = ReadImportedMaterialCache(file);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->size(), 1u);
    EXPECT_TRUE(std::isnan(read->front().Roughness));
    EXPECT_EQ(read->front().DiffuseColor[3], std::numeric_limits<float>::infinity());
    EXPECT_EQ(read->front().DiffuseTextureTransform[2], -std::numeric_limits<float>::infinity());
    EXPECT_EQ(WriteImportedMaterialCache(file, {written}), ImportedMaterialCacheWrite::Unchanged);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// The model loader itself records the materials: a load writes the record under
// the derived-cache root, and loading the unchanged model again rewrites neither
// the record nor the tracked database. The database never holds the record's data.
TEST(ImportedMaterialCache, ModelLoadRecordsItsMaterialsOnceInTheDerivedCache)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_imported_material_model_load");
    std::error_code ec;
    fs::create_directories(root / "Assets", ec);
    const fs::path modelPath = WriteTriangleModel(root / "Assets");
    const fs::path databaseFile = root / "AssetDatabase.assetdb";

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root / "Assets", &pool, databaseFile, root / ".Cache" / "AssetDatabase"));
    assets.GetRegistry().WaitForStartupScan();
    ASSERT_TRUE(assets.GetRegistry().RegisterAsset(modelPath));
    const GUID modelGuid = assets.GetRegistry().GetAssetGUID(modelPath);
    ASSERT_FALSE(modelGuid.IsNull());
    EXPECT_TRUE(assets.GetRegistry().AcceptsDerivedRecords(modelPath));

    LoadModel(assets, modelGuid, modelPath);
    const auto file = ImportedMaterialCacheFile(assets.GetRegistry(), modelPath, modelGuid);
    ASSERT_TRUE(file.has_value());
    // <project>/.Cache/ModelMaterials/<guid>.json: a sibling of the derived
    // AssetDatabase cache, never beside the tracked database.
    EXPECT_TRUE(fs::equivalent(file->parent_path().parent_path(), root / ".Cache"));
    const auto recorded = ReadImportedMaterialCache(*file);
    ASSERT_TRUE(recorded.has_value()) << file->string();
    ASSERT_EQ(recorded->size(), 1u);
    EXPECT_EQ(recorded->front().Name, "cached_material");
    EXPECT_EQ(recorded->front().AlphaMode, AlphaMode::Mask);
    const auto recordWriteTime = fs::last_write_time(*file);
    ASSERT_TRUE(assets.GetRegistry().SaveToFile({}));
    const std::string databaseAfterFirstLoad = ReadBytes(databaseFile);
    EXPECT_EQ(databaseAfterFirstLoad.find("cached_material"), std::string::npos)
        << "imported material data reached the tracked asset database";

    LoadModel(assets, modelGuid, modelPath);
    EXPECT_EQ(fs::last_write_time(*file), recordWriteTime);
    ASSERT_TRUE(assets.GetRegistry().SaveToFile({}));
    EXPECT_EQ(ReadBytes(databaseFile), databaseAfterFirstLoad);

    assets.Shutdown();
    fs::remove_all(root, ec);
}

// A packaged game mounts its content from the shipped manifest, with the
// arguments the engine passes at startup. Loading a model there records nothing:
// the install directory holds exactly what was shipped.
TEST(ImportedMaterialCache, PackagedGameModelLoadRecordsNothing)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_imported_material_packaged");
    std::error_code ec;
    fs::create_directories(root / "Assets", ec);
    const fs::path modelPath = WriteTriangleModel(root / "Assets");
    const GUID modelGuid("22a0544a-5a86-43ea-b8dc-7fbe90aa4592");
    std::ofstream(root / "Assets" / ".assetmanifest", std::ios::binary)
        << R"({"format":"assetmanifest","version":2})" << "\n"
        << R"({"guid":"22a0544a-5a86-43ea-b8dc-7fbe90aa4592","path":"triangle.gltf","type":"Model"})" << "\n";
    const auto listTree = [&root] {
        std::vector<std::string> entries;
        for (const auto& entry : fs::recursive_directory_iterator(root))
            entries.push_back(entry.path().lexically_relative(root).generic_string());
        std::sort(entries.begin(), entries.end());
        return entries;
    };
    const std::vector<std::string> shipped = listTree();

    JobSystem::WorkStealingThreadPool pool(2);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root / "Assets", &pool, root / "AssetDatabase.assetdb",
                                  root / ".Cache" / "AssetDatabase"));
    ASSERT_EQ(assets.GetRegistry().GetAssetGUID(modelPath), modelGuid);

    EXPECT_FALSE(assets.GetRegistry().AcceptsDerivedRecords(modelPath));

    LoadModel(assets, modelGuid, modelPath);
    EXPECT_FALSE(ImportedMaterialCacheFile(assets.GetRegistry(), modelPath, modelGuid).has_value());
    assets.Shutdown();
    EXPECT_EQ(listTree(), shipped);

    fs::remove_all(root, ec);
}

TEST(ImportedMaterialCache, RecordsWithoutVertexColorPolicyAreMisses)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_imported_material_vertex_color_schema");
    const fs::path file = root / "model.json";
    ASSERT_EQ(WriteImportedMaterialCache(file, {MakeImportedMaterial()}), ImportedMaterialCacheWrite::Written);
    const nlohmann::json current = nlohmann::json::parse(ReadBytes(file));
    const int currentVersion = current.at("version").get<int>();
    for (int version = 1; version <= currentVersion; ++version)
    {
        SCOPED_TRACE(version);
        auto incomplete = current;
        incomplete["version"] = version;
        incomplete["materials"][0].erase("ignoresVertexColor");
        std::ofstream(file, std::ios::binary | std::ios::trunc) << incomplete.dump();
        EXPECT_FALSE(ReadImportedMaterialCache(file).has_value());
    }
    auto invalid = current;
    invalid["materials"][0]["ignoresVertexColor"] = "true";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << invalid.dump();
    EXPECT_FALSE(ReadImportedMaterialCache(file).has_value());
    std::error_code ec;
    fs::remove_all(root, ec);
}
