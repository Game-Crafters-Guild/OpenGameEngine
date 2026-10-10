#pragma once

#include "AssetCore/Asset.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine {

// Serialize a MaterialDocument to JSON (schemaVersion 3).
nlohmann::json SerializeMaterialDocument(const MaterialDocument& doc);

// Text-based material asset (.material JSON, schemaVersion 2 or 3).
class MaterialAsset : public Asset
{
public:
    MaterialAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Material, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const MaterialDocument& GetDocument() const { return m_Doc; }
    const std::vector<std::string>& GetErrors() const { return m_Errors; }

private:
    bool ParseFromText(const std::string& text, const std::filesystem::path& sourcePathForRelativeErrors);

private:
    MaterialDocument m_Doc{};
    std::vector<std::string> m_Errors;
};

} // namespace GameEngine

