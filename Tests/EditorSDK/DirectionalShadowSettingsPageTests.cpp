#include "Editor/Settings/DirectionalShadowSettingsPage.h"

#include "Editor/Settings/RenderProjectSettings.h"

#include <gtest/gtest.h>

namespace ed = GameEngine::Editor;

TEST(DirectionalShadowSettingsPageTests, DescribesRenderingSectionThroughSharedFields)
{
    const ed::SettingsCategoryDescriptor section =
        ed::CreateDirectionalShadowSettingsSection();

    EXPECT_EQ(section.CategoryId, "directionalShadows");
    EXPECT_EQ(section.Title, "Directional Shadows");
    EXPECT_EQ(section.Group, ed::SettingsCategoryGroup::ProjectSettings);
    ASSERT_EQ(section.Fields.size(), 2u);

    const ed::SettingsFieldDescriptor& resolution = section.Fields[0];
    EXPECT_EQ(resolution.Label, "Shadow Resolution");
    ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::DropdownField>(
        resolution.Control));

    const auto& dropdown =
        std::get<ed::SettingsFieldDescriptor::DropdownField>(resolution.Control);
    EXPECT_EQ(dropdown.DefaultValue, "2048 (Default)");
    EXPECT_EQ(dropdown.Options,
              (std::vector<std::string>{"1024 (Performance)", "2048 (Default)",
                                        "4096 (High)", "8192 (Ultra)"}));
    EXPECT_TRUE(static_cast<bool>(dropdown.Get));
    EXPECT_TRUE(static_cast<bool>(dropdown.Set));
}

TEST(DirectionalShadowSettingsPageTests, DescribesTheStableProjectionToggleDefaultOff)
{
    const ed::SettingsCategoryDescriptor section =
        ed::CreateDirectionalShadowSettingsSection();

    ASSERT_EQ(section.Fields.size(), 2u);
    const ed::SettingsFieldDescriptor& projection = section.Fields[1];
    EXPECT_EQ(projection.Label, "Stable Projection");
    ASSERT_TRUE(
        std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(projection.Control));

    const auto& toggle = std::get<ed::SettingsFieldDescriptor::ToggleField>(projection.Control);
    // Defaults OFF: Stable removes the crawl but costs cascade-0 texel density,
    // measured at 2.5x on a ground-level pose -- worse than the artifact it
    // fixes until a project tightens its splits to absorb it.
    EXPECT_FALSE(toggle.DefaultValue);
    EXPECT_TRUE(static_cast<bool>(toggle.Get));
    EXPECT_TRUE(static_cast<bool>(toggle.Set));
}
