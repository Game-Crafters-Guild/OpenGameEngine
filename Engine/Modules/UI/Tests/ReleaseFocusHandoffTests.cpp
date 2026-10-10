// A click handler that claims focus for another element during the release
// dispatch keeps that focus. The release edge's default focus assignment must
// not hand it back to the control that was pressed.
//
// The editor's top-toolbar Universal Search button is the production shape:
// its click handler opens the search dialog, which focuses its query field.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <button id="opener"/>
  <button id="field"/>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; padding: 40px; }
#opener { width: 120px; height: 40px; }
#field { width: 120px; height: 40px; }
)";

} // namespace

TEST(ReleaseFocusHandoff, ClickHandlerFocusSurvivesTheReleaseEdge)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    GameEngine::UIElement* opener = fx.Element("opener");
    ASSERT_NE(opener, nullptr);
    opener->RegisterEventHandler(GameEngine::kEventButtonClick, [&fx](GameEngine::UIEvent&) {
        fx.Manager().SetFocusById("field");
    });

    ASSERT_TRUE(fx.FocusViaClick("opener"));
    EXPECT_EQ(fx.Manager().GetFocusedElementId(), "field")
        << "the release edge handed focus back to the pressed button";
}
