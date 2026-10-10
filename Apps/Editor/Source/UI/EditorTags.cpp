#include "UI/EditorTags.h"

#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"

namespace GameEngine {

std::vector<EditorTagDefinition> EditorTags::GetDefaults()
{
    // Nine colors maximally separated in hue, brighter variants for visibility.
    return {
        {"Red", "#FF5252"},
        {"Orange", "#FF7043"},
        {"Yellow", "#FFEE58"},
        {"Green", "#66BB6A"},
        {"Teal", "#26A69A"},
        {"Blue", "#42A5F5"},
        {"Indigo", "#7986CB"},
        {"Purple", "#BA68C8"},
        {"Pink", "#F06292"},
    };
}

std::vector<EditorTagDefinition> EditorTags::Load()
{
    const std::filesystem::path& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return GetDefaults();

    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    if (!store.Load(&err))
        return GetDefaults();

    const nlohmann::json& root = store.Json();
    auto it = root.find("tags");
    if (it == root.end() || !it->is_array() || it->empty())
        return GetDefaults();

    std::vector<EditorTagDefinition> out;
    out.reserve(it->size());
    for (const auto& el : *it)
    {
        if (!el.is_object())
            continue;
        EditorTagDefinition def;
        if (el.contains("name") && el["name"].is_string())
            def.Name = el["name"].get<std::string>();
        else
            continue;
        if (el.contains("color") && el["color"].is_string())
            def.Color = el["color"].get<std::string>();
        out.push_back(std::move(def));
    }
    if (out.empty())
        return GetDefaults();
    return out;
}

bool EditorTags::Save(const std::vector<EditorTagDefinition>& tags)
{
    const std::filesystem::path& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return false;

    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    store.Load(&err);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tags)
    {
        nlohmann::json obj;
        obj["name"] = t.Name;
        obj["color"] = t.Color;
        arr.push_back(std::move(obj));
    }
    store.SetJson("tags", arr);
    return store.Save(&err);
}

} // namespace GameEngine
