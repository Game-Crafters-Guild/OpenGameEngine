#pragma once

#include "Editor/Settings/SettingsStore.h"

namespace GameEngine::Editor
{

class UniversalSearchSettings
{
public:
    static UniversalSearchSettings& Get()
    {
        static UniversalSearchSettings instance;
        return instance;
    }

    bool GetKeepOpenAfterSelection() const { return m_KeepOpenAfterSelection; }

    void SetKeepOpenAfterSelection(bool value)
    {
        m_KeepOpenAfterSelection = value;
        auto prefs = OpenEditorPreferences();
        std::string error;
        prefs.Load(&error);
        prefs.SetBool("universalSearch.keepOpenAfterSelection", value);
        prefs.Save(&error);
    }

private:
    UniversalSearchSettings()
    {
        auto prefs = OpenEditorPreferences();
        std::string error;
        prefs.Load(&error);
        prefs.TryGetBool("universalSearch.keepOpenAfterSelection", m_KeepOpenAfterSelection);
    }

    bool m_KeepOpenAfterSelection = false;
};

} // namespace GameEngine::Editor
