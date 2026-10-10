// The terrain panel's sections are the material inspector's sections: a header bar over the rows,
// not a bold line above them. Two halves have to meet for that — the panel asks for a class, the
// stylesheet answers it — and a half that goes missing fails silently, leaving either the control's
// default plate indented inside the component body, or a bar that nothing styles.
//
// Source-level, because the terrain inspector cannot be built in this process: it is a component
// inspector over a live World and the terrain service, and the sibling guards in this target
// (TerrainMaterialPanelVocabularyTests, PanelDefaultTabIconTests) scan the tree for the same reason.
// What is locked is that the two halves stay paired, not the declarations — restyle freely.
//
// The stylesheet half of the pair is asserted once, by
// TerrainRoleMaterials.TheSectionHeaderRuleExistsInTheInspectorStylesheet, which also carries the
// positive control that the shared rule still paints. This file owns the panel half.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadEditorFile(const std::string& relativePath)
{
    const std::filesystem::path path = std::filesystem::path(GE_EDITOR_SOURCE_DIR) / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of the named free function, from its signature to the closing brace in column 0. Scoping to
// it is the point: every section in the panel is built by AddSection, so finding the classes
// *anywhere* in the file would keep passing after AddSection stopped adding them.
std::string FunctionBody(const std::string& source, const std::string& signatureFragment)
{
    const size_t start = source.find(signatureFragment);
    if (start == std::string::npos)
        return {};
    const size_t open = source.find("\n{", start);
    if (open == std::string::npos)
        return {};
    const size_t end = source.find("\n}", open + 1);
    if (end == std::string::npos)
        return {};
    return source.substr(open, end - open);
}

// The selector a rule actually carries: the text between the previous rule's closing brace and
// this rule's opening brace, with block comments removed and whitespace collapsed. Reading it that
// way is the point — a comment placed inside the selector above is valid CSS that silently makes
// the rule below a descendant of it, so a search for the selector's own text still finds it.
std::string SelectorSegmentBefore(const std::string& sheet, size_t openBrace)
{
    const size_t previous = sheet.rfind(char(125), openBrace);
    const size_t start = (previous == std::string::npos) ? 0 : previous + 1;
    std::string segment = sheet.substr(start, openBrace - start);

    for (size_t open = segment.find("/*"); open != std::string::npos; open = segment.find("/*"))
    {
        const size_t close = segment.find("*/", open + 2);
        if (close == std::string::npos)
        {
            segment.erase(open);
            break;
        }
        segment.erase(open, close + 2 - open);
    }

    std::string collapsed;
    for (const char c : segment)
    {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            if (!collapsed.empty() && collapsed.back() != ' ')
                collapsed.push_back(' ');
        }
        else
        {
            collapsed.push_back(c);
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ')
        collapsed.pop_back();
    return collapsed;
}

} // namespace

TEST(TerrainInspectorSectionStyle, EverySectionIsBuiltByTheOneHelperThatStylesItAsABar)
{
    const std::string panel = ReadEditorFile("Source/Terrain/TerrainInspector.cpp");
    ASSERT_FALSE(panel.empty()) << "the terrain panel source did not read; this test is vacuous";

    const std::string addSection = FunctionBody(panel, "UIElement* AddSection(UIElement* parent");
    ASSERT_FALSE(addSection.empty())
        << "AddSection is gone or renamed; this test no longer looks at the code that styles a "
           "section, so re-point it before trusting it";

    // Every terrain section goes through the ONE inspector-wide helper. That is the whole
    // guarantee: the classes themselves live with the helper, so a restyle there reaches terrain
    // without touching this panel.
    EXPECT_NE(addSection.find("InspectorUI::AddComponentSection"), std::string::npos)
        << "a terrain section is built by hand instead of the shared inspector helper";

    const std::string helper = ReadEditorFile("Source/Inspectors/InspectorComponentSection.cpp");
    ASSERT_FALSE(helper.empty()) << "the shared section helper did not read; this test is vacuous";

    // A section wears the shared foldout treatment AND the component-section class that pulls its
    // bar out of the component body's indent. Both, in the helper that builds every section.
    EXPECT_NE(helper.find("\"rp-foldout\""), std::string::npos)
        << "the shared sections no longer take the inspector's shared foldout treatment";
    EXPECT_NE(helper.find("\"inspector-component-section\""), std::string::npos)
        << "the shared sections no longer identify as component sections";

    // A subheader is what the sections replaced. The panel may still use one nowhere.
    EXPECT_EQ(panel.find("\"inspector-body-subheader\""), std::string::npos)
        << "a terrain group is back to a bold line where its siblings are bars";
}

// The panel is rebuilt from scratch on a domain switch, a library bind and every texture
// assignment — including assignments made from inside a layer card. A section that reopens on each
// of those is a section nobody can close, so no section may be built with a hardcoded expansion
// state instead of the remembered one.
TEST(TerrainInspectorSectionStyle, NoSectionHardcodesItsExpansionInsteadOfRememberingIt)
{
    const std::string panel = ReadEditorFile("Source/Terrain/TerrainInspector.cpp");
    ASSERT_FALSE(panel.empty());

    const std::string addSection = FunctionBody(panel, "UIElement* AddSection(UIElement* parent");
    ASSERT_FALSE(addSection.empty());
    EXPECT_NE(addSection.find("InspectorUI::AddComponentSection"), std::string::npos)
        << "a terrain section is built by hand instead of the shared inspector helper";

    // The remembered flag is read into the control and written back when the user folds it. It
    // lives with the helper, which is what makes it true for every inspector section at once.
    const std::string helper = ReadEditorFile("Source/Inspectors/InspectorComponentSection.cpp");
    ASSERT_FALSE(helper.empty()) << "the shared section helper did not read; this test is vacuous";
    EXPECT_NE(helper.find("SetExpanded(expanded)"), std::string::npos)
        << "the shared helper no longer seeds the section from its remembered state";
    EXPECT_NE(helper.find("SetOnExpandedChanged"), std::string::npos)
        << "the shared helper no longer records the user's choice, so nothing is remembered";

    // The panel itself sets no expansion: every section defers to the helper, so a direct call
    // here is a card that reopens on every rebuild.
    size_t occurrences = 0;
    for (size_t pos = panel.find("SetExpanded("); pos != std::string::npos;
         pos = panel.find("SetExpanded(", pos + 1))
        ++occurrences;
    EXPECT_EQ(occurrences, 0u)
        << "a terrain section sets its expansion directly instead of going through AddSection, so "
           "it reopens on every panel rebuild";
}

// The grass Backlight row carries a strength AND the colour it tints, in ONE row. That claim is a
// PAIR like the section bars above: the panel asks for a class and the stylesheet answers it. With
// the class missing the row still builds, still holds both controls, and renders as two lines —
// the field container lays a float field out full-width and the swatch wraps under it. That is
// invisible to every structural check (the nodes are all present, in the right parent) and obvious
// in a screenshot, which is exactly the failure this file exists to catch for sections.
TEST(TerrainInspectorSectionStyle, TheBacklightRowAsksForTheClassThatKeepsItOneRow)
{
    const std::string panel = ReadEditorFile("Source/Terrain/TerrainInspector.cpp");
    ASSERT_FALSE(panel.empty()) << "the terrain panel source did not read; this test is vacuous";

    // The panel half: the swatch is appended to the float row's own field container, and that
    // container is given the row class.
    const size_t addClass = panel.find("inspector-grass-value-and-swatch");
    ASSERT_NE(addClass, std::string::npos)
        << "the backlight row no longer asks for the row class, so its swatch wraps onto a second "
           "line and the one row is two";
    const size_t swatch = panel.find("AddGrassColorSwatch(field", addClass);
    EXPECT_NE(swatch, std::string::npos)
        << "the class is added but the swatch is not appended to the container that wears it";

    // The stylesheet half. A class with no rule behind it fails the same way the wrap does.
    const std::filesystem::path css =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/UI/theme/inspector.css";
    std::ifstream in(css, std::ios::binary);
    ASSERT_TRUE(in.good()) << "the inspector stylesheet did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string sheet = ss.str();

    const size_t rule = sheet.find(".inspector-grass-value-and-swatch {");
    ASSERT_NE(rule, std::string::npos)
        << "nothing styles the class the panel asks for, so the swatch wraps";
    const size_t end = sheet.find(char(125), rule);
    ASSERT_NE(end, std::string::npos);
    const std::string body = sheet.substr(rule, end - rule);
    EXPECT_NE(body.find("flex-direction: row"), std::string::npos)
        << "the row class no longer lays its children out in a row";

    // The float field has to yield width, or it fills the container and pushes the swatch out
    // regardless of the direction above.
    const size_t child = sheet.find(".inspector-grass-value-and-swatch > .float-field {");
    ASSERT_NE(child, std::string::npos)
        << "nothing constrains the float field, so it takes the whole row and the swatch wraps";
    const size_t childEnd = sheet.find(char(125), child);
    ASSERT_NE(childEnd, std::string::npos);
    EXPECT_NE(sheet.substr(child, childEnd - child).find("flex:"), std::string::npos)
        << "the float field no longer shares the row";
}

// The grass Root Fade row is one two-thumb slider with the span's two ends written beside the
// track, and it is the same PAIR again: the panel asks for a class, the stylesheet answers it.
// Both value labels have to hold one fixed width, because the track between them is
// `flex: 1 1 0` — content-sized labels grow and shrink with their own text, so the track resizes
// under the thumb the user is dragging. Nothing structural sees that: the labels are present, in
// the right parent, carrying the right text.
TEST(TerrainInspectorSectionStyle, TheRootFadeValuesAskForTheClassThatHoldsTheirWidth)
{
    const std::string panel = ReadEditorFile("Source/Terrain/TerrainInspector.cpp");
    ASSERT_FALSE(panel.empty()) << "the terrain panel source did not read; this test is vacuous";

    // The panel half. Scoped to the row, so the class cannot be answered by some other row's use
    // of it further up the panel.
    const size_t row = panel.find("\"Root Fade\"");
    ASSERT_NE(row, std::string::npos)
        << "the Root Fade row is gone or renamed; re-point this test before trusting it";
    EXPECT_NE(panel.find("SetRangeMode(true)", row), std::string::npos)
        << "the Root Fade row is no longer one two-thumb slider, so the pair of end values this "
           "test is about may not be what the row shows";
    EXPECT_NE(panel.find("\"inspector-range-value\"", row), std::string::npos)
        << "the Root Fade end values no longer ask for the width class, so they size to their own "
           "text and the track resizes under a moving thumb";

    // The stylesheet half.
    const std::filesystem::path css =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/UI/theme/inspector.css";
    std::ifstream in(css, std::ios::binary);
    ASSERT_TRUE(in.good()) << "the inspector stylesheet did not read; this test is vacuous";
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string sheet = ss.str();

    const size_t rule = sheet.find(".inspector-range-value {");
    ASSERT_NE(rule, std::string::npos)
        << "nothing styles the class the panel asks for, so the end values content-size";

    const size_t brace = sheet.find(char(123), rule);
    ASSERT_NE(brace, std::string::npos);

    // The class has to stand alone. Scoped to anything — a comment folded into the selector above
    // is enough — it stops matching in the terrain inspector, and no parse error is logged because
    // the result is still valid CSS that simply means something else.
    EXPECT_EQ(SelectorSegmentBefore(sheet, brace), ".inspector-range-value")
        << "the value class is scoped instead of standing alone, so it never matches in the "
           "terrain inspector and the rule below is inert";

    const size_t end = sheet.find(char(125), brace);
    ASSERT_NE(end, std::string::npos);
    const std::string body = sheet.substr(brace, end - brace);
    EXPECT_NE(body.find("flex: 0 0 "), std::string::npos)
        << "the end values no longer refuse to grow or shrink, so the track between them moves as "
           "the values are dragged";

    // Positive control: the track really is the flexible half of the row, which is what makes a
    // content-sized value label move it.
    const size_t track = sheet.find(".inspector-field.inspector-slider-with-value > .property-slider {");
    ASSERT_NE(track, std::string::npos)
        << "the slider row no longer sizes its track, so this test's premise is stale";
    const size_t trackEnd = sheet.find(char(125), track);
    ASSERT_NE(trackEnd, std::string::npos);
    EXPECT_NE(sheet.substr(track, trackEnd - track).find("flex: 1 1 0"), std::string::npos)
        << "the track no longer takes the row's slack, so fixing the value widths may be moot";
}
