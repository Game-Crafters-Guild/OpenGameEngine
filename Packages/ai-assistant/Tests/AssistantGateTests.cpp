// The assistant's gate over the class table: what each mode lets each class of tool do,
// the window-capture refusal, the binding, and that a connection which never bound is
// left alone; asking (a call that waits for Allow, Allow for this turn or Deny, and what
// ends its wait), the ledger's record of each call, and the one undo step an admitted
// edit records, through the registry as the debug server calls it; Undo this turn (the
// top-of-stack rule, its tooltips, "undone" rows, the trimmed case) and the log line.

#include "AgentCallRows.h"
#include "AssistantGate.h"
#include "AssistantMode.h"
#include "AssistantTools.h"

#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace GameEngine
{
namespace
{
using Editor::DebugRequestDecision;
using Editor::DebugRequestGateContext;

const std::filesystem::path kProject = std::filesystem::temp_directory_path() / "AssistantGateTests-project";

// One tool of each class, sent as the server sends it.
struct ClassSample
{
    AssistantToolClass Class;
    std::string_view Tool;
};
constexpr ClassSample kSamples[] = {
    {AssistantToolClass::Read, "get_scene_hierarchy"},  {AssistantToolClass::View, "select_entity"},
    {AssistantToolClass::UndoableEdit, "set_component"}, {AssistantToolClass::Input, "send_key"},
    {AssistantToolClass::Gated, "save_scene"},           {AssistantToolClass::Denied, "undo"},
};

// The request id the debug server gives the next request; never reused.
uint64_t NextRequestId()
{
    static uint64_t s_Last = 0;
    return ++s_Last;
}

// The method and parameters the MCP server sends for `tool`: its method, or
// assistant_authorize for a tool that runs in the server's process.
std::pair<std::string, nlohmann::json> Request(std::string_view tool, nlohmann::json arguments)
{
    const AssistantTool* row = AssistantTools::Find(tool);
    const bool hostSide = row && row->Method.empty();
    if (hostSide)
        return {"assistant_authorize", nlohmann::json{{"tool", tool}, {"arguments", std::move(arguments)}}};
    return {std::string(row ? row->Method : tool), std::move(arguments)};
}

// The gate's first answer to a new request for `tool`.
Editor::DebugRequestVerdict Call(AssistantGate& gate, uint32_t clientId, std::string_view tool,
                                 nlohmann::json arguments = nlohmann::json::object())
{
    const auto [method, params] = Request(tool, std::move(arguments));
    return gate.Before(DebugRequestGateContext{method, clientId, &params, NextRequestId()});
}

// A gate with one session in `mode`, its turn 1 running, bound on `clientId`.
AssistantGate::SessionId OpenBound(AssistantGate& gate, AssistantMode mode, uint32_t clientId)
{
    const AssistantGate::SessionId session = gate.Open(mode, kProject);
    gate.StartTurn(session, 1);
    const nlohmann::json params{{"token", gate.Token(session)}};
    EXPECT_EQ(gate.Before(DebugRequestGateContext{"assistant_bind", clientId, &params}).Decision,
              DebugRequestDecision::Run);
    return session;
}
} // namespace

TEST(AssistantGateTests, EveryModeRunsOrRefusesEachClassWithAMessageNamingTheWayOut)
{
    struct Cell
    {
        AssistantMode Mode;
        AssistantToolClass Class;
        DebugRequestDecision Decision;
        std::string_view Refusal;
    };
    constexpr DebugRequestDecision Runs = DebugRequestDecision::Run;
    constexpr DebugRequestDecision Asks = DebugRequestDecision::Park;
    constexpr DebugRequestDecision Refused = DebugRequestDecision::Refuse;
    const Cell cells[] = {
        {AssistantMode::ReadOnly, AssistantToolClass::Read, Runs, ""},
        {AssistantMode::ReadOnly, AssistantToolClass::View, Refused,
         "select_entity is not available in Read only: ask the user to switch the conversation to Auto."},
        {AssistantMode::ReadOnly, AssistantToolClass::UndoableEdit, Refused,
         "set_component is not available in Read only: ask the user to switch the conversation to Auto."},
        {AssistantMode::ReadOnly, AssistantToolClass::Input, Refused,
         "send_key is not available in Read only: ask the user to switch the conversation to Auto."},
        {AssistantMode::ReadOnly, AssistantToolClass::Gated, Refused,
         "save_scene is not available in Read only: ask the user to switch the conversation to Auto."},
        {AssistantMode::Auto, AssistantToolClass::Read, Runs, ""},
        {AssistantMode::Auto, AssistantToolClass::View, Runs, ""},
        {AssistantMode::Auto, AssistantToolClass::UndoableEdit, Runs, ""},
        {AssistantMode::Auto, AssistantToolClass::Input, Runs, ""},
        {AssistantMode::Auto, AssistantToolClass::Gated, Asks, ""},
        {AssistantMode::AskBeforeEveryEdit, AssistantToolClass::Read, Runs, ""},
        {AssistantMode::AskBeforeEveryEdit, AssistantToolClass::View, Runs, ""},
        {AssistantMode::AskBeforeEveryEdit, AssistantToolClass::UndoableEdit, Asks, ""},
        {AssistantMode::AskBeforeEveryEdit, AssistantToolClass::Input, Asks, ""},
        {AssistantMode::AskBeforeEveryEdit, AssistantToolClass::Gated, Asks, ""},
        {AssistantMode::EditFreely, AssistantToolClass::Read, Runs, ""},
        {AssistantMode::EditFreely, AssistantToolClass::View, Runs, ""},
        {AssistantMode::EditFreely, AssistantToolClass::UndoableEdit, Runs, ""},
        {AssistantMode::EditFreely, AssistantToolClass::Input, Runs, ""},
        {AssistantMode::EditFreely, AssistantToolClass::Gated, Runs, ""},
    };
    for (const Cell& cell : cells)
    {
        AssistantGate gate;
        OpenBound(gate, cell.Mode, 7);
        const ClassSample* sample = nullptr;
        for (const ClassSample& candidate : kSamples)
            if (candidate.Class == cell.Class)
                sample = &candidate;
        ASSERT_NE(sample, nullptr);
        ASSERT_EQ(AssistantTools::Find(sample->Tool)->Class, cell.Class) << sample->Tool;
        const Editor::DebugRequestVerdict verdict = Call(gate, 7, sample->Tool);
        const std::string where =
            std::string(DescribeAssistantMode(cell.Mode).Label) + " / " + std::string(sample->Tool);
        EXPECT_EQ(verdict.Decision, cell.Decision) << where;
        EXPECT_EQ(verdict.Refusal, cell.Refusal) << where;
    }
    for (const AssistantModeChoice& mode : AssistantModeChoices())
    {
        AssistantGate gate;
        OpenBound(gate, mode.Mode, 7);
        const Editor::DebugRequestVerdict verdict = Call(gate, 7, "undo");
        EXPECT_EQ(verdict.Decision, DebugRequestDecision::Refuse) << mode.Label;
        EXPECT_TRUE(verdict.Refusal.starts_with("undo is not available to the assistant: ")) << verdict.Refusal;
    }
}

TEST(AssistantGateTests, AHostSideWriterIsDecidedThroughAuthorizeLikeAnEditorCall)
{
    AssistantGate gate;
    const AssistantGate::SessionId session = OpenBound(gate, AssistantMode::Auto, 3);
    EXPECT_EQ(Call(gate, 3, "create_component", {{"name", "Health"}}).Decision, DebugRequestDecision::Park);
    EXPECT_EQ(Call(gate, 3, "list_ecs_systems").Decision, DebugRequestDecision::Run);
    gate.SetMode(session, AssistantMode::EditFreely);
    EXPECT_EQ(Call(gate, 3, "create_component", {{"name", "Health"}}).Decision, DebugRequestDecision::Run);
    EXPECT_EQ(Call(gate, 3, "build_engine").Decision, DebugRequestDecision::Refuse);
}

TEST(AssistantGateTests, AToolThatActsOnTheMachineIsRefusedAPathOutsideTheProject)
{
    AssistantGate gate;
    OpenBound(gate, AssistantMode::EditFreely, 3);
    const std::string outside = (kProject.parent_path() / "Elsewhere").string();
    for (const nlohmann::json& arguments :
         {nlohmann::json{{"name", "Waves"}, {"directory", outside}},
          nlohmann::json{{"name", "Waves"}, {"directory", "../Elsewhere"}},
          nlohmann::json{{"path", "Graphs/../../escape.graph"}, {"typeId", "Print"}}})
    {
        const std::string tool = arguments.contains("path") ? "add_game_graph_node" : "create_game_system";
        const Editor::DebugRequestVerdict verdict = Call(gate, 3, tool, arguments);
        EXPECT_EQ(verdict.Decision, DebugRequestDecision::Refuse) << arguments.dump();
        EXPECT_NE(verdict.Refusal.find("is outside the project"), std::string::npos) << verdict.Refusal;
    }
    EXPECT_EQ(Call(gate, 3, "create_game_system", {{"name", "Waves"}, {"directory", "Assets/Scripts"}}).Decision,
              DebugRequestDecision::Run);
    EXPECT_EQ(Call(gate, 3, "create_game_system",
                   {{"name", "Waves"}, {"directory", (kProject / "Assets" / "Scripts").string()}})
                  .Decision,
              DebugRequestDecision::Run);
    EXPECT_EQ(Call(gate, 3, "create_game_system", {{"name", "Waves"}}).Decision, DebugRequestDecision::Run);
}

// An editor method that writes a file is bounded the same way: save_scene to a path
// outside the project is refused.
TEST(AssistantGateTests, AnEditorWriteOutsideTheProjectIsRefused)
{
    AssistantGate gate;
    OpenBound(gate, AssistantMode::EditFreely, 8);
    const Editor::DebugRequestVerdict outside =
        Call(gate, 8, "save_scene", {{"path", (kProject.parent_path() / "Elsewhere.scene").string()}});
    EXPECT_EQ(outside.Decision, DebugRequestDecision::Refuse);
    EXPECT_NE(outside.Refusal.find("is outside the project"), std::string::npos) << outside.Refusal;
    EXPECT_EQ(Call(gate, 8, "save_scene", {{"path", (kProject / "Assets" / "Main.scene").string()}}).Decision,
              DebugRequestDecision::Run);
    EXPECT_EQ(Call(gate, 8, "save_scene").Decision, DebugRequestDecision::Run);
}

// With no project open, a tool that acts on the machine is refused even with no path.
TEST(AssistantGateTests, WithNoProjectOpenAToolThatActsOnTheMachineIsRefused)
{
    AssistantGate gate;
    const AssistantGate::SessionId session = gate.Open(AssistantMode::EditFreely, {});
    gate.StartTurn(session, 1);
    const nlohmann::json bind{{"token", gate.Token(session)}};
    ASSERT_EQ(gate.Before(DebugRequestGateContext{"assistant_bind", 11, &bind}).Decision, DebugRequestDecision::Run);
    const Editor::DebugRequestVerdict verdict = Call(gate, 11, "create_game_system", {{"name", "Waves"}});
    EXPECT_EQ(verdict.Decision, DebugRequestDecision::Refuse);
    EXPECT_NE(verdict.Refusal.find("no project is open"), std::string::npos) << verdict.Refusal;
}

TEST(AssistantGateTests, EveryToolHasAClassAnActionAndASubject)
{
    for (const AssistantTool& tool : AssistantTools::All())
    {
        EXPECT_FALSE(tool.Action.empty()) << tool.Name;
        EXPECT_EQ(tool.Class == AssistantToolClass::Denied, !tool.DeniedReason.empty()) << tool.Name;
        EXPECT_TRUE(AssistantTools::Subject(tool.Name, nlohmann::json::object()).empty()) << tool.Name;
    }
    const nlohmann::json setLight{{"entityId", 12}, {"component", "DirectionalLight"}, {"values", {{"Elevation", 12}}}};
    EXPECT_EQ(AssistantTools::Subject("mcp__editor_assistant__set_component", setLight),
              "12 · DirectionalLight · Elevation 12");
    const std::vector<std::string> attached = AssistantTools::AttachedNames();
    EXPECT_EQ(std::find(attached.begin(), attached.end(), "undo"), attached.end());
    EXPECT_NE(std::find(attached.begin(), attached.end(), "input_text"), attached.end());
}

TEST(AssistantGateTests, AWindowCaptureIsRefusedInEveryModeAndTheRenderedFrameIsNot)
{
    for (const AssistantModeChoice& mode : AssistantModeChoices())
    {
        AssistantGate gate;
        OpenBound(gate, mode.Mode, 5);
        const Editor::DebugRequestVerdict window =
            Call(gate, 5, "take_screenshot", {{"target", "window"}, {"allowWindowCapture", true}});
        EXPECT_EQ(window.Decision, DebugRequestDecision::Refuse) << mode.Label;
        EXPECT_TRUE(window.Refusal.starts_with("Window captures are not available to the assistant")) << mode.Label;
        EXPECT_EQ(Call(gate, 5, "take_screenshot", {{"target", "viewport"}}).Decision, DebugRequestDecision::Run)
            << mode.Label;
    }
}

TEST(AssistantGateTests, AConnectionThatNeverBoundIsLeftAlone)
{
    AssistantGate gate;
    OpenBound(gate, AssistantMode::ReadOnly, 1);
    EXPECT_EQ(Call(gate, 2, "save_scene").Decision, DebugRequestDecision::Run);
    EXPECT_EQ(Call(gate, 2, "undo").Decision, DebugRequestDecision::Run);
    const nlohmann::json authorize{{"tool", "create_component"}};
    EXPECT_EQ(gate.Before(DebugRequestGateContext{"assistant_authorize", 2, &authorize, NextRequestId()}).Decision,
              DebugRequestDecision::Refuse);
}

// A closed connection's binding is forgotten; the server never reuses its id, so the
// only trace is the gate's own state: the id no longer reads as bound.
TEST(AssistantGateTests, AClosedConnectionsBindingIsForgotten)
{
    AssistantGate gate;
    OpenBound(gate, AssistantMode::ReadOnly, 6);
    ASSERT_EQ(Call(gate, 6, "save_scene").Decision, DebugRequestDecision::Refuse);
    gate.ClientClosed(6);
    EXPECT_EQ(Call(gate, 6, "save_scene").Decision, DebugRequestDecision::Run);
}

TEST(AssistantGateTests, ABindingNeedsAnOpenSessionsTokenAndARunningTurn)
{
    AssistantGate gate;
    const nlohmann::json stale{{"token", "not-a-session"}};
    EXPECT_EQ(gate.Before(DebugRequestGateContext{"assistant_bind", 4, &stale}).Decision,
              DebugRequestDecision::Refuse);

    const AssistantGate::SessionId session = OpenBound(gate, AssistantMode::EditFreely, 4);
    gate.EndTurn(session);
    const Editor::DebugRequestVerdict idle = Call(gate, 4, "get_log");
    EXPECT_EQ(idle.Refusal, "No turn is running in this conversation.");

    gate.StartTurn(session, 2);
    EXPECT_EQ(Call(gate, 4, "get_log").Decision, DebugRequestDecision::Run);
    const std::string token = gate.Token(session);
    gate.Close(session);
    EXPECT_EQ(Call(gate, 4, "get_log").Decision, DebugRequestDecision::Refuse);
    const nlohmann::json closed{{"token", token}};
    EXPECT_EQ(gate.Before(DebugRequestGateContext{"assistant_bind", 9, &closed}).Decision,
              DebugRequestDecision::Refuse);
}

namespace
{
// A scene edit as a handler commits it: one command on the undo history.
class StepCommand final : public Editor::IEditorCommand
{
public:
    explicit StepCommand(int& value)
        : m_Value(value)
    {
    }
    const char* GetName() const override { return "Set Light"; }
    void Do() override { ++m_Value; }
    void Undo() override { --m_Value; }

private:
    int& m_Value;
};

// The debug server's side of one bound connection: each request offered through a
// registry with the gate registered, as the server offers it, with the editor's undo
// history and a clock the test moves.
class BoundConnection
{
public:
    static constexpr uint32_t kClient = 21;

    explicit BoundConnection(AssistantMode mode)
        : m_Gate([this] { return m_Now; })
    {
        m_Registry.Register(m_Gate.AsDebugRequestGate());
        m_Session = OpenBound(m_Gate, mode, kClient);
    }

    // Offers request `requestId` for `tool`; an admitted request runs `handler` and is
    // answered as the server answers it. Returns the gates' decision.
    DebugRequestDecision Offer(uint64_t requestId, std::string_view tool,
                               nlohmann::json arguments = nlohmann::json::object(),
                               const std::function<void(Editor::UndoRedoService&)>& handler = {})
    {
        const auto [method, params] = Request(tool, std::move(arguments));
        const DebugRequestGateContext context{method, kClient, &params, requestId, &Undo};
        Editor::DebugRequestVerdict verdict;
        {
            Editor::DebugRequestGateRegistry::RequestScope scope(m_Registry, context);
            verdict = scope.Admit();
            if (verdict.Decision == DebugRequestDecision::Park)
                return verdict.Decision;
            if (verdict.Decision == DebugRequestDecision::Run && handler)
                handler(Undo);
        }
        const nlohmann::json response = verdict.Decision == DebugRequestDecision::Run
                                            ? nlohmann::json{{"ok", true}, {"result", nlohmann::json::object()}}
                                            : nlohmann::json{{"ok", false}, {"error", verdict.Refusal}};
        m_Registry.NotifyAnswered(context, &response);
        return verdict.Decision;
    }

    const AssistantAction& Action(uint64_t requestId) const { return *m_Gate.Ledger(m_Session)->Find(requestId); }
    void Answer(uint64_t requestId, AssistantAnswer answer) { m_Gate.Answer(m_Session, requestId, answer); }
    void Pass(std::chrono::steady_clock::duration time) { m_Now += time; }
    AssistantGate& Gate() { return m_Gate; }
    AssistantGate::SessionId Session() const { return m_Session; }

    Editor::UndoRedoService Undo;

private:
    std::chrono::steady_clock::time_point m_Now{};
    AssistantGate m_Gate;
    Editor::DebugRequestGateRegistry m_Registry;
    AssistantGate::SessionId m_Session = 0;
};

// A handler that commits two commands, as set_component's write and its notification
// edit can.
std::function<void(Editor::UndoRedoService&)> TwoCommands(int& value)
{
    return [&value](Editor::UndoRedoService& undo) {
        undo.Execute(std::make_unique<StepCommand>(value));
        undo.Execute(std::make_unique<StepCommand>(value));
    };
}

const nlohmann::json kSetLight{{"entityId", 12}, {"component", "DirectionalLight"}, {"values", {{"Elevation", 12}}}};
} // namespace

// An admitted edit is one undo step named for it, whatever its handler commits; the
// ledger holds the step's id, the one the history reports.
TEST(AssistantGateTests, AnAdmittedEditIsOneNamedUndoStepWithItsIdInTheLedger)
{
    BoundConnection connection(AssistantMode::Auto);
    int value = 0;
    ASSERT_EQ(connection.Offer(101, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Run);

    ASSERT_EQ(connection.Undo.GetUndoCount(), 1u);
    EXPECT_STREQ(connection.Undo.PeekUndoName(), "Assistant: Set DirectionalLight (12)");
    const AssistantAction& action = connection.Action(101);
    EXPECT_EQ(action.State, AssistantActionState::Done);
    EXPECT_EQ(action.UndoEntryId, connection.Undo.GetUndoEntryIdAt(0));
    EXPECT_EQ(action.UndoName, "Assistant: Set DirectionalLight (12)");
    EXPECT_EQ(action.Subject, "12 · DirectionalLight · Elevation 12");
    connection.Undo.Undo();
    EXPECT_EQ(value, 0) << "one undo takes back the whole call";
}

// A refused call runs nothing and leaves no step; a call that changes nothing leaves none
// either, and a handler that throws leaves no compound open behind it.
TEST(AssistantGateTests, ARefusedThrowingOrEmptyCallLeavesNoStepAndNoOpenCompound)
{
    BoundConnection connection(AssistantMode::ReadOnly);
    int value = 0;
    EXPECT_EQ(connection.Offer(201, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Refuse);
    EXPECT_EQ(connection.Undo.GetUndoCount(), 0u);
    EXPECT_EQ(connection.Action(201).State, AssistantActionState::Refused);
    EXPECT_EQ(connection.Action(201).Message,
              "set_component is not available in Read only: ask the user to switch the conversation to Auto.");

    connection.Gate().SetMode(connection.Session(), AssistantMode::Auto);
    EXPECT_EQ(connection.Offer(202, "delete_entity", {{"entityId", 3}}, [](Editor::UndoRedoService&) {}),
              DebugRequestDecision::Run);
    EXPECT_EQ(connection.Undo.GetUndoCount(), 0u);
    EXPECT_EQ(connection.Action(202).UndoEntryId, 0u);

    EXPECT_THROW(connection.Offer(203, "create_entity", {{"name", "Lantern_01"}},
                                  [&value](Editor::UndoRedoService& undo) {
                                      undo.Execute(std::make_unique<StepCommand>(value));
                                      throw std::runtime_error("handler failure");
                                  }),
                 std::runtime_error);
    ASSERT_EQ(connection.Undo.GetUndoCount(), 1u);
    EXPECT_STREQ(connection.Undo.PeekUndoName(), "Assistant: Create Entity (Lantern_01)");
    connection.Undo.Execute(std::make_unique<StepCommand>(value));
    EXPECT_EQ(connection.Undo.GetUndoCount(), 2u) << "the user's next edit is a step of its own, not inside a "
                                                      "compound the throw left open";
}

// A call that asks waits, offered again each update, until the user answers: Allow runs
// it, Deny refuses it with the declined message.
TEST(AssistantGateTests, AnAskedCallRunsOnlyAfterAllowAndDenyAnswersTheDeclinedMessage)
{
    BoundConnection connection(AssistantMode::AskBeforeEveryEdit);
    int value = 0;
    ASSERT_EQ(connection.Offer(301, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Park);
    EXPECT_EQ(connection.Offer(301, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Park);
    EXPECT_EQ(value, 0);
    const AssistantAction& waiting = connection.Action(301);
    EXPECT_EQ(waiting.State, AssistantActionState::Waiting);
    EXPECT_EQ(waiting.WaitingFor, AssistantWaitReason::Answer);

    connection.Answer(301, AssistantAnswer::Allow);
    EXPECT_EQ(connection.Offer(301, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Run);
    EXPECT_EQ(value, 2);
    EXPECT_EQ(connection.Undo.GetUndoCount(), 1u);
    EXPECT_EQ(connection.Action(301).State, AssistantActionState::Done);

    ASSERT_EQ(connection.Offer(302, "create_entity", {{"name", "Lantern_02"}}), DebugRequestDecision::Park);
    connection.Answer(302, AssistantAnswer::Deny);
    EXPECT_EQ(connection.Offer(302, "create_entity", {{"name", "Lantern_02"}}), DebugRequestDecision::Refuse);
    EXPECT_EQ(connection.Action(302).State, AssistantActionState::Declined);
    EXPECT_EQ(connection.Action(302).Message, "The user declined: Create entity (Lantern_02)");
}

// Allow for this turn covers the answered call's tool only: another tool still asks, and
// the next turn asks again. A call it ran without asking records the answer it ran on.
TEST(AssistantGateTests, AllowForThisTurnRunsTheSameToolsLaterCallsAndTheNextTurnAsksAgain)
{
    BoundConnection connection(AssistantMode::Auto);
    ASSERT_EQ(connection.Offer(401, "save_scene"), DebugRequestDecision::Park);
    connection.Answer(401, AssistantAnswer::AllowForTurn);
    EXPECT_EQ(connection.Offer(401, "save_scene"), DebugRequestDecision::Run);
    EXPECT_EQ(connection.Offer(402, "save_scene"), DebugRequestDecision::Run);
    EXPECT_EQ(connection.Action(402).Answer, AssistantAnswer::AllowForTurn);
    EXPECT_EQ(connection.Offer(403, "set_play_mode", {{"action", "play"}}), DebugRequestDecision::Park)
        << "another tool still asks";

    connection.Gate().StartTurn(connection.Session(), 2);
    EXPECT_EQ(connection.Offer(404, "save_scene"), DebugRequestDecision::Park);
}

// One waiting budget per call: a call allowed after nine minutes that then waits for the
// user's drag is refused when its first ten minutes are up, never after the MCP server gave
// up on it.
TEST(AssistantGateTests, AnAllowedCallThatWaitsForADragKeepsItsFirstWaitingBudget)
{
    BoundConnection connection(AssistantMode::AskBeforeEveryEdit);
    int value = 0;
    ASSERT_EQ(connection.Offer(801, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Park);
    connection.Pass(std::chrono::minutes(9));
    connection.Answer(801, AssistantAnswer::Allow);
    Editor::UndoRedoService::SnapshotTarget target;
    target.Capture = [](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) {
        out.assign(1, 0);
        return true;
    };
    Editor::UndoRedoService::InteractiveEdit drag = connection.Undo.BeginInteractiveEdit("Move Rock_04", target);
    EXPECT_EQ(connection.Offer(801, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Park);
    EXPECT_EQ(connection.Action(801).WaitingFor, AssistantWaitReason::Edit);
    connection.Pass(std::chrono::seconds(61));
    EXPECT_EQ(connection.Offer(801, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Refuse);
    EXPECT_EQ(value, 0);
}

// A waiting call ends without running when its turn stops, when its connection closes,
// or when nobody answers within the limit; each row says which.
TEST(AssistantGateTests, AWaitingCallIsCancelledByStopOrAClosedConnectionAndRefusedAfterTheLimit)
{
    BoundConnection connection(AssistantMode::Auto);
    ASSERT_EQ(connection.Offer(501, "save_scene"), DebugRequestDecision::Park);
    connection.Gate().EndTurn(connection.Session());
    EXPECT_EQ(connection.Offer(501, "save_scene"), DebugRequestDecision::Refuse);
    EXPECT_EQ(connection.Action(501).State, AssistantActionState::Cancelled);
    EXPECT_EQ(connection.Action(501).Message, "Cancelled: the session ended");

    connection.Gate().StartTurn(connection.Session(), 2);
    ASSERT_EQ(connection.Offer(502, "save_scene"), DebugRequestDecision::Park);
    connection.Pass(AssistantGate::kWaitLimit - std::chrono::seconds(1));
    EXPECT_EQ(connection.Offer(502, "save_scene"), DebugRequestDecision::Park);
    connection.Pass(std::chrono::seconds(1));
    EXPECT_EQ(connection.Offer(502, "save_scene"), DebugRequestDecision::Refuse);
    EXPECT_EQ(connection.Action(502).State, AssistantActionState::Refused);
    EXPECT_EQ(connection.Action(502).Message, "The user did not answer within 10 minutes");

    ASSERT_EQ(connection.Offer(503, "save_scene"), DebugRequestDecision::Park);
    const AssistantGate::SessionId session = connection.Session();
    connection.Gate().ClientClosed(BoundConnection::kClient);
    const AssistantAction* closed = connection.Gate().Ledger(session)->Find(503);
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->State, AssistantActionState::Cancelled);
}

// An undoable edit that arrives while the user drags waits for the drag to commit, then
// runs as its own step above the drag's.
TEST(AssistantGateTests, AnEditDuringTheUsersDragWaitsForItsCommit)
{
    BoundConnection connection(AssistantMode::Auto);
    int dragged = 0;
    Editor::UndoRedoService::SnapshotTarget target;
    target.Capture = [&dragged](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) {
        out.assign(1, static_cast<std::uint8_t>(dragged));
        return true;
    };
    target.Apply = [&dragged](const Editor::UndoRedoService::SnapshotTarget::Snapshot& in) {
        dragged = in.front();
        return true;
    };
    Editor::UndoRedoService::InteractiveEdit drag = connection.Undo.BeginInteractiveEdit("Move Rock_04", target);
    int value = 0;

    ASSERT_EQ(connection.Offer(601, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Park);
    EXPECT_EQ(connection.Action(601).WaitingFor, AssistantWaitReason::Edit);
    EXPECT_EQ(connection.Offer(602, "get_log"), DebugRequestDecision::Run) << "a read does not wait for a drag";
    dragged = 1;
    drag.Commit();

    EXPECT_EQ(connection.Offer(601, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Run);
    ASSERT_EQ(connection.Undo.GetUndoCount(), 2u);
    EXPECT_STREQ(connection.Undo.GetUndoNameAt(0), "Move Rock_04");
    EXPECT_STREQ(connection.Undo.GetUndoNameAt(1), "Assistant: Set DirectionalLight (12)");
}

namespace
{
std::function<void(Editor::UndoRedoService&)> OneCommand(int& value)
{
    return [&value](Editor::UndoRedoService& undo) { undo.Execute(std::make_unique<StepCommand>(value)); };
}

AssistantTurnUndo DescribeTurn(BoundConnection& connection, uint64_t turn)
{
    connection.Gate().Update(&connection.Undo);
    return AssistantTurnUndo::Describe(connection.Gate().Ledger(connection.Session())->Actions(), turn,
                                       connection.Gate().UndoHistory());
}
} // namespace

// Undo this turn takes back exactly the turn's steps, newest first, at the next update;
// the rows read "undone", and Redo from the Edit menu brings the steps back.
TEST(AssistantGateTests, UndoThisTurnUndoesTheTurnsStepsNewestFirstAndRedoBringsThemBack)
{
    BoundConnection connection(AssistantMode::Auto);
    int value = 0;
    ASSERT_EQ(connection.Offer(701, "set_component", kSetLight, TwoCommands(value)), DebugRequestDecision::Run);
    ASSERT_EQ(connection.Offer(702, "create_entity", {{"name", "Lantern_01"}}, OneCommand(value)),
              DebugRequestDecision::Run);
    const AssistantTurnUndo ready = DescribeTurn(connection, 1);
    EXPECT_EQ(ready.Status, AssistantTurnUndo::State::Ready);
    EXPECT_EQ(ready.Tooltip, "Undo the 2 edits this reply made, newest first.");

    connection.Gate().RequestUndoTurn(connection.Session(), 1);
    EXPECT_EQ(DescribeTurn(connection, 1).Status, AssistantTurnUndo::State::Undone);
    EXPECT_EQ(value, 0);
    EXPECT_EQ(connection.Undo.GetUndoCount(), 0u);
    EXPECT_STREQ(connection.Undo.PeekRedoName(), "Assistant: Set DirectionalLight (12)")
        << "undone newest first, so Redo starts from the turn's first step";
    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For(
        {}, connection.Gate().Ledger(connection.Session())->Actions(), 1, connection.Gate().UndoHistory());
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].UndoState, "undone");

    connection.Undo.Redo();
    connection.Undo.Redo();
    EXPECT_EQ(DescribeTurn(connection, 1).Status, AssistantTurnUndo::State::Ready);
    EXPECT_EQ(value, 3);
}

// A step above the turn's (the user's own edit) disables Undo this turn and names that
// step; a request made anyway undoes nothing; once the user undoes their edit it is ready.
TEST(AssistantGateTests, AUserStepOnTopDisablesUndoThisTurnAndNamesItUntilTheUserUndoesIt)
{
    BoundConnection connection(AssistantMode::Auto);
    int value = 0;
    ASSERT_EQ(connection.Offer(711, "create_entity", {{"name", "Lantern_01"}}, OneCommand(value)),
              DebugRequestDecision::Run);
    connection.Undo.Execute(std::make_unique<StepCommand>(value));

    const AssistantTurnUndo blocked = DescribeTurn(connection, 1);
    EXPECT_EQ(blocked.Status, AssistantTurnUndo::State::Blocked);
    EXPECT_EQ(blocked.Tooltip, "Your edit 'Set Light' came after this turn: undo it first, or use Undo History.");
    connection.Gate().RequestUndoTurn(connection.Session(), 1);
    connection.Gate().Update(&connection.Undo);
    EXPECT_EQ(connection.Undo.GetUndoCount(), 2u) << "the user's edit is never undone";

    connection.Undo.Undo();
    EXPECT_EQ(DescribeTurn(connection, 1).Status, AssistantTurnUndo::State::Ready);
}

TEST(AssistantGateTests, ATurnWhoseOldestStepWasTrimmedIsTooOldToUndoAsATurn)
{
    BoundConnection connection(AssistantMode::Auto);
    connection.Undo = Editor::UndoRedoService(Editor::UndoRedoService::Config{2});
    int value = 0;
    ASSERT_EQ(connection.Offer(721, "create_entity", {{"name", "Lantern_01"}}, OneCommand(value)),
              DebugRequestDecision::Run);
    ASSERT_EQ(connection.Offer(722, "create_entity", {{"name", "Lantern_02"}}, OneCommand(value)),
              DebugRequestDecision::Run);
    connection.Undo.Execute(std::make_unique<StepCommand>(value));

    const AssistantTurnUndo old = DescribeTurn(connection, 1);
    EXPECT_EQ(old.Status, AssistantTurnUndo::State::TooOld);
    EXPECT_EQ(old.Tooltip, "Too old to undo as a turn: its first edit is past the undo history's limit.");
}

TEST(AssistantGateTests, TheLogLineNamesTheCallItsOutcomeAndItsStep)
{
    AssistantAction ran;
    ran.Turn = 3;
    ran.Tool = "set_component";
    ran.Subject = "12 · DirectionalLight";
    ran.State = AssistantActionState::Done;
    ran.UndoEntryId = 41;
    EXPECT_EQ(AssistantActionLogLine(7, ran),
              "AI Assistant: conversation 7 turn 3: set_component 12 · DirectionalLight -> ran undo #41");

    AssistantAction refused;
    refused.Turn = 3;
    refused.Tool = "save_scene";
    refused.Subject = std::string(200, 'x');
    refused.State = AssistantActionState::Refused;
    refused.Message = "Not now.";
    EXPECT_EQ(AssistantActionLogLine(7, refused), "AI Assistant: conversation 7 turn 3: save_scene " +
                                                      std::string(120, 'x') + "\xE2\x80\xA6 -> refused: Not now.");
}
} // namespace GameEngine
