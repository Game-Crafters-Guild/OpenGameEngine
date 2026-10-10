#include "Assets/ImportedMaterialCache.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ModelAsset.h"
#include "FileSystem/FileSystem.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>

namespace GameEngine {

namespace {

// Bump when the record layout or what a field means changes; a reader rejects
// any other version and the next model load rewrites the record. Version 3
// preserves the imported material's vertex-color policy for project warm-up.
constexpr int kImportedMaterialCacheVersion = 3;
constexpr const char* kImportedMaterialCacheDirectory = "ModelMaterials";

// JSON has no NaN or infinity (a writer emits null), so a non-finite value is
// stored by name; otherwise its record could never be read back and the model's
// materials would stay unwarmed.
nlohmann::json FloatToJson(float value)
{
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value > 0.0f ? "inf" : "-inf";
    return value;
}

float FloatFromJson(const nlohmann::json& value)
{
    if (!value.is_string())
        return value.get<float>();
    const std::string name = value.get<std::string>();
    if (name == "nan")
        return std::numeric_limits<float>::quiet_NaN();
    if (name == "inf")
        return std::numeric_limits<float>::infinity();
    if (name == "-inf")
        return -std::numeric_limits<float>::infinity();
    throw std::runtime_error("imported material cache: unknown float value");
}

nlohmann::json FloatsToJson(std::span<const float> values)
{
    nlohmann::json list = nlohmann::json::array();
    for (const float value : values)
        list.push_back(FloatToJson(value));
    return list;
}

void FloatsFromJson(const nlohmann::json& value, std::span<float> out)
{
    if (!value.is_array() || value.size() != out.size())
        throw std::runtime_error("imported material cache: array length mismatch");
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = FloatFromJson(value[i]);
}

nlohmann::json MaterialToJson(const ImportedMaterialData& material)
{
    return {
        {"name", material.Name},
        {"diffuseTexture", material.DiffuseTexture},
        {"normalTexture", material.NormalTexture},
        {"specularTexture", material.SpecularTexture},
        {"emissiveTexture", material.EmissiveTexture},
        {"occlusionTexture", material.OcclusionTexture},
        {"roughnessTexture", material.RoughnessTexture},
        {"metallicTexture", material.MetallicTexture},
        {"diffuseTextureTransform", FloatsToJson(material.DiffuseTextureTransform)},
        {"normalTextureTransform", FloatsToJson(material.NormalTextureTransform)},
        {"specularTextureTransform", FloatsToJson(material.SpecularTextureTransform)},
        {"emissiveTextureTransform", FloatsToJson(material.EmissiveTextureTransform)},
        {"occlusionTextureTransform", FloatsToJson(material.OcclusionTextureTransform)},
        {"roughnessTextureTransform", FloatsToJson(material.RoughnessTextureTransform)},
        {"metallicTextureTransform", FloatsToJson(material.MetallicTextureTransform)},
        {"diffuseColor", FloatsToJson(material.DiffuseColor)},
        {"specularColor", FloatsToJson(material.SpecularColor)},
        {"emissiveColor", FloatsToJson(material.EmissiveColor)},
        {"shininess", FloatToJson(material.Shininess)},
        {"metallic", FloatToJson(material.Metallic)},
        {"roughness", FloatToJson(material.Roughness)},
        {"doubleSided", material.DoubleSided},
        {"alphaMode", static_cast<uint32>(material.AlphaMode)},
        {"alphaCutoff", FloatToJson(material.AlphaCutoff)},
        {"alphaModeInferred", material.AlphaModeInferred},
        {"ignoresVertexColor", material.IgnoresVertexColor},
    };
}

ImportedMaterialData MaterialFromJson(const nlohmann::json& value)
{
    ImportedMaterialData material{};
    material.Name = value.at("name").get<String>();
    material.DiffuseTexture = value.at("diffuseTexture").get<String>();
    material.NormalTexture = value.at("normalTexture").get<String>();
    material.SpecularTexture = value.at("specularTexture").get<String>();
    material.EmissiveTexture = value.at("emissiveTexture").get<String>();
    material.OcclusionTexture = value.at("occlusionTexture").get<String>();
    material.RoughnessTexture = value.at("roughnessTexture").get<String>();
    material.MetallicTexture = value.at("metallicTexture").get<String>();
    FloatsFromJson(value.at("diffuseTextureTransform"), material.DiffuseTextureTransform);
    FloatsFromJson(value.at("normalTextureTransform"), material.NormalTextureTransform);
    FloatsFromJson(value.at("specularTextureTransform"), material.SpecularTextureTransform);
    FloatsFromJson(value.at("emissiveTextureTransform"), material.EmissiveTextureTransform);
    FloatsFromJson(value.at("occlusionTextureTransform"), material.OcclusionTextureTransform);
    FloatsFromJson(value.at("roughnessTextureTransform"), material.RoughnessTextureTransform);
    FloatsFromJson(value.at("metallicTextureTransform"), material.MetallicTextureTransform);
    FloatsFromJson(value.at("diffuseColor"), material.DiffuseColor);
    FloatsFromJson(value.at("specularColor"), material.SpecularColor);
    FloatsFromJson(value.at("emissiveColor"), material.EmissiveColor);
    material.Shininess = FloatFromJson(value.at("shininess"));
    material.Metallic = FloatFromJson(value.at("metallic"));
    material.Roughness = FloatFromJson(value.at("roughness"));
    material.DoubleSided = value.at("doubleSided").get<bool>();
    const uint32 alphaMode = value.at("alphaMode").get<uint32>();
    if (alphaMode > static_cast<uint32>(AlphaMode::Blend))
        throw std::runtime_error("imported material cache: unknown alpha mode");
    material.AlphaMode = static_cast<AlphaMode>(alphaMode);
    material.AlphaCutoff = FloatFromJson(value.at("alphaCutoff"));
    material.AlphaModeInferred = value.at("alphaModeInferred").get<bool>();
    material.IgnoresVertexColor = value.at("ignoresVertexColor").get<bool>();
    return material;
}

String SerializeMaterials(const Vector<ImportedMaterialData>& materials)
{
    nlohmann::json list = nlohmann::json::array();
    for (const ImportedMaterialData& material : materials)
        list.push_back(MaterialToJson(material));
    const nlohmann::json record = {
        {"version", kImportedMaterialCacheVersion},
        {"materials", std::move(list)},
    };
    return record.dump();
}

} // namespace

std::optional<std::filesystem::path> ImportedMaterialCacheFile(const AssetRegistry& registry,
                                                               const std::filesystem::path& modelPath,
                                                               const GUID& modelGuid)
{
    if (modelGuid.IsNull() || !registry.AcceptsDerivedRecords(modelPath))
        return std::nullopt;
    const std::optional<std::filesystem::path> cacheRoot = registry.TryGetCacheRoot(modelPath);
    if (!cacheRoot)
        return std::nullopt;
    return *cacheRoot / kImportedMaterialCacheDirectory / (modelGuid.ToString() + ".json");
}

ImportedMaterialCacheWrite WriteImportedMaterialCache(const std::filesystem::path& file,
                                                      const Vector<ImportedMaterialData>& materials)
{
    const String contents = SerializeMaterials(materials);
    String existing;
    if (ReadFileTextShared(file, existing) && existing == contents)
        return ImportedMaterialCacheWrite::Unchanged;

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temp = FileSystem::MakeTemporarySiblingPath(file);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return ImportedMaterialCacheWrite::Failed;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!out)
            return ImportedMaterialCacheWrite::Failed;
    }
    return FileSystem::PublishFile(temp, file) ? ImportedMaterialCacheWrite::Written
                                               : ImportedMaterialCacheWrite::Failed;
}

void RemoveImportedMaterialCache(const AssetRegistry& registry, const std::filesystem::path& modelPath,
                                 const GUID& modelGuid)
{
    if (const auto file = ImportedMaterialCacheFile(registry, modelPath, modelGuid))
    {
        std::error_code ec;
        std::filesystem::remove(*file, ec);
    }
}

std::optional<Vector<ImportedMaterialData>> ReadImportedMaterialCache(const std::filesystem::path& file)
{
    String text;
    if (!ReadFileTextShared(file, text) || text.empty())
        return std::nullopt;
    try
    {
        const nlohmann::json record = nlohmann::json::parse(text);
        if (record.at("version").get<int>() != kImportedMaterialCacheVersion)
            return std::nullopt;
        Vector<ImportedMaterialData> materials;
        for (const nlohmann::json& value : record.at("materials"))
            materials.push_back(MaterialFromJson(value));
        return materials;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace GameEngine
