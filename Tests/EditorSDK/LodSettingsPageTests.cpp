// The LOD settings page's registration shape — the rows a user actually sees.
//
// Asserts the descriptor the SettingsPanel renders from, not the panel: the
// per-view-class rows are a new default-visible surface, and a row that fails
// to register is invisible rather than loud. Field Get/Set closures are NOT
// invoked here — they reach the live EngineCore — so this covers the shape
// (labels, control kinds, slider bounds), which is what silently regresses.

#include "Editor/Settings/LodSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Engine/Rendering/MeshLODThresholds.h"

#include <gtest/gtest.h>

#include <string>

namespace ed = GameEngine::Editor;
namespace gr = GameEngine::Rendering;

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

class LodSettingsPageTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Registration is replace-forward under the same CategoryId, so calling
        // this in several tests re-registers in place rather than duplicating.
        ed::RegisterLodSettingsCategory();
        ASSERT_TRUE(ed::EditorSettingsRegistry::Get().TryGet("levelOfDetail", m_Category));
    }

    ed::SettingsCategoryDescriptor m_Category;
};

TEST_F(LodSettingsPageTest, RegistersUnderProjectSettings)
{
    EXPECT_EQ(m_Category.Title, "Level of Detail");
    EXPECT_EQ(m_Category.Group, ed::SettingsCategoryGroup::ProjectSettings);
    EXPECT_EQ(m_Category.TreeRowClass, "level-of-detail-row");
}

TEST_F(LodSettingsPageTest, BothViewClassesGetAnEnableAndAPercentRow)
{
    for (const char* className : {"Game View", "Scene View"})
    {
        const std::string toggleLabel = std::string("Override ") + className + " Budget";
        const std::string sliderLabel = std::string(className) + " Budget Percent";

        const auto* toggle = FindField(m_Category, toggleLabel);
        ASSERT_NE(toggle, nullptr) << "missing row: " << toggleLabel;
        EXPECT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::ToggleField>(
            toggle->Control))
            << toggleLabel << " must be an enable/disable toggle";

        const auto* slider = FindField(m_Category, sliderLabel);
        ASSERT_NE(slider, nullptr) << "missing row: " << sliderLabel;
        ASSERT_TRUE(std::holds_alternative<ed::SettingsFieldDescriptor::SliderField>(
            slider->Control))
            << sliderLabel << " must be a percent slider";

        // The slider bounds ARE the engine's clamp range: a narrower slider
        // would clamp a hand-edited value on page open and write it back.
        const auto& s = std::get<ed::SettingsFieldDescriptor::SliderField>(slider->Control);
        EXPECT_FLOAT_EQ(s.MinValue, gr::kMinLodBudgetPercent);
        EXPECT_FLOAT_EQ(s.MaxValue, gr::kMaxLodBudgetPercent);
        EXPECT_FLOAT_EQ(s.DefaultValue, gr::kDefaultLodBudgetPercent);
    }
}

TEST_F(LodSettingsPageTest, NoRowIsOfferedForPreviewOrThumbnailViews)
{
    // Deliberate: a thumbnail scales the MODEL to fill the frame, so its
    // coverage is the framing multiplier whatever the mesh — above the LOD
    // ceiling, so LOD0 wins at every budget and the knob would move nothing.
    for (const auto& field : m_Category.Fields)
    {
        EXPECT_EQ(field.Label.find("Preview"), std::string::npos)
            << "unexpected preview row: " << field.Label;
        EXPECT_EQ(field.Label.find("Thumbnail"), std::string::npos)
            << "unexpected thumbnail row: " << field.Label;
    }
}

TEST_F(LodSettingsPageTest, SpellsScreenSpaceErrorOutInEveryRowItGoverns)
{
    // House rule: the surface never says "SSE". These rows are the ones whose
    // behaviour is Screen-Space-Error-only, so each must say so in full.
    for (const char* label : {"Override Game View Budget", "Game View Budget Percent",
                              "Override Scene View Budget", "Scene View Budget Percent"})
    {
        const auto* field = FindField(m_Category, label);
        ASSERT_NE(field, nullptr) << label;
        EXPECT_NE(field->Tooltip.find("Screen-Space Error"), std::string::npos)
            << label << " must name the mapping it applies to, spelled out";
    }
}

} // namespace
