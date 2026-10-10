// The view -> panel + focus-policy mapping of focus_view.
//
// The trap this locks: the two viewport panels need OPPOSITE handling of UI
// focus, and getting it backwards produces a bug that no screenshot shows. The
// Scene View drives its camera through the editor input action system, so a
// focused viewport element makes UIManager consume W/A/S/D before those actions
// see them (SceneViewPanel::FocusViewport clears focus for exactly this reason);
// the Game View is the reverse, its viewport must hold focus for input to reach
// the running game. Both variants leave the right tab in front and look
// identical in a capture — only the keyboard stops working.

#include <gtest/gtest.h>

#include "DebugServer/ViewFocusRequest.h"

#include "EditorPanelIds.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
using GameEngine::Editor::ViewFocusRequest;
using json = nlohmann::json;
namespace PanelIds = GameEngine::EditorPanelIds;

ViewFocusRequest Parse(const char* view)
{
    return ViewFocusRequest(json{{"view", view}});
}

TEST(ViewFocusRequestTests, SceneResolvesToTheSceneViewPanel)
{
    const ViewFocusRequest r = Parse("scene");
    ASSERT_TRUE(r.Error().empty()) << r.Error();
    EXPECT_STREQ(r.PanelId(), PanelIds::SceneView);
}

TEST(ViewFocusRequestTests, GameResolvesToTheGameViewPanel)
{
    const ViewFocusRequest r = Parse("game");
    ASSERT_TRUE(r.Error().empty()) << r.Error();
    EXPECT_STREQ(r.PanelId(), PanelIds::GameView);
}

TEST(ViewFocusRequestTests, TheTwoViewsAskForOppositeUiFocus)
{
    // The whole point of the type. Scene View must have UI focus CLEARED so the
    // editor input action system sees its camera keys; Game View must have its
    // viewport FOCUSED so runtime input arrives. Swapping these compiles, keeps
    // both tabs working, and silently breaks one view's keyboard.
    EXPECT_FALSE(Parse("scene").WantsViewportUiFocus())
        << "focusing the Scene View viewport would let UIManager eat W/A/S/D";
    EXPECT_TRUE(Parse("game").WantsViewportUiFocus())
        << "the Game View viewport must hold focus for input to reach the game";
}

TEST(ViewFocusRequestTests, TheTwoViewsAreDifferentPanels)
{
    EXPECT_STRNE(Parse("scene").PanelId(), Parse("game").PanelId());
}

TEST(ViewFocusRequestTests, AViewNamingNothingIsAnErrorNotADefault)
{
    // Falling back to a default view would answer "focused" for a panel the
    // caller never asked about, and the next screenshot would capture it.
    const ViewFocusRequest r = Parse("gameview");
    ASSERT_FALSE(r.Error().empty());
    EXPECT_NE(r.Error().find("gameview"), std::string::npos) << "name the value that matched nothing";
    EXPECT_NE(r.Error().find("scene"), std::string::npos) << "state the values that would match";
    EXPECT_NE(r.Error().find("game"), std::string::npos);
}

TEST(ViewFocusRequestTests, AMissingOrWrongTypedViewIsAnError)
{
    EXPECT_FALSE(ViewFocusRequest(json::object()).Error().empty());
    EXPECT_FALSE(ViewFocusRequest(json{{"view", nullptr}}).Error().empty());
    EXPECT_FALSE(ViewFocusRequest(json{{"view", 0}}).Error().empty());
    EXPECT_FALSE(ViewFocusRequest(json{{"view", true}}).Error().empty());
}

TEST(ViewFocusRequestTests, ViewMatchingIsExact)
{
    // Reachable over raw IPC with any string, so a near-miss must be refused
    // rather than guessed at.
    EXPECT_FALSE(Parse("Scene").Error().empty());
    EXPECT_FALSE(Parse("SceneView").Error().empty());
    EXPECT_FALSE(Parse("").Error().empty());
}

TEST(ViewFocusRequestTests, ARefusedRequestCarriesNoPanel)
{
    const ViewFocusRequest r = Parse("inspector");
    ASSERT_FALSE(r.Error().empty());
    EXPECT_EQ(r.PanelId(), nullptr) << "a refused request must not hand the handler a panel to open";
}

} // namespace
