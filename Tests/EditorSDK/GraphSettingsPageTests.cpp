// Node Graph settings page registration shape — the rows a user actually sees.
//
// Asserts the descriptor the SettingsPanel renders from, not the panel: a row
// that fails to register is invisible rather than loud. Field Get/Set closures
// are NOT invoked here — they reach GraphCanvas / GraphPanel / NodeColorSettings
// — so this covers the shape (labels, control kinds, frozen pref keys).

#include "Editor/Settings/GraphSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Graph/GraphCanvas.h"
#include "Graph/GraphNodeRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <variant>

namespace ed = GameEngine::Editor;

namespace
{

const ed::SettingsFieldDescriptor* FindField(const ed::SettingsCategoryDescriptor& category,
                                             const std::string& label)
{
    for (const auto& field : category.Fields)
    {
        if (field.Label == label)
            return &field;
    }
    return nullptr;
}

class GraphSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ed::RegisterGraphSettingsCategory();
        ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("nodeGraph", m_Category));
    }

    ed::SettingsCategoryDescriptor m_Category;
};

TEST_F(GraphSettingsPageTest, RegistersUnderUiGroup)
{
    EXPECT_EQ(m_Category.Title, "Node Graph");
    EXPECT_EQ(m_Category.Group, ed::SettingsCategoryGroup::UI);
    EXPECT_EQ(m_Category.TreeRowClass, "ui-node-graph-row");
    EXPECT_TRUE(m_Category.ParentCategoryId.empty());
}

TEST_F(GraphSettingsPageTest, FrozenPreferenceKeysUnchanged)
{
    struct Row
    {
        const char* Label;
        const char* PrefKey;
    };
    const Row rows[] = {
        {"Palette Panel Scrollbar", "nodeGraph.panelScrollbars"},
        {"Variables Panel Scrollbar", "nodeGraph.variablesScrollbars"},
        {"Node Header Alignment", GameEngine::GraphCanvas::kNodeHeaderAlignmentPreference},
        {"Node Corner Radius", GameEngine::GraphCanvas::kNodeCornerRadiusPreference},
        {"Node Drop Shadows", GameEngine::GraphCanvas::kNodeDropShadowsPreference},
        {"Node Shadow Offset X", GameEngine::GraphCanvas::kNodeDropShadowOffsetXPreference},
        {"Node Shadow Offset Y", GameEngine::GraphCanvas::kNodeDropShadowOffsetYPreference},
        {"Node Shadow Blur", GameEngine::GraphCanvas::kNodeDropShadowBlurPreference},
        {"Node Shadow Opacity", GameEngine::GraphCanvas::kNodeDropShadowOpacityPreference},
        {"Rounded Connection Corners", GameEngine::GraphCanvas::kConnectionRoundedCornersPreference},
    };
    for (const Row& row : rows)
    {
        const auto* field = FindField(m_Category, row.Label);
        ASSERT_NE(field, nullptr) << "missing row: " << row.Label;
        EXPECT_EQ(field->PrefKey, row.PrefKey) << row.Label;
    }
}

TEST_F(GraphSettingsPageTest, ControlKindsMatchTheLegacyPage)
{
    const char* toggleLabels[] = {"Palette Panel Scrollbar", "Variables Panel Scrollbar",
                                  "Node Drop Shadows", "Rounded Connection Corners"};
    for (const char* label : toggleLabels)
    {
        const auto* field = FindField(m_Category, label);
        ASSERT_NE(field, nullptr) << label;
        EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(field->Control))
            << label;
    }
    const auto* alignment = FindField(m_Category, "Node Header Alignment");
    ASSERT_NE(alignment, nullptr);
    EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::DropdownField>(alignment->Control));
    const auto* radius = FindField(m_Category, "Node Corner Radius");
    ASSERT_NE(radius, nullptr);
    EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::SliderField>(radius->Control));
    const auto* body = FindField(m_Category, "Node Body Color");
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ColorField>(body->Control));
}

TEST_F(GraphSettingsPageTest, CornerRadiusSliderBoundsMatchCanvas)
{
    const auto* field = FindField(m_Category, "Node Corner Radius");
    ASSERT_NE(field, nullptr);
    const auto& slider = std::get<ed::SettingsFieldDescriptor::SliderField>(field->Control);
    EXPECT_FLOAT_EQ(slider.MinValue, GameEngine::GraphCanvas::kMinNodeCornerRadius);
    EXPECT_FLOAT_EQ(slider.MaxValue, GameEngine::GraphCanvas::kMaxNodeCornerRadius);
    EXPECT_FLOAT_EQ(slider.DefaultValue, GameEngine::GraphCanvas::kDefaultNodeCornerRadius);
}

TEST_F(GraphSettingsPageTest, RegistersGameLogicAndMaterialTypeColors)
{
    const auto* state = FindField(m_Category, "State");
    ASSERT_NE(state, nullptr) << "Game Logic State color row missing — catalog empty?";
    EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ColorField>(state->Control));

    bool foundColorConstant = false;
    int colorRows = 0;
    for (const auto& field : m_Category.Fields)
    {
        if (!std::holds_alternative<ed::SettingsFieldDescriptor::ColorField>(field.Control))
            continue;
        ++colorRows;
        if (field.SearchKeywords.find("ColorConstant") != std::string::npos)
            foundColorConstant = true;
    }
    EXPECT_TRUE(foundColorConstant) << "Material ColorConstant color row missing";
    EXPECT_GE(colorRows, 10) << "expected body color plus per-type rows";
}

TEST_F(GraphSettingsPageTest, RegistersAnimationTypeColors)
{
    bool foundClipPlayer = false;
    for (const auto& field : m_Category.Fields)
    {
        if (field.SearchKeywords.find("nodegraph-typecolor:animation:ClipPlayer") != std::string::npos)
        {
            foundClipPlayer = true;
            EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ColorField>(field.Control));
        }
    }
    EXPECT_TRUE(foundClipPlayer) << "Animation ClipPlayer color row missing after KindId catalog";
}

TEST_F(GraphSettingsPageTest, PrepareFieldsRebuildsTypeColorsAfterLateRegister)
{
    constexpr const char* kLateTypeId = "AdvTypeColorLate";
    GameEngine::GraphNodeRegistry::Get().Register(
        GameEngine::Graph::kKindIdMaterial, {kLateTypeId, "Adv Late", "AdvProbe", {}});

    const std::string needle = std::string("nodegraph-typecolor:material:") + kLateTypeId;
    auto hasNeedle = [&](const ed::SettingsCategoryDescriptor& category)
    {
        for (const auto& field : category.Fields)
        {
            if (field.SearchKeywords.find(needle) != std::string::npos)
                return true;
        }
        return false;
    };

    EXPECT_FALSE(hasNeedle(m_Category));
    ASSERT_TRUE(static_cast<bool>(m_Category.PrepareFields));
    ed::SettingsCategoryDescriptor rebuilt = m_Category;
    m_Category.PrepareFields(rebuilt);
    EXPECT_TRUE(hasNeedle(rebuilt)) << "late material type missing after PrepareFields";
}

} // namespace
