// The parameter contract of set_parallax_steps_view.
//
// The view is a debug switch a caller flips while comparing captures, so the two traps are the ones
// that silently show the wrong frame: a toggle that ignores the state the Scene Views are in (it
// would switch the view off when the caller meant to see it), and a non-boolean `enable` read as
// truthy (the string "false" would turn the view on).

#include <gtest/gtest.h>

#include "DebugServer/ParallaxStepsViewRequest.h"

#include <nlohmann/json.hpp>

namespace
{
using GameEngine::Editor::ParallaxStepsViewRequest;
using json = nlohmann::json;

TEST(ParallaxStepsViewRequestTests, EnableSetsTheViewWhateverItShowsNow)
{
    for (const bool shownNow : {false, true})
    {
        const ParallaxStepsViewRequest on(json{{"enable", true}}, shownNow);
        ASSERT_TRUE(on.Error().empty()) << on.Error();
        EXPECT_TRUE(on.Shown());
        const ParallaxStepsViewRequest off(json{{"enable", false}}, shownNow);
        ASSERT_TRUE(off.Error().empty()) << off.Error();
        EXPECT_FALSE(off.Shown());
    }
}

TEST(ParallaxStepsViewRequestTests, LeavingEnableOutTogglesFromWhatTheViewsShow)
{
    EXPECT_TRUE(ParallaxStepsViewRequest(json::object(), false).Shown());
    EXPECT_FALSE(ParallaxStepsViewRequest(json::object(), true).Shown());
    EXPECT_TRUE(ParallaxStepsViewRequest(json{{"enable", nullptr}}, false).Shown());
}

TEST(ParallaxStepsViewRequestTests, ANonBooleanEnableIsRefusedNotGuessed)
{
    for (const json& value : {json("false"), json(0), json(1), json::array()})
    {
        const ParallaxStepsViewRequest request(json{{"enable", value}}, false);
        EXPECT_NE(request.Error().find("'enable'"), std::string::npos) << value.dump();
    }
}

} // namespace
