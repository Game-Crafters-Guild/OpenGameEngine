#include "AgentSessionState.h"

#include "AiAssistantSettings.h"
#include "Providers/ClaudeApiProvider.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"

#include "Editor/EditorPaths.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
AgentSessionState& AgentSessionState::Get()
{
    static AgentSessionState state;
    return state;
}

std::shared_ptr<IAgentProvider> AgentSessionState::Provider(std::string_view providerId)
{
    std::string executable;
    if (providerId == ClaudeSessionProvider::kId)
        executable = AiAssistantSettings::ClaudeExecutable();
    else if (providerId == CodexSessionProvider::kId)
        executable = AiAssistantSettings::CodexExecutable();
    else if (providerId != ClaudeApiProvider::kId)
        return nullptr;
    const std::filesystem::path directory = Editor::GetCurrentEditorProjectPaths().projectRoot;

    std::lock_guard lock(m_Mutex);
    Connection& connection = FindOrAdd(providerId);
    if (connection.Provider && connection.Executable == executable && connection.Directory == directory)
        return connection.Provider;

    // A session belongs to one executable's store and one project directory.
    connection.Executable = executable;
    connection.Directory = directory;
    connection.SessionId.clear();
    if (providerId == ClaudeSessionProvider::kId)
        connection.Provider = std::make_shared<ClaudeSessionProvider>(executable, directory);
    else if (providerId == CodexSessionProvider::kId)
        connection.Provider = std::make_shared<CodexSessionProvider>(executable, directory);
    else
        connection.Provider = std::make_shared<ClaudeApiProvider>();
    return connection.Provider;
}

std::shared_ptr<CliSessionProvider> AgentSessionState::SessionProvider(std::string_view providerId)
{
    if (providerId != ClaudeSessionProvider::kId && providerId != CodexSessionProvider::kId)
        return nullptr;
    return std::static_pointer_cast<CliSessionProvider>(Provider(providerId));
}

std::string AgentSessionState::SessionId(std::string_view providerId) const
{
    std::lock_guard lock(m_Mutex);
    const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                 [providerId](const Connection& c) { return c.ProviderId == providerId; });
    return it == m_Connections.end() ? std::string() : it->SessionId;
}

void AgentSessionState::SetSessionId(const IAgentProvider& provider, std::string sessionId)
{
    std::filesystem::path directory;
    {
        std::lock_guard lock(m_Mutex);
        const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                     [&provider](const Connection& c) { return c.Provider.get() == &provider; });
        if (it == m_Connections.end())
            return;
        it->SessionId = sessionId;
        directory = it->Directory;
    }
    // Outside the lock: the store reads and writes a file.
    AiAssistantSettings::SetLastSession(provider.Id(), directory, sessionId);
}

void AgentSessionState::ResumeSession(std::string_view providerId, std::string sessionId)
{
    if (const std::shared_ptr<IAgentProvider> provider = Provider(providerId))
        SetSessionId(*provider, std::move(sessionId));
}

void AgentSessionState::ForgetSessions()
{
    std::lock_guard lock(m_Mutex);
    for (Connection& connection : m_Connections)
        connection.SessionId.clear();
}

std::optional<AssistantMode> AgentSessionState::SessionMode(std::string_view providerId,
                                                            std::string_view sessionId) const
{
    std::filesystem::path directory = Editor::GetCurrentEditorProjectPaths().projectRoot;
    {
        std::lock_guard lock(m_Mutex);
        const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                     [providerId](const Connection& c) { return c.ProviderId == providerId; });
        if (it != m_Connections.end() && it->Provider)
            directory = it->Directory;
    }
    return AiAssistantSettings::SessionMode(providerId, directory, sessionId);
}

void AgentSessionState::SetSessionMode(std::string_view providerId, AssistantMode mode)
{
    std::filesystem::path directory;
    std::string sessionId;
    {
        std::lock_guard lock(m_Mutex);
        const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                     [providerId](const Connection& c) { return c.ProviderId == providerId; });
        if (it == m_Connections.end() || it->SessionId.empty())
            return;
        directory = it->Directory;
        sessionId = it->SessionId;
    }
    // Outside the lock: the store reads and writes a file.
    AiAssistantSettings::SetSessionMode(providerId, directory, sessionId, mode);
}

AgentSessionState::Connection& AgentSessionState::FindOrAdd(std::string_view providerId)
{
    const auto it = std::find_if(m_Connections.begin(), m_Connections.end(),
                                 [providerId](const Connection& c) { return c.ProviderId == providerId; });
    if (it != m_Connections.end())
        return *it;
    Connection& added = m_Connections.emplace_back();
    added.ProviderId = std::string(providerId);
    return added;
}
} // namespace GameEngine
