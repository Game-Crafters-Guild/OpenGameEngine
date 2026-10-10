#include "AiAssistantLoginStatusRow.h"

#include "AgentSessionState.h"
#include "AiAssistantSettings.h"
#include "Providers/CancelToken.h"
#include "Providers/CliSessionProvider.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIManager.h"

#include "Logger/Logger.h"

#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace GameEngine
{
// Written by the check thread, read by PollCheck(). The row's destructor cancels the
// check and joins the thread, so a CLI that does not answer never outlives the page.
struct AiAssistantLoginStatusRow::PendingCheck
{
    CancelToken Cancel;
    std::mutex Mutex;
    bool Done = false;
    std::string Text;
};

AiAssistantLoginStatusRow::AiAssistantLoginStatusRow()
{
    AddClass("settings-row");

    auto label = std::make_unique<Label>();
    label->SetText("Login status");
    label->SetTooltip("Asks the chosen local session's CLI whether it is logged in.");
    label->AddClass("settings-row-label");
    AddChild(std::move(label));

    auto status = std::make_unique<Label>();
    status->SetText("Not checked");
    m_Status = status.get();
    AddChild(std::move(status));

    auto check = std::make_unique<Button>();
    check->SetText("Check");
    check->AddClass("small");
    check->AddClass("secondary");
    check->SetOnClick([this](UIEvent&) { StartCheck(); });
    AddChild(std::move(check));
}

AiAssistantLoginStatusRow::~AiAssistantLoginStatusRow()
{
    if (m_Pending)
        m_Pending->Cancel.Cancel();
    if (m_CheckThread.joinable())
        m_CheckThread.join();
    StopPolling();
}

void AiAssistantLoginStatusRow::StartCheck()
{
    if (m_Pending)
        return;
    // The previous check has ended (PollCheck cleared m_Pending); reap its thread.
    if (m_CheckThread.joinable())
        m_CheckThread.join();

    const std::string providerId = AiAssistantSettings::Provider();
    std::shared_ptr<CliSessionProvider> provider = AgentSessionState::Get().SessionProvider(providerId);
    if (!provider)
    {
        m_Status->SetText("Claude (API) has no login: it reads its key from the environment when a turn starts.");
        return;
    }

    UIManager* manager = GetOwnerManager();
    if (!manager)
        return;

    m_Status->SetText("Checking...");
    m_Pending = std::make_shared<PendingCheck>();
    // A thread of its own, never a JobSystem job: the check waits on a child process
    // for up to its time limit. CheckLogin returns once the destructor cancels it.
    m_CheckThread = std::thread(RunCheck, m_Pending, std::move(provider));
    m_RefreshManager = UIManagerRef(manager);
    m_RefreshToken = manager->RegisterPeriodicRefresh([this] { PollCheck(); });
}

// An exception that escapes a thread function terminates the process, so whatever the
// check throws becomes the row's text instead.
void AiAssistantLoginStatusRow::RunCheck(std::shared_ptr<PendingCheck> pending,
                                         std::shared_ptr<CliSessionProvider> provider)
{
    std::string text;
    try
    {
        text = provider->CheckLogin(pending->Cancel).Text;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AI Assistant: the login check for '{}' threw: {}", provider->Id(), e.what());
        text = "Could not check the login: " + std::string(e.what()) + ". Try again; the editor log has the details.";
    }
    catch (...)
    {
        Logger::Log::Error("AI Assistant: the login check for '{}' threw a non-standard exception", provider->Id());
        text = "Could not check the login: an unknown error. Try again; the editor log has the details.";
    }
    std::lock_guard lock(pending->Mutex);
    pending->Text = std::move(text);
    pending->Done = true;
}

void AiAssistantLoginStatusRow::PollCheck()
{
    if (!m_Pending)
        return;
    std::string text;
    {
        std::lock_guard lock(m_Pending->Mutex);
        if (!m_Pending->Done)
            return;
        text = std::move(m_Pending->Text);
    }
    m_Status->SetText(text);
    m_Pending.reset();
    StopPolling();
}

void AiAssistantLoginStatusRow::StopPolling()
{
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
    m_RefreshManager = UIManagerRef();
    m_RefreshToken = 0;
}
} // namespace GameEngine
