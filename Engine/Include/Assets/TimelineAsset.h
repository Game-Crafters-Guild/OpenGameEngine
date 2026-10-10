#pragma once

#include "AssetCore/Asset.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine {

// Composite multi-track timeline asset (.timeline JSON).
// Stores the parsed JSON document; consumers (e.g. AnimationWindowPanel) own
// the schema and convert to/from their own model types. Mirrors how
// MaterialAsset stores a parsed MaterialDocument: parser creates the asset,
// AssetManager calls Load(), Load() reads the file into m_Doc.
class TimelineAsset : public Asset
{
public:
    TimelineAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Timeline, path) {}

    bool Load() override
    {
        std::ifstream file(GetPath());
        if (!file.is_open()) return false;
        std::stringstream ss;
        ss << file.rdbuf();
        return ParseJson(ss.str());
    }

    bool LoadFromData(const Vector<uint8>& data) override
    {
        return ParseJson(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    }

    void Unload() override
    {
        m_Doc = nlohmann::json{};
    }

    const nlohmann::json& GetDocument() const { return m_Doc; }
    void SetDocument(nlohmann::json doc) { m_Doc = std::move(doc); }

private:
    bool ParseJson(const std::string& text)
    {
        try
        {
            m_Doc = nlohmann::json::parse(text);
            return m_Doc.is_object();
        }
        catch (const std::exception&)
        {
            m_Doc = nlohmann::json{};
            return false;
        }
    }

    nlohmann::json m_Doc;
};

} // namespace GameEngine
