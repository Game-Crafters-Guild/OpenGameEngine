// Where a debug-server "elementId" turns into the point a synthesized pointer aims at.
//
// FindById is unbounded and visibility-blind: it returns hidden elements and mount hosts
// too, and those carry a 0x0 layout box. Taking the centre of a 0x0 box yields the box's
// origin, so the pointer is aimed at a corner of the window rather than at the element,
// the press lands on whatever occupies that corner, and the caller is told it worked.
// Observed live in the editor's default layout: `Hierarchy`, `Inspector` and `SceneView`
// each resolved to the same point and each landed on the dock tab bar, all three replying
// `clicked: true`. The refusal pinned here is what stops that, so these tests are about
// which resolutions are allowed to produce coordinates at all.
#include <gtest/gtest.h>

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/ElementPointerTarget.h"

#include "UI/Internal/LayoutAccess.h"
#include "UI/UIElement.h"

#include <limits>
#include <memory>
#include <string>

using namespace GameEngine;
using nlohmann::json;

namespace
{

// Adds a child with an id and a committed layout rect, and returns it. The rect is written
// through the layout-solver accessor because these tests run no Yoga solve — the values
// under test are exactly the ones a solve would have committed.
UIElement* AddChildWithRect(UIElement& parent, const std::string& id, float x, float y, float w, float h)
{
    auto child = std::make_unique<UIElement>();
    child->SetId(id);
    UIElement* raw = child.get();
    parent.AddChild(std::move(child));
    UILayoutAccess::SetLastLayoutRect(*raw, x, y, w, h);
    return raw;
}

std::string RefusalMessage(UIElement& root, const std::string& elementId)
{
    float x = -1.0f;
    float y = -1.0f;
    json error;
    EXPECT_FALSE(Editor::ResolveElementPointerTarget(&root, elementId, x, y, error));
    EXPECT_TRUE(Editor::IsRefusal(error)) << error.dump();
    return Editor::HandlerResponse("0", error).value("error", std::string());
}

} // namespace

TEST(ElementPointerTargetTests, LaidOutElementResolvesToItsCentre)
{
    UIElement root;
    root.SetId("root");
    AddChildWithRect(root, "ProjectionToggle", 3945.0f, 73.0f, 24.0f, 24.0f);

    float x = 0.0f;
    float y = 0.0f;
    json error = json();

    ASSERT_TRUE(Editor::ResolveElementPointerTarget(&root, "ProjectionToggle", x, y, error));
    EXPECT_FLOAT_EQ(x, 3957.0f);
    EXPECT_FLOAT_EQ(y, 85.0f);
    EXPECT_TRUE(error.is_null()) << "a successful resolution must leave the error object untouched";
}

// The search has to be unbounded to be useful: a depth sweep of the editor's default layout
// put the button above between depth 12 and 14, far below the depth a tree dump defaults to.
TEST(ElementPointerTargetTests, ResolvesTargetsNestedFarBelowAnyTreeDumpDepth)
{
    UIElement root;
    UIElement* cursor = &root;
    for (int depth = 0; depth < 13; ++depth)
        cursor = AddChildWithRect(*cursor, "level" + std::to_string(depth), 0.0f, 0.0f, 800.0f, 600.0f);
    AddChildWithRect(*cursor, "DeepButton", 100.0f, 200.0f, 40.0f, 20.0f);

    float x = 0.0f;
    float y = 0.0f;
    json error;

    ASSERT_TRUE(Editor::ResolveElementPointerTarget(&root, "DeepButton", x, y, error));
    EXPECT_FLOAT_EQ(x, 120.0f);
    EXPECT_FLOAT_EQ(y, 210.0f);
}

TEST(ElementPointerTargetTests, MissingIdIsRefusedAndNamed)
{
    UIElement root;
    AddChildWithRect(root, "Present", 0.0f, 0.0f, 10.0f, 10.0f);

    const std::string message = RefusalMessage(root, "Absent");
    EXPECT_NE(message.find("Absent"), std::string::npos) << "the refusal must name the id that was not found: " << message;
}

// The defect this whole file exists for. A hidden element keeps its id and its place in the
// tree but loses its box, and a 0x0 box at the origin makes "the centre of the element" and
// "the top-left corner of the window" the same point.
TEST(ElementPointerTargetTests, ZeroAreaElementIsRefusedInsteadOfAimingAtItsOrigin)
{
    UIElement root;
    AddChildWithRect(root, "InlineSelectBtn", 0.0f, 0.0f, 0.0f, 0.0f);

    float x = -1.0f;
    float y = -1.0f;
    json error;

    EXPECT_FALSE(Editor::ResolveElementPointerTarget(&root, "InlineSelectBtn", x, y, error));
    EXPECT_FLOAT_EQ(x, -1.0f) << "a refused resolution must not write coordinates";
    EXPECT_FLOAT_EQ(y, -1.0f) << "a refused resolution must not write coordinates";

    const std::string message = Editor::HandlerResponse("0", error).value("error", std::string());
    EXPECT_NE(message.find("InlineSelectBtn"), std::string::npos) << message;
    EXPECT_NE(message.find("no layout box"), std::string::npos) << message;
}

// A non-zero origin must not rescue a zero-size element. This is the exact shape the three
// panel ids had in the live editor: y=40 from the dock layout, 0x0 size, so the "centre"
// was (0,40) for all of them and every one of them hit the tab bar there.
TEST(ElementPointerTargetTests, ZeroSizeAtANonZeroOriginIsStillRefused)
{
    UIElement root;
    AddChildWithRect(root, "Hierarchy", 0.0f, 40.0f, 0.0f, 0.0f);
    AddChildWithRect(root, "Inspector", 0.0f, 40.0f, 0.0f, 0.0f);

    EXPECT_NE(RefusalMessage(root, "Hierarchy").find("Hierarchy"), std::string::npos);
    EXPECT_NE(RefusalMessage(root, "Inspector").find("Inspector"), std::string::npos);
}

// A box flat on ONE axis is a real, reachable target and must keep resolving. Splitters are
// the shape that matters: zero-wide and full-height, they resolve to a point on their own
// line, hit-testing accepts a point on that edge, and elementId drags on them work. Refusing
// these alongside the fully-collapsed ones breaks a path that already worked.
TEST(ElementPointerTargetTests, OneAxisFlatBoxesStillResolveToTheirEdge)
{
    UIElement root;
    AddChildWithRect(root, "assets-splitter", 512.0f, 100.0f, 0.0f, 294.0f);
    AddChildWithRect(root, "FlatRow", 40.0f, 10.0f, 30.0f, 0.0f);

    float x = 0.0f;
    float y = 0.0f;
    json error;

    ASSERT_TRUE(Editor::ResolveElementPointerTarget(&root, "assets-splitter", x, y, error))
        << "a zero-width splitter is grabbable and must not be refused";
    EXPECT_FLOAT_EQ(x, 512.0f) << "the resolved point must stay on the splitter's line";
    EXPECT_FLOAT_EQ(y, 247.0f);

    ASSERT_TRUE(Editor::ResolveElementPointerTarget(&root, "FlatRow", x, y, error));
    EXPECT_FLOAT_EQ(x, 55.0f);
    EXPECT_FLOAT_EQ(y, 10.0f);
}

// Size can be finite while the origin is not, and the dimension check never looks at the
// origin. Injection re-checks and then drops the point silently, which loses the id that
// produced it — so the refusal has to happen here, where the element can still be named.
TEST(ElementPointerTargetTests, NonFinitePointIsRefusedAndNamesTheElement)
{
    UIElement root;
    AddChildWithRect(root, "DriftedPanel", std::numeric_limits<float>::quiet_NaN(), 10.0f, 40.0f, 20.0f);

    const std::string message = RefusalMessage(root, "DriftedPanel");
    EXPECT_NE(message.find("DriftedPanel"), std::string::npos) << message;
    EXPECT_NE(message.find("non-finite"), std::string::npos) << message;
}

TEST(ElementPointerTargetTests, NullRootIsRefusedRatherThanDereferenced)
{
    float x = 0.0f;
    float y = 0.0f;
    json error;

    EXPECT_FALSE(Editor::ResolveElementPointerTarget(nullptr, "Anything", x, y, error));
    EXPECT_TRUE(Editor::IsRefusal(error));
}
