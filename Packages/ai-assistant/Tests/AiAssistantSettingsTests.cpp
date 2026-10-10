#include "AiAssistantSettings.h"
#include "ScopedEnvironmentVariable.h"
#include "ScopedPreferencesRoot.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>

namespace GameEngine
{
namespace
{
using Field = Editor::SettingsFieldDescriptor;

std::string Lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Sets every value a row offers (each dropdown option, a path for the text rows)
// and checks the row reads it back.
void SetEveryValue(const Field& field)
{
    if (const auto* dropdown = std::get_if<Field::DropdownField>(&field.Control))
    {
        for (const Field::DropdownField::Option& option : dropdown->OptionsProvider())
        {
            dropdown->Set(option.Value);
            EXPECT_EQ(dropdown->Get(), option.Value) << field.Label;
        }
    }
    else if (const auto* text = std::get_if<Field::StringField>(&field.Control))
    {
        text->Set("gpt-fixture");
        EXPECT_EQ(text->Get(), "gpt-fixture") << field.Label;
    }
    else if (const auto* path = std::get_if<Field::PathField>(&field.Control))
    {
        path->Set("C:/Tools/agent.exe");
        EXPECT_EQ(path->Get(), "C:/Tools/agent.exe") << field.Label;
    }
}
} // namespace

TEST(AiAssistantSettingsTests, NoRowNamesAKey)
{
    const Editor::SettingsCategoryDescriptor category = AiAssistantSettings::BuildCategory();
    ASSERT_FALSE(category.Fields.empty());
    for (const Field& field : category.Fields)
    {
        for (const std::string& text : {field.Label, field.Tooltip, field.PrefKey})
        {
            const std::string lower = Lowercase(text);
            EXPECT_EQ(lower.find("key"), std::string::npos) << field.Label << ": " << text;
            EXPECT_EQ(lower.find("token"), std::string::npos) << field.Label << ": " << text;
        }
    }
}

TEST(AiAssistantSettingsTests, EveryRowPersistsAndPreferencesNeverHoldTheKey)
{
    ScopedPreferencesRoot preferences;
    ScopedEnvironmentVariable key("ANTHROPIC_API_KEY", "sk-ant-fixture-not-a-key");

    const Editor::SettingsCategoryDescriptor category = AiAssistantSettings::BuildCategory();
    for (const Field& field : category.Fields)
        SetEveryValue(field);

    AiAssistantSettings::SetProvider("claude-session");
    const uint64_t generation = AiAssistantSettings::ProviderGeneration();
    AiAssistantSettings::SetProvider("codex-session");
    EXPECT_EQ(AiAssistantSettings::Provider(), "codex-session");
    EXPECT_EQ(AiAssistantSettings::ProviderGeneration(), generation + 1);
    AiAssistantSettings::SetProvider("codex-session");
    AiAssistantSettings::SetProvider("not-a-provider");
    EXPECT_EQ(AiAssistantSettings::Provider(), "codex-session");
    EXPECT_EQ(AiAssistantSettings::ProviderGeneration(), generation + 1);

    const std::filesystem::path file = Editor::OpenEditorPreferences().GetFilePath();
    std::ifstream stream(file, std::ios::binary);
    ASSERT_TRUE(stream) << file;
    const std::string contents(std::istreambuf_iterator<char>(stream), {});
    EXPECT_NE(contents.find("aiAssistant.provider"), std::string::npos);
    EXPECT_EQ(contents.find("sk-ant-"), std::string::npos);
}

// A Claude session turn always names its model and effort: opus at high until the user
// chooses, and the first model or the initial effort when the stored one is no longer
// offered.
TEST(AiAssistantSettingsTests, AClaudeSessionAlwaysAsksForAnOfferedModelAndEffort)
{
    ScopedPreferencesRoot preferences;
    EXPECT_EQ(AiAssistantSettings::Model("claude-session"), "opus");
    EXPECT_EQ(AiAssistantSettings::Effort("claude-session"), "high");

    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    prefs.SetString("aiAssistant.claudeSessionModel", "default");
    prefs.SetString("aiAssistant.claudeSessionEffort", "default");
    prefs.Save();
    EXPECT_EQ(AiAssistantSettings::Model("claude-session"), "opus");
    EXPECT_EQ(AiAssistantSettings::Effort("claude-session"), "high");
    EXPECT_EQ(AiAssistantSettings::Effort("claude-api"), "") << "a connection without effort levels asks for none";
}

// The last session is kept per connection and per project, in the project's per-user
// settings; an empty id removes it.
TEST(AiAssistantSettingsTests, TheLastSessionIsStoredPerConnectionAndProject)
{
    ScopedPreferencesRoot preferences;
    const std::filesystem::path game = std::filesystem::temp_directory_path() / "AiAssistantSettingsTests-game";
    const std::filesystem::path other = std::filesystem::temp_directory_path() / "AiAssistantSettingsTests-other";

    AiAssistantSettings::SetLastSession("claude-session", game, "claude-id");
    AiAssistantSettings::SetLastSession("codex-session", game, "codex-id");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", game), "claude-id");
    EXPECT_EQ(AiAssistantSettings::LastSession("codex-session", game), "codex-id");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", other), "");

    const std::filesystem::path file = Editor::OpenUserProjectSettings(game).GetFilePath();
    std::ifstream stream(file, std::ios::binary);
    const std::string contents(std::istreambuf_iterator<char>(stream), {});
    EXPECT_NE(contents.find("\"aiAssistant.session.claude-session\""), std::string::npos) << file;
    stream.close();

    AiAssistantSettings::SetLastSession("claude-session", game, "");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", game), "");
    EXPECT_EQ(AiAssistantSettings::LastSession("codex-session", game), "codex-id");
    AiAssistantSettings::SetLastSession("claude-session", {}, "ignored");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", {}), "");
}
} // namespace GameEngine
