// The action/state contract of set_play_mode.
//
// The trap this locks: play-mode transitions are the one editor action where a
// silent no-op is indistinguishable from success. PlayModeManager::EnterPlayMode
// and ExitPlayMode both `return` immediately when the editor is already in the
// state being asked for, so a handler that just forwarded the request would
// answer "ok" to `enter` while already playing — and the caller's next step
// (screenshot the running game, read runtime components) would silently describe
// edit mode instead. Every transition is therefore legal from exactly one state
// and an error from every other.
//
// The second half is the wire vocabulary: get_editor_state's "playMode" field and
// set_play_mode's response are the same strings from the same function, so a
// caller can compare what it asked for against what it got. A spelling that
// drifted between the two would break that comparison silently.
//
// The third is activateGameView, which decides whether managed UI resolves at
// all during the session: the Game View's composite is what publishes the
// gameplay UI host, so with another tab in front C# Ui.FindElement answers null
// and the session looks like a broken ABI. It must default to off (the layout is
// the caller's) and must never read a non-boolean as true — "false" is truthy
// under a coerced parse, which would raise a view the caller asked to leave down.

#include <gtest/gtest.h>

#include "DebugServer/PlayModeRequest.h"

#include "Automation/UiReplayCommandIds.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace
{
using GameEngine::Editor::PlayModeRequest;
using GameEngine::Editor::PlayModeState;
using GameEngine::Editor::PlayModeStateToString;
using json = nlohmann::json;

namespace Cmd = GameEngine::UiReplayCommandIds;

PlayModeRequest Parse(const char* action, PlayModeState from)
{
    return PlayModeRequest(json{{"action", action}}, from);
}

// ---------------------------------------------------------------------------
// Accepted transitions map onto the toolbar's own replay commands
// ---------------------------------------------------------------------------

TEST(PlayModeRequestTests, EnterFromEditingIsTheToolbarPlayCommand)
{
    const PlayModeRequest r = Parse("enter", PlayModeState::Edit);
    ASSERT_TRUE(r.Error().empty()) << r.Error();
    EXPECT_EQ(r.CommandId(), Cmd::PlayEnter);
    EXPECT_TRUE(r.IsEnter()) << "the handler needs this to tell a refused entry from a real one";
}

TEST(PlayModeRequestTests, ExitFromPlayingOrPausedIsTheToolbarStopCommand)
{
    for (const PlayModeState from : {PlayModeState::Play, PlayModeState::Paused})
    {
        const PlayModeRequest r = Parse("exit", from);
        ASSERT_TRUE(r.Error().empty()) << PlayModeStateToString(from) << ": " << r.Error();
        EXPECT_EQ(r.CommandId(), Cmd::PlayStop);
        EXPECT_FALSE(r.IsEnter());
    }
}

TEST(PlayModeRequestTests, ExitIsLegalFromChangeReview)
{
    // Exiting play can leave pending editor-command changes staged instead of
    // returning straight to Edit. That state is only escapable through the same
    // Stop command, so refusing `exit` there would strand the caller.
    const PlayModeRequest r = Parse("exit", PlayModeState::ChangeReview);
    ASSERT_TRUE(r.Error().empty()) << r.Error();
    EXPECT_EQ(r.CommandId(), Cmd::PlayStop);
}

TEST(PlayModeRequestTests, PauseAndResumeAreTheTwoDirectionsOfOneToggle)
{
    const PlayModeRequest pause = Parse("pause", PlayModeState::Play);
    ASSERT_TRUE(pause.Error().empty()) << pause.Error();
    EXPECT_EQ(pause.CommandId(), Cmd::PlayTogglePause);

    const PlayModeRequest resume = Parse("resume", PlayModeState::Paused);
    ASSERT_TRUE(resume.Error().empty()) << resume.Error();
    EXPECT_EQ(resume.CommandId(), Cmd::PlayTogglePause);
}

// ---------------------------------------------------------------------------
// Refusals — the half that stops a no-op reading as success
// ---------------------------------------------------------------------------

TEST(PlayModeRequestTests, EnteringWhileAlreadyRunningIsAnErrorNotANoOp)
{
    for (const PlayModeState from : {PlayModeState::Play, PlayModeState::Paused,
                                     PlayModeState::ChangeReview, PlayModeState::EnteringPlay,
                                     PlayModeState::ExitingPlay})
    {
        const PlayModeRequest r = Parse("enter", from);
        ASSERT_FALSE(r.Error().empty())
            << "enter from " << PlayModeStateToString(from) << " must not read as success";
        EXPECT_NE(r.Error().find(PlayModeStateToString(from)), std::string::npos)
            << "name the state that refused it: " << r.Error();
    }
}

TEST(PlayModeRequestTests, ExitingWhileAlreadyEditingIsAnErrorNotANoOp)
{
    // The sharpest case: the underlying Stop command answers "true" from Edit, so
    // only this refusal stops `exit` reporting success having done nothing.
    const PlayModeRequest r = Parse("exit", PlayModeState::Edit);
    ASSERT_FALSE(r.Error().empty());
    EXPECT_NE(r.Error().find("editing"), std::string::npos) << r.Error();
}

TEST(PlayModeRequestTests, PauseAndResumeRefuseEveryStateButTheirOwn)
{
    for (const PlayModeState from : {PlayModeState::Edit, PlayModeState::Paused,
                                     PlayModeState::ChangeReview})
        EXPECT_FALSE(Parse("pause", from).Error().empty())
            << "pause from " << PlayModeStateToString(from);

    for (const PlayModeState from : {PlayModeState::Edit, PlayModeState::Play,
                                     PlayModeState::ChangeReview})
        EXPECT_FALSE(Parse("resume", from).Error().empty())
            << "resume from " << PlayModeStateToString(from);
}

TEST(PlayModeRequestTests, ARefusalStillNamesTheActionItRefused)
{
    const PlayModeRequest r = Parse("pause", PlayModeState::Edit);
    ASSERT_FALSE(r.Error().empty());
    EXPECT_NE(r.Error().find("pause"), std::string::npos) << r.Error();
    EXPECT_NE(r.Error().find("playing"), std::string::npos)
        << "state the state it would be legal from: " << r.Error();
}

// ---------------------------------------------------------------------------
// activateGameView — the opt-in that decides whether managed UI resolves
// ---------------------------------------------------------------------------

TEST(PlayModeRequestTests, GameViewActivationIsOffUnlessAskedFor)
{
    // Raising a view changes what the user is looking at, so it is opt-in: an
    // omitted parameter enters play and touches nobody's dock layout.
    EXPECT_FALSE(Parse("enter", PlayModeState::Edit).ActivateGameView());
    EXPECT_FALSE(PlayModeRequest(json{{"action", "enter"}, {"activateGameView", nullptr}},
                                 PlayModeState::Edit)
                     .ActivateGameView())
        << "an explicit null is the same as absent";
}

TEST(PlayModeRequestTests, GameViewActivationIsCarriedForEveryAction)
{
    // Not gated on `enter` here: the flag says what the caller wants the editor
    // to look like, and the handler owns when raising a view is useful.
    const std::pair<const char*, PlayModeState> kCases[] = {{"enter", PlayModeState::Edit},
                                                            {"exit", PlayModeState::Play},
                                                            {"pause", PlayModeState::Play},
                                                            {"resume", PlayModeState::Paused}};
    for (const auto& [action, from] : kCases)
    {
        const PlayModeRequest r(json{{"action", action}, {"activateGameView", true}}, from);
        ASSERT_TRUE(r.Error().empty()) << action << ": " << r.Error();
        EXPECT_TRUE(r.ActivateGameView()) << action;
    }

    const PlayModeRequest off(json{{"action", "enter"}, {"activateGameView", false}},
                              PlayModeState::Edit);
    ASSERT_TRUE(off.Error().empty()) << off.Error();
    EXPECT_FALSE(off.ActivateGameView());
}

TEST(PlayModeRequestTests, ANonBooleanGameViewActivationIsAnErrorRatherThanTruthy)
{
    // The string "false" is truthy under every permissive read there is, so a
    // coerced parse would rearrange the dock layout of a caller that asked for
    // the opposite. Refuse instead of guessing.
    for (const json& value : {json("false"), json("true"), json(0), json(1), json::array()})
    {
        const PlayModeRequest r(json{{"action", "enter"}, {"activateGameView", value}},
                                PlayModeState::Edit);
        EXPECT_FALSE(r.Error().empty()) << "accepted " << value.dump();
        EXPECT_NE(r.Error().find("activateGameView"), std::string::npos)
            << "name the parameter that was wrong: " << r.Error();
        EXPECT_EQ(r.CommandId(), 0u) << "a refused request must not hand the handler a live command";
    }
}

// ---------------------------------------------------------------------------
// Malformed requests
// ---------------------------------------------------------------------------

TEST(PlayModeRequestTests, AnActionNamingNothingIsAnErrorListingTheRealOnes)
{
    const PlayModeRequest r = Parse("step", PlayModeState::Play);
    ASSERT_FALSE(r.Error().empty()) << "the editor has no frame-step action to fall back on";
    EXPECT_NE(r.Error().find("step"), std::string::npos) << "name the value that matched nothing";
    EXPECT_NE(r.Error().find("enter"), std::string::npos) << "state the values that would match";
    EXPECT_NE(r.Error().find("resume"), std::string::npos);
}

TEST(PlayModeRequestTests, AMissingOrWrongTypedActionIsAnError)
{
    // Skipping a malformed action would fall through to whatever the default
    // command id happens to be — a transition nobody asked for.
    EXPECT_FALSE(PlayModeRequest(json::object(), PlayModeState::Edit).Error().empty());
    EXPECT_FALSE(PlayModeRequest(json{{"action", nullptr}}, PlayModeState::Edit).Error().empty());
    EXPECT_FALSE(PlayModeRequest(json{{"action", 1}}, PlayModeState::Edit).Error().empty());
    EXPECT_FALSE(PlayModeRequest(json{{"action", true}}, PlayModeState::Edit).Error().empty());
}

TEST(PlayModeRequestTests, ARefusedRequestCarriesNoCommand)
{
    const PlayModeRequest r = Parse("enter", PlayModeState::Play);
    ASSERT_FALSE(r.Error().empty());
    EXPECT_EQ(r.CommandId(), 0u) << "a refused request must not hand the handler a live command id";
}

TEST(PlayModeRequestTests, ActionMatchingIsExact)
{
    // The schema constrains the tool, but the handler is reachable over raw IPC
    // with anything; a near-miss must be an error rather than a guess.
    EXPECT_FALSE(Parse("Enter", PlayModeState::Edit).Error().empty());
    EXPECT_FALSE(Parse("play", PlayModeState::Edit).Error().empty());
    EXPECT_FALSE(Parse("", PlayModeState::Edit).Error().empty());
}

// ---------------------------------------------------------------------------
// Wire vocabulary shared with get_editor_state
// ---------------------------------------------------------------------------

TEST(PlayModeRequestTests, EveryStateHasItsOwnWireSpelling)
{
    const PlayModeState kAll[] = {PlayModeState::Edit,        PlayModeState::EnteringPlay,
                                  PlayModeState::Play,        PlayModeState::Paused,
                                  PlayModeState::ExitingPlay, PlayModeState::ChangeReview};

    // These exact strings are what get_editor_state has always reported; a caller
    // comparing its set_play_mode response against a polled get_editor_state
    // depends on the two agreeing.
    EXPECT_STREQ(PlayModeStateToString(PlayModeState::Edit), "editing");
    EXPECT_STREQ(PlayModeStateToString(PlayModeState::EnteringPlay), "entering_play");
    EXPECT_STREQ(PlayModeStateToString(PlayModeState::Play), "playing");
    EXPECT_STREQ(PlayModeStateToString(PlayModeState::Paused), "paused");
    EXPECT_STREQ(PlayModeStateToString(PlayModeState::ExitingPlay), "exiting_play");
    EXPECT_STREQ(PlayModeStateToString(PlayModeState::ChangeReview), "change_review");

    for (const PlayModeState a : kAll)
    {
        EXPECT_STRNE(PlayModeStateToString(a), "unknown") << "every enumerator needs a spelling";
        for (const PlayModeState b : kAll)
        {
            if (a == b)
                continue;
            EXPECT_STRNE(PlayModeStateToString(a), PlayModeStateToString(b))
                << "two states sharing a spelling would be indistinguishable on the wire";
        }
    }
}

} // namespace
