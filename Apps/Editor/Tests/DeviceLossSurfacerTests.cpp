// The terminal device-loss prompt must never block the main thread.
//
// The editor runs one thread: frame loop, input, autosave, scripting and the
// debug-server IPC drain (EditorApplication::Update -> FlushPendingRequests) all
// execute on it. A modal dialog held on that thread stops every one of them, which
// presents as a process that is alive, still accepting IPC connections, and never
// answering — the signature of a hang, produced by a dialog waiting for a click that
// an unattended run (harness, CI, unwatched machine) will never supply.

#include "DeviceLossSurfacer.h"

#include "Rendering/Core/Device.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using GameEngine::Editor::DeviceLossSurfacer;
using GameEngine::Rendering::DeviceHealth;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

namespace
{

// A prompt that stands until the test dismisses it, standing in for a real modal
// nobody clicks. Shared by shared_ptr because the surfacer presents on a detached
// thread that may outlive the test body.
struct StandingPrompt
{
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Dismissed = false;
    std::atomic<int> Presentations{0};
    int Answer = 2;

    int Present()
    {
        Presentations.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lock(Mutex);
        // Bounded so a regression fails the assertions instead of hanging the suite.
        Cv.wait_for(lock, std::chrono::seconds(5), [this] { return Dismissed; });
        return Answer;
    }

    void Dismiss(int answer)
    {
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Answer = answer;
            Dismissed = true;
        }
        Cv.notify_all();
    }
};

DeviceLossSurfacer::ChoicePresenter PresenterFor(std::shared_ptr<StandingPrompt> prompt)
{
    return [prompt](const std::string&, const std::string&, const std::vector<std::string>&,
                    int) { return prompt->Present(); };
}

// Waits for the detached presenter thread to have actually entered the dialog, so
// "shown once" assertions do not race thread startup.
bool WaitForPresentation(const std::shared_ptr<StandingPrompt>& prompt, int expected)
{
    const auto deadline = Clock::now() + Ms(2000);
    while (Clock::now() < deadline)
    {
        if (prompt->Presentations.load(std::memory_order_relaxed) >= expected)
            return true;
        std::this_thread::sleep_for(Ms(1));
    }
    return false;
}

// The threshold a single Poll() must beat. A frame-loop tick that spends longer than
// this in health surfacing has stopped being a poll.
constexpr Ms kPollBudget{250};

} // namespace

// RED before the fix: Poll() calls the modal inline, so it does not return until the
// prompt is dismissed — here, the presenter's 5 s bound.
TEST(DeviceLossSurfacer, PollReturnsWhileTerminalPromptStands)
{
    auto prompt = std::make_shared<StandingPrompt>();
    DeviceLossSurfacer surfacer(PresenterFor(prompt));
    const DeviceLossSurfacer::Actions actions{};

    const auto start = Clock::now();
    surfacer.Poll(DeviceHealth::Failed, nullptr, actions);
    const auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - start);

    EXPECT_LT(elapsed.count(), kPollBudget.count())
        << "Poll() blocked for " << elapsed.count()
        << " ms on the terminal prompt; the main thread must keep running";

    prompt->Dismiss(2);
}

// The reported symptom, directly: with the prompt unanswered the frame loop must keep
// completing ticks. Every tick that returns here is a tick that would have drained the
// IPC queue in the editor.
TEST(DeviceLossSurfacer, MainLoopKeepsTickingWhileTerminalPromptStands)
{
    auto prompt = std::make_shared<StandingPrompt>();
    DeviceLossSurfacer surfacer(PresenterFor(prompt));
    const DeviceLossSurfacer::Actions actions{};

    constexpr int kTicks = 200;
    int completed = 0;
    Ms worst{0};

    const auto start = Clock::now();
    for (int i = 0; i < kTicks; ++i)
    {
        const auto tickStart = Clock::now();
        surfacer.Poll(DeviceHealth::Failed, nullptr, actions);
        worst = std::max(worst, std::chrono::duration_cast<Ms>(Clock::now() - tickStart));
        ++completed;
    }
    const auto total = std::chrono::duration_cast<Ms>(Clock::now() - start);

    EXPECT_EQ(completed, kTicks);
    EXPECT_LT(worst.count(), kPollBudget.count()) << "slowest tick was " << worst.count() << " ms";
    EXPECT_LT(total.count(), 2000) << kTicks << " ticks took " << total.count() << " ms";

    prompt->Dismiss(2);
}

// One prompt per Failed episode, however many ticks pass while it stands.
TEST(DeviceLossSurfacer, TerminalPromptIsPresentedOncePerEpisode)
{
    auto prompt = std::make_shared<StandingPrompt>();
    DeviceLossSurfacer surfacer(PresenterFor(prompt));
    const DeviceLossSurfacer::Actions actions{};

    for (int i = 0; i < 50; ++i)
        surfacer.Poll(DeviceHealth::Failed, nullptr, actions);

    ASSERT_TRUE(WaitForPresentation(prompt, 1));
    for (int i = 0; i < 50; ++i)
        surfacer.Poll(DeviceHealth::Failed, nullptr, actions);

    EXPECT_EQ(prompt->Presentations.load(), 1);
    prompt->Dismiss(2);
}

// The answer still has to arrive, and still has to run on the polling (main) thread.
TEST(DeviceLossSurfacer, AnswerIsAppliedOnALaterPollOnThePollingThread)
{
    auto prompt = std::make_shared<StandingPrompt>();
    DeviceLossSurfacer surfacer(PresenterFor(prompt));

    const auto pollingThread = std::this_thread::get_id();
    bool savedAll = false;
    bool exited = false;
    std::thread::id savedOn{};

    DeviceLossSurfacer::Actions actions;
    actions.SaveAll = [&] {
        savedAll = true;
        savedOn = std::this_thread::get_id();
    };
    actions.ExitApp = [&] { exited = true; };

    surfacer.Poll(DeviceHealth::Failed, nullptr, actions);
    ASSERT_TRUE(WaitForPresentation(prompt, 1));
    EXPECT_FALSE(savedAll) << "the answer was applied before the prompt was answered";

    prompt->Dismiss(1); // "Save All & Close"

    const auto deadline = Clock::now() + Ms(2000);
    while (!exited && Clock::now() < deadline)
    {
        surfacer.Poll(DeviceHealth::Failed, nullptr, actions);
        std::this_thread::sleep_for(Ms(1));
    }

    EXPECT_TRUE(savedAll);
    EXPECT_TRUE(exited);
    EXPECT_EQ(savedOn, pollingThread);
}

// Non-terminal health must not prompt at all — the toast path owns those.
TEST(DeviceLossSurfacer, NonTerminalHealthNeverPrompts)
{
    auto prompt = std::make_shared<StandingPrompt>();
    DeviceLossSurfacer surfacer(PresenterFor(prompt));
    const DeviceLossSurfacer::Actions actions{};

    for (const DeviceHealth health : {DeviceHealth::Healthy, DeviceHealth::Hung,
                                      DeviceHealth::Lost, DeviceHealth::Rebuilding,
                                      DeviceHealth::AwaitingReprovision})
    {
        const auto start = Clock::now();
        surfacer.Poll(health, nullptr, actions);
        EXPECT_LT(std::chrono::duration_cast<Ms>(Clock::now() - start).count(),
                  kPollBudget.count());
    }

    EXPECT_EQ(prompt->Presentations.load(), 0);
}
