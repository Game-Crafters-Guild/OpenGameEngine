#pragma once

#include "Providers/CliSessionList.h"

#include "UI/Controls/SearchDialog.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
/// What a row of the session list does when chosen; the row's SearchResultItem::UserData.
struct AgentSessionChoice
{
    enum class Kind : uint8_t
    {
        /// Start an empty conversation in a new session.
        NewSession,
        /// Continue the session stored as the connection's last one in the project.
        ResumeLast,
        /// Continue a session from the list.
        Session,
    };
    Kind Choice = Kind::NewSession;
    /// The session to continue; for ResumeLast, the list's summary of the stored id,
    /// or just the id (zero Time) when the list does not hold it.
    CliSessionSummary Session;
};

/// The Session control's rows: "New session", "Resume last" (the stored session's
/// prompt muted beneath) when a last session is stored, then the connection's
/// sessions newest first. A query keeps the sessions
/// whose first prompt contains it (ignoring ASCII case) and drops the two fixed rows.
/// The sessions arrive once their read ends (SetSessions); a search begun before
/// then delivers its rows, the fixed ones included, when they arrive.
class AgentSessionSearchProvider final : public ISearchProvider
{
public:
    /// `lastSessionId` is the id offered as Resume last; empty offers none.
    explicit AgentSessionSearchProvider(std::string lastSessionId);

    /// The connection's sessions; delivered to a search that waits for them.
    void SetSessions(CliSessionList list);
    /// The sessions are known (SetSessions ran).
    bool HasSessions() const { return m_Sessions.has_value(); }

    void BeginSearch(const std::string& query, ResultSink sink) override;
    void CancelSearch() override;
    std::string FormatResultSummary(std::span<const SearchResultItem> results) const override;
    std::string GetPlaceholderText() const override { return "Search sessions by their first prompt"; }

private:
    std::vector<SearchResultItem> FixedRows() const;
    std::vector<SearchResultItem> SessionRows(const std::string& query) const;

    std::string m_LastSessionId;
    std::optional<CliSessionList> m_Sessions;
    /// A search waiting for the sessions.
    std::string m_PendingQuery;
    ResultSink m_PendingSink;
};
} // namespace GameEngine
