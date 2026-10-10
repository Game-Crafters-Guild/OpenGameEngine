// Child-list mutations must restyle the siblings whose structural pseudo-class
// match they flip. Each test resolves the style once, mutates the child list,
// updates again and checks the resolved background of the siblings whose
// :last-child / :only-child / :nth-last-child / :last-of-type / :empty state
// changed.

#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

namespace
{

constexpr uint32_t kMarked = 0xFFFF0000u; // rgb(255, 0, 0), ARGB
constexpr uint32_t kUnmarked = 0x00000000u;

// Owns the manager for one test, on the shared device; the layout and stylesheet
// come from strings.
struct StructuralFixture
{
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement ReleaseRetirement;
    Rendering::IDevice* Device = nullptr;
    std::unique_ptr<UIManager> Ui;

    bool Build(const std::string& xml, const std::string& css)
    {
        UIRegistration::RegisterBuiltInControls();
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        std::unique_ptr<UIElement> root;
        if (!XMLParser::ParseLayoutFromString(xml, root))
            return false;
        Stylesheet sheet{};
        if (!CSSParser::ParseStylesFromString(css, sheet))
            return false;
        Ui = std::make_unique<UIManager>(Device);
        Ui->SetRoot(std::move(root));
        Ui->AddStylesheet(std::make_shared<Stylesheet>(sheet));
        Update();
        return true;
    }

    void Update()
    {
        Ui->Update(0.0f, /*interactive=*/false);
        Ui->Update(0.0f, /*interactive=*/false);
    }

    UIElement* Find(const char* id) const { return Ui->GetRootElement()->FindById(id); }

    uint32_t Background(const char* id) const
    {
        const UIElement* el = Find(id);
        return el ? el->GetResolvedStyle().Visual.BackgroundColor : kUnmarked;
    }
};

std::unique_ptr<UIElement> MakeItem(const char* id)
{
    auto el = std::make_unique<UIElement>();
    el->SetId(id);
    el->AddClass("item");
    return el;
}

} // namespace

TEST(StructuralPseudoRestyleTests, AppendMovesLastChildOffPreviousLastSibling)
{
    StructuralFixture fx;
    if (!fx.Build(R"(<uielement id='root'><uielement id='a' class='item'/><uielement id='b' class='item'/></uielement>)",
                  ".item:last-child { background-color: rgb(255, 0, 0); }"))
        GTEST_SKIP() << "Device init failed";
    ASSERT_EQ(fx.Background("b"), kMarked);

    fx.Ui->GetRootElement()->AddChild(MakeItem("c"));
    fx.Update();

    EXPECT_EQ(fx.Background("b"), kUnmarked) << "the previous last child kept its :last-child style";
    EXPECT_EQ(fx.Background("c"), kMarked);
}

TEST(StructuralPseudoRestyleTests, NestedLastChildInNotIsTracked)
{
    StructuralFixture fx;
    if (!fx.Build(R"(<uielement id='root'><uielement id='a' class='item'/><uielement id='b' class='item'/></uielement>)",
                  ".item:not(:last-child) { background-color: rgb(255, 0, 0); }"))
        GTEST_SKIP() << "Device init failed";
    ASSERT_EQ(fx.Background("b"), kUnmarked);

    fx.Ui->GetRootElement()->AddChild(MakeItem("c"));
    fx.Update();

    EXPECT_EQ(fx.Background("b"), kMarked) << ":not(:last-child) did not re-match after an append";
}

TEST(StructuralPseudoRestyleTests, InsertAndRemoveToggleOnlyChildOnTheOtherSibling)
{
    StructuralFixture fx;
    if (!fx.Build(R"(<uielement id='root'><uielement id='a' class='item'/></uielement>)",
                  ".item:only-child { background-color: rgb(255, 0, 0); }"))
        GTEST_SKIP() << "Device init failed";
    ASSERT_EQ(fx.Background("a"), kMarked);

    UIElement* root = fx.Ui->GetRootElement();
    root->InsertChild(0, MakeItem("b"));
    fx.Update();
    EXPECT_EQ(fx.Background("a"), kUnmarked) << "a kept :only-child after a sibling was inserted";

    root->RemoveChild(fx.Find("b"));
    fx.Update();
    EXPECT_EQ(fx.Background("a"), kMarked) << "a did not regain :only-child after its sibling was removed";
}

TEST(StructuralPseudoRestyleTests, AppendShiftsNthLastChildOfEveryPrecedingSibling)
{
    StructuralFixture fx;
    if (!fx.Build(R"(<uielement id='root'><uielement id='a' class='item'/><uielement id='b' class='item'/></uielement>)",
                  ".item:nth-last-child(2) { background-color: rgb(255, 0, 0); }"))
        GTEST_SKIP() << "Device init failed";
    ASSERT_EQ(fx.Background("a"), kMarked);

    fx.Ui->GetRootElement()->AddChild(MakeItem("c"));
    fx.Update();

    EXPECT_EQ(fx.Background("a"), kUnmarked) << "a kept :nth-last-child(2) after an append";
    EXPECT_EQ(fx.Background("b"), kMarked);
}

TEST(StructuralPseudoRestyleTests, RemoveMovesLastOfTypeToEarlierSameTypeSibling)
{
    StructuralFixture fx;
    if (!fx.Build(R"(<uielement id='root'><uielement id='a' class='item'/><uielement id='b' class='item'/><label id='c' class='item'>c</label></uielement>)",
                  ".item:last-of-type { background-color: rgb(255, 0, 0); }"))
        GTEST_SKIP() << "Device init failed";
    ASSERT_EQ(fx.Background("a"), kUnmarked);
    ASSERT_EQ(fx.Background("b"), kMarked);

    fx.Ui->GetRootElement()->RemoveChild(fx.Find("b"));
    fx.Update();

    EXPECT_EQ(fx.Background("a"), kMarked) << "a did not become :last-of-type when the later element of its type was removed";
}

TEST(StructuralPseudoRestyleTests, FirstChildAddedClearsParentEmpty)
{
    StructuralFixture fx;
    if (!fx.Build(R"(<uielement id='root'><uielement id='box' class='item'/></uielement>)",
                  ".item:empty { background-color: rgb(255, 0, 0); }"))
        GTEST_SKIP() << "Device init failed";
    ASSERT_EQ(fx.Background("box"), kMarked);

    fx.Find("box")->AddChild(std::make_unique<UIElement>());
    fx.Update();

    EXPECT_EQ(fx.Background("box"), kUnmarked) << "the parent kept :empty after its first child was added";
}
