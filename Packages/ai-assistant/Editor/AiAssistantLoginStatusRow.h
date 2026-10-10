#pragma once

#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"

#include <cstdint>
#include <memory>
#include <thread>

namespace GameEngine
{
class CliSessionProvider;
class Label;
class UIManager;

/// The settings page's Login status row: a Check button that asks the chosen local
/// session's CLI for its login (seconds, so on a short-lived thread of its own) and
/// shows the answer in the row. The answer names the login method only.
class AiAssistantLoginStatusRow final : public UIElement
{
public:
    AiAssistantLoginStatusRow();
    /// Cancels a check in flight and waits for its thread.
    ~AiAssistantLoginStatusRow() override;

private:
    struct PendingCheck;

    void StartCheck();
    static void RunCheck(std::shared_ptr<PendingCheck> pending, std::shared_ptr<CliSessionProvider> provider);
    void PollCheck();
    void StopPolling();

    Label* m_Status = nullptr;
    std::shared_ptr<PendingCheck> m_Pending;
    std::thread m_CheckThread;
    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
};
} // namespace GameEngine
