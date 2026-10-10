#include "AgentCallFileMenu.h"
#include "AgentCallImageView.h"
#include "AgentCallMedia.h"
#include "AgentCallResourceResolver.h"
#include "AgentCallResourceTile.h"
#include "AgentCallRow.h"
#include "AgentConversationController.h"
#include "AgentConversationRow.h"
#include "AgentConversationRowModel.h"
#include "AgentModelPopover.h"
#include "AgentReplyActions.h"
#include "ModeControlFit.h"
#include "AgentSessionSearchProvider.h"
#include "AgentSessionState.h"
#include "AiAssistantSettings.h"
#include "AssistantAttachment.h"
#include "AssistantMode.h"
#include "Conversation.h"
#include "AgentStatusMessages.h"
#include "ScopedEnvironmentVariable.h"
#include "ScopedPreferencesRoot.h"
#include "TurnMailbox.h"
#include "Providers/CancelToken.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CliSessionProvider.h"
#include "Providers/IAgentProvider.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Editor/Assets/EditorAssetActions.h"
#include "Editor/CaptureOutputDirectory.h"
#include "Editor/EditorPaths.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Shell.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <any>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace GameEngine
{
namespace
{
using namespace std::chrono_literals;

constexpr auto kPumpTimeout = 10s;

UIElement* FindWithClass(UIElement& root, const std::string& className)
{
    if (root.HasClass(className))
        return &root;
    for (const auto& child : root.GetChildren())
        if (UIElement* found = FindWithClass(*child, className))
            return found;
    return nullptr;
}

/// What one RunTurn() call saw.
struct ObservedTurn
{
    std::string UserText;
    std::string Model;
    std::string Effort;
    size_t HistorySize = 0;
    std::thread::id Thread;
    size_t JobSystemWorkerId = 0;
};

/// A message API that answers each turn with `Deltas` one-character deltas. While
/// `Hold` is set a turn waits after delta `HoldAfterDelta` until it is released or
/// cancelled, so a test can act while it runs.
class ScriptedProvider final : public IAgentProvider
{
public:
    explicit ScriptedProvider(std::string id) : m_Id(std::move(id)) {}

    std::string_view Id() const override { return m_Id; }
    ProviderCapabilities Capabilities() const override { return {.Streams = true}; }

    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) override
    {
        // A thread-local read: SIZE_MAX on any thread that is not a pool worker.
        static JobSystem::WorkStealingThreadPool s_WorkerProbe(0);
        {
            std::lock_guard lock(m_Mutex);
            m_Observed.push_back({request.UserText, request.Model, request.Effort, request.History.size(),
                                  std::this_thread::get_id(), s_WorkerProbe.GetCurrentWorkerId()});
        }
        Started = true;

        TurnResult result;
        for (int delta = 0; delta < Deltas; ++delta)
        {
            events.OnTextDelta("x");
            result.Text += "x";
            if (ThrowOnce.exchange(false))
                throw std::runtime_error("scripted provider failure");
            while (delta == HoldAfterDelta && Hold && (IgnoreCancel || !cancel.IsCancelled()))
            {
                Holding = true;
                std::this_thread::sleep_for(1ms);
            }
            if (cancel.IsCancelled())
            {
                result.Outcome = TurnOutcome::Stopped;
                events.OnFinished(result);
                Finished = true;
                return;
            }
        }
        result.Outcome = FinalOutcome;
        result.Model = AnsweredModel;
        events.OnFinished(result);
        Finished = true;
    }

    std::vector<ObservedTurn> Observed() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Observed;
    }

    int Deltas = 3;
    /// The model a turn reports as the one that answered; empty for none.
    std::string AnsweredModel;
    /// How a turn that runs to its end ends.
    TurnOutcome FinalOutcome = TurnOutcome::Succeeded;
    /// The delta after which a held turn waits (0-based).
    int HoldAfterDelta = 0;
    std::atomic<bool> Hold{false};
    /// A held turn waits for its release even when cancelled, as a CLI that is slow to
    /// exit does.
    std::atomic<bool> IgnoreCancel{false};
    /// The next turn throws after its first delta, as a provider hitting an
    /// unexpected error would.
    std::atomic<bool> ThrowOnce{false};
    std::atomic<bool> Holding{false};
    std::atomic<bool> Started{false};
    std::atomic<bool> Finished{false};

private:
    std::string m_Id;
    mutable std::mutex m_Mutex;
    std::vector<ObservedTurn> m_Observed;
};

/// A controller whose connections are the given fakes.
AgentConversationController MakeController(std::vector<std::shared_ptr<ScriptedProvider>> providers)
{
    return AgentConversationController(
        [providers = std::move(providers)](std::string_view id) -> std::shared_ptr<IAgentProvider>
        {
            for (const auto& provider : providers)
                if (provider->Id() == id)
                    return provider;
            return nullptr;
        });
}

/// Calls Update() once per millisecond, the panel's frame loop, until `done` holds.
template <typename Predicate>
bool PumpUntil(AgentConversationController& controller, Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + kPumpTimeout;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        controller.Update();
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

template <typename Predicate>
bool WaitFor(Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + kPumpTimeout;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

MessageStatus ReplyStatus(const AgentConversationController& controller, size_t turnIndex)
{
    return controller.GetConversation().Messages()[turnIndex * 2 + 1].Status;
}

/// The Claude (local session) connection as the panel gets it (AgentSessionState),
/// running FakeAgentCli in a project folder of its own, with fresh preferences.
class LocalClaudeSession
{
public:
    LocalClaudeSession()
        : m_Project(std::filesystem::temp_directory_path() / "AiAssistantPanelTests-project")
    {
        std::filesystem::create_directories(m_Project);
        EngineCore::GetInstance().SetWorkspaceRoot(m_Project);
        using Field = Editor::SettingsFieldDescriptor;
        const Editor::SettingsCategoryDescriptor category = AiAssistantSettings::BuildCategory();
        const auto executable = std::find_if(category.Fields.begin(), category.Fields.end(),
                                             [](const Field& field) { return field.Label == "Claude executable"; });
#if defined(_WIN32)
        const char* fakeCli = "FakeAgentCli.exe";
#else
        const char* fakeCli = "FakeAgentCli";
#endif
        // Another executable first, so the connection is a new provider whose login
        // check runs again in this test.
        const auto& path = std::get<Field::PathField>(executable->Control);
        path.Set("not-the-fake-cli");
        AgentSessionState::Get().Provider(ClaudeSessionProvider::kId);
        path.Set((TestDirectory() / fakeCli).string());
        AgentSessionState::Get().ForgetSessions();
    }
    ~LocalClaudeSession()
    {
        std::error_code error;
        std::filesystem::remove_all(m_Project, error);
    }

    static std::filesystem::path TestDirectory() { return Platform::GetExecutablePath().parent_path(); }
    static std::filesystem::path ProjectRoot() { return Editor::GetCurrentEditorProjectPaths().projectRoot; }
    static std::string StoredSession()
    {
        return AiAssistantSettings::LastSession(ClaudeSessionProvider::kId, ProjectRoot());
    }

    /// A controller whose connections are AgentSessionState's, as the panel's are.
    static AgentConversationController MakeController()
    {
        return AgentConversationController([](std::string_view id) { return AgentSessionState::Get().Provider(id); });
    }

private:
    ScopedPreferencesRoot m_Preferences;
    std::filesystem::path m_Project;
};

constexpr const char* kFakeCliSession = "00000000-0000-4000-8000-000000000002";

/// A local session whose session list is scripted: each ListSessions() call returns
/// one session named "<id>-<call number>", and while `Hold` is set waits until it is
/// released or cancelled, at most kPumpTimeout. It never runs a turn.
class ListingProvider final : public CliSessionProvider
{
public:
    explicit ListingProvider(std::string id)
        : CliSessionProvider("", {})
        , m_Id(std::move(id))
    {
    }

    std::string_view Id() const override { return m_Id; }
    LoginStatus CheckLogin(const CancelToken&) const override { return {}; }

    CliSessionList ListSessions(const CancelToken& cancel) const override
    {
        const int call = ++Reads;
        Reading = true;
        // Bounded, so a read that is never cancelled fails its test instead of hanging the suite.
        const auto deadline = std::chrono::steady_clock::now() + kPumpTimeout;
        while (Hold && !cancel.IsCancelled() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        SawCancel = cancel.IsCancelled();
        Reading = false;
        CliSessionList list;
        list.Sessions.push_back({m_Id + "-" + std::to_string(call), "", {}, 0});
        return list;
    }

    mutable std::atomic<int> Reads{0};
    mutable std::atomic<bool> Reading{false};
    mutable std::atomic<bool> SawCancel{false};
    std::atomic<bool> Hold{false};

private:
    std::string_view DisplayName() const override { return "Listing"; }
    std::vector<std::string> TurnArguments(const AgentTurnRequest&) const override { return {}; }
    void ReadLine(std::string_view, TurnState&) const override {}
    std::string CheckReadiness(const CancelToken&) const override { return {}; }

    std::string m_Id;
};

/// A local session whose tools check (ToolsBlocker) waits until `Release` is set,
/// whether or not the check is cancelled: a CLI probe that does not answer at once.
class SlowCheckProvider final : public CliSessionProvider
{
public:
    explicit SlowCheckProvider(std::string id)
        : CliSessionProvider("", {})
        , m_Id(std::move(id))
    {
    }

    std::string_view Id() const override { return m_Id; }
    LoginStatus CheckLogin(const CancelToken&) const override { return {}; }
    CliSessionList ListSessions(const CancelToken&) const override { return {}; }

    mutable std::atomic<bool> Checking{false};
    std::atomic<bool> Release{false};

private:
    std::string_view DisplayName() const override { return "Slow check"; }
    std::vector<std::string> TurnArguments(const AgentTurnRequest&) const override { return {}; }
    void ReadLine(std::string_view, TurnState&) const override {}
    std::string CheckReadiness(const CancelToken&) const override { return {}; }
    std::string ToolsBlocker(const std::string&, uint16_t, const CancelToken&) const override
    {
        Checking = true;
        const auto deadline = std::chrono::steady_clock::now() + kPumpTimeout;
        while (!Release && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return {};
    }

    std::string m_Id;
};

/// Connections that are the given listing providers.
AgentConversationController::ProviderLookup ListingLookup(std::vector<std::shared_ptr<ListingProvider>> providers)
{
    return [providers = std::move(providers)](std::string_view id) -> std::shared_ptr<IAgentProvider>
    {
        for (const auto& provider : providers)
            if (provider->Id() == id)
                return provider;
        return nullptr;
    };
}

std::string FirstListedId(const AgentConversationController& controller)
{
    const auto& list = controller.SessionList();
    return list && !list->Sessions.empty() ? list->Sessions.front().Id : std::string();
}
} // namespace

TEST(AiAssistantPanelTests, ACompletionBehind200DeltasIsDeliveredWithEveryDelta)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    provider->Deltas = 200;
    provider->HoldAfterDelta = 199;
    provider->Hold = true;
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    // 200 deltas arrive while the UI does not drain; one drain shows all of them.
    controller.Send("hello", "", "");
    controller.Update();
    ASSERT_TRUE(WaitFor([&] { return provider->Holding.load(); }));
    EXPECT_TRUE(controller.Update());
    EXPECT_EQ(controller.GetConversation().Messages()[1].Status, MessageStatus::InProgress);
    EXPECT_EQ(controller.GetConversation().Messages()[1].Text, std::string(200, 'x'));

    // The completion behind them is delivered.
    provider->Hold = false;
    ASSERT_TRUE(WaitFor([&] { return provider->Finished.load(); }));
    EXPECT_TRUE(controller.Update());
    EXPECT_EQ(controller.GetConversation().Messages()[1].Status, MessageStatus::Complete);
    EXPECT_FALSE(controller.IsBusy());
}

TEST(AiAssistantPanelTests, TheMailboxCoalescesAFramesDeltasIntoOneUpdate)
{
    TurnMailbox mailbox;
    for (int delta = 0; delta < 50; ++delta)
        mailbox.PostText(7, "ab");
    mailbox.PostToolActivity(7, {.Name = "markup_list", .InputJson = "{}"});
    TurnResult result;
    result.Outcome = TurnOutcome::Succeeded;
    mailbox.PostFinished(7, result);

    const std::vector<TurnUpdate> updates = mailbox.Take();
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].Turn, 7u);
    EXPECT_EQ(updates[0].Text.size(), 100u);
    ASSERT_EQ(updates[0].ToolActivities.size(), 1u);
    EXPECT_TRUE(updates[0].Result.has_value());
    EXPECT_TRUE(mailbox.Take().empty());
}

TEST(AiAssistantPanelTests, AQueuedSecondTurnRunsAfterTheFirstAndReceivesItAsHistory)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    provider->Hold = true;
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    controller.Send("first", "", "");
    controller.Send("second", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return provider->Started.load(); }));
    EXPECT_EQ(ReplyStatus(controller, 0), MessageStatus::InProgress);
    EXPECT_EQ(ReplyStatus(controller, 1), MessageStatus::Queued);

    provider->Hold = false;
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    EXPECT_EQ(ReplyStatus(controller, 0), MessageStatus::Complete);
    EXPECT_EQ(ReplyStatus(controller, 1), MessageStatus::Complete);

    const std::vector<ObservedTurn> turns = provider->Observed();
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].UserText, "first");
    EXPECT_EQ(turns[0].HistorySize, 0u);
    EXPECT_EQ(turns[1].UserText, "second");
    EXPECT_EQ(turns[1].HistorySize, 2u) << "the first turn's question and answer";
}

// A reply that completed with no text has nothing to resend, and resending its
// question alone would put two user messages in a row; the turn is left out whole.
TEST(AiAssistantPanelTests, AnEmptyCompletedReplyIsLeftOutOfTheHistoryWithItsQuestion)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    provider->Deltas = 0;
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    controller.Send("first", "", "");
    controller.Send("second", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    ASSERT_EQ(ReplyStatus(controller, 0), MessageStatus::Complete);

    const std::vector<ObservedTurn> turns = provider->Observed();
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[1].HistorySize, 0u) << "the first turn's question went out without its empty answer";
}

// An exception escaping a provider must end its turn, never the editor: the turn
// thread reports it as a failure and the queue moves on.
TEST(AiAssistantPanelTests, AProviderThatThrowsFailsItsTurnAndTheNextTurnRuns)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    provider->ThrowOnce = true;
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    controller.Send("first", "", "");
    controller.Send("second", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));

    const auto messages = controller.GetConversation().Messages();
    EXPECT_EQ(ReplyStatus(controller, 0), MessageStatus::Failed);
    EXPECT_EQ(messages[1].Text, "x") << "the text that arrived before the failure stays";
    EXPECT_NE(messages[1].Notice.find("scripted provider failure"), std::string::npos) << messages[1].Notice;
    EXPECT_EQ(ReplyStatus(controller, 1), MessageStatus::Complete);
    EXPECT_EQ(provider->Observed().size(), 2u);
}

// The panel is destroyed on a package reload from inside the new module's DllMain, where a
// thread cannot exit: the controller's destructor must not wait for its turn (or any of its
// threads) to end. It cancels the turn and returns; the turn ends on its own afterwards.
TEST(AiAssistantPanelTests, DestroyingTheControllerWithATurnRunningDoesNotWaitForIt)
{
    auto provider = std::make_shared<ScriptedProvider>("slow");
    provider->Hold = true;
    provider->IgnoreCancel = true;
    auto controller = std::make_unique<AgentConversationController>(
        [provider](std::string_view) -> std::shared_ptr<IAgentProvider> { return provider; });
    controller->SetProvider("slow");
    ASSERT_TRUE(controller->Send("hi", "", ""));
    ASSERT_TRUE(PumpUntil(*controller, [&] { return provider->Holding.load(); }));
    std::thread release([&provider] {
        std::this_thread::sleep_for(500ms);
        provider->Hold = false;
    });

    const auto start = std::chrono::steady_clock::now();
    controller.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    release.join();

    EXPECT_LT(elapsed, 250ms) << "the destructor waited for the turn";
    EXPECT_TRUE(WaitFor([&] { return provider->Finished.load(); })) << "the turn ends on its own afterwards";
}

TEST(AiAssistantPanelTests, StopEndsTheRunningTurnAndClearsTheQueue)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    provider->Hold = true;
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    controller.Send("first", "", "");
    controller.Send("second", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return provider->Started.load(); }));

    controller.Stop();
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    EXPECT_EQ(ReplyStatus(controller, 0), MessageStatus::Stopped);
    EXPECT_EQ(controller.GetConversation().Messages()[1].Text, "x") << "the text so far stays";
    EXPECT_EQ(ReplyStatus(controller, 1), MessageStatus::Cancelled);
    EXPECT_EQ(provider->Observed().size(), 1u) << "the queued turn never ran";
}

TEST(AiAssistantPanelTests, TurnsRunOnTheControllersOwnThreadNeverOnAJobSystemWorker)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    controller.Send("first", "", "");
    controller.Send("second", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));

    const std::vector<ObservedTurn> turns = provider->Observed();
    ASSERT_EQ(turns.size(), 2u);
    for (const ObservedTurn& turn : turns)
    {
        EXPECT_EQ(turn.JobSystemWorkerId, SIZE_MAX) << "a turn ran on a JobSystem worker";
        EXPECT_NE(turn.Thread, std::this_thread::get_id()) << "a turn ran on the UI thread";
    }
    EXPECT_EQ(turns[0].Thread, turns[1].Thread) << "one dedicated thread runs every turn";
}

TEST(AiAssistantPanelTests, AProviderSwitchMidConversationStartsANewConversation)
{
    auto first = std::make_shared<ScriptedProvider>("fake");
    auto second = std::make_shared<ScriptedProvider>("other");
    first->Hold = true;
    AgentConversationController controller = MakeController({first, second});
    controller.SetProvider("fake");

    controller.Send("question", "", "");
    controller.Send("queued", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return first->Started.load(); }));

    controller.SetProvider("other");
    EXPECT_TRUE(controller.GetConversation().Messages().empty());
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    EXPECT_TRUE(controller.GetConversation().Messages().empty()) << "the stopped reply lands nowhere";
    EXPECT_EQ(first->Observed().size(), 1u);

    controller.Send("hello", "", "");
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    ASSERT_EQ(controller.GetConversation().Messages().size(), 2u);
    EXPECT_EQ(controller.GetConversation().Messages()[1].ProviderId, "other");
    EXPECT_EQ(second->Observed().size(), 1u);
}

TEST(AiAssistantPanelTests, TheRowModelShowsEachStageOfAReply)
{
    Conversation conversation;
    const Conversation::TurnId turn =
        conversation.AddTurn("hi", std::string(ClaudeApiProvider::kId), "claude-opus-5-5", "", {});

    const AgentConversationRowModel user = AgentConversationRowModel::From(*conversation.UserMessage(turn), {}, turn, {});
    EXPECT_EQ(user.Header, "You");
    EXPECT_EQ(user.Text, "hi");
    EXPECT_TRUE(user.Status.empty());

    auto reply = [&] { return AgentConversationRowModel::From(*conversation.Reply(turn), {}, turn, {}); };
    EXPECT_EQ(reply().Header, "Claude (API) · Opus 5.5");
    EXPECT_EQ(reply().Status, "Queued");
    EXPECT_FALSE(reply().CanStop);
    EXPECT_FALSE(reply().ShowsText) << "a queued reply has no text, so no blank body line";

    conversation.Start(turn);
    const ToolActivity tools[] = {{.Name = "mcp__editor_assistant__set_component",
                                   .InputJson = R"({"entityId":12,"component":"DirectionalLight"})",
                                   .Id = "call-1"},
                                  {.Name = "get_log", .Id = "call-2"}};
    conversation.AppendReply(turn, "Hel", tools, {});
    EXPECT_EQ(reply().Text, "Hel");
    EXPECT_EQ(reply().Status, "Generating...");
    // Calls the editor has no record of are rows of their own, as the provider reported them.
    ASSERT_EQ(reply().Calls.size(), 2u);
    EXPECT_EQ(reply().Calls[0].Action, "Set component");
    EXPECT_EQ(AgentCallPiecesText(reply().Calls[0].Summary), "#12 · DirectionalLight");
    EXPECT_EQ(reply().Calls[1].Action, "Read the log");
    // A call that ended in an error says so, whichever drain its result arrived in.
    const ToolFailure failed[] = {{"call-1", false, "Invalid entity"}};
    conversation.AppendReply(turn, "", {}, failed);
    EXPECT_EQ(AgentCallPiecesText(reply().Calls[0].Summary), "#12 · DirectionalLight · (failed) Invalid entity");
    EXPECT_TRUE(reply().ShowsText);
    EXPECT_TRUE(reply().CanStop);
    EXPECT_FALSE(reply().ShowsReplyActions);

    TurnResult result;
    result.Outcome = TurnOutcome::Succeeded;
    result.Text = "Hello";
    result.Note = "The reply reached the token limit.";
    result.CostUsd = 0.0123;
    result.InputTokens = 1200;
    result.OutputTokens = 340;
    conversation.Finish(turn, result);
    EXPECT_EQ(reply().Text, "Hello");
    EXPECT_EQ(reply().Status, "The reply reached the token limit. · $0.0123 · 1200 tokens in, 340 out · 1 failed");
    EXPECT_FALSE(reply().CanStop);
    EXPECT_TRUE(reply().ShowsReplyActions);

    result.Outcome = TurnOutcome::Failed;
    result.Error = "Set ANTHROPIC_API_KEY before starting the editor.";
    conversation.Finish(turn, result);
    EXPECT_TRUE(reply().Failed);
    EXPECT_EQ(reply().Status, "Set ANTHROPIC_API_KEY before starting the editor. · 1 failed");
    EXPECT_FALSE(reply().ShowsReplyActions);

    // A reply stopped mid-sentence keeps its text usable and says it was cut short.
    result.Outcome = TurnOutcome::Stopped;
    result.Text = "Hello, wor";
    conversation.Finish(turn, result);
    EXPECT_TRUE(reply().Stopped);
    EXPECT_FALSE(reply().Failed);
    EXPECT_EQ(reply().Text, "Hello, wor\xE2\x80\xA6") << "an ellipsis marks where the reply was cut";
    EXPECT_EQ(conversation.Reply(turn)->Text, "Hello, wor") << "the marker is the row's, never the message's";
    EXPECT_EQ(reply().Status, "Stopped by you · $0.0123 · 1200 tokens in, 340 out · 1 failed");
    EXPECT_TRUE(reply().ShowsReplyActions) << "the partial text can still be copied";
    result.InputTokens = 1;
    conversation.Finish(turn, result);
    EXPECT_EQ(reply().Status, "Stopped by you · $0.0123 · 1 token in, 340 out · 1 failed");

    // A queued turn that Stop cleared never ran: it says so in its own words, with
    // no body line, nothing to mark as cut and nothing to copy.
    const Conversation::TurnId queued = conversation.AddTurn("next", std::string(ClaudeApiProvider::kId), "", "", {});
    conversation.CancelQueued(queued);
    EXPECT_EQ(conversation.Reply(queued)->Status, MessageStatus::Cancelled);
    const AgentConversationRowModel cleared = AgentConversationRowModel::From(*conversation.Reply(queued), {}, queued, {});
    EXPECT_EQ(cleared.Status, "Cancelled before it started");
    EXPECT_FALSE(cleared.ShowsText);
    EXPECT_FALSE(cleared.Stopped);
    EXPECT_TRUE(cleared.Text.empty());
    EXPECT_FALSE(cleared.ShowsReplyActions);
    EXPECT_TRUE(reply().ShowsText) << "a stopped reply keeps its body";
}

namespace
{
AssistantAction Recorded(uint64_t requestId, uint64_t turn, std::string tool, AssistantToolClass toolClass,
                         std::string arguments, std::string subject, AssistantActionState state)
{
    AssistantAction action;
    action.RequestId = requestId;
    action.Turn = turn;
    action.Tool = std::move(tool);
    action.Class = toolClass;
    action.Arguments = std::move(arguments);
    action.Subject = std::move(subject);
    action.State = state;
    return action;
}

AssistantAction Stepped(AssistantAction action, uint64_t entryId, std::string name)
{
    action.UndoEntryId = entryId;
    action.UndoName = std::move(name);
    return action;
}
} // namespace

// A row's summary names entities as links (the one a create_entity made, from the editor's
// answer), draws vectors as their components, and never shows raw JSON; Details
// pretty-prints the call with its keys muted and its vectors drawn the same way.
TEST(AiAssistantPanelTests, TheSummaryLinksEntitiesAndDrawsVectorsAndTheDetailsArePrettyPrinted)
{
    using Kind = AgentCallPiece::Kind;
    AssistantAction created = Recorded(1, 1, "create_entity", AssistantToolClass::UndoableEdit,
                                       R"({"name":"Lantern_01","position":{"x":12,"y":0,"z":4.25}})",
                                       "Lantern_01", AssistantActionState::Done);
    created.Result = R"({"id":"3","ok":true,"result":{"entityId":1048584}})";
    const AgentCallPieces summary = AgentCallSummary(created);
    ASSERT_EQ(summary.size(), 3u);
    EXPECT_EQ(summary[0], AgentCallPiece::Entity(1048584));
    EXPECT_EQ(summary[1], AgentCallPiece::MutedWords("·"));
    EXPECT_EQ(summary[2], AgentCallPiece::VectorOf({12, 0, 4.25}));
    EXPECT_EQ(AgentCallPiecesText(summary), "#1048584 · x 12 y 0 z 4.25");

    const AgentCallPieces set =
        AgentCallSummary("set_component", R"({"entityId":7,"component":"Transform","values":{"position":{"x":1,"y":2,"z":3}}})");
    EXPECT_EQ(AgentCallPiecesText(set), "#7 · Transform · position x 1 y 2 z 3");
    EXPECT_EQ(set[0].Type, Kind::Entity);
    EXPECT_EQ(set.back().Type, Kind::Vector);

    const std::vector<AgentCallPieces> lines = AgentCallJsonLines(nlohmann::json::parse(
        R"({"component":"Transform","values":{"position":{"x":1,"y":2,"z":3}}})"));
    ASSERT_EQ(lines.size(), 6u);
    EXPECT_EQ(AgentCallPiecesText(lines[0]), "{");
    EXPECT_EQ(lines[1][1], AgentCallPiece::MutedWords("component:"));
    EXPECT_EQ(AgentCallPiecesText(lines[1]), "   component: \"Transform\"");
    EXPECT_EQ(lines[3][2].Type, Kind::Vector) << "a vector stays on its key's line";
}

// The rows come from the editor's record: one per call in arrival order, consecutive calls
// of one tool with the same arguments folded, each with its glyph, its outcome and what it
// left in the undo history; a call only the provider reported gets a row of its own saying
// how it ended (the CLI refused it, or it failed with an error); the footer counts the
// turn's steps, refusals and failures, those calls included.
TEST(AiAssistantPanelTests, TheCallRowsComeFromTheLedgerFoldedWithTheirUndoStateAndCounts)
{
    using Mark = AgentCallRowModel::Mark;
    constexpr uint64_t kTurn = 4;
    AssistantAction failed = Recorded(6, kTurn, "set_component", AssistantToolClass::UndoableEdit,
                                      R"({"entityId":99})", "99", AssistantActionState::Failed);
    failed.Message = "Invalid entity";
    AssistantAction waiting = Recorded(7, kTurn, "create_component", AssistantToolClass::Gated, R"({"name":"Health"})",
                                       "Health", AssistantActionState::Waiting);
    waiting.WaitingFor = AssistantWaitReason::Answer;
    const AssistantAction actions[] = {
        Stepped(Recorded(1, kTurn, "create_entity", AssistantToolClass::UndoableEdit,
                         R"({"name":"Lantern_01","position":{"x":12}})", "Lantern_01 · x 12", AssistantActionState::Done),
                11, "Assistant: Create Entity (Lantern_01)"),
        Stepped(Recorded(2, kTurn, "create_entity", AssistantToolClass::UndoableEdit, R"({"name":"Lantern_02"})",
                         "Lantern_02", AssistantActionState::Done),
                12, "Assistant: Create Entity (Lantern_02)"),
        Stepped(Recorded(3, kTurn, "create_entity", AssistantToolClass::UndoableEdit, R"({"name":"Lantern_03"})",
                         "Lantern_03", AssistantActionState::Done),
                13, "Assistant: Create Entity (Lantern_03)"),
        Recorded(4, kTurn, "look_at", AssistantToolClass::View, R"({"entityId":5})", "5", AssistantActionState::Done),
        Recorded(5, kTurn + 1, "get_log", AssistantToolClass::Read, "{}", "", AssistantActionState::Done),
        failed,
        waiting,
    };
    using End = ToolCallEnd;
    const ToolCall reported[] = {{"mcp__editor_assistant__create_entity", "{}", "a", End::Unknown, ""},
                                 {"mcp__editor_assistant__create_entity", "{}", "b", End::Unknown, ""},
                                 {"mcp__editor_assistant__create_entity", "{}", "c", End::Unknown, ""},
                                 {"mcp__editor_assistant__look_at", "{}", "d", End::Unknown, ""},
                                 {"mcp__editor_assistant__set_component", "{}", "e", End::Failed, "Invalid entity"},
                                 {"mcp__editor_assistant__create_component", "{}", "f", End::Unknown, ""},
                                 {"mcp__editor_assistant__undo", "{}", "g", End::Refused, ""},
                                 {"mcp__editor_assistant__set_play_mode", R"({"action":"play"})", "h", End::Failed,
                                  "Invalid enum value. Expected 'enter' | 'exit' | 'pause' | 'resume'"}};

    AssistantUndoSnapshot history;
    history.Undo = {11, 12, 13};
    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For(reported, actions, kTurn, history);
    ASSERT_EQ(rows.size(), 7u);
    EXPECT_EQ(rows[0].Glyph, Mark::Done);
    EXPECT_EQ(AgentCallPiecesText(rows[0].Summary), "Lantern_01 · x 12");
    EXPECT_EQ(rows[0].UndoState, "Undo step: Assistant: Create Entity (Lantern_01)");
    EXPECT_EQ(AgentCallPiecesText(rows[1].Summary), "Lantern_02 , Lantern_03 (2 calls)");
    EXPECT_EQ(rows[1].UndoState, "2 undo steps");
    EXPECT_EQ(rows[2].Glyph, Mark::ViewOnly);
    EXPECT_EQ(rows[2].UndoState, "not an undo step");
    EXPECT_EQ(rows[3].Glyph, Mark::Refused);
    EXPECT_EQ(AgentCallPiecesText(rows[3].Summary), "#99 · (failed) Invalid entity");
    EXPECT_EQ(rows[3].UndoState, "nothing changed");
    EXPECT_EQ(rows[4].Glyph, Mark::Waiting);
    EXPECT_TRUE(rows[4].Asks);
    EXPECT_EQ(rows[4].RequestId, 7u);
    EXPECT_EQ(AgentCallPiecesText(rows[4].Summary), "Health · not an undo step");
    EXPECT_EQ(rows[4].Waiting, "Waiting for your answer");
    // Calls the editor never received: the CLI refused one, another failed at the server.
    EXPECT_EQ(rows[5].Glyph, Mark::Refused);
    EXPECT_EQ(rows[5].Action, "Undo");
    EXPECT_EQ(AgentCallPiecesText(rows[5].Summary), "(refused) Not allowed for the assistant");
    EXPECT_EQ(rows[5].UndoState, "nothing changed");
    EXPECT_EQ(rows[6].Glyph, Mark::Refused);
    EXPECT_EQ(AgentCallPiecesText(rows[6].Summary),
              "play · (failed) Invalid enum value. Expected 'enter' | 'exit' | 'pause' | 'resume'");
    const bool namesLantern03 = std::any_of(rows[1].Details.begin(), rows[1].Details.end(), [](const AgentCallPieces& line) {
        return AgentCallPiecesText(line).find("\"Lantern_03\"") != std::string::npos;
    });
    EXPECT_TRUE(namesLantern03) << "a folded row's Details hold each call";

    EXPECT_EQ(AgentCallRowModel::Counts(reported, actions, kTurn), "3 undo steps · 1 refused · 2 failed")
        << "refused and failed counted apart, the calls the editor never received included (card #128 D)";
    EXPECT_TRUE(AgentCallRowModel::Counts({}, actions, kTurn + 1).empty());
}
// A call the server rejected never reaches the editor; the assistant's retry with other
// arguments does. The retry matches the editor's record by its arguments, so the failed
// call keeps its row above the retry (rows follow call order), the retry shows once, and
// the footer counts the failure.
TEST(AiAssistantPanelTests, ARetriedCallKeepsTheFailedRowAndShowsTheRetryOnce)
{
    using Mark = AgentCallRowModel::Mark;
    constexpr uint64_t kTurn = 5;
    const AssistantAction actions[] = {Recorded(1, kTurn, "set_play_mode", AssistantToolClass::Gated,
                                                R"({"action":"enter"})", "enter", AssistantActionState::Done)};
    using End = ToolCallEnd;
    const ToolCall reported[] = {{"mcp__editor_assistant__set_play_mode", R"({"action":"play"})", "a", End::Failed,
                                  "Invalid enum value. Expected 'enter' | 'exit' | 'pause' | 'resume'"},
                                 {"mcp__editor_assistant__set_play_mode", R"({ "action": "enter" })", "b",
                                  End::Unknown, ""}};

    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For(reported, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].Glyph, Mark::Refused);
    EXPECT_EQ(AgentCallPiecesText(rows[0].Summary),
              "play · (failed) Invalid enum value. Expected 'enter' | 'exit' | 'pause' | 'resume'");
    EXPECT_EQ(rows[1].Glyph, Mark::Done);
    EXPECT_EQ(rows[1].RequestId, 1u);
    EXPECT_EQ(AgentCallRowModel::Counts(reported, actions, kTurn), "1 failed");
}

// A call that failed before the editor (a dropped connection) and its retry with the same
// arguments: the editor's one record ran, so it is the retry's. The failed call keeps its
// row above the retry, the retry shows once, and the footer counts the failure.
TEST(AiAssistantPanelTests, ARetryWithTheSameArgumentsKeepsTheFailedRow)
{
    using Mark = AgentCallRowModel::Mark;
    constexpr uint64_t kTurn = 7;
    const AssistantAction actions[] = {Recorded(1, kTurn, "set_play_mode", AssistantToolClass::Gated,
                                                R"({"action":"enter"})", "enter", AssistantActionState::Done)};
    using End = ToolCallEnd;
    const ToolCall reported[] = {{"mcp__editor_assistant__set_play_mode", R"({"action":"enter"})", "a", End::Failed,
                                  "MCP error -32000: Connection closed"},
                                 {"mcp__editor_assistant__set_play_mode", R"({"action":"enter"})", "b", End::Unknown, ""}};

    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For(reported, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].Glyph, Mark::Refused);
    EXPECT_EQ(AgentCallPiecesText(rows[0].Summary), "enter · (failed) MCP error -32000: Connection closed");
    EXPECT_EQ(rows[1].Glyph, Mark::Done);
    EXPECT_EQ(rows[1].RequestId, 1u);
    EXPECT_EQ(AgentCallRowModel::Counts(reported, actions, kTurn), "1 failed");
}

// A call the CLI refused never reached the editor, so it is never taken for the editor's
// record of a later call of the same tool.
TEST(AiAssistantPanelTests, ACallTheCliRefusedMatchesNoRecord)
{
    constexpr uint64_t kTurn = 6;
    const AssistantAction actions[] = {Recorded(1, kTurn, "save_scene", AssistantToolClass::Gated,
                                                R"({"path":"Lanterns.scene"})", "Lanterns.scene",
                                                AssistantActionState::Done)};
    using End = ToolCallEnd;
    const ToolCall reported[] = {{"mcp__editor_assistant__save_scene", R"({"path":"Lanterns.scene"})", "a", End::Refused, ""},
                                 {"mcp__editor_assistant__save_scene", R"({"path":"Lanterns.scene"})", "b", End::Unknown, ""}};

    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For(reported, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(AgentCallPiecesText(rows[0].Summary), "Lanterns.scene · (refused) Not allowed for the assistant");
    EXPECT_EQ(rows[1].RequestId, 1u);
    EXPECT_EQ(AgentCallRowModel::Counts(reported, actions, kTurn), "1 refused");
}

// A call that ran because the user allowed it says so; calls folded into one row name a
// subject they share once.
TEST(AiAssistantPanelTests, AnAllowedCallSaysSoAndAFoldNamesARepeatedSubjectOnce)
{
    constexpr uint64_t kTurn = 2;
    AssistantAction first = Recorded(1, kTurn, "save_scene", AssistantToolClass::Gated, R"({"path":"Lanterns.scene"})",
                                     "Lanterns.scene", AssistantActionState::Done);
    first.Answer = AssistantAnswer::AllowForTurn;
    AssistantAction second = first;
    second.RequestId = 2;
    const AssistantAction actions[] = {first, second};
    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For({}, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(AgentCallPiecesText(rows[0].Summary), "Lanterns.scene (2 calls) · allowed for this turn");

    AssistantAction allowed = first;
    allowed.Answer = AssistantAnswer::Allow;
    const AssistantAction once[] = {allowed};
    EXPECT_EQ(AgentCallPiecesText(AgentCallRowModel::For({}, once, kTurn, {})[0].Summary), "Lanterns.scene · you allowed");
}

// An image result (a screenshot, a captured texture) is one inline image under its call's row:
// the PNG the tool wrote, by its path and size alone, scaled down uniformly to the row's width
// and the height cap and never up; the row names no asset for the image itself.
TEST(AiAssistantPanelTests, AnImageResultIsOneInlineImageOfBoundedSize)
{
    constexpr uint64_t kTurn = 3;
    const std::string capture = (Editor::CaptureOutputDirectory() / "screenshot_viewport_1.png").generic_string();
    const auto captureResult = [](const std::string& path) {
        return R"({"id":"9","ok":true,"result":{"filePath":")" + path +
               R"(","width":1920,"height":1080,"method":"rendergraph"}})";
    };
    AssistantAction shot = Recorded(1, kTurn, "take_screenshot", AssistantToolClass::Read, R"({"target":"viewport"})",
                                    "viewport", AssistantActionState::Done);
    shot.Result = captureResult(capture);
    const AssistantAction actions[] = {shot};
    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For({}, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_TRUE(rows[0].Image.has_value());
    EXPECT_EQ(*rows[0].Image, (AgentCallImage{capture, 1920, 1080}));
    EXPECT_TRUE(rows[0].Resources.empty()) << "the capture's own file is the image, not an asset tile";

    // Only a capture the editor wrote is an image: not another tool's result of the same shape,
    // not a file outside the capture directory, not a call that did not run.
    AssistantAction otherTool = shot;
    otherTool.Tool = "get_build_status";
    AssistantAction elsewhere = shot;
    elsewhere.Result = captureResult("C:/Users/someone/Pictures/screenshot_viewport_1.png");
    AssistantAction refused = shot;
    refused.State = AssistantActionState::Refused;
    for (const AssistantAction& notAnImage : {otherTool, elsewhere, refused})
    {
        const AssistantAction one[] = {notAnImage};
        EXPECT_FALSE(AgentCallRowModel::For({}, one, kTurn, {})[0].Image.has_value())
            << notAnImage.Tool << " " << notAnImage.Result;
    }

    EXPECT_EQ(FitAgentCallImage(1920, 1080, 300.0f, 180.0f), (AgentCallImageSize{300.0f, 168.75f}));
    EXPECT_EQ(FitAgentCallImage(400, 1600, 300.0f, 180.0f), (AgentCallImageSize{45.0f, 180.0f}));
    EXPECT_EQ(FitAgentCallImage(200, 100, 300.0f, 180.0f), (AgentCallImageSize{200.0f, 100.0f})) << "never up";

    std::vector<std::string> asked;
    Editor::EditorAssetActions thumbnails;
    thumbnails.ShowImage = [&asked](UIElement&, const std::filesystem::path& image) {
        asked.push_back(image.generic_string());
    };
    Editor::SetEditorAssetActions(thumbnails);
    AgentCallRow row("1:0", [](uint64_t, AssistantAnswer) {});
    row.Show(rows[0]);
    Editor::SetEditorAssetActions({});
    std::vector<UIElement*> images;
    std::function<void(UIElement&)> collect = [&](UIElement& element) {
        if (element.HasClass("agent-call-image") && !element.HasClass("hidden"))
            images.push_back(&element);
        for (const auto& child : element.GetChildren())
            collect(*child);
    };
    collect(row);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(asked, (std::vector<std::string>{capture}))
        << "the picture is the thumbnail service's inline copy";
    EXPECT_FALSE(images[0]->Overrides().Get(Style::BackgroundImage).has_value())
        << "the view never loads the full-size capture itself";

    // The image is sized within the row's width and the height cap, at its own aspect.
    auto& view = static_cast<AgentCallImageView&>(*images[0]);
    EXPECT_TRUE(view.Fit(300.0f, 180.0f));
    const auto width = view.Overrides().Get(Style::Width);
    const auto height = view.Overrides().Get(Style::Height);
    ASSERT_TRUE(width.has_value() && height.has_value());
    EXPECT_FLOAT_EQ(width->Value, 300.0f);
    EXPECT_FLOAT_EQ(height->Value, 168.75f);
    EXPECT_TRUE(view.Fit(1200.0f, 180.0f));
    EXPECT_FLOAT_EQ(view.Overrides().Get(Style::Width)->Value, 320.0f) << "the cap holds a wide row";
    EXPECT_FLOAT_EQ(view.Overrides().Get(Style::Height)->Value, 180.0f);
    EXPECT_FALSE(view.Fit(1200.0f, 180.0f)) << "an unchanged fit reports no movement";
}

// The inline image and an asset tile offer the Assets panel's file menu, in its words: Open,
// the platform's Show in Explorer and Copy Full Path, each through the editor's asset actions.
TEST(AiAssistantPanelTests, AnImageAndATileOfferTheAssetsPanelsFileMenu)
{
    std::vector<std::string> ran;
    Editor::EditorAssetActions actions;
    const auto record = [&ran](const char* name) {
        return [&ran, name](const std::filesystem::path& file) { ran.push_back(name + (" " + file.generic_string())); };
    };
    actions.Open = record("open");
    actions.ShowInFileManager = record("show");
    actions.CopyPath = record("copy");
    Editor::SetEditorAssetActions(actions);
    for (const std::filesystem::path file :
         {Editor::CaptureOutputDirectory() / "screenshot_viewport_1.png", std::filesystem::path("C:/Project/Materials/Brick.mat")})
    {
        ran.clear();
        std::vector<ContextMenuManipulator::Item> menu = AgentCallFileMenu(file);
        std::vector<std::string> labels;
        for (ContextMenuManipulator::Item& item : menu)
        {
            labels.push_back(item.Path);
            item.OnActivate();
        }
        EXPECT_EQ(labels, (std::vector<std::string>{"Open", Editor::ShowInFileManagerLabel(), "Copy Full Path"}));
        const std::string path = file.generic_string();
        EXPECT_EQ(ran, (std::vector<std::string>{"open " + path, "show " + path, "copy " + path}));
    }
    Editor::SetEditorAssetActions({});
}

// The inline image's picture lives only while the image is in the panel: it is released (its
// background dropped and the UI's texture for it evicted) when the image leaves the panel, and
// asked for again when it comes back.
TEST(AiAssistantPanelTests, AnInlineImageReleasesItsPictureWhenItLeavesThePanel)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    int requests = 0;
    Editor::EditorAssetActions actions;
    actions.ShowImage = [&requests](UIElement& element, const std::filesystem::path&) {
        ++requests;
        UI::Layout::SetBackgroundPath(element, "C:/captures/screenshot_viewport_1_thumb.png");
    };
    Editor::SetEditorAssetActions(actions);
    {
        UIManager ui(device.get());
        ui.SetRoot(std::make_unique<UIElement>());
        UIElement* root = ui.GetRootElement();
        auto view = std::make_unique<AgentCallImageView>("1:0");
        AgentCallImageView* image = view.get();
        root->AddChild(std::move(view));
        image->Show(AgentCallImage{(Editor::CaptureOutputDirectory() / "screenshot_viewport_1.png").generic_string(),
                                   1920, 1080});
        ui.Update(0.0f, /*interactive=*/false);
        ASSERT_EQ(requests, 1);
        ASSERT_TRUE(image->Overrides().Get(Style::BackgroundImage).has_value());

        std::unique_ptr<UIElement> taken = root->TakeChild(image);
        ui.Update(0.0f, /*interactive=*/false);
        EXPECT_FALSE(image->Overrides().Get(Style::BackgroundImage).has_value())
            << "an image out of the panel holds no picture";

        root->AddChild(std::move(taken));
        ui.Update(0.0f, /*interactive=*/false);
        EXPECT_EQ(requests, 2) << "an image back in the panel asks for its picture again";
        EXPECT_TRUE(image->Overrides().Get(Style::BackgroundImage).has_value());
    }
    Editor::SetEditorAssetActions({});
}

namespace
{
// The first label under `root` whose text starts with `prefix`; null when there is none.
const Label* FindLabelStartingWith(const UIElement& root, std::string_view prefix)
{
    if (const auto* label = dynamic_cast<const Label*>(&root); label && label->GetText().starts_with(prefix))
        return label;
    for (const auto& child : root.GetChildren())
        if (const Label* found = child ? FindLabelStartingWith(*child, prefix) : nullptr)
            return found;
    return nullptr;
}

} // namespace

namespace
{
// The conversation row as the panel lays it out: the row layout instantiated from the package's
// own .uxml and stylesheet (staged beside the test under an "ai-assistant" asset source), in a
// history list of the panel's width, its rows fitted from the list's OnPostLayout as
// PromptPanel::OnPostLayout fits them.
class FittingHistoryList final : public UIElement
{
public:
    AgentConversationRow* Row = nullptr;
    void OnPostLayout() override
    {
        if (Row)
            Row->FitHeight();
    }
};

struct ConversationLayout
{
    std::unique_ptr<Rendering::IDevice> Device = MakeHeadlessDevice();
    JobSystem::WorkStealingThreadPool Pool{2};
    AssetManager Assets;
    std::unique_ptr<UIManager> Ui;
    FittingHistoryList* List = nullptr;
    AgentConversationRow* Row = nullptr;
};

// A turn's reply: one call refused with a long MCP validation error, and a footer with counts
// and the reply's actions.
AgentConversationRowModel TurnWithALongReason(const std::string& reason)
{
    AgentCallRowModel call;
    call.Glyph = AgentCallRowModel::Mark::Refused;
    call.Action = "Change play mode";
    call.Summary = {AgentCallPiece::Words("play"), AgentCallPiece::MutedWords("\xC2\xB7"), AgentCallPiece::Words(reason)};
    call.UndoState = "nothing changed";
    AgentConversationRowModel model;
    model.Header = "Claude (local session)";
    model.Text = "Done.";
    model.Calls = {call};
    model.Status = "1 token in, 1 out \xC2\xB7 2 undo steps \xC2\xB7 2 failed";
    model.ShowsUndoTurn = true;
    model.ShowsReplyActions = true;
    return model;
}

// Builds `layout` with its list `listWidth` wide (the panel less its scrollbar) and shows
// `model` in one row, stacked below the panel's breakpoint as the panel stacks it; lays it out
// once, the frame the row first appears in. An empty string, or the reason it could not.
std::string BuildConversation(ConversationLayout& layout, float listWidth, const AgentConversationRowModel& model)
{
    const std::filesystem::path staged = LocalClaudeSession::TestDirectory() / "AiAssistantPanelFixtures";
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "AiAssistantPanelTests-layout";
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    std::filesystem::create_directories(scratch / "Assets", error);
    if (!layout.Assets.Initialize(scratch / "Assets", &layout.Pool, scratch / "AssetDatabase.assetdb",
                                  scratch / ".Cache" / "AssetDatabase"))
        return "AssetManager init failed";
    AssetSourceDesc package{};
    package.Alias = "ai-assistant";
    package.Root = staged / "ai-assistant";
    if (!layout.Assets.RegisterSource(package))
        return "package source registration failed";
    layout.Assets.WaitForStartupScan("ai-assistant");

    std::ifstream cssFile(staged / "ai-assistant" / "Editor" / "UI" / "panels" / "PromptPanel.css");
    if (!cssFile)
        return "PromptPanel.css not staged";
    // The editor's core sheet hides `.hidden`; the panel relies on it.
    const std::string css =
        std::string((std::istreambuf_iterator<char>(cssFile)), std::istreambuf_iterator<char>()) +
        "\n.hidden { display: none; }\n";
    auto sheet = std::make_shared<Stylesheet>();
    if (!UIParsing::CSSParser::ParseStylesFromString(css, *sheet))
        return "PromptPanel.css did not parse";

    layout.Ui = std::make_unique<UIManager>(layout.Device.get(), &layout.Assets);
    layout.Ui->SetLayoutSizeOverride(static_cast<uint32_t>(listWidth), 900);
    auto list = std::make_unique<FittingHistoryList>();
    list->Overrides().Set(Style::Width, StyleLength::Px(listWidth));
    layout.List = list.get();
    layout.Ui->SetRoot(std::move(list));
    layout.List->AddStylesheet(sheet);
    auto row = std::make_unique<AgentConversationRow>(1, [] {}, [] {}, [](const AgentReplyAction&) {},
                                                      [](uint64_t, AssistantAnswer) {}, [] {});
    layout.Row = row.get();
    if (!layout.Row->Build(*layout.Ui))
        return "the row layout did not instantiate";
    layout.List->AddChild(std::move(row));
    layout.List->Row = layout.Row;
    layout.Row->SetCallsStacked(listWidth < 520.0f);
    layout.Row->Show(model, "10:21");
    layout.Ui->Update(0.016f, /*interactive=*/false);
    return {};
}

const UIElement* FindByClass(const UIElement& root, std::string_view cls)
{
    if (root.HasClass(std::string(cls)))
        return &root;
    for (const auto& child : root.GetChildren())
        if (const UIElement* found = child ? FindByClass(*child, cls) : nullptr)
            return found;
    return nullptr;
}
} // namespace

// A call that did not run ends its row with why. In the conversation as the panel lays it out,
// at the panel's 597 px (wide rows) and 356 px (stacked), a long reason takes two lines, cut
// after a whole token with an ellipsis, in the frame the row first appears and in every frame
// after, never ending on an opening bracket or brace; opening Details leaves the cut as it
// was; a short one is shown whole. Details keeps the whole text (the model's, which the row
// never cuts). The panel fits its rows only on frames that lay them out anew, so a cut that
// needs later frames to finish can be left where it stopped.
TEST(AiAssistantPanelTests, ALongReasonTakesTwoLinesInTheConversation)
{
    const std::string reason = "(failed) MCP error -32602: Input validation error: Invalid arguments for tool "
                               "set_play_mode: [ { \"received\": \"play\", \"code\": \"invalid_enum_value\", "
                               "\"options\": [ \"enter\", \"exit\", \"pause\", \"resume\" ], \"path\": [ \"action\" ], "
                               "\"message\": \"Invalid enum value. Expected 'enter' | 'exit' | 'pause' | 'resume', "
                               "received 'play'\" } ]";
    // The history list at a 597 px and a 356 px panel (the panel less its 18 px scrollbar).
    for (const float listWidth : {579.0f, 338.0f})
    {
        ConversationLayout layout;
        if (!layout.Device)
            GTEST_SKIP() << "No device";
        const std::string built = BuildConversation(layout, listWidth, TurnWithALongReason(reason));
        ASSERT_TRUE(built.empty()) << built;
        const Label* words = FindLabelStartingWith(*layout.Row, "(failed)");
        ASSERT_NE(words, nullptr);
        const UIElement* details = FindByClass(*layout.Row, "agent-call-details-toggle");
        ASSERT_NE(details, nullptr);
        const float line = details->GetLayoutHeight();
        ASSERT_GT(line, 0.0f);
        EXPECT_TRUE(words->GetText().ends_with("\xE2\x80\xA6")) << listWidth << ": " << words->GetText();
        EXPECT_LE(words->GetLayoutHeight(), 2.0f * line + 0.5f) << listWidth << ": two lines at most";
        EXPECT_GT(words->GetLayoutHeight(), line + 0.5f) << listWidth << ": the cut keeps two lines, not one";
        const std::string cut = words->GetText().substr(0, words->GetText().size() - 3);
        EXPECT_TRUE(reason.compare(0, cut.size(), cut) == 0 &&
                    (reason[cut.size()] == ' ' || cut.back() == ',' || cut.back() == ':' ||
                     (cut.back() == '"' && !std::isalnum(static_cast<unsigned char>(reason[cut.size()])))))
            << listWidth << ": the cut ends a word or a token: " << cut;
        EXPECT_TRUE(cut.back() != '[' && cut.back() != '{') << listWidth << ": the cut ends on a bracket: " << cut;
        const std::string firstFrame = words->GetText();
        for (int frame = 0; frame < 8; ++frame)
            layout.Ui->Update(0.016f, /*interactive=*/false);
        EXPECT_EQ(words->GetText(), firstFrame) << listWidth << ": the cut holds in later frames";

        UIEvent press{};
        press.Id = kEventMouseUp;
        press.Button = 0;
        press.Target = const_cast<UIElement*>(details);
        press.CurrentTarget = press.Target;
        press.Target->DispatchEvent(press);
        for (int frame = 0; frame < 4; ++frame)
            layout.Ui->Update(0.016f, /*interactive=*/false);
        ASSERT_EQ(static_cast<const Label*>(details)->GetText(), "Hide details");
        EXPECT_EQ(words->GetText(), firstFrame) << listWidth << ": opening Details leaves the cut as it was";

        AgentConversationRowModel shorter = TurnWithALongReason("(failed) Invalid entity");
        layout.Row->Show(shorter, "10:21");
        layout.Ui->Update(0.016f, /*interactive=*/false);
        words = FindLabelStartingWith(*layout.Row, "(failed)");
        ASSERT_NE(words, nullptr);
        EXPECT_EQ(words->GetText(), "(failed) Invalid entity") << listWidth << ": a reason that fits is shown whole";
    }
}

// In a narrow panel the footer stacks: the status takes the card's whole line and the reply's
// actions sit under it, right-aligned.
TEST(AiAssistantPanelTests, ANarrowFooterPutsTheStatusOnItsOwnLineAndTheActionsUnderIt)
{
    ConversationLayout layout;
    if (!layout.Device)
        GTEST_SKIP() << "No device";
    const std::string built = BuildConversation(layout, 338.0f, TurnWithALongReason("(failed) Invalid entity"));
    ASSERT_TRUE(built.empty()) << built;
    const UIElement* footer = FindByClass(*layout.Row, "agent-row-footer");
    const UIElement* status = FindByClass(*layout.Row, "agent-row-status");
    const UIElement* actions = FindByClass(*layout.Row, "agent-row-actions");
    ASSERT_TRUE(footer && status && actions);
    const Box4& padding = footer->GetLayoutPadding();
    const float content = footer->GetLayoutWidth() - padding.Left - padding.Right;
    EXPECT_NEAR(status->GetLayoutWidth(), content, 0.5f) << "the status spans the footer's line";
    EXPECT_GE(actions->GetLayoutY(), status->GetLayoutY() + status->GetLayoutHeight() - 0.5f)
        << "the actions sit under the status";
    EXPECT_NEAR(actions->GetLayoutX() + actions->GetLayoutWidth(), footer->GetLayoutX() + footer->GetLayoutWidth() - padding.Right,
                0.5f)
        << "the actions are right-aligned";
}

// A material the assistant names by GUID resolves through the asset database to a tile with
// the editor's thumbnail of that asset, its name and Open; a material path that is not there
// is a tile with its name and "not found", never an empty tile.
TEST(AiAssistantPanelTests, ANamedAssetResolvesToItsThumbnailAndAMissingOneShowsItsName)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "AiAssistantPanelTests-assets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Materials");
    std::ofstream(root / "Materials" / "Brick.mat") << "{}";
    AssetRegistry registry;
    ASSERT_TRUE(registry.Initialize(root));
    ASSERT_TRUE(registry.RegisterAsset(root / "Materials" / "Brick.mat"));
    const std::string brick = registry.GetAssetGUID(root / "Materials" / "Brick.mat").ToString();

    constexpr uint64_t kTurn = 2;
    const AssistantAction set = Recorded(
        1, kTurn, "set_component", AssistantToolClass::UndoableEdit,
        R"({"entityId":7,"component":"MeshRenderer","values":{"material":")" + brick +
            R"(","fallback":"Materials/Gone.mat"}})",
        "7", AssistantActionState::Done);
    const AssistantAction actions[] = {set};
    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For({}, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].Resources, (std::vector<std::string>{"Materials/Gone.mat", brick})) << "members in name order";
    EXPECT_FALSE(rows[0].Image.has_value());

    const AgentCallResourceResolver resolver(registry, [&root](const std::filesystem::path& path) {
        return path.is_relative() ? root / path : path;
    });
    const std::optional<AgentCallResource> found = resolver.Resolve(brick);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->Found);
    EXPECT_EQ(found->Name, "Brick.mat");
    EXPECT_EQ(found->Type, AssetType::Material);
    const std::optional<AgentCallResource> missing = resolver.Resolve("Materials/Gone.mat");
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(missing->Found);
    EXPECT_EQ(missing->Name, "Gone.mat");
    EXPECT_FALSE(resolver.Resolve("00000000-0000-4000-8000-000000000123").has_value())
        << "a GUID no asset has is not shown";
    const std::string byPath = (root / "Materials" / "Brick.mat").generic_string();
    const std::vector<AgentCallResource> tiles =
        resolver.ResolveAll(std::vector<std::string>{brick, byPath, "Materials/Gone.mat"});
    ASSERT_EQ(tiles.size(), 2u) << "the material's GUID and its path are one tile";
    EXPECT_EQ(tiles[0].Reference, brick);
    EXPECT_EQ(tiles[1].Name, "Gone.mat");
    const std::string goneByPath = (root / "Materials" / "Gone.mat").generic_string();
    EXPECT_EQ(resolver.ResolveAll(std::vector<std::string>{"Materials/Gone.mat", goneByPath}).size(), 1u)
        << "a missing asset named by two spellings of its path is one tile";

    // A path of exactly a GUID's length is still a path.
    const std::string longPath = "Materials/Gone_missing_mat1.material";
    ASSERT_EQ(longPath.size(), 36u);
    const std::optional<AgentCallResource> longMissing = resolver.Resolve(longPath);
    ASSERT_TRUE(longMissing.has_value()) << "a 36-character path is not read as a GUID";
    EXPECT_FALSE(longMissing->Found);
    EXPECT_EQ(longMissing->Name, "Gone_missing_mat1.material");

    std::vector<std::filesystem::path> thumbnailsAsked;
    Editor::EditorAssetActions actionsStub;
    actionsStub.ShowThumbnail = [&](UIElement& element, const std::filesystem::path& asset, int) {
        EXPECT_TRUE(element.HasClass("agent-call-resource-thumbnail"));
        thumbnailsAsked.push_back(asset);
    };
    Editor::SetEditorAssetActions(actionsStub);
    const auto texts = [](UIElement& tile) {
        std::vector<std::string> words;
        for (const auto& child : tile.GetChildren())
            if (const auto* label = dynamic_cast<const Label*>(child.get()))
                words.push_back(label->GetText());
        return words;
    };
    AgentCallResourceTile foundTile(*found, "1:0:0");
    AgentCallResourceTile missingTile(*missing, "1:0:1");
    foundTile.RequestThumbnail();
    missingTile.RequestThumbnail();
    Editor::SetEditorAssetActions({});
    EXPECT_EQ(thumbnailsAsked, (std::vector<std::filesystem::path>{found->Path}));
    EXPECT_EQ(texts(foundTile), (std::vector<std::string>{"Brick.mat", "Material", "Open"}));
    EXPECT_EQ(texts(missingTile), (std::vector<std::string>{"Gone.mat", "not found"}));
    EXPECT_EQ(missingTile.GetTooltip(), "Not in the project when the call ran: Materials/Gone.mat")
        << "the tile says it was resolved when the call ran";
    registry.Shutdown();
    std::filesystem::remove_all(root);
}

// A result that is neither an image nor an asset leaves the row as it was: no image, no tiles,
// the same summary and Details.
TEST(AiAssistantPanelTests, ANonImageResultLeavesTheRowUnchanged)
{
    constexpr uint64_t kTurn = 5;
    AssistantAction log = Recorded(1, kTurn, "get_log", AssistantToolClass::Read, R"({"lines":20})", "",
                                   AssistantActionState::Done);
    log.Result = R"({"id":"4","ok":true,"result":{"lines":["Loaded scene","Saved"],"filePath":"C:/logs/editor.log"}})";
    const AssistantAction actions[] = {log};
    const std::vector<AgentCallRowModel> rows = AgentCallRowModel::For({}, actions, kTurn, {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].Image.has_value());
    EXPECT_TRUE(rows[0].Resources.empty());
    EXPECT_EQ(rows[0].Action, "Read the log");
    EXPECT_EQ(AgentCallPiecesText(rows[0].Details.front()), "Call get_log");
}

// The header names the model asked for until the turn reports the one that answered,
// in its vendor's words, and the effort asked for.
TEST(AiAssistantPanelTests, TheReplyHeaderNamesTheModelThatAnsweredAndTheEffort)
{
    Conversation conversation;
    const std::string session(ClaudeSessionProvider::kId);
    const Conversation::TurnId turn = conversation.AddTurn("hi", session, "opus", "xhigh", {});
    auto header = [&](Conversation::TurnId id)
    { return AgentConversationRowModel::From(*conversation.Reply(id), {}, id, {}).Header; };
    EXPECT_EQ(header(turn), "Claude (local session) · Opus (latest) · Extra high");

    conversation.Start(turn);
    TurnResult result;
    result.Outcome = TurnOutcome::Succeeded;
    result.Model = "claude-opus-5-5[1m]";
    conversation.Finish(turn, result);
    EXPECT_EQ(header(turn), "Claude (local session) · Opus 5.5 · Extra high");

    const Conversation::TurnId dated = conversation.AddTurn("hi", session, "haiku", "low", {});
    result.Model = "claude-haiku-4-5-20251001";
    conversation.Finish(dated, result);
    EXPECT_EQ(header(dated), "Claude (local session) · Haiku 4.5 · Low");

    const Conversation::TurnId codexDefault = conversation.AddTurn("hi", "codex-session", "", "", {});
    EXPECT_EQ(header(codexDefault), "Codex (local session) · default model");
    const Conversation::TurnId codex = conversation.AddTurn("hi", "codex-session", "gpt-5-codex", "high", {});
    EXPECT_EQ(header(codex), "Codex (local session) · gpt-5-codex") << "another vendor's id as it is, no effort";
}

// A successful turn records the model that answered for the model it asked for, which
// the model button and the settings page then name; a failed turn records nothing, so
// its family keeps "(latest)".
TEST(AiAssistantPanelTests, ASuccessfulTurnRecordsTheModelThatAnswered)
{
    auto provider = std::make_shared<ScriptedProvider>("fake-answering");
    provider->AnsweredModel = "claude-fake-2-1";
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake-answering");

    ASSERT_TRUE(controller.Send("hi", "fake-family", "high"));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    EXPECT_EQ(AiAssistantSettings::AnsweredModel("fake-answering", "fake-family"), "claude-fake-2-1");
    EXPECT_EQ(AiAssistantSettings::ModelLabel("fake-answering", "fake-family"), "Fake 2.1");

    provider->AnsweredModel = "claude-fake-3-0";
    provider->FinalOutcome = TurnOutcome::Failed;
    ASSERT_TRUE(controller.Send("hi", "failing-family", "high"));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    EXPECT_EQ(AiAssistantSettings::AnsweredModel("fake-answering", "failing-family"), "")
        << "a failed turn names no version";
}

// The model popover, built from its layout, offers the connection's models and its
// effort levels as slider stops, stores each change in the user's settings at once, and
// its button reads the choice: the family until a turn names the version. A choice made
// on the settings page reaches it, and a connection without levels or without a model
// list hides the slider or the button.
TEST(AiAssistantPanelTests, TheModelPopoverStoresEachChoiceAndItsButtonReadsIt)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    ScopedPreferencesRoot preferences;
    UIRegistration::RegisterBuiltInControls();
    JobSystem::WorkStealingThreadPool pool(2);
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "AiAssistantPanelTests-popover";
    std::filesystem::create_directories(root);
    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(root, &pool));
    AssetSourceDesc package;
    package.Alias = "ai-assistant";
    package.Root = Platform::GetExecutablePath().parent_path() / "AiAssistantPanelFixtures" / "ai-assistant";
    ASSERT_TRUE(assets.RegisterSource(package));
    UIManager ui(device.get(), &assets);
    ui.SetRoot(std::make_unique<UIElement>());

    Button button;
    auto owned = std::make_unique<AgentModelPopover>(button);
    AgentModelPopover& popover = *owned;
    ui.GetRootElement()->AddChild(std::move(owned));
    ASSERT_TRUE(popover.Build(ui));
    const std::string session(ClaudeSessionProvider::kId);
    popover.SetProvider(session);
    popover.Open();
    auto* models = dynamic_cast<Dropdown*>(popover.FindById("AgentModelDropdown"));
    auto* effort = dynamic_cast<Slider*>(popover.FindById("AgentEffortSlider"));
    UIElement* effortRow = popover.FindById("AgentEffortRow");
    ASSERT_TRUE(models && effort && effortRow);
    EXPECT_TRUE(popover.IsPopupOpen());
    EXPECT_EQ(button.GetText(), "Opus (latest) · High") << "the initial pair, never a version";
    EXPECT_TRUE(button.GetTooltip().starts_with("Opus (latest) · High\n")) << "the whole text, for a cut label";
    EXPECT_EQ(models->GetSelectedLabel(), "Opus (latest)");

    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::ModelChoices(session))
    {
        models->SetSelectedValue(std::string(choice.Id));
        EXPECT_EQ(AiAssistantSettings::Model(session), choice.Id);
        EXPECT_EQ(models->GetSelectedLabel(), choice.Label);
    }
    EXPECT_EQ(AiAssistantSettings::Model(session), "haiku");

    // One stop per level: low, medium, high, xhigh, max.
    EXPECT_EQ(effort->GetMin(), 0.0f);
    EXPECT_EQ(effort->GetMax(), 4.0f);
    EXPECT_EQ(effort->GetStep(), 1.0f);
    EXPECT_EQ(effort->GetValue(), 2.0f);
    effort->SetValue(4.0f);
    EXPECT_EQ(AiAssistantSettings::Effort(session), "max");
    EXPECT_EQ(button.GetText(), "Haiku (latest) · Max");

    AiAssistantSettings::SetAnsweredModel(session, "haiku", "claude-haiku-5-5");
    AiAssistantSettings::SetEffort(session, "xhigh");
    AiAssistantSettings::SetEffort(session, "not-a-level");
    popover.Update();
    EXPECT_EQ(button.GetText(), "Haiku 5.5 · Extra high") << "the version that answered, then the settings page's effort";
    EXPECT_EQ(models->GetSelectedLabel(), "Haiku 5.5");
    EXPECT_EQ(effort->GetValue(), 3.0f);

    popover.SetProvider(std::string(ClaudeApiProvider::kId));
    EXPECT_FALSE(popover.IsPopupOpen()) << "another connection closes it";
    popover.Open();
    EXPECT_TRUE(effortRow->HasClass("hidden")) << "Claude (API) has no effort levels";
    EXPECT_EQ(button.GetText(), "Opus 5.5");
    popover.SetProvider("codex-session");
    EXPECT_TRUE(button.HasClass("hidden")) << "Codex names its own models in settings";
}

// The model button is as wide as the widest text it can show for the connection, measured
// in its own face, so the row never moves with the choice; narrower, it drops "(latest)",
// then the effort, before its label is cut.
TEST(AiAssistantPanelTests, TheModelButtonTakesItsWidestTextAndShortensItsTextToFit)
{
    auto device = MakeHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No device";
    ScopedPreferencesRoot preferences;
    UIRegistration::RegisterBuiltInControls();
    UIManager ui(device.get());
    ui.SetRoot(std::make_unique<UIElement>());
    auto ownedButton = std::make_unique<Button>();
    Button& button = *ownedButton;
    ui.GetRootElement()->AddChild(std::move(ownedButton));
    auto owned = std::make_unique<AgentModelPopover>(button);
    AgentModelPopover& popover = *owned;
    ui.GetRootElement()->AddChild(std::move(owned));
    const std::string session(ClaudeSessionProvider::kId);
    popover.SetProvider(session);
    auto settle = [&]
    {
        for (int frame = 0; frame < 4; ++frame)
        {
            ui.Update(0.016f, /*interactive=*/false);
            popover.Update();
        }
    };
    settle();

    float widest = 0.0f;
    for (const AiAssistantSettings::Choice& model : AiAssistantSettings::ModelChoices(session))
        for (const AiAssistantSettings::Choice& effort : AiAssistantSettings::EffortChoices(session))
            widest = std::max(widest, button.WidthForText(std::string(model.Label) + " · " + std::string(effort.Label)));
    ASSERT_GT(widest, 0.0f) << "the button's text is measured once laid out";
    const auto width = button.Overrides().Get(Style::Width);
    ASSERT_TRUE(width && width->IsPx());
    EXPECT_FLOAT_EQ(width->Value, std::ceil(widest));
    EXPECT_EQ(button.GetText(), "Opus (latest) · High");

    button.Overrides().Set(Style::Width, StyleLength::Px(std::ceil(button.WidthForText("Opus · High"))));
    settle();
    EXPECT_EQ(button.GetText(), "Opus · High") << "\"(latest)\" goes first";
    button.Overrides().Set(Style::Width, StyleLength::Px(std::ceil(button.WidthForText("Opus"))));
    settle();
    EXPECT_EQ(button.GetText(), "Opus") << "then the effort; the tooltip keeps both";
    EXPECT_TRUE(button.GetTooltip().starts_with("Opus (latest) · High\n"));
}

// The mode control keeps its widest label whole while the control line still leaves the
// model button its minimum and the connection its first word, and always beside the field;
// on a narrower line it shows its compact labels, whole while they fit.
TEST(AiAssistantPanelTests, TheModeControlKeepsItsLabelWholeOnlyWhileTheConnectionStaysReadable)
{
    ModeControlFit fit;
    fit.WholeLabelPx = 122.0f;
    fit.CompactLabelPx = 60.0f;
    fit.OthersPx = 130.0f;
    fit.ModelMinimumPx = 56.0f;
    fit.ConnectionReadablePx = 70.0f;
    fit.Stacked = true;
    const auto expect = [&](std::optional<float> minimum, bool compact, const char* what)
    {
        const ModeControlLayout layout = ModeControlLayoutFor(fit);
        EXPECT_EQ(layout.MinimumPx, minimum) << what;
        EXPECT_EQ(layout.Compact, compact) << what;
    };
    fit.AvailablePx = 579.0f;
    expect(122.0f, false, "a 597 px panel");
    fit.AvailablePx = 378.0f;
    expect(122.0f, false, "exactly enough");
    fit.AvailablePx = 338.0f;
    expect(60.0f, true, "a 356 px panel: the compact labels whole");
    fit.AvailablePx = 300.0f;
    expect(std::nullopt, true, "narrower: the compact labels at the stylesheet's minimum");
    fit.Stacked = false;
    expect(122.0f, false, "beside the field");
}

// A failed reply's Retry is a new turn: the same prompt, connection, model and effort, queued
// behind the running turn like any prompt, and sent with the history that turn would
// have had. The failed turn stays in the transcript, marked retried, and is retried
// once: a second click sends nothing.
TEST(AiAssistantPanelTests, RetryQueuesTheFailedPromptOnceAsANewTurn)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    provider->ThrowOnce = true;
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    ASSERT_TRUE(controller.Send("first", "model-a", "high"));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    ASSERT_EQ(ReplyStatus(controller, 0), MessageStatus::Failed);
    EXPECT_FALSE(AgentConversationRowModel::From(controller.GetConversation().Messages()[1], {}, 0, {}).Retried);

    provider->Hold = true;
    ASSERT_TRUE(controller.Send("second", "", ""));
    ASSERT_TRUE(PumpUntil(controller, [&] { return provider->Holding.load(); }));
    controller.Retry(0); // a user message: nothing to retry
    controller.Retry(1);
    controller.Retry(1);
    ASSERT_EQ(controller.GetConversation().Messages().size(), 6u) << "one Retry, however many clicks";
    EXPECT_EQ(ReplyStatus(controller, 2), MessageStatus::Queued) << "a Retry waits like any prompt";
    const AgentConversationRowModel failedRow =
        AgentConversationRowModel::From(controller.GetConversation().Messages()[1], {}, 0, {});
    EXPECT_TRUE(failedRow.Failed);
    EXPECT_TRUE(failedRow.Retried) << "the row reads Retried below in place of Retry";

    provider->Hold = false;
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    const auto messages = controller.GetConversation().Messages();
    EXPECT_EQ(messages[1].Status, MessageStatus::Failed) << "the failed turn stays as it was";
    EXPECT_EQ(messages[4].Text, "first");
    EXPECT_EQ(messages[5].Status, MessageStatus::Complete);
    EXPECT_EQ(messages[5].ProviderId, "fake");
    EXPECT_EQ(messages[5].Model, "model-a");
    EXPECT_EQ(messages[5].Effort, "high");
    controller.Retry(5);
    EXPECT_EQ(controller.GetConversation().Messages().size(), 6u) << "a complete reply has no Retry";

    const std::vector<ObservedTurn> turns = provider->Observed();
    ASSERT_EQ(turns.size(), 3u);
    EXPECT_EQ(turns[0].Effort, "high") << "the turn asks for the effort it was sent with";
    EXPECT_EQ(turns[2].UserText, "first");
    EXPECT_EQ(turns[2].Model, "model-a");
    EXPECT_EQ(turns[2].Effort, "high") << "a retry asks for the failed turn's model and effort";
    EXPECT_EQ(turns[2].HistorySize, 2u) << "the second turn's question and answer, not the failed turn";
}

TEST(AiAssistantPanelTests, APromptOverTheMessageCapIsRefusedWholeAndNotQueued)
{
    auto provider = std::make_shared<ScriptedProvider>("fake");
    AgentConversationController controller = MakeController({provider});
    controller.SetProvider("fake");

    EXPECT_FALSE(controller.Send(std::string(Conversation::kMaxMessageBytes + 1, 'a'), "", ""));
    EXPECT_TRUE(controller.GetConversation().Messages().empty());
    EXPECT_FALSE(controller.IsBusy());

    EXPECT_EQ(PromptTooLongStatus(Conversation::kMaxMessageBytes + 1),
              "The prompt is 65 KB, over the 64 KB a message can hold: shorten it and send again.")
        << "the size rounds up";

    EXPECT_TRUE(controller.Send(std::string(Conversation::kMaxMessageBytes, 'a'), "", ""));
    EXPECT_EQ(controller.GetConversation().Messages()[0].Text.size(), Conversation::kMaxMessageBytes);
}

// A reply past the cap keeps its first 64 KB, cut on a character boundary: the row
// marks the cut, Copy says what it copied, and the next turn resends the stored text.
TEST(AiAssistantPanelTests, AReplyOverTheMessageCapIsStoredTruncatedAndResentAsStored)
{
    constexpr size_t kCap = Conversation::kMaxMessageBytes;
    Conversation conversation;
    const Conversation::TurnId turn = conversation.AddTurn("hi", "fake", "", "", {});
    conversation.Start(turn);

    // A two-byte character straddles the cap, so the cut falls before it.
    conversation.AppendReply(turn, std::string(kCap - 1, 'a'), {}, {});
    EXPECT_FALSE(conversation.Reply(turn)->Truncated);
    conversation.AppendReply(turn, "\xC3\xA9tail", {}, {});
    EXPECT_TRUE(conversation.Reply(turn)->Truncated);
    EXPECT_EQ(conversation.Reply(turn)->Text, std::string(kCap - 1, 'a'));
    conversation.AppendReply(turn, "b", {}, {});
    EXPECT_EQ(conversation.Reply(turn)->Text.size(), kCap - 1) << "nothing is appended past the cut";

    TurnResult result;
    result.Outcome = TurnOutcome::Succeeded;
    result.Text = std::string(kCap + 100, 'z');
    conversation.Finish(turn, result);
    const ConversationMessage& reply = *conversation.Reply(turn);
    EXPECT_EQ(reply.Text, std::string(kCap, 'z'));
    EXPECT_TRUE(reply.Truncated);

    const AgentConversationRowModel row = AgentConversationRowModel::From(reply, {}, 0, {});
    EXPECT_EQ(row.Text, std::string(kCap, 'z') + "\xE2\x80\xA6");
    EXPECT_EQ(row.Status, "Reply truncated at 64 KB");
    EXPECT_TRUE(row.ShowsReplyActions);
    EXPECT_EQ(CopyReplyFeedback(reply), "Reply copied: its first 64 KB, where it was truncated");

    const Conversation::TurnId next = conversation.AddTurn("more", "fake", "", "", {});
    const std::vector<ConversationMessage> history = conversation.HistoryBefore(next);
    ASSERT_EQ(history.size(), 2u);
    EXPECT_EQ(history[1].Text, std::string(kCap, 'z')) << "the stored text, with no row marker";

    // A reply within the cap is stored whole and copied without a note.
    result.Text = "short";
    conversation.Finish(next, result);
    EXPECT_FALSE(conversation.Reply(next)->Truncated);
    EXPECT_EQ(CopyReplyFeedback(*conversation.Reply(next)), "Reply copied");
}

// Resume last continues the stored session: the first turn passes its id. A new
// session (the panel's New session: a new conversation, the sessions forgotten)
// passes none.
TEST(AiAssistantPanelTests, AContinuedSessionResumesItsIdOnTheFirstTurnAndANewSessionDoesNot)
{
    LocalClaudeSession local;
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "args");
    AiAssistantSettings::SetLastSession(ClaudeSessionProvider::kId, local.ProjectRoot(), "stored-session");
    AgentConversationController controller = local.MakeController();
    controller.SetProvider(std::string(ClaudeSessionProvider::kId));

    CliSessionSummary stored;
    stored.Id = local.StoredSession();
    controller.ContinueSession(stored);
    ASSERT_TRUE(controller.Send("hello", "", ""));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    const std::string resumed = controller.GetConversation().Messages()[1].Text;
    EXPECT_NE(resumed.find("--resume stored-session "), std::string::npos) << resumed;
    EXPECT_EQ(local.StoredSession(), kFakeCliSession) << "the session the CLI reported is stored";

    controller.NewConversation();
    AgentSessionState::Get().ForgetSessions();
    ASSERT_TRUE(controller.Send("hello", "", ""));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    const std::string fresh = controller.GetConversation().Messages()[1].Text;
    EXPECT_NE(fresh.find("args="), std::string::npos) << fresh;
    EXPECT_EQ(fresh.find("--resume"), std::string::npos) << fresh;
}

// The mode control's memory: a new conversation starts in the mode last chosen (Auto
// until the user first chooses), and a resumed session in the mode it last ran in; a
// session this editor never ran starts in the last chosen one.
TEST(AiAssistantPanelTests, ANewConversationStartsInTheLastModeChosenAndAResumedSessionInItsOwn)
{
    LocalClaudeSession local;
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "args");
    AgentConversationController controller = local.MakeController();
    controller.SetProvider(std::string(ClaudeSessionProvider::kId));
    EXPECT_EQ(controller.Mode(), AssistantMode::Auto);

    controller.SetMode(AssistantMode::ReadOnly);
    ASSERT_TRUE(controller.Send("hello", "", ""));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    ASSERT_EQ(local.StoredSession(), kFakeCliSession);

    controller.NewConversation();
    AgentSessionState::Get().ForgetSessions();
    EXPECT_EQ(controller.Mode(), AssistantMode::ReadOnly);
    controller.SetMode(AssistantMode::EditFreely);
    AgentConversationController reopened = local.MakeController();
    reopened.SetProvider(std::string(ClaudeSessionProvider::kId));
    EXPECT_EQ(reopened.Mode(), AssistantMode::EditFreely);

    CliSessionSummary ran;
    ran.Id = kFakeCliSession;
    reopened.ContinueSession(ran);
    EXPECT_EQ(reopened.Mode(), AssistantMode::ReadOnly);
    CliSessionSummary neverRan;
    neverRan.Id = "00000000-0000-4000-8000-000000000077";
    reopened.ContinueSession(neverRan);
    EXPECT_EQ(reopened.Mode(), AssistantMode::EditFreely);
}

// A mode chosen while a turn runs, before the CLI has reported its session, is the mode
// stored for that session.
TEST(AiAssistantPanelTests, AModeChosenBeforeTheSessionIdArrivesIsTheSessionsMode)
{
    LocalClaudeSession local;
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "slow-init");
    AgentConversationController controller = local.MakeController();
    controller.SetProvider(std::string(ClaudeSessionProvider::kId));
    controller.SetMode(AssistantMode::Auto);
    ASSERT_TRUE(controller.Send("hello", "", ""));
    ASSERT_TRUE(PumpUntil(controller, [&] { return controller.GetConversation().Messages()[1].Status ==
                                                   MessageStatus::InProgress; }));
    controller.SetMode(AssistantMode::EditFreely);
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));

    ASSERT_EQ(local.StoredSession(), kFakeCliSession);
    EXPECT_EQ(AiAssistantSettings::SessionMode(ClaudeSessionProvider::kId, local.ProjectRoot(), kFakeCliSession),
              AssistantMode::EditFreely);
}

// Leaving a connection while its tools check runs does not wait for that check: the
// panel's thread moves on and the check ends on its own.
TEST(AiAssistantPanelTests, LeavingAConnectionDuringItsToolsCheckDoesNotWaitForIt)
{
    LocalClaudeSession local;
    // The tools are available: a debug port, a staged server, and the fake CLI as Node.js.
    Editor::SetEditorDebugPort(9);
    const std::filesystem::path script = AssistantAttachment::StagedServerScript();
    // <executable dir>/mcp, removed again unless a real server is staged there.
    const std::filesystem::path stagedServer = script.parent_path().parent_path();
    const bool stagedBefore = std::filesystem::exists(stagedServer);
    if (!stagedBefore)
    {
        std::filesystem::create_directories(script.parent_path());
        std::ofstream(script) << "// test stand-in\n";
    }
#if defined(_WIN32)
    const char* fakeNode = "FakeAgentCli.exe";
#else
    const char* fakeNode = "FakeAgentCli";
#endif
    const Editor::SettingsCategoryDescriptor category = AiAssistantSettings::BuildCategory();
    for (const Editor::SettingsFieldDescriptor& field : category.Fields)
        if (field.Label == "Node executable")
            std::get<Editor::SettingsFieldDescriptor::PathField>(field.Control)
                .Set((LocalClaudeSession::TestDirectory() / fakeNode).string());

    auto slow = std::make_shared<SlowCheckProvider>("slow");
    auto other = std::make_shared<SlowCheckProvider>("other");
    other->Release = true;
    AgentConversationController controller([&](std::string_view id) -> std::shared_ptr<IAgentProvider> {
        return id == "slow" ? std::static_pointer_cast<IAgentProvider>(slow) : other;
    });
    controller.SetProvider("slow");
    ASSERT_TRUE(PumpUntil(controller, [&] { return slow->Checking.load(); }));

    const auto before = std::chrono::steady_clock::now();
    controller.SetProvider("other");
    const auto waited = std::chrono::steady_clock::now() - before;
    slow->Release = true;

    EXPECT_LT(waited, 500ms) << "leaving the connection waited for its check";
    Editor::SetEditorDebugPort(0);
    if (!stagedBefore)
        std::filesystem::remove_all(stagedServer);
}

// A stored id the CLI no longer knows fails the first turn with the CLI's message;
// the id is forgotten, in memory and in the project's settings, so Retry starts a new
// session.
TEST(AiAssistantPanelTests, ASessionTheCliNoLongerKnowsIsForgottenAndRetryStartsANewOne)
{
    LocalClaudeSession local;
    const std::string fixture = (local.TestDirectory() / "AiAssistantPanelFixtures" / "claude-dead-session.stdout").string();
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    AgentConversationController controller = local.MakeController();
    controller.SetProvider(std::string(ClaudeSessionProvider::kId));
    CliSessionSummary dead;
    dead.Id = "00000000-0000-4000-8000-000000000009";
    controller.ContinueSession(dead);
    EXPECT_EQ(local.StoredSession(), dead.Id) << "a chosen session is stored";
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
        ASSERT_TRUE(controller.Send("hello", "", ""));
        ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    }

    const ConversationMessage& failed = controller.GetConversation().Messages()[1];
    EXPECT_EQ(failed.Status, MessageStatus::Failed);
    EXPECT_NE(failed.Notice.find("No conversation found with session ID"), std::string::npos) << failed.Notice;
    EXPECT_TRUE(controller.SessionWasLost());
    EXPECT_FALSE(controller.ContinuedSession().has_value()) << "the Continued session row goes";
    EXPECT_EQ(AgentSessionState::Get().SessionId(ClaudeSessionProvider::kId), "");
    EXPECT_EQ(local.StoredSession(), "");

    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "args");
    controller.Retry(1);
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));
    EXPECT_FALSE(controller.SessionWasLost());
    const std::string retried = controller.GetConversation().Messages()[3].Text;
    EXPECT_NE(retried.find("args="), std::string::npos) << retried;
    EXPECT_EQ(retried.find("--resume"), std::string::npos) << retried;
}

// A failure that is not the CLI saying the session is gone (here the login check
// refusing) keeps the chosen session: the user fixes the login and continues it.
TEST(AiAssistantPanelTests, ALoginRefusalKeepsTheChosenSession)
{
    LocalClaudeSession local;
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "logged-out");
    AgentConversationController controller = local.MakeController();
    controller.SetProvider(std::string(ClaudeSessionProvider::kId));
    CliSessionSummary chosen;
    chosen.Id = "00000000-0000-4000-8000-0000000000aa";
    controller.ContinueSession(chosen);
    ASSERT_TRUE(controller.Send("hello", "", ""));
    ASSERT_TRUE(PumpUntil(controller, [&] { return !controller.IsBusy(); }));

    const ConversationMessage& failed = controller.GetConversation().Messages()[1];
    EXPECT_EQ(failed.Status, MessageStatus::Failed);
    EXPECT_NE(failed.Notice.find("Not logged in"), std::string::npos) << failed.Notice;
    EXPECT_FALSE(controller.SessionWasLost());
    EXPECT_TRUE(controller.ContinuedSession().has_value());
    EXPECT_EQ(AgentSessionState::Get().SessionId(ClaudeSessionProvider::kId), chosen.Id);
    EXPECT_EQ(local.StoredSession(), chosen.Id);
}

// The list is read off the UI thread and arrives only through Update(); a request
// while a read runs reads once more when it ends.
TEST(AiAssistantPanelTests, TheSessionListArrivesThroughUpdateAndARequestDuringAReadReadsOnceMore)
{
    auto provider = std::make_shared<ListingProvider>("local");
    provider->Hold = true;
    AgentConversationController controller(ListingLookup({provider}));
    controller.SetProvider("local");

    controller.RequestSessionList();
    ASSERT_TRUE(WaitFor([&] { return provider->Reading.load(); }));
    EXPECT_NE(provider->Reads.load(), 0) << "the read runs without an Update";
    controller.RequestSessionList();
    controller.RequestSessionList();
    provider->Hold = false;
    ASSERT_TRUE(WaitFor([&] { return provider->Reads == 1 && !provider->Reading; }));
    EXPECT_FALSE(controller.SessionList().has_value()) << "nothing arrives until Update";

    ASSERT_TRUE(PumpUntil(controller, [&] { return FirstListedId(controller) == "local-2"; }));
    for (int frame = 0; frame < 20; ++frame)
    {
        controller.Update();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(provider->Reads.load(), 2) << "two requests during one read read once more, not twice";
}

// A list read for a connection the user has since left never shows for the new one.
TEST(AiAssistantPanelTests, ASessionListReadForAConnectionLeftMeanwhileIsDropped)
{
    auto left = std::make_shared<ListingProvider>("left");
    auto chosen = std::make_shared<ListingProvider>("chosen");
    left->Hold = true;
    AgentConversationController controller(ListingLookup({left, chosen}));
    controller.SetProvider("left");

    controller.RequestSessionList();
    ASSERT_TRUE(WaitFor([&] { return left->Reading.load(); }));
    controller.SetProvider("chosen");
    controller.RequestSessionList();
    left->Hold = false;
    ASSERT_TRUE(PumpUntil(controller, [&] { return controller.SessionList().has_value(); }));
    EXPECT_EQ(FirstListedId(controller), "chosen-1");
    EXPECT_EQ(left->Reads.load(), 1);
}

// Closing the panel mid-read cancels the read and returns without waiting for its thread
// (see DestroyingTheControllerWithATurnRunningDoesNotWaitForIt); the read ends on its own.
TEST(AiAssistantPanelTests, DestroyingTheControllerMidReadCancelsTheReadWithoutWaitingForIt)
{
    auto provider = std::make_shared<ListingProvider>("local");
    provider->Hold = true;
    auto controller = std::make_unique<AgentConversationController>(ListingLookup({provider}));
    controller->SetProvider("local");
    controller->RequestSessionList();
    ASSERT_TRUE(WaitFor([&] { return provider->Reading.load(); }));

    const auto start = std::chrono::steady_clock::now();
    controller.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_TRUE(WaitFor([&] { return provider->SawCancel.load() && !provider->Reading.load(); }))
        << "the read saw the cancel and ended";
}

// The Session control's rows wait for the read; then New session, Resume last (with
// the stored session's prompt) and the sessions newest first. A query searches the
// first prompts.
TEST(AiAssistantPanelTests, TheSessionListOffersNewAndResumeLastAndSearchesFirstPrompts)
{
    AgentSessionSearchProvider search("id-older");
    std::vector<SearchResultItem> rows;
    bool complete = false;
    const ISearchProvider::ResultSink sink = [&](std::vector<SearchResultItem> batch, bool isComplete)
    {
        rows.insert(rows.end(), batch.begin(), batch.end());
        complete = isComplete;
    };
    search.BeginSearch("", sink);
    EXPECT_TRUE(rows.empty()) << "nothing before the read ends";

    const auto now = std::chrono::system_clock::now();
    CliSessionList list;
    list.Sessions = {{"id-newer", "Fix the water shader", now, 3}, {"id-older", "Why does the terrain flicker", now - 1h, 1}};
    search.SetSessions(list);
    ASSERT_TRUE(complete);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].Label, "New session");
    EXPECT_EQ(rows[1].Label, "Resume last");
    EXPECT_NE(rows[1].Detail.find(" \xC2\xB7 1 turn \xC2\xB7 Why does the terrain flicker"), std::string::npos)
        << "date and count first, the prompt last where the ellipsis cuts: " << rows[1].Detail;
    EXPECT_NE(rows[3].Detail.find(" \xC2\xB7 1 turn"), std::string::npos) << "the word the Continued row counts";
    EXPECT_EQ(rows[2].Label, "Fix the water shader");
    EXPECT_EQ(rows[3].Label, "Why does the terrain flicker");

    rows.clear();
    search.CancelSearch();
    search.BeginSearch("WHY does", sink);
    ASSERT_EQ(rows.size(), 1u);
    const auto* choice = std::any_cast<AgentSessionChoice>(&rows[0].UserData);
    ASSERT_NE(choice, nullptr);
    EXPECT_EQ(choice->Choice, AgentSessionChoice::Kind::Session);
    EXPECT_EQ(choice->Session.Id, "id-older");

    // The first row of the continued conversation.
    EXPECT_EQ(ContinuedSessionText(choice->Session).find("Continued session from "), 0u);
    EXPECT_NE(ContinuedSessionText(choice->Session).find(" \xC2\xB7 1 earlier turn not shown"), std::string::npos);
    CliSessionSummary unlisted;
    unlisted.Id = "id-gone";
    EXPECT_EQ(ContinuedSessionText(unlisted), "Continued session \xC2\xB7 earlier turns not shown");
}

TEST(AiAssistantPanelTests, SessionFooterCountsSessionsWithoutTheNewAndResumeActions)
{
    for (const std::string lastSession : {std::string{}, std::string{"id-older"}})
    {
        SCOPED_TRACE(lastSession);
        AgentSessionSearchProvider search(lastSession);
        SearchDialog dialog;
        dialog.SetProvider(&search);
        dialog.Show();
        CliSessionList list;
        list.Sessions = {{"id-newer", "Water shader", {}, 1}, {"id-older", "Terrain", {}, 1},
                         {"id-harbor", "Harbor", {}, 1}, {"id-graph", "Render graph", {}, 1}};
        search.SetSessions(std::move(list));
        const auto* footer = dynamic_cast<Label*>(FindWithClass(dialog, "search-dialog-status-text"));
        ASSERT_NE(footer, nullptr);
        EXPECT_EQ(footer->GetText(), "4 sessions");

        dialog.Close();
        list.Sessions = {{"id-newer", "Water shader", {}, 1}};
        search.SetSessions(std::move(list));
        dialog.Show();
        EXPECT_EQ(footer->GetText(), "1 session");

        dialog.Close();
        search.SetSessions({});
        dialog.Show();
        EXPECT_EQ(footer->GetText(), "0 sessions");
    }
}
} // namespace GameEngine
