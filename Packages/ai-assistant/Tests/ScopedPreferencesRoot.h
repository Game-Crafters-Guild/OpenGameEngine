#pragma once

#include "ScopedEnvironmentVariable.h"

#include <filesystem>
#include <system_error>

namespace GameEngine
{
/// Points the editor preferences at a fresh folder for one test.
class ScopedPreferencesRoot
{
public:
    ScopedPreferencesRoot()
        : m_Root(std::filesystem::temp_directory_path() / "AiAssistantSettingsTests")
        , m_Variable("GE_EDITOR_USER_DATA_ROOT", m_Root.string().c_str())
    {
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    ~ScopedPreferencesRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Root, error);
    }

    ScopedPreferencesRoot(const ScopedPreferencesRoot&) = delete;
    ScopedPreferencesRoot& operator=(const ScopedPreferencesRoot&) = delete;

private:
    std::filesystem::path m_Root;
    ScopedEnvironmentVariable m_Variable;
};
} // namespace GameEngine
