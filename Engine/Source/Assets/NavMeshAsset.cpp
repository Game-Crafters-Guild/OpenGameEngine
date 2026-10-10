#include "Assets/NavMeshAsset.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace GameEngine {

NavMeshAsset::NavMeshAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::NavigationMesh, path)
{
}

NavMeshAsset::~NavMeshAsset()
{
    Unload();
}

bool NavMeshAsset::Load()
{
    if (GetState() == AssetState::Loaded)
    {
        return true;
    }

    SetState(AssetState::Loading);

    if (!Exists())
    {
        Logger::Log::Error("NavMesh file does not exist: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    Vector<uint8> fileData;
    if (!ReadFileBytesShared(GetPath(), fileData))
    {
        Logger::Log::Error("Failed to read NavMesh file: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    return LoadFromData(fileData);
}

bool NavMeshAsset::LoadFromData(const Vector<uint8>& data)
{
    if (GetState() == AssetState::Loaded)
    {
        return true;
    }

    SetState(AssetState::Loading);

    if (data.empty())
    {
        Logger::Log::Error("Empty NavMesh data provided");
        SetState(AssetState::Failed);
        return false;
    }

    try
    {
        const auto json = nlohmann::json::parse(data.begin(), data.end());

        const uint32 version = json.value("version", 0u);
        if (version != 1)
        {
            Logger::Log::Error("NavMesh unsupported version: {}", version);
            SetState(AssetState::Failed);
            return false;
        }

        if (!json.contains("settings") || !json.at("settings").is_object())
        {
            Logger::Log::Error("NavMesh JSON missing or invalid 'settings' object");
            SetState(AssetState::Failed);
            return false;
        }
        const auto& settings = json.at("settings");
        m_Settings.CellSize = settings.value("cellSize", 0.3f);
        m_Settings.CellHeight = settings.value("cellHeight", 0.2f);
        m_Settings.AgentRadius = settings.value("agentRadius", 0.6f);
        m_Settings.AgentHeight = settings.value("agentHeight", 2.0f);
        m_Settings.AgentMaxClimb = settings.value("agentMaxClimb", 0.9f);
        m_Settings.AgentMaxSlope = settings.value("agentMaxSlope", 45.0f);
        m_Settings.RegionMinSize = settings.value("regionMinSize", 8.0f);
        m_Settings.RegionMergeSize = settings.value("regionMergeSize", 20.0f);
        m_Settings.EdgeMaxLen = settings.value("edgeMaxLen", 12.0f);
        m_Settings.EdgeMaxError = settings.value("edgeMaxError", 1.3f);
        m_Settings.DetailSampleDist = settings.value("detailSampleDist", 6.0f);
        m_Settings.DetailSampleMaxError = settings.value("detailSampleMaxError", 1.0f);
        m_Settings.VertsPerPoly = settings.value("vertsPerPoly", 6);

        m_SourceGeometryGUIDs.clear();
        if (json.contains("sourceGeometry"))
        {
            for (const auto& guidStr : json.at("sourceGeometry"))
            {
                m_SourceGeometryGUIDs.emplace_back(GUID(guidStr.get<std::string>()));
            }
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        Logger::Log::Error("NavMesh JSON parse error: {}", e.what());
        SetState(AssetState::Failed);
        return false;
    }

    SetState(AssetState::Loaded);
    Logger::Log::Info("NavMesh loaded: {} ({} source geometries, cellSize={})",
                      GetName(), m_SourceGeometryGUIDs.size(), m_Settings.CellSize);
    return true;
}

void NavMeshAsset::Unload()
{
    m_Settings = Pathfinding::NavMeshSettings{};
    m_SourceGeometryGUIDs.clear();
    SetState(AssetState::Unloaded);
}

const Pathfinding::NavMeshSettings& NavMeshAsset::GetSettings() const
{
    return m_Settings;
}

const std::vector<GUID>& NavMeshAsset::GetSourceGeometryGUIDs() const
{
    return m_SourceGeometryGUIDs;
}

bool NavMeshAsset::SaveToFile(const std::filesystem::path& path,
                              const Pathfinding::NavMeshSettings& settings,
                              const std::vector<GUID>& geometryGuids)
{
    nlohmann::json json;
    json["version"] = 1;

    nlohmann::json settingsJson;
    settingsJson["cellSize"] = settings.CellSize;
    settingsJson["cellHeight"] = settings.CellHeight;
    settingsJson["agentRadius"] = settings.AgentRadius;
    settingsJson["agentHeight"] = settings.AgentHeight;
    settingsJson["agentMaxClimb"] = settings.AgentMaxClimb;
    settingsJson["agentMaxSlope"] = settings.AgentMaxSlope;
    settingsJson["regionMinSize"] = settings.RegionMinSize;
    settingsJson["regionMergeSize"] = settings.RegionMergeSize;
    settingsJson["edgeMaxLen"] = settings.EdgeMaxLen;
    settingsJson["edgeMaxError"] = settings.EdgeMaxError;
    settingsJson["detailSampleDist"] = settings.DetailSampleDist;
    settingsJson["detailSampleMaxError"] = settings.DetailSampleMaxError;
    settingsJson["vertsPerPoly"] = settings.VertsPerPoly;
    json["settings"] = settingsJson;

    nlohmann::json geometryArray = nlohmann::json::array();
    for (const auto& guid : geometryGuids)
    {
        geometryArray.push_back(guid.ToString());
    }
    json["sourceGeometry"] = geometryArray;

    auto parentDir = path.parent_path();
    if (!parentDir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parentDir, ec);
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        Logger::Log::Error("NavMesh SaveToFile: failed to open '{}'", path.string());
        return false;
    }

    file << json.dump(4);

    if (!file.good())
    {
        Logger::Log::Error("NavMesh SaveToFile: write error for '{}'", path.string());
        return false;
    }

    Logger::Log::Info("NavMesh saved: {} ({} source geometries)", path.string(), geometryGuids.size());
    return true;
}

} // namespace GameEngine
