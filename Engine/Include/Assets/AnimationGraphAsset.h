#pragma once

#include "AssetCore/Asset.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine {

// Pose-graph asset (.animgraph JSON). Nested AnimationGraphSerializer
// documents use `rootNode`. Authoring Graph::Model documents use `nodes`
// (compiled at instantiate when `rootNode` is absent).
class AnimationGraphAsset : public Asset
{
public:
    AnimationGraphAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::AnimationGraph, path) {}

    bool Load() override
    {
        std::ifstream file(GetPath());
        if (!file.is_open())
            return false;
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
