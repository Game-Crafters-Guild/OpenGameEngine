#include "AgentSessionState.h"
#include "AiAssistantSettings.h"
#include "Providers/IAgentProvider.h"
#include "ScopedPreferencesRoot.h"

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <variant>

namespace GameEngine
{
TEST(AgentSessionStateTests, ATurnOfAReplacedProviderCannotRecordItsSessionIntoTheNewConnection)
{
    using Field = Editor::SettingsFieldDescriptor;
    ScopedPreferencesRoot preferences;
    const Editor::SettingsCategoryDescriptor category = AiAssistantSettings::BuildCategory();
    const auto executableRow = std::find_if(category.Fields.begin(), category.Fields.end(),
                                            [](const Field& field) { return field.Label == "Claude executable"; });
    ASSERT_NE(executableRow, category.Fields.end());
    const auto& executable = std::get<Field::PathField>(executableRow->Control);
    AgentSessionState& state = AgentSessionState::Get();

    executable.Set("C:/Tools/old/claude.exe");
    const std::shared_ptr<IAgentProvider> replaced = state.Provider("claude-session");
    executable.Set("C:/Tools/new/claude.exe");
    const std::shared_ptr<IAgentProvider> current = state.Provider("claude-session");
    ASSERT_NE(replaced, current);

    state.SetSessionId(*replaced, "old-session");
    EXPECT_EQ(state.SessionId("claude-session"), "");
    state.SetSessionId(*current, "new-session");
    EXPECT_EQ(state.SessionId("claude-session"), "new-session");
}
} // namespace GameEngine
