#pragma once

#include "AssetCore/Asset.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine {

// Lane-based clip arrangement asset (.clipset JSON).
// Stores the parsed JSON document; consumers (e.g. AnimationWindowPanel) own
// the schema and convert to/from their own model types. See TimelineAsset
// for the same pattern applied to multi-track timelines.
class ClipSetAsset : public Asset
{
public:
    ClipSetAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::ClipSet, path) {}

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
