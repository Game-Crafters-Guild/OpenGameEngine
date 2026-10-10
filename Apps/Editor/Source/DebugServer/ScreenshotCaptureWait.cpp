#include "DebugServer/ScreenshotCaptureWait.h"

#include <format>
#include <utility>

namespace GameEngine::Editor
{

ScreenshotCaptureWait::ScreenshotCaptureWait(bool allowWindowCapture)
    : m_AllowWindowCapture(allowWindowCapture)
{
}

void ScreenshotCaptureWait::RecordRefusal(std::string_view reason)
{
    // A readback declared on an earlier frame and then lost (its frame was
    // abandoned) leaves the wait back where it started, so the refusal count is
    // what governs again.
    m_ReadbackDeclared = false;
    ++m_RefusedAttempts;

    // The give-up rule reports one refusal that keeps repeating, so a different
    // reason starts that run over rather than inheriting the previous one's
    // count.
    if (m_LastRefusal == reason)
    {
        ++m_RepeatedRefusals;
        return;
    }
    m_LastRefusal.assign(reason);
    m_RepeatedRefusals = 1;
}

void ScreenshotCaptureWait::RecordBlocked(std::string_view reason)
{
    m_ReadbackDeclared = false;
    m_Blocked = true;
    m_LastRefusal.assign(reason);
}

void ScreenshotCaptureWait::RecordReadbackDeclared()
{
    m_ReadbackDeclared = true;
}

ScreenshotCaptureWait::Outcome ScreenshotCaptureWait::Step(double elapsedSeconds)
{
    if (m_Blocked)
        return GiveUp(m_LastRefusal);

    if (!m_ReadbackDeclared && m_RepeatedRefusals >= kScreenshotMaxRefusedAttempts)
        return GiveUp(m_LastRefusal + " (unchanged over " + std::to_string(m_RepeatedRefusals) +
                      " rendered frames)");

    if (elapsedSeconds < kScreenshotWaitDeadlineSeconds)
        return Outcome::KeepWaiting;

    const std::string elapsed = std::format("{:.1f}s", elapsedSeconds);
    if (m_ReadbackDeclared)
        return GiveUp("the GPU readback did not complete within " + elapsed);
    if (m_RefusedAttempts > 1)
        return GiveUp(m_LastRefusal + " (still unresolved after " + elapsed + ", over " +
                      std::to_string(m_RefusedAttempts) + " rendered frames)");
    // One rendered frame in the whole deadline: the refusal describes that one
    // frame, and the frame loop itself is what kept the request waiting.
    if (m_RefusedAttempts == 1)
        return GiveUp(m_LastRefusal + " (still unresolved after " + elapsed +
                      ", in which the editor rendered one frame)");
    return GiveUp("the editor rendered no frame within " + elapsed +
                  " — its window is minimized, hidden, or its swapchain is not presenting");
}

ScreenshotCaptureWait::Outcome ScreenshotCaptureWait::GiveUp(std::string reason)
{
    m_GiveUpReason = std::move(reason);
    return m_AllowWindowCapture ? Outcome::ServeWindowCapture : Outcome::Fail;
}

std::string ScreenshotFailureMessage(std::string_view reason, bool offerWindowCapture)
{
    std::string message = "Screenshot failed: ";
    message.append(reason);
    message += '.';
    if (offerWindowCapture)
        message += " Pass allowWindowCapture=true to accept an OS window capture of the editor "
                   "instead of the rendered frame.";
    return message;
}

} // namespace GameEngine::Editor
