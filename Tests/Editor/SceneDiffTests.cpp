#include "VersionControl/SceneDiff.h"

#include <gtest/gtest.h>

using namespace GameEngine::Editor;

namespace
{
const SceneObjectDiff* FindObject(const std::vector<SceneObjectDiff>& diff, const std::string& key)
{
    for (const auto& object : diff)
        if (object.StableKey == key)
            return &object;
    return nullptr;
}

const ScenePropertyDiff* FindProperty(const SceneObjectDiff& object, const std::string& key)
{
    for (const auto& property : object.Properties)
        if (property.Key == key)
            return &property;
    return nullptr;
}
} // namespace

TEST(SceneDiffTests, AlignsEntitiesByStableIdAcrossReordering)
{
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="first"]
Name.value = "First"
Transform.position = (0, 0, 0)

[entity id="second"]
Name.value = "Second"
Transform.position = (1, 0, 0)
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="second"]
Name.value = "Second"
Transform.position = (1, 0, 0)

[entity id="first"]
Name.value = "First"
Transform.position = (2, 0, 0)
)";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 2u);
    const auto* first = FindObject(diff, "entity:first");
    const auto* second = FindObject(diff, "entity:second");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->State, SceneDiffState::Modified);
    EXPECT_EQ(second->State, SceneDiffState::Unchanged);
    ASSERT_NE(FindProperty(*first, "Transform.position"), nullptr);
    EXPECT_EQ(FindProperty(*first, "Transform.position")->State, SceneDiffState::Modified);
}

TEST(SceneDiffTests, ReportsAddedRemovedAndModifiedInspectorProperties)
{
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Player"
Transform.position = (0, 0, 0)
Transform.scale = (1, 1, 1)
Legacy.enabled = true
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Hero"
Transform.position = (5, 0, 0)
Transform.scale = (1, 1, 1)
Camera.enabled = true
)";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_EQ(diff[0].OriginalLabel, "Player");
    EXPECT_EQ(diff[0].CurrentLabel, "Hero");
    EXPECT_EQ(FindProperty(diff[0], "Transform.position")->State, SceneDiffState::Modified);
    EXPECT_EQ(FindProperty(diff[0], "Transform.scale")->State, SceneDiffState::Unchanged);
    EXPECT_EQ(FindProperty(diff[0], "Legacy.enabled")->State, SceneDiffState::Removed);
    EXPECT_EQ(FindProperty(diff[0], "Camera.enabled")->State, SceneDiffState::Added);
}

TEST(SceneDiffTests, KeepsEditorSectionsOutOfThePrecedingEntity)
{
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Player"

[editor_camera]
position = (0, 1, 2)
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Player"

[editor_camera]
position = (0, 3, 2)
)";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 2u);
    const auto* entity = FindObject(diff, "entity:player");
    const auto* camera = FindObject(diff, "editor_camera:0");
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(camera, nullptr);
    EXPECT_EQ(entity->State, SceneDiffState::Unchanged);
    EXPECT_FALSE(camera->IsEntity);
    EXPECT_EQ(FindProperty(*camera, "position")->State, SceneDiffState::Modified);
}

TEST(SceneDiffTests, ReportsHierarchyEntityAdditionAndRemoval)
{
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="removed"]
Name.value = "Removed"
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="added"]
Name.value = "Added"
)";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 2u);
    ASSERT_NE(FindObject(diff, "entity:removed"), nullptr);
    ASSERT_NE(FindObject(diff, "entity:added"), nullptr);
    EXPECT_EQ(FindObject(diff, "entity:removed")->State, SceneDiffState::Removed);
    EXPECT_EQ(FindObject(diff, "entity:added")->State, SceneDiffState::Added);
}

TEST(SceneDiffTests, ReparentingAnEntityIsAChange)
{
    // Parenting is a header attribute, not a component field, so it reaches
    // neither the property loop nor the label comparison.
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Player"

[entity id="child" parent="player"]
Name.value = "Child"
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Player"

[entity id="child" parent="enemy"]
Name.value = "Child"
)";

    const auto diff = BuildSceneDiff(original, current);
    const auto* child = FindObject(diff, "entity:child");
    ASSERT_NE(child, nullptr);
    EXPECT_TRUE(child->WasReparented());
    EXPECT_EQ(child->OriginalParent, "player");
    EXPECT_EQ(child->CurrentParent, "enemy");
    EXPECT_EQ(child->State, SceneDiffState::Modified);

    const auto* player = FindObject(diff, "entity:player");
    ASSERT_NE(player, nullptr);
    EXPECT_FALSE(player->WasReparented());
    EXPECT_EQ(player->State, SceneDiffState::Unchanged);
}

TEST(SceneDiffTests, MovingAnEntityToTheRootIsAReparent)
{
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="child" parent="player"]
Name.value = "Child"
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="child"]
Name.value = "Child"
)";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_TRUE(diff[0].WasReparented());
    EXPECT_EQ(diff[0].OriginalParent, "player");
    EXPECT_TRUE(diff[0].CurrentParent.empty());
    EXPECT_EQ(diff[0].State, SceneDiffState::Modified);
}

TEST(SceneDiffTests, UnchangedParentIsNotAChange)
{
    constexpr std::string_view scene = R"([scene name="Test" version=1]
[entity id="child" parent="player"]
Name.value = "Child"
)";

    const auto diff = BuildSceneDiff(scene, scene);
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_FALSE(diff[0].WasReparented());
    EXPECT_EQ(diff[0].State, SceneDiffState::Unchanged);
}

TEST(SceneDiffTests, TruncatedSectionHeaderStillOpensItsSection)
{
    // A header missing its closing bracket must not fold into the previous
    // object: that drops the section from one side and reports every entity in
    // it as added against an intact baseline.
    constexpr std::string_view original = R"([scene name="Test" version=1]
[entity id="player"
Name.value = "Player"
)";
    constexpr std::string_view current = R"([scene name="Test" version=1]
[entity id="player"]
Name.value = "Player"
)";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_EQ(diff[0].StableKey, "entity:player");
    EXPECT_EQ(diff[0].State, SceneDiffState::Unchanged);
}

TEST(SceneDiffTests, ParsesCrlfLineEndingsWithoutFalseChanges)
{
    constexpr std::string_view original =
        "[scene name=\"Test\" version=1]\r\n[entity id=\"player\"]\r\n"
        "Name.value = \"Player\"\r\nTransform.position = (0, 0, 0)\r\n";
    constexpr std::string_view current =
        "[scene name=\"Test\" version=1]\n[entity id=\"player\"]\n"
        "Name.value = \"Player\"\nTransform.position = (1, 0, 0)\n";

    const auto diff = BuildSceneDiff(original, current);
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_EQ(diff[0].StableKey, "entity:player");
    EXPECT_EQ(diff[0].State, SceneDiffState::Modified);
    // The CR must not survive into the key or the value, or every property of a
    // CRLF scene reads as changed against an LF baseline.
    ASSERT_NE(FindProperty(diff[0], "Name.value"), nullptr);
    EXPECT_EQ(FindProperty(diff[0], "Name.value")->State, SceneDiffState::Unchanged);
    EXPECT_EQ(FindProperty(diff[0], "Transform.position")->State, SceneDiffState::Modified);
    EXPECT_EQ(FindProperty(diff[0], "Transform.position")->OriginalValue, "(0, 0, 0)");
}

TEST(SceneDiffTests, StatePresentationCoversEveryState)
{
    EXPECT_STREQ(SceneDiffStateLabel(SceneDiffState::Added), "Added");
    EXPECT_STREQ(SceneDiffStateLabel(SceneDiffState::Removed), "Removed");
    EXPECT_STREQ(SceneDiffStateLabel(SceneDiffState::Modified), "Modified");
    EXPECT_STREQ(SceneDiffStateLabel(SceneDiffState::Unchanged), "Unchanged");

    // Mirrors the --ui_color_vcs_* tokens in theme/tokens.css.
    EXPECT_EQ(SceneDiffStateColorArgb(SceneDiffState::Added), 0xFF9DFF00u);
    EXPECT_EQ(SceneDiffStateColorArgb(SceneDiffState::Removed), 0xFFFF9500u);
    EXPECT_EQ(SceneDiffStateColorArgb(SceneDiffState::Modified), 0xFF549BFFu);
    EXPECT_EQ(SceneDiffStateColorArgb(SceneDiffState::Unchanged), 0xFF888888u);
}

TEST(SceneDiffTests, DoesNotMistakeGuidForIdAttribute)
{
    constexpr std::string_view scene = R"([scene name="Test" version=1]
[resource path="Materials/Test.mat" guid="1234"]
type = "Material"
)";

    const auto diff = BuildSceneDiff(scene, scene);
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_EQ(diff[0].StableKey, "resource:Materials/Test.mat");
}
