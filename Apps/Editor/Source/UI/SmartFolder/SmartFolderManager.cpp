#include "UI/SmartFolder/SmartFolderManager.h"
#include "Editor/EditorPaths.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>
#include <algorithm>

namespace GameEngine {

namespace {

nlohmann::json FilterToJson(const SmartFolderFilter& filter)
{
    nlohmann::json j;
    switch (filter.FilterType) {
        case SmartFolderFilter::Type::FileType: j["type"] = "FileType"; break;
        case SmartFolderFilter::Type::NameContains: j["type"] = "NameContains"; break;
        case SmartFolderFilter::Type::Regex: j["type"] = "Regex"; break;
        case SmartFolderFilter::Type::Tag: j["type"] = "Tag"; break;
        case SmartFolderFilter::Type::VCSStatus: j["type"] = "VCSStatus"; break;
    }
    j["value"] = filter.Value;
    return j;
}

SmartFolderFilter FilterFromJson(const nlohmann::json& j)
{
    SmartFolderFilter filter;
    std::string typeStr = j.value("type", "NameContains");
    if (typeStr == "FileType") filter.FilterType = SmartFolderFilter::Type::FileType;
    else if (typeStr == "NameContains") filter.FilterType = SmartFolderFilter::Type::NameContains;
    else if (typeStr == "Regex") filter.FilterType = SmartFolderFilter::Type::Regex;
    else if (typeStr == "Tag") filter.FilterType = SmartFolderFilter::Type::Tag;
    else if (typeStr == "VCSStatus") filter.FilterType = SmartFolderFilter::Type::VCSStatus;
    filter.Value = j.value("value", "");
    return filter;
}

nlohmann::json SmartFolderToJson(const SmartFolder& folder)
{
    nlohmann::json j;
    j["id"] = folder.Id;
    j["name"] = folder.Name;
    j["directoryScope"] = folder.DirectoryScope.string();
    j["combineMode"] = (folder.CombineMode == FilterCombineMode::And) ? "And" : "Or";
    j["isGlobal"] = folder.IsGlobal;
    
    nlohmann::json filters = nlohmann::json::array();
    for (const auto& f : folder.Filters) {
        filters.push_back(FilterToJson(f));
    }
    j["filters"] = filters;
    
    return j;
}

SmartFolder SmartFolderFromJson(const nlohmann::json& j, bool isGlobal)
{
    SmartFolder folder;
    folder.Id = j.value("id", GenerateSmartFolderId());
    folder.Name = j.value("name", "Smart Folder");
    folder.DirectoryScope = j.value("directoryScope", "");
    folder.IsGlobal = j.value("isGlobal", isGlobal);
    
    std::string combineStr = j.value("combineMode", "Or");
    folder.CombineMode = (combineStr == "And") ? FilterCombineMode::And : FilterCombineMode::Or;
    
    if (j.contains("filters") && j["filters"].is_array()) {
        for (const auto& fj : j["filters"]) {
            folder.Filters.push_back(FilterFromJson(fj));
        }
    }
    
    return folder;
}

} // anonymous namespace

SmartFolderManager::SmartFolderManager() = default;
SmartFolderManager::~SmartFolderManager() = default;

void SmartFolderManager::SetProjectRoot(const std::filesystem::path& projectRoot)
{
    m_ProjectRoot = projectRoot;
}

std::filesystem::path SmartFolderManager::GetGlobalStoragePath() const
{
    auto global = Editor::GetEditorGlobalPaths();
    return global.userDataRoot / "SmartFolders.json";
}

std::filesystem::path SmartFolderManager::GetProjectStoragePath() const
{
    // Prefer using the standardized Editor project paths API
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();

    if (!projectPaths.projectEditorRoot.empty()) {
        auto path = projectPaths.projectEditorRoot / "SmartFolders.json";
        return path;
    }
    
    // Fallback to manually set project root
    if (m_ProjectRoot.empty()) {
        return {};
    }
    auto path = m_ProjectRoot / ".Editor" / "SmartFolders.json";
    return path;
}

void SmartFolderManager::Load()
{
    m_SmartFolders.clear();
    
    // Load global smart folders
    auto globalPath = GetGlobalStoragePath();
    LoadFromFile(globalPath, true);
    
    // Load project smart folders
    auto projectPath = GetProjectStoragePath();
    if (!projectPath.empty()) {
        LoadFromFile(projectPath, false);
    }
    
    NotifyChanged();
}

void SmartFolderManager::LoadFromFile(const std::filesystem::path& path, bool isGlobal)
{
    if (path.empty()) {
        return;
    }
    
    Editor::SettingsStore store(path);
    std::string err;
    if (!store.Load(&err)) {
        // Normal case for new projects/users; keep it quiet unless there's a hard failure to parse.
        return;
    }
    
    try {
        const auto& root = store.Json();
        if (root.contains("smartFolders") && root["smartFolders"].is_array()) {
            for (const auto& j : root["smartFolders"]) {
                SmartFolder folder = SmartFolderFromJson(j, isGlobal);
                folder.IsGlobal = isGlobal; // Override with actual storage location
                m_SmartFolders.push_back(std::move(folder));
            }
        }
    }
    catch (const std::exception& e) {
        Logger::Log::Warning("[SmartFolderManager] Failed to parse {}: {}", path.string(), e.what());
    }
}

void SmartFolderManager::Save()
{
    // Save global smart folders
    auto globalPath = GetGlobalStoragePath();
    SaveToFile(globalPath, true);
    
    // Save project smart folders
    auto projectPath = GetProjectStoragePath();
    if (!projectPath.empty()) {
        SaveToFile(projectPath, false);
    }
}

void SmartFolderManager::SaveToFile(const std::filesystem::path& path, bool isGlobal) const
{
    if (path.empty()) {
        Logger::Log::Warning("[SmartFolderManager] SaveToFile called with empty path");
        return;
    }
    
    Editor::SettingsStore store(path);
    
    // Build smartFolders array
    nlohmann::json foldersArray = nlohmann::json::array();
    int count = 0;
    for (const auto& folder : m_SmartFolders) {
        if (folder.IsGlobal == isGlobal) {
            foldersArray.push_back(SmartFolderToJson(folder));
            count++;
        }
    }
    
    store.SetJson("smartFolders", foldersArray);

    std::string err;
    if (!store.Save(&err)) {
        Logger::Log::Warning("[SmartFolderManager] Failed to save {}: {}", path.string(), err);
    }
}

SmartFolder* SmartFolderManager::GetById(const std::string& id)
{
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&id](const SmartFolder& f) { return f.Id == id; });
    return (it != m_SmartFolders.end()) ? &(*it) : nullptr;
}

const SmartFolder* SmartFolderManager::GetById(const std::string& id) const
{
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&id](const SmartFolder& f) { return f.Id == id; });
    return (it != m_SmartFolders.end()) ? &(*it) : nullptr;
}

std::string SmartFolderManager::Create(const std::string& name, bool isGlobal)
{
    SmartFolder folder;
    folder.Id = GenerateSmartFolderId();
    folder.Name = name;
    folder.IsGlobal = isGlobal;
    folder.CombineMode = FilterCombineMode::Or;

    m_SmartFolders.push_back(std::move(folder));
    Save();
    NotifyChanged();

    return m_SmartFolders.back().Id;
}

void SmartFolderManager::Update(const SmartFolder& folder)
{
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&folder](const SmartFolder& f) { return f.Id == folder.Id; });
    
    if (it != m_SmartFolders.end()) {
        *it = folder;
        Save();
        NotifyChanged();
    }
}

void SmartFolderManager::Delete(const std::string& id)
{
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&id](const SmartFolder& f) { return f.Id == id; });
    
    if (it != m_SmartFolders.end()) {
        m_SmartFolders.erase(it);
        Save();
        NotifyChanged();
    }
}

void SmartFolderManager::NotifyChanged()
{
    if (m_OnChanged) {
        m_OnChanged();
    }
}

// Low-level methods for undo/redo (don't trigger OnChanged callback)

void SmartFolderManager::AddFolder(const SmartFolder& folder)
{
    // Check if folder with this ID already exists
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&folder](const SmartFolder& f) { return f.Id == folder.Id; });
    
    if (it == m_SmartFolders.end()) {
        m_SmartFolders.push_back(folder);
        Save();
    }
}

void SmartFolderManager::RemoveFolder(const std::string& id)
{
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&id](const SmartFolder& f) { return f.Id == id; });
    
    if (it != m_SmartFolders.end()) {
        m_SmartFolders.erase(it);
        Save();
    }
}

void SmartFolderManager::UpdateFolder(const SmartFolder& folder)
{
    auto it = std::find_if(m_SmartFolders.begin(), m_SmartFolders.end(),
        [&folder](const SmartFolder& f) { return f.Id == folder.Id; });
    
    if (it != m_SmartFolders.end()) {
        *it = folder;
        Save();
    }
}

} // namespace GameEngine
