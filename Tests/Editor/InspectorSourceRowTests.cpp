#include <gtest/gtest.h>

#include "Inspectors/InspectorUIHelpers.h"

#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <filesystem>

using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::InspectorUI::AddSourceFileRow;

namespace
{

TEST(InspectorSourceRowTests, TheSourceRowNamesTheFileInAReadableLabel)
{
    UIElement root;
    AddSourceFileRow(&root, std::filesystem::path("/proj/assets/Rock.glsl"));

    const auto& children = root.GetChildren();
    ASSERT_EQ(children.size(), 2u);

    auto* header = dynamic_cast<Label*>(children[0].get());
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->GetText(), "Source:");
    EXPECT_TRUE(header->HasClass("inspector-section-subheader"));

    auto* path = dynamic_cast<Label*>(children[1].get());
    ASSERT_NE(path, nullptr);
    EXPECT_EQ(path->GetText(), std::filesystem::path("/proj/assets/Rock.glsl").string());
    EXPECT_TRUE(path->HasClass("inspector-source-path"));
}

// The inspector column is far too narrow to read code in, so the row must stay a flat
// label pair. A nested host reappearing here is the inline preview coming back.
TEST(InspectorSourceRowTests, TheSourceRowEmbedsNoInlineCodePreview)
{
    UIElement root;
    AddSourceFileRow(&root, std::filesystem::path("/proj/assets/Rock.glsl"));

    for (const auto& child : root.GetChildren())
    {
        EXPECT_TRUE(child->GetChildren().empty());
        EXPECT_FALSE(child->HasClass("inspector-script-source-host"));
        EXPECT_FALSE(child->HasClass("script-editor-textarea"));
    }
}

TEST(InspectorSourceRowTests, ANullParentIsIgnored)
{
    AddSourceFileRow(nullptr, std::filesystem::path("/proj/assets/Rock.glsl"));
}

} // namespace
