#pragma once

#include "Events/Event.h"
#include "UI/UIScaleSettings.h"

#include <filesystem>

namespace GameEngine
{
class UIManager;
}

namespace GameEngine::Editor
{

// Holds an editor game UI host at the open project's authored UI scale policy,
// so the editor previews the scale a build ships. The project file is read when
// the open project changes and after the settings page saves it; Apply itself
// only assigns the cached policy, so a host calls it every frame it renders.
class ProjectUIScaleBinding
{
  public:
    ProjectUIScaleBinding();
    // The save subscription captures this object.
    ProjectUIScaleBinding(const ProjectUIScaleBinding&) = delete;
    ProjectUIScaleBinding& operator=(const ProjectUIScaleBinding&) = delete;

    void Apply(UIManager& ui);

  private:
    EventSubscription m_SavedSubscription;
    std::filesystem::path m_LoadedWorkspaceRoot;
    UI::UIScaleSettings m_Settings;
    bool m_Stale = true;
};

} // namespace GameEngine::Editor
