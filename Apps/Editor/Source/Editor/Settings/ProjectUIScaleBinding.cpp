#include "Editor/Settings/ProjectUIScaleBinding.h"

#include "Core/Engine.h"
#include "Editor/Settings/UIScaleProjectSettingsWriter.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"
#include "UI/UIManager.h"

namespace GameEngine::Editor
{

ProjectUIScaleBinding::ProjectUIScaleBinding()
    : m_SavedSubscription(UIScaleProjectSettingsSaved().Subscribe([this]() { m_Stale = true; }))
{
}

void ProjectUIScaleBinding::Apply(UIManager& ui)
{
    const std::filesystem::path& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (m_Stale || workspaceRoot != m_LoadedWorkspaceRoot)
    {
        m_Settings = UIScaleProjectSettings::Load(workspaceRoot);
        m_LoadedWorkspaceRoot = workspaceRoot;
        m_Stale = false;
    }
    ui.SetScaleSettings(m_Settings);
}

} // namespace GameEngine::Editor
