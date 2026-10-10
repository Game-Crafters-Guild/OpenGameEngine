#pragma once

#include <string>
#include <string_view>

namespace GameEngine::Editor
{

// A capture answers, or names why it cannot, inside this window.
inline constexpr double kScreenshotWaitDeadlineSeconds = 5.0;

// A refusal that repeats over this many render-phase attempts is reported
// instead of waited out: a scene pane with no on-screen rect does not heal on
// its own, and making the caller wait the full deadline for that answer helps
// nobody.
inline constexpr int kScreenshotMaxRefusedAttempts = 8;

// The debug server's own deferred budget for a capture, counted in polls. The
// wait below is what ends a request and names the reason; this is only a
// backstop against a poll that never decides, so it has to outlast the deadline
// at the fastest tick rate the editor reaches — a tick that skips the render
// phase costs about a millisecond.
inline constexpr int kScreenshotPollBackstop = 30000;

// How long a deferred screenshot keeps asking the render graph for a frame, and
// what the caller is told when it stops. One instance per capture request.
//
// The poll driving it runs once per main-loop tick, and a tick does not have to
// render: a minimized window, a swapchain image that never frees, or a frame
// that declares no UI all skip the render phase — and the capture's render-phase
// attempt with it. A budget counted in polls therefore ends the request while
// the editor simply had not drawn yet, which is why this wait ends only on its
// own wall-clock deadline, on a blocked window, or on the render phase refusing
// attempt after attempt.
class ScreenshotCaptureWait
{
  public:
    enum class Outcome
    {
        KeepWaiting,        // re-arm the render-phase attempt and poll again
        ServeWindowCapture, // the caller opted into the OS window substitute
        Fail,               // no substitute allowed — report GiveUpReason()
    };

    explicit ScreenshotCaptureWait(bool allowWindowCapture);

    // The render phase ran and declined to declare the readback. Retried: a
    // scene view catching up on extraction declines for a frame or two.
    void RecordRefusal(std::string_view reason);

    // A condition no later frame clears — a minimized window has no swapchain
    // image to render into. Ends the wait on the next step.
    void RecordBlocked(std::string_view reason);

    // The readback is declared; from here the wait is on the GPU copy.
    void RecordReadbackDeclared();

    // One poll. `elapsedSeconds` is measured from the request arriving.
    Outcome Step(double elapsedSeconds);

    // Why the wait ended, in the words the caller gets. Empty until it ends.
    const std::string& GiveUpReason() const { return m_GiveUpReason; }

  private:
    Outcome GiveUp(std::string reason);

    bool m_AllowWindowCapture = false;
    bool m_ReadbackDeclared = false;
    bool m_Blocked = false;
    // Every render-phase refusal, whatever its reason: how many frames the
    // editor actually rendered while this request waited.
    int m_RefusedAttempts = 0;
    // Consecutive refusals carrying m_LastRefusal — what "unchanged over N
    // rendered frames" counts, and what the give-up rule measures.
    int m_RepeatedRefusals = 0;
    std::string m_LastRefusal;
    std::string m_GiveUpReason;
};

// The caller-facing error for a capture that could not be served, built from the
// render phase's own `reason`. `offerWindowCapture` adds the sentence naming the
// opt-in flag, which is worth saying only to a caller that has not already
// passed it and only where an OS window capture can stand in at all.
std::string ScreenshotFailureMessage(std::string_view reason, bool offerWindowCapture);

} // namespace GameEngine::Editor
