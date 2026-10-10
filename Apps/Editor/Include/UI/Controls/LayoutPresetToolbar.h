#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

// Editor bottom toolbar: dynamic layout preset buttons + "+" save button.
// Instantiated from UXML via the UI element factory registry.
class LayoutPresetToolbar final : public UIElement
{
  public:
    LayoutPresetToolbar();
    ~LayoutPresetToolbar() override = default;

    void SetPresets(std::vector<std::string> presetNames);
    void SetActiveIndex(int index);

    void SetOnRecallPreset(std::function<void(int)> callback);
    void SetOnSavePreset(std::function<void()> callback);
    void SetOnRemovePreset(std::function<void(int)> callback);
    void SetOnOverwritePreset(std::function<void(int)> callback);
    // The preset buttons' right-click menu actions that live with the host: rename and
    // export/import go through file dialogs and the preset store the host owns.
    void SetOnRenamePreset(std::function<void(int)> callback);
    void SetOnExportPreset(std::function<void(int)> callback);
    void SetOnExportAllPresets(std::function<void()> callback);
    void SetOnImportPresets(std::function<void()> callback);

    void OnPostLayout() override;

  private:
    void ScheduleRebuild();
    void RebuildNow();
    void UpdateActiveClasses();

    // Configuration
    std::vector<std::string> m_PresetNames;
    int m_ActiveIndex = 0;

    // Rebuild control
    bool m_NeedsRebuild = true;
    bool m_RebuildScheduled = false;

    // Callbacks
    std::function<void(int)> m_OnRecallPreset;
    std::function<void()> m_OnSavePreset;
    std::function<void(int)> m_OnRemovePreset;
    std::function<void(int)> m_OnOverwritePreset;
    std::function<void(int)> m_OnRenamePreset;
    std::function<void(int)> m_OnExportPreset;
    std::function<void()> m_OnExportAllPresets;
    std::function<void()> m_OnImportPresets;
};

} // namespace GameEngine

