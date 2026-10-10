// The material inspector keeps the inspector's row rhythm. Every section bar, row and line sits
// one row gap below whatever is above it, open or closed, in the entity inspector and in the asset
// view; a texture slot's field starts on the value column; a material row is as tall as the
// component rows around it. Each is a rule in inspector.css that selects a class the material
// inspector sets, so these tests lay the real widgets out over the real stylesheets. The foldouts
// and lines come from the builders BuildMaterialInspectorUI uses (MaterialInspectorSections.h),
// the rows from the InspectorDrag helpers, and the last test keeps the inspectors building through
// those builders. BuildMaterialInspectorUI itself needs a running engine for its asset lookups,
// which is why its tree is reproduced rather than called.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/MaterialInspectorSections.h"
#include "UI/AssetField.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/InspectorSection.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <memory>
#include <string>

using namespace GameEngine;
using InspectorLayoutTesting::InspectorLayoutFixture;
using InspectorLayoutTesting::ReadEditorFile;

namespace
{

// A whole material inspector, laid out top to bottom, fits in this.
constexpr uint32_t kViewportH = 2400;

// Layout lands on whole or half pixels; anything past this is a different gap, not rounding.
constexpr float kTolerancePx = 0.5f;

// InspectorPanel pads its asset content by this much on each side (kInspectorAssetContentHorizontalInsetPx).
constexpr float kAssetContentInsetPx = 16.0f;

float Bottom(const UIElement* e)
{
    return e->GetLayoutY() + e->GetLayoutHeight();
}

float GapBetween(const UIElement* above, const UIElement* below)
{
    return below->GetLayoutY() - Bottom(above);
}

float CentreY(const UIElement* e)
{
    return e->GetLayoutY() + e->GetLayoutHeight() * 0.5f;
}

UIElement* Bar(Foldout* section)
{
    return section->GetHeader();
}

// The first element under `root` (itself included) that carries `className`, depth first.
const UIElement* FindWithClass(const UIElement* root, const char* className)
{
    if (root->HasClass(className))
        return root;
    for (const auto& child : root->GetChildren())
    {
        if (const UIElement* found = FindWithClass(child.get(), className))
            return found;
    }
    return nullptr;
}

// The colour a numeric field draws its value in: its text input's.
uint32_t ValueTextColour(const FloatField* field)
{
    const UIElement* input = FindWithClass(field, "float-field-input");
    return input ? input->GetResolvedStyle().Visual.Color : 0u;
}

// A control added by an InspectorDrag row helper sits in the row's field container.
UIElement* FieldContainerOf(UIElement* control)
{
    return control->GetParent();
}

Foldout* AddSection(UIElement* root, const std::string& title, bool expanded)
{
    std::unique_ptr<Foldout> section = Editor::MakeMaterialSection(title, expanded);
    Foldout* raw = section.get();
    root->AddChild(std::move(section));
    return raw;
}

// Text inside a row, as the File section's path is: part of the row, not a line of its own.
void AddRowText(UIElement* row, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass("inspector-text");
    label->SetText(text);
    row->AddChild(std::move(label));
}

// The sections of BuildMaterialInspectorUI, with each kind of line they hold: rows, a note that
// ends a section, a texture slot, a lone button, and closed sections one after another.
struct MaterialTree
{
    Foldout* shader = nullptr;
    UIElement* lightingModelField = nullptr;
    UIElement* lightingModelRow = nullptr;
    UIElement* doubleSidedRow = nullptr;
    Foldout* properties = nullptr;
    UIElement* notice = nullptr;
    Foldout* textures = nullptr;
    UIElement* textureRow = nullptr;
    UIElement* textureName = nullptr;
    UIElement* textureFieldContainer = nullptr;
    UIElement* textureField = nullptr;
    UIElement* warning = nullptr;
    Foldout* diagnostics = nullptr;
    UIElement* recompile = nullptr;
    Foldout* advanced = nullptr;
    Foldout* file = nullptr;
};

MaterialTree BuildMaterialSections(UIElement* root)
{
    using namespace InspectorDrag;
    MaterialTree t;
    root->AddClass("material-inspector");

    t.shader = AddSection(root, "Shader", true);
    UIElement* shaderContent = t.shader->GetContentContainer();
    t.lightingModelField = FieldContainerOf(
        InspectorUI::AddDropdownRow(shaderContent, "Lighting Model", {{"StandardPBR", "StandardPBR"}}, 0));
    t.lightingModelRow = t.lightingModelField->GetParent();
    t.doubleSidedRow = FieldContainerOf(AddToggleRow(shaderContent, "Double Sided", false, [](bool) {}))->GetParent();

    t.properties = AddSection(root, "Properties", true);
    AddFloatRowWithDrag(t.properties->GetContentContainer(), "Metallic", 0.0f, [](float) {}, [](float) {});
    t.notice = Editor::AddMaterialLine(t.properties->GetContentContainer(),
                                       "Not declared by the surface (stored, but never reaches the shader):\n - customGlow");

    t.textures = AddSection(root, "Textures", true);
    UIElement* textureRow = InspectorUI::AddRow(t.textures->GetContentContainer());
    textureRow->AddClass("material-texture-row");
    Label* name = InspectorUI::AddLabel(textureRow, "Albedo");
    name->AddClass("material-texture-name");
    t.textureName = name;
    t.textureFieldContainer = InspectorUI::AddFieldContainer(textureRow);
    auto field = std::make_unique<AssetField>();
    field->AddClass("dropdown-asset-field");
    t.textureField = field.get();
    t.textureFieldContainer->AddChild(std::move(field));
    t.textureRow = textureRow;
    t.warning = Editor::AddMaterialWarning(t.textures->GetContentContainer(),
                                           "Hex Tiling and a height map do not combine: the relief is not drawn.");

    t.diagnostics = AddSection(root, "Shader Diagnostics", true);
    Editor::AddMaterialLine(t.diagnostics->GetContentContainer(), "Shader: FAILED");
    auto recompile = std::make_unique<Button>();
    recompile->AddClass("inspector-text");
    recompile->AddClass("material-line");
    recompile->SetText("Recompile Shader");
    t.recompile = recompile.get();
    t.diagnostics->GetContentContainer()->AddChild(std::move(recompile));

    t.advanced = AddSection(root, "Advanced Shader", false);
    InspectorUI::AddAssetFieldRow(t.advanced->GetContentContainer(), "Surface Shader", GUID::Null(), {AssetType::Shader},
                     nullptr, [](const GUID&) {});
    t.file = AddSection(root, "File", false);
    AddRowText(InspectorUI::AddRow(t.file->GetContentContainer()), "Assets/Materials/Brick.material");
    return t;
}

std::unique_ptr<UIElement> MakePanel()
{
    auto panel = std::make_unique<UIElement>();
    panel->AddClass("inspector-panel");
    panel->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Column);
    return panel;
}

InspectorLayoutFixture MakeFixture()
{
    InspectorLayoutFixture fixture;
    fixture.ViewportH = kViewportH;
    // A closed section's content is hidden by the Foldout control's own sheet.
    fixture.ExtraSheets = {"Assets/UI/controls/Foldout.css"};
    return fixture;
}

// The entity inspector: a Mesh Renderer section with a toggle row, then the Material section the
// way the Mesh Renderer inspector fills it — a Material row, and the Material Properties foldout
// holding the material's sections.
struct EntityInspector
{
    UIElement* componentToggleRow = nullptr;
    UIElement* materialRow = nullptr;
    Foldout* materialProperties = nullptr;
    MaterialTree material;
};

EntityInspector BuildEntityInspector(UIElement* panel)
{
    EntityInspector e;
    auto meshRenderer = std::make_unique<InspectorSection>("Mesh Renderer");
    e.componentToggleRow =
        FieldContainerOf(InspectorDrag::AddToggleRow(meshRenderer->GetContentRoot(), "CastShadows", true, [](bool) {}))
            ->GetParent();
    panel->AddChild(std::move(meshRenderer));

    auto materialSection = std::make_unique<InspectorSection>("Material");
    UIElement* body = materialSection->GetContentRoot();
    e.materialRow = InspectorUI::AddRow(body);
    InspectorUI::AddLabel(e.materialRow, "Material");
    std::unique_ptr<Foldout> properties = Editor::MakeMaterialPropertiesFoldout();
    e.materialProperties = properties.get();
    e.material = BuildMaterialSections(properties->GetContentContainer());
    body->AddChild(std::move(properties));
    panel->AddChild(std::move(materialSection));
    return e;
}

} // namespace

// A section bar keeps one row gap under it, open or closed, the gap between two rows. Rows carry
// their gap below them and hide with a closed section, so a bar without its own gap has the first
// row of an open section sitting on it, and closed sections stack bar on bar.
TEST(MaterialInspectorLayout, EveryBarKeepsTheRowGapUnderIt)
{
    auto panel = MakePanel();
    const EntityInspector e = BuildEntityInspector(panel.get());
    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    const MaterialTree& m = e.material;
    const float rowGap = GapBetween(m.lightingModelRow, m.doubleSidedRow);
    ASSERT_GT(rowGap, 0.0f) << "two rows in one section have no gap between them, so the row gap "
                               "this test compares against is not in effect; this test is vacuous";

    EXPECT_NEAR(GapBetween(Bar(m.shader), m.lightingModelRow), rowGap, kTolerancePx)
        << "an open section's first row does not sit a row gap under its bar";
    EXPECT_NEAR(GapBetween(Bar(e.materialProperties), Bar(m.shader)), rowGap, kTolerancePx)
        << "the first section bar does not sit a row gap under the Material Properties bar";
    EXPECT_NEAR(GapBetween(Bar(m.advanced), Bar(m.file)), rowGap, kTolerancePx)
        << "two closed sections' bars are not a row gap apart";
    EXPECT_NEAR(GapBetween(m.doubleSidedRow, Bar(m.properties)), rowGap, kTolerancePx)
        << "the next bar does not sit a row gap under an open section's last row";
}

// A line that is not a row — a note, a diagnostics block, a lone button — keeps the same gap, so it
// does not sit on the bar beneath it when it ends a section.
TEST(MaterialInspectorLayout, ALineThatIsNotARowKeepsTheRowGap)
{
    auto panel = MakePanel();
    const EntityInspector e = BuildEntityInspector(panel.get());
    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    const MaterialTree& m = e.material;
    const float rowGap = GapBetween(m.lightingModelRow, m.doubleSidedRow);
    ASSERT_GT(rowGap, 0.0f) << "the row gap is not in effect; this test is vacuous";
    ASSERT_GT(m.notice->GetLayoutHeight(), 0.0f) << "the note measured no text; this test is vacuous";

    EXPECT_NEAR(GapBetween(m.notice, Bar(m.textures)), rowGap, kTolerancePx)
        << "a note that ends a section sits on the next section's bar";
    EXPECT_NEAR(GapBetween(m.recompile, Bar(m.advanced)), rowGap, kTolerancePx)
        << "the Recompile button sits on the next section's bar";
}

// A warning line (the height slot's refusal) keeps the rhythm too: one row gap under the row above
// it, not the warning treatment's own margin on top of the row's gap, and one above the next bar.
TEST(MaterialInspectorLayout, AWarningLineKeepsTheRowGap)
{
    auto panel = MakePanel();
    const EntityInspector e = BuildEntityInspector(panel.get());
    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    const MaterialTree& m = e.material;
    const float rowGap = GapBetween(m.lightingModelRow, m.doubleSidedRow);
    ASSERT_GT(rowGap, 0.0f) << "the row gap is not in effect; this test is vacuous";
    ASSERT_GT(m.warning->GetLayoutHeight(), 0.0f) << "the warning measured no text; this test is vacuous";

    EXPECT_NEAR(GapBetween(m.textureRow, m.warning), rowGap, kTolerancePx)
        << "the warning sits more than a row gap under the texture slot above it";
    EXPECT_NEAR(GapBetween(m.warning, Bar(m.diagnostics)), rowGap, kTolerancePx)
        << "the warning sits on the next section's bar";
}

// The warning keeps the warning colour in the asset view, whose rule for plain inspector text is more
// specific than the warning's and would otherwise turn it the soft grey of the lines around it.
TEST(MaterialInspectorLayout, AWarningLineKeepsTheWarningColourInTheAssetView)
{
    auto panel = MakePanel();
    auto reference = std::make_unique<Label>();
    reference->AddClass("inspector-warning");
    reference->SetText("A warning outside any inspector content");
    const Label* referenceWarning = reference.get();
    panel->AddChild(std::move(reference));
    auto assetContent = std::make_unique<UIElement>();
    assetContent->AddClass("inspector-asset-content");
    auto root = std::make_unique<UIElement>();
    const MaterialTree m = BuildMaterialSections(root.get());
    assetContent->AddChild(std::move(root));
    panel->AddChild(std::move(assetContent));

    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    ASSERT_NE(m.notice->GetResolvedStyle().Visual.Color, referenceWarning->GetResolvedStyle().Visual.Color)
        << "a plain line already has the warning colour; this test is vacuous";
    EXPECT_EQ(m.warning->GetResolvedStyle().Visual.Color, referenceWarning->GetResolvedStyle().Visual.Color)
        << "the material warning lost the warning colour in the asset view";
}

// A row whose value is unused in the material's current state (Relief Depth while the relief is
// refused) reads as inactive: its label and the value it still shows take the disabled text colour
// together, beside an active row's.
TEST(MaterialInspectorLayout, AnInactiveRowDimsItsLabelAndItsValue)
{
    auto panel = MakePanel();
    auto root = std::make_unique<UIElement>();
    root->AddClass("material-inspector");
    const auto active = InspectorDrag::AddSliderWithFloatValueRow(root.get(), "Active", 0.02f, 0.0f, 0.1f);
    const auto inactive = InspectorDrag::AddSliderWithFloatValueRow(root.get(), "Inactive", 0.02f, 0.0f, 0.1f);
    Editor::MakeMaterialRowInactive(inactive);
    panel->AddChild(std::move(root));
    InspectorLayoutFixture fixture = MakeFixture();
    // The numeric field's disabled look is its own sheet's, which the field attaches in the editor.
    fixture.ExtraSheets.push_back("Assets/UI/controls/FloatField.css");
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    ASSERT_NE(FindWithClass(inactive.ValueField, "float-field-input"), nullptr)
        << "the numeric field has no text input under the class its rules select; this test is vacuous";
    const uint32_t inactiveLabel = inactive.Label->GetResolvedStyle().Visual.Color;
    EXPECT_NE(inactiveLabel, active.Label->GetResolvedStyle().Visual.Color)
        << "an inactive row's label reads like an active one's";
    EXPECT_NE(ValueTextColour(inactive.ValueField), ValueTextColour(active.ValueField))
        << "an inactive row's value reads like an active one's";
    EXPECT_EQ(ValueTextColour(inactive.ValueField), inactiveLabel)
        << "an inactive row's value stands out from its greyed label";
}

// An empty texture slot's asset field starts where every other field in the inspector does, and
// sits centred on its slot name.
TEST(MaterialInspectorLayout, ATextureSlotFieldStartsOnTheValueColumn)
{
    auto panel = MakePanel();
    const EntityInspector e = BuildEntityInspector(panel.get());
    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    const MaterialTree& m = e.material;
    ASSERT_GT(m.lightingModelField->GetLayoutX(), m.lightingModelRow->GetLayoutX())
        << "the value column does not start after the label column; this test is vacuous";

    EXPECT_NEAR(m.textureFieldContainer->GetLayoutX(), m.lightingModelField->GetLayoutX(), kTolerancePx)
        << "the texture slot's field is off the value column the other fields start on";
    EXPECT_NEAR(CentreY(m.textureField), CentreY(m.textureName), kTolerancePx)
        << "the texture slot's field is not centred on its slot name";
}

// A material row is exactly as tall as the component rows it sits among: a toggle row in the
// material and one in the Mesh Renderer section above it hold the same controls.
TEST(MaterialInspectorLayout, AMaterialRowIsAsTallAsAComponentRow)
{
    auto panel = MakePanel();
    const EntityInspector e = BuildEntityInspector(panel.get());
    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    ASSERT_GT(e.componentToggleRow->GetLayoutHeight(), 0.0f) << "the component row has no height; this test is vacuous";
    EXPECT_NEAR(e.material.doubleSidedRow->GetLayoutHeight(), e.componentToggleRow->GetLayoutHeight(), kTolerancePx)
        << "a material toggle row is a different height from the component toggle row around it";
}

// Selecting a material asset lays the same sections out in the asset inspector's content: rows
// span that content as the other asset inspectors' rows do, and the bars start with them.
TEST(MaterialInspectorLayout, TheAssetViewRowsSpanTheAssetContent)
{
    auto panel = MakePanel();
    auto assetContent = std::make_unique<UIElement>();
    assetContent->AddClass("inspector-asset-content");
    assetContent->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::PaddingLeft, StyleLength::Px(kAssetContentInsetPx))
        .Set(Style::PaddingRight, StyleLength::Px(kAssetContentInsetPx));
    UIElement* content = assetContent.get();
    auto root = std::make_unique<UIElement>();
    const MaterialTree m = BuildMaterialSections(root.get());
    content->AddChild(std::move(root));
    panel->AddChild(std::move(assetContent));

    InspectorLayoutFixture fixture = MakeFixture();
    if (!fixture.Build(std::move(panel)))
        GTEST_SKIP() << fixture.Diagnostic;

    const float innerLeft = content->GetLayoutX() + kAssetContentInsetPx;
    const float innerRight = content->GetLayoutX() + content->GetLayoutWidth() - kAssetContentInsetPx;
    ASSERT_GT(innerRight, innerLeft) << "the asset content has no width; this test is vacuous";

    EXPECT_NEAR(m.lightingModelRow->GetLayoutX(), innerLeft, kTolerancePx)
        << "the asset view's rows start inside the asset content's edge";
    EXPECT_NEAR(m.lightingModelRow->GetLayoutX() + m.lightingModelRow->GetLayoutWidth(), innerRight, kTolerancePx)
        << "the asset view's rows stop short of the asset content's edge";
    EXPECT_NEAR(Bar(m.shader)->GetLayoutX(), m.lightingModelRow->GetLayoutX(), kTolerancePx)
        << "the asset view's section bars do not start with their rows";
    EXPECT_NEAR(GapBetween(Bar(m.shader), m.lightingModelRow), GapBetween(m.lightingModelRow, m.doubleSidedRow),
                kTolerancePx)
        << "the asset view's first row does not sit a row gap under its bar";
}

// The rules above select what the material inspector sets: .material-inspector on the root it
// builds into, a plain root in the asset view, and the classes its builders give each foldout and
// line. A borrowed component body around the asset view's root brings the component's icon-column
// indent and padding with it, and a foldout or line built by hand misses the classes its rules
// select.
TEST(MaterialInspectorLayout, TheInspectorSetsTheClassesItsRowRulesSelect)
{
    const std::string source = ReadEditorFile("Source/Inspectors/MaterialInspector.cpp");
    const std::string meshRenderer = ReadEditorFile("Source/Inspectors/MeshRendererInspector.cpp");
    ASSERT_FALSE(source.empty()) << "the material inspector source did not read; this test is vacuous";
    ASSERT_FALSE(meshRenderer.empty()) << "the mesh renderer inspector source did not read; this test is vacuous";

    EXPECT_NE(source.find("root->AddClass(\"material-inspector\")"), std::string::npos)
        << "the material inspector no longer marks its root, so none of its row rules apply";
    EXPECT_EQ(source.find("\"inspector-section-body\""), std::string::npos)
        << "the material inspector wraps itself in a component section body again, which indents "
           "the asset view by the component's icon column";
    EXPECT_EQ(source.find("std::make_unique<Foldout>"), std::string::npos)
        << "the material inspector builds a section by hand, so its bar and content miss the classes "
           "the section rules select; build it with Editor::MakeMaterialSection";
    EXPECT_EQ(source.find("AddTextBlock("), std::string::npos)
        << "the material inspector adds a line with the generic text block, which misses the class "
           "the line gap rule selects; add it with Editor::AddMaterialLine";
    for (const char* button : {"convertBtn", "recompileBtn"})
    {
        EXPECT_NE(source.find(std::string(button) + "->AddClass(\"material-line\")"), std::string::npos)
            << button << " lost the class the line gap rule selects, so it sits on whatever is below it";
    }
    EXPECT_NE(meshRenderer.find("Editor::MakeMaterialPropertiesFoldout()"), std::string::npos)
        << "the mesh renderer inspector builds the Material Properties foldout by hand, so its bar and "
           "content miss the classes their rules select";
}
