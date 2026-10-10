#pragma once

#include "AssistantMode.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class CliSessionProvider;
class IAgentProvider;

/// The AI Assistant's connections for the life of the editor process, kept while
/// the panel is closed: one provider per connection (so a CLI's version and login
/// checks run once) and the session id each local session continues, so a closed
/// and reopened panel resumes its conversation; each connection's last session is
/// also stored per project, so a later editor run can offer it. Thread-safe.
class AgentSessionState
{
public:
    static AgentSessionState& Get();

    /// The provider for `providerId` as the settings configure it now: the same
    /// instance while its executable and the project are unchanged, a new one
    /// (forgetting the session) when either changes. Nullptr for an unknown id.
    std::shared_ptr<IAgentProvider> Provider(std::string_view providerId);
    /// Provider() for a local session; nullptr for any other connection.
    std::shared_ptr<CliSessionProvider> SessionProvider(std::string_view providerId);

    /// The session `providerId` continues; empty starts a new one.
    std::string SessionId(std::string_view providerId) const;
    /// Records the session a turn of `provider` reported, and stores it as the
    /// connection's last session in the project (AiAssistantSettings::SetLastSession);
    /// an empty id forgets the session in both. Ignored when `provider` is no longer
    /// its connection's (the executable or the project changed while the turn ran),
    /// so the new connection never resumes another one's session.
    void SetSessionId(const IAgentProvider& provider, std::string sessionId);
    /// Makes `providerId`'s next turn continue `sessionId`, a session the user chose,
    /// and stores it as SetSessionId() does. Ignored for an unknown id.
    void ResumeSession(std::string_view providerId, std::string sessionId);
    /// Forgets every session, so each connection's next turn starts a new one.
    void ForgetSessions();

    /// The mode the session `sessionId` of `providerId` last ran in, read from the
    /// connection's project (AiAssistantSettings::SessionMode); nullopt when unknown.
    std::optional<AssistantMode> SessionMode(std::string_view providerId, std::string_view sessionId) const;
    /// Stores `mode` as the mode of the session `providerId` continues now, in the
    /// connection's project; does nothing while the connection has no session.
    void SetSessionMode(std::string_view providerId, AssistantMode mode);

private:
    struct Connection
    {
        std::string ProviderId;
        std::string Executable;
        std::filesystem::path Directory;
        std::shared_ptr<IAgentProvider> Provider;
        std::string SessionId;
    };

    Connection& FindOrAdd(std::string_view providerId);

    mutable std::mutex m_Mutex;
    std::vector<Connection> m_Connections;
};
} // namespace GameEngine
