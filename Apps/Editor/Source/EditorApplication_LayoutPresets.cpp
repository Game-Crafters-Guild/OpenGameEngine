#include "EditorApplication.h"

#include "Core/Engine.h"
#include "Docking/LayoutPresetController.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorDockNodeJson.h"
#include "Panels/RenameLayoutModal.h"
#include "Panels/SaveSceneChangesModal.h"
#include "Platform/Shell.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/LayoutPresetToolbar.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace GameEngine
{

void EditorApplication::RenameLayoutPreset(int index, const std::string& newName)
{
    if (index <= 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;

    std::string trimmed = newName;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    if (trimmed.empty())
        return;
    if (m_LayoutPresets[(size_t)index].name == trimmed)
        return;

    m_LayoutPresets[(size_t)index].name = trimmed;
    SyncLayoutPresetToolbar();
    SaveLayoutPresetsToPreferences();
}

void EditorApplication::PromptRenameLayoutPreset(int index)
{
    if (index <= 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;

    if (!m_RenameLayoutModal)
    {
        if (m_Windows.empty() || !m_Windows[0] || !m_Windows[0]->ui)
            return;
        UIElement* rootEl = m_Windows[0]->ui->GetRootElement();
        if (!rootEl)
            return;
        auto modal = std::make_unique<RenameLayoutModal>();
        m_RenameLayoutModal = modal.get();
        rootEl->AddChild(std::move(modal));
    }

    const std::string currentName = m_LayoutPresets[(size_t)index].name;
    m_RenameLayoutModal->SetOnCommit([this, index](const std::string& newName)
                                     { this->RenameLayoutPreset(index, newName); });
    m_RenameLayoutModal->SetOnCancel([]() {});
    m_RenameLayoutModal->Show("Rename Layout", currentName);
}

bool EditorApplication::ExportLayoutPresetToFile(int index, const std::filesystem::path& path) const
{
    if (index < 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return false;
    const LayoutPreset& p = m_LayoutPresets[(size_t)index];
    if (!p.layout)
        return false;

    nlohmann::json entry = nlohmann::json::object();
    entry["name"] = p.name;
    entry["layout"] = DockNodeToJson(p.layout.get());

    nlohmann::json root = nlohmann::json::object();
    root["schemaVersion"] = 1;
    root["presets"] = nlohmann::json::array({entry});

    std::ofstream out(path);
    if (!out.is_open())
        return false;
    out << root.dump(2);
    return out.good();
}

bool EditorApplication::ExportAllLayoutPresetsToFile(const std::filesystem::path& path) const
{
    nlohmann::json presets = nlohmann::json::array();
    for (size_t i = 1; i < m_LayoutPresets.size(); ++i)
    {
        const LayoutPreset& p = m_LayoutPresets[i];
        if (p.name.empty() || !p.layout)
            continue;
        nlohmann::json entry = nlohmann::json::object();
        entry["name"] = p.name;
        entry["layout"] = DockNodeToJson(p.layout.get());
        presets.push_back(std::move(entry));
    }

    nlohmann::json root = nlohmann::json::object();
    root["schemaVersion"] = 1;
    root["presets"] = std::move(presets);

    std::ofstream out(path);
    if (!out.is_open())
        return false;
    out << root.dump(2);
    return out.good();
}

int EditorApplication::ImportLayoutPresetsFromFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return 0;

    nlohmann::json root;
    try
    {
        in >> root;
    }
    catch (...)
    {
        return 0;
    }

    const auto itPresets = root.find("presets");
    if (itPresets == root.end() || !itPresets->is_array())
        return 0;

    int imported = 0;
    for (const auto& p : *itPresets)
    {
        if (!p.is_object())
            continue;
        const auto itName = p.find("name");
        const auto itLayout = p.find("layout");
        if (itName == p.end() || !itName->is_string())
            continue;
        if (itLayout == p.end() || !itLayout->is_object())
            continue;

        std::unique_ptr<DockNode> layout = DockNodeFromJson(*itLayout);
        if (!layout)
            continue;

        std::string name = itName->get<std::string>();
        std::string unique = name;
        int suffix = 2;
        auto nameExists = [this](const std::string& n)
        {
            for (const auto& existing : m_LayoutPresets)
                if (existing.name == n)
                    return true;
            return false;
        };
        while (nameExists(unique))
        {
            unique = name + " (" + std::to_string(suffix) + ")";
            ++suffix;
        }

        LayoutPreset lp;
        lp.name = std::move(unique);
        lp.layout = std::move(layout);
        m_LayoutPresets.emplace_back(std::move(lp));
        ++imported;
    }

    if (imported > 0)
    {
        SyncLayoutPresetToolbar();
        SaveLayoutPresetsToPreferences();
    }
    return imported;
}

void EditorApplication::ExportLayoutPresetInteractive(int index)
{
    if (index < 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;
    const std::string stem = m_LayoutPresets[(size_t)index].name.empty()
                                 ? std::string("Layout")
                                 : m_LayoutPresets[(size_t)index].name;
    const std::filesystem::path initial = stem + ".layout.json";
    const std::filesystem::path picked =
        Platform::SaveFile(initial, "Layout Preset", "*.layout.json");
    if (!picked.empty())
        ExportLayoutPresetToFile(index, picked);
}

void EditorApplication::ExportAllLayoutPresetsInteractive()
{
    const std::filesystem::path initial = "Layouts.layouts.json";
    const std::filesystem::path picked =
        Platform::SaveFile(initial, "Layout Presets", "*.layouts.json");
    if (!picked.empty())
        ExportAllLayoutPresetsToFile(picked);
}

void EditorApplication::ImportLayoutPresetsInteractive()
{
    const std::filesystem::path picked =
        Platform::SelectFile({}, "Layout Presets", "*.layout.json;*.layouts.json;*.json");
    if (!picked.empty())
        ImportLayoutPresetsFromFile(picked);
}

void EditorApplication::InitializeLayoutPresets()
{
    m_LayoutPresets.clear();

    // Prefer the editor-authored default docking tree (parsed from layout.uxml).
    if (m_DefaultDockLayout)
    {
        LayoutPreset preset;
        preset.name = "Default";
        preset.layout = CloneDockNode(m_DefaultDockLayout.get());
        m_LayoutPresets.emplace_back(std::move(preset));
        m_ActiveLayoutPresetIndex = 0;
        return;
    }

    // Fallback: capture current docking root if available.
    if (m_Docking && m_Docking->GetRoot())
    {
        LayoutPreset preset;
        preset.name = "Default";
        preset.layout = CloneDockNode(m_Docking->GetRoot());
        m_LayoutPresets.emplace_back(std::move(preset));
        m_ActiveLayoutPresetIndex = 0;
        return;
    }

    m_ActiveLayoutPresetIndex = -1;
}

namespace
{
static const char* DockPositionToString(DockPosition p)
{
    switch (p)
    {
    case DockPosition::Left:
        return "Left";
    case DockPosition::Right:
        return "Right";
    case DockPosition::Top:
        return "Top";
    case DockPosition::Bottom:
        return "Bottom";
    case DockPosition::Center:
        return "Center";
    default:
        return "Left";
    }
}

static bool TryParseDockPosition(const nlohmann::json& j, DockPosition& out)
{
    if (!j.is_string())
        return false;
    const std::string s = j.get<std::string>();
    if (s == "Left" || s == "left")
    {
        out = DockPosition::Left;
        return true;
    }
    if (s == "Right" || s == "right")
    {
        out = DockPosition::Right;
        return true;
    }
    if (s == "Top" || s == "top")
    {
        out = DockPosition::Top;
        return true;
    }
    if (s == "Bottom" || s == "bottom")
    {
        out = DockPosition::Bottom;
        return true;
    }
    if (s == "Center" || s == "center")
    {
        out = DockPosition::Center;
        return true;
    }
    return false;
}

} // namespace

nlohmann::json DockNodeToJson(const DockNode* n)
{
    if (!n)
        return nlohmann::json();

    if (n->IsLeaf())
    {
        nlohmann::json j;
        j["kind"] = "leaf";
        j["tabs"] = nlohmann::json::array();
        for (const auto& t : n->GetTabs())
            j["tabs"].push_back(t);
        const std::string active = n->GetActivePanelId();
        if (!active.empty())
            j["active"] = active;
        return j;
    }

    // Split
    nlohmann::json j;
    j["kind"] = "split";
    j["dir"] = DockPositionToString(n->GetSplitDirection());
    j["ratio"] = n->GetSplitRatio();
    j["minFirstPx"] = n->GetMinFirstPx();
    j["minSecondPx"] = n->GetMinSecondPx();
    j["first"] = DockNodeToJson(n->First());
    j["second"] = DockNodeToJson(n->Second());
    return j;
}

std::unique_ptr<DockNode> DockNodeFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return nullptr;

    const auto itKind = j.find("kind");
    if (itKind == j.end() || !itKind->is_string())
        return nullptr;
    const std::string kind = itKind->get<std::string>();

    if (kind == "leaf")
    {
        auto leaf = DockNode::MakeLeaf();
        const auto itTabs = j.find("tabs");
        if (itTabs != j.end() && itTabs->is_array())
        {
            for (const auto& t : *itTabs)
            {
                if (t.is_string())
                    leaf->AddTab(t.get<std::string>());
            }
        }
        const auto itActive = j.find("active");
        if (itActive != j.end() && itActive->is_string())
        {
            (void)leaf->ActivateTab(itActive->get<std::string>());
        }
        return leaf;
    }

    if (kind == "split")
    {
        DockPosition dir = DockPosition::Left;
        const auto itDir = j.find("dir");
        (void)TryParseDockPosition(itDir != j.end() ? *itDir : nlohmann::json(), dir);

        float ratio = 0.5f;
        const auto itRatio = j.find("ratio");
        if (itRatio != j.end() && itRatio->is_number())
            ratio = itRatio->get<float>();

        auto first = DockNodeFromJson(j.value("first", nlohmann::json()));
        auto second = DockNodeFromJson(j.value("second", nlohmann::json()));
        if (!first || !second)
            return nullptr;

        auto split = std::make_unique<DockNode>();
        split->SetSplit(dir, ratio, std::move(first), std::move(second));

        float minFirst = 100.0f;
        float minSecond = 100.0f;
        const auto itMinFirst = j.find("minFirstPx");
        const auto itMinSecond = j.find("minSecondPx");
        if (itMinFirst != j.end() && itMinFirst->is_number())
            minFirst = itMinFirst->get<float>();
        if (itMinSecond != j.end() && itMinSecond->is_number())
            minSecond = itMinSecond->get<float>();
        split->SetMinChildSizes(minFirst, minSecond);

        return split;
    }

    return nullptr;
}


void EditorApplication::LoadLayoutPresetsFromPreferences()
{
    // Load persisted presets from the project folder so they travel with the
    // project (checked into VCS alongside other project settings). Default
    // preset (index 0) always comes from layout.uxml / captured default.
    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return;
    GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)prefs.Load(&err); // missing file is OK

    const auto& root = prefs.Json();
    const auto it = root.find("ui.layoutPresets");
    if (it == root.end() || !it->is_object())
        return;

    const nlohmann::json& obj = *it;

    // Append user presets (skip index 0).
    const auto itPresets = obj.find("presets");
    if (itPresets != obj.end() && itPresets->is_array())
    {
        for (const auto& p : *itPresets)
        {
            if (!p.is_object())
                continue;

            const auto itName = p.find("name");
            const auto itLayout = p.find("layout");
            if (itName == p.end() || !itName->is_string())
                continue;
            if (itLayout == p.end() || !itLayout->is_object())
                continue;

            std::unique_ptr<DockNode> layout = DockNodeFromJson(*itLayout);
            if (!layout)
                continue;

            LayoutPreset lp;
            lp.name = itName->get<std::string>();
            lp.layout = std::move(layout);
            m_LayoutPresets.emplace_back(std::move(lp));
        }
    }

    // Restore active preset selection.
    int desired = -1;
    std::string activeName;
    const auto itActiveName = obj.find("activeName");
    if (itActiveName != obj.end() && itActiveName->is_string())
        activeName = itActiveName->get<std::string>();

    if (!activeName.empty())
    {
        for (int i = 0; i < static_cast<int>(m_LayoutPresets.size()); ++i)
        {
            if (m_LayoutPresets[(size_t)i].name == activeName)
            {
                desired = i;
                break;
            }
        }
    }
    if (desired < 0)
    {
        const auto itActiveIndex = obj.find("activeIndex");
        if (itActiveIndex != obj.end() && itActiveIndex->is_number_integer())
            desired = itActiveIndex->get<int>();
    }

    if (desired < -1)
        desired = -1;
    if (desired >= static_cast<int>(m_LayoutPresets.size()))
        desired = static_cast<int>(m_LayoutPresets.size()) - 1;

    // Apply the stored layout without touching UI (dockspace bind/rebuild happens later in Initialize()).
    if (desired >= 0 && desired < static_cast<int>(m_LayoutPresets.size()))
    {
        const auto& preset = m_LayoutPresets[(size_t)desired];
        if (!m_Docking)
        {
            OnLayoutPresetRestoreFailed();
            return;
        }

        UIManager* mainUi =
            (!m_Windows.empty() && m_Windows[0]) ? m_Windows[0]->ui.get() : nullptr;
        if (!Editor::LayoutPresetController::Apply(
                *m_Docking, mainUi, preset.layout.get(), false))
        {
            OnLayoutPresetRestoreFailed();
            return;
        }

        m_ActiveLayoutPresetIndex = desired;
        return;
    }

    // Preserve "no highlight" state if requested.
    if (desired == -1)
    {
        m_ActiveLayoutPresetIndex = -1;
    }
}

void EditorApplication::OnLayoutPresetRestoreFailed()
{
    m_ActiveLayoutPresetIndex = -1;
    SaveLayoutPresetsToPreferences();
    UIManager* mainUi =
        (!m_Windows.empty() && m_Windows[0]) ? m_Windows[0]->ui.get() : nullptr;
    Editor::LayoutPresetController::SetActiveToolbarIndex(
        mainUi, m_ActiveLayoutPresetIndex);
}

void EditorApplication::SaveLayoutPresetsToPreferences() const
{
    // Persist into the project folder so presets are shared with the project
    // (e.g. committed to VCS), not per-user.
    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return;
    GameEngine::Editor::SettingsStore prefs = GameEngine::Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)prefs.Load(&err); // keep existing keys

    nlohmann::json obj = nlohmann::json::object();
    obj["schemaVersion"] = 1;

    std::string activeName;
    if (m_ActiveLayoutPresetIndex >= 0 && m_ActiveLayoutPresetIndex < static_cast<int>(m_LayoutPresets.size()))
    {
        activeName = m_LayoutPresets[(size_t)m_ActiveLayoutPresetIndex].name;
    }
    obj["activeIndex"] = m_ActiveLayoutPresetIndex;
    if (!activeName.empty())
        obj["activeName"] = activeName;

    nlohmann::json presets = nlohmann::json::array();
    for (size_t i = 1; i < m_LayoutPresets.size(); ++i)
    {
        const LayoutPreset& p = m_LayoutPresets[i];
        if (p.name.empty() || !p.layout)
            continue;
        nlohmann::json entry = nlohmann::json::object();
        entry["name"] = p.name;
        entry["layout"] = DockNodeToJson(p.layout.get());
        presets.push_back(std::move(entry));
    }
    obj["presets"] = std::move(presets);

    prefs.SetJson("ui.layoutPresets", obj);
    (void)prefs.Save(&err);
}


void EditorApplication::SyncLayoutPresetToolbar()
{
    if (m_Windows.empty())
        return;
    auto& main = m_Windows[0];
    if (!main || !main->ui)
        return;

    UIElement* root = main->ui->GetRootElement();
    if (!root)
        return;

    auto* toolbar = dynamic_cast<LayoutPresetToolbar*>(root->FindById("LayoutButtonContainer"));
    if (!toolbar)
        return;

    // Bind callbacks (idempotent; toolbar rebuilds itself when callbacks change).
    toolbar->SetOnRecallPreset([this](int index)
                               { this->RequestSwitchLayoutPreset(index); });
    toolbar->SetOnRemovePreset([this](int index)
                               { this->RemoveLayoutPreset(index); });
    toolbar->SetOnSavePreset([this]()
                             {
                                 const int n = static_cast<int>(m_LayoutPresets.size());
                                 const std::string name = "Layout " + std::to_string(n);
                                 this->SaveCurrentLayoutAsPreset(name); });
    toolbar->SetOnOverwritePreset([this](int index)
                                  { this->OverwriteLayoutPreset(index); });
    toolbar->SetOnRenamePreset([this](int index)
                               { this->PromptRenameLayoutPreset(index); });
    toolbar->SetOnExportPreset([this](int index)
                               { this->ExportLayoutPresetInteractive(index); });
    toolbar->SetOnExportAllPresets([this]()
                                   { this->ExportAllLayoutPresetsInteractive(); });
    toolbar->SetOnImportPresets([this]()
                                { this->ImportLayoutPresetsInteractive(); });

    // Push current preset names + active index.
    std::vector<std::string> names;
    names.reserve(m_LayoutPresets.size());
    for (const auto& p : m_LayoutPresets)
    {
        names.push_back(p.name);
    }
    toolbar->SetPresets(std::move(names));
    toolbar->SetActiveIndex(m_ActiveLayoutPresetIndex);
}

void EditorApplication::RecallLayoutPreset(int index)
{
    if (index < 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;
    if (!m_Docking)
        return;

    const LayoutPreset& preset = m_LayoutPresets[(size_t)index];
    if (!preset.layout)
        return;

    UIManager* mainUi =
        (!m_Windows.empty() && m_Windows[0]) ? m_Windows[0]->ui.get() : nullptr;
    DismissTornOffPanelsForLayoutRestore();
    if (!Editor::LayoutPresetController::Apply(
            *m_Docking, mainUi, preset.layout.get(), true))
    {
        OnLayoutPresetRestoreFailed();
        return;
    }

    m_ActiveLayoutPresetIndex = index;
    SaveLayoutPresetsToPreferences();

    Editor::LayoutPresetController::SetActiveToolbarIndex(
        mainUi, m_ActiveLayoutPresetIndex);
}

bool EditorApplication::IsActiveLayoutDirty() const
{
    if (!m_Docking || !m_Docking->GetRoot())
        return false;
    if (m_ActiveLayoutPresetIndex < 0 ||
        m_ActiveLayoutPresetIndex >= static_cast<int>(m_LayoutPresets.size()))
        return false;

    const LayoutPreset& active = m_LayoutPresets[(size_t)m_ActiveLayoutPresetIndex];
    if (!active.layout)
        return false;

    UIManager* mainUi =
        (!m_Windows.empty() && m_Windows[0]) ? m_Windows[0]->ui.get() : nullptr;
    return Editor::LayoutPresetController::IsCurrentLayoutDirty(
        *m_Docking, mainUi, active.layout.get());
}

void EditorApplication::RequestSwitchLayoutPreset(int index)
{
    if (index < 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;

    // Allow re-clicking the active preset to reload it when the layout has been modified.
    if (index == m_ActiveLayoutPresetIndex && !IsActiveLayoutDirty())
        return;

    if (!IsActiveLayoutDirty())
    {
        RecallLayoutPreset(index);
        return;
    }

    // Lazy-create the modal on first use; reuse thereafter.
    if (!m_LayoutChangesModal)
    {
        if (m_Windows.empty() || !m_Windows[0] || !m_Windows[0]->ui)
        {
            RecallLayoutPreset(index);
            return;
        }
        UIElement* rootEl = m_Windows[0]->ui->GetRootElement();
        if (!rootEl)
        {
            RecallLayoutPreset(index);
            return;
        }
        auto modal = std::make_unique<SaveSceneChangesModal>();
        m_LayoutChangesModal = modal.get();
        rootEl->AddChild(std::move(modal));
    }

    const std::string activeName =
        (m_ActiveLayoutPresetIndex >= 0 && m_ActiveLayoutPresetIndex < (int)m_LayoutPresets.size())
            ? m_LayoutPresets[(size_t)m_ActiveLayoutPresetIndex].name
            : std::string("current layout");

    m_LayoutChangesModal->SetOnSave([this, index]()
                                    {
                                        const int n = static_cast<int>(m_LayoutPresets.size());
                                        const std::string name = "Layout " + std::to_string(n);
                                        SaveCurrentLayoutAsPreset(name);
                                        RecallLayoutPreset(index);
                                    });
    m_LayoutChangesModal->SetOnDontSave([this, index]()
                                        { RecallLayoutPreset(index); });
    m_LayoutChangesModal->SetOnCancel([]() {});

    m_LayoutChangesModal->Show(
        "Unsaved layout changes",
        "\"" + activeName + "\" has unsaved changes. Save them as a new layout before switching?");
}

void EditorApplication::SaveCurrentLayoutAsPreset(const std::string& name)
{
    if (!m_Docking || !m_Docking->GetRoot())
        return;

    // Sync current splitter positions from UI (WeightedPane weights) into the model
    // so the saved layout reflects the exact current state.
    if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
    {
        if (UIElement* rootEl = m_Windows[0]->ui->GetRootElement())
        {
            if (auto* ds = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock")))
                ds->SyncSplitRatiosFromUI();
        }
    }

    LayoutPreset preset;
    preset.name = name.empty() ? "Layout" : name;
    preset.layout = CloneDockNode(m_Docking->GetRoot());
    if (!preset.layout)
        return;

    m_LayoutPresets.emplace_back(std::move(preset));
    m_ActiveLayoutPresetIndex = static_cast<int>(m_LayoutPresets.size()) - 1;

    SyncLayoutPresetToolbar();
    SaveLayoutPresetsToPreferences();
}

void EditorApplication::RemoveLayoutPreset(int index)
{
    // Never remove the default preset.
    if (index <= 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;

    const bool removedWasActive = (m_ActiveLayoutPresetIndex == index);

    m_LayoutPresets.erase(m_LayoutPresets.begin() + index);

    if (removedWasActive)
    {
        // Keep current layout; just clear the "active preset" highlight.
        m_ActiveLayoutPresetIndex = -1;
    }
    else if (m_ActiveLayoutPresetIndex > index)
    {
        m_ActiveLayoutPresetIndex--;
    }

    if (m_ActiveLayoutPresetIndex >= static_cast<int>(m_LayoutPresets.size()))
    {
        m_ActiveLayoutPresetIndex = static_cast<int>(m_LayoutPresets.size()) - 1;
    }

    SyncLayoutPresetToolbar();
    SaveLayoutPresetsToPreferences();
}

void EditorApplication::OverwriteLayoutPreset(int index)
{
    // Never overwrite the default preset.
    if (index <= 0 || index >= static_cast<int>(m_LayoutPresets.size()))
        return;

    if (!m_Docking || !m_Docking->GetRoot())
        return;

    // Sync current splitter positions from UI into the model.
    if (!m_Windows.empty() && m_Windows[0] && m_Windows[0]->ui)
    {
        if (UIElement* rootEl = m_Windows[0]->ui->GetRootElement())
        {
            if (auto* ds = dynamic_cast<DockspaceElement*>(rootEl->FindById("dock")))
                ds->SyncSplitRatiosFromUI();
        }
    }

    auto cloned = CloneDockNode(m_Docking->GetRoot());
    if (!cloned)
        return;

    m_LayoutPresets[index].layout = std::move(cloned);
    m_ActiveLayoutPresetIndex = index;

    SyncLayoutPresetToolbar();
    SaveLayoutPresetsToPreferences();
}


} // namespace GameEngine
