#include "AgentSessionSearchProvider.h"

#include "AgentStatusMessages.h"

#include "Types/StringUtils.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr SearchItemId kNewSessionRow = 1;
constexpr SearchItemId kResumeLastRow = 2;
constexpr SearchItemId kFirstSessionRow = 3;

// "2026-10-07 14:05 · 3 turns": the count names what the Continued session row counts.
std::string SessionDetail(const CliSessionSummary& session)
{
    return SessionTimeText(session.Time) + " \xC2\xB7 " + std::to_string(session.PromptCount) +
           (session.PromptCount == 1 ? " turn" : " turns");
}

// A session without a prompt the reader recognized is shown by its id.
std::string SessionLabel(const CliSessionSummary& session)
{
    return session.FirstPrompt.empty() ? session.Id : session.FirstPrompt;
}
} // namespace

AgentSessionSearchProvider::AgentSessionSearchProvider(std::string lastSessionId)
    : m_LastSessionId(std::move(lastSessionId))
{
}

void AgentSessionSearchProvider::SetSessions(CliSessionList list)
{
    m_Sessions = std::move(list);
    if (!m_PendingSink)
        return;
    ResultSink sink = std::exchange(m_PendingSink, nullptr);
    std::vector<SearchResultItem> rows = m_PendingQuery.empty() ? FixedRows() : std::vector<SearchResultItem>{};
    std::vector<SearchResultItem> sessions = SessionRows(m_PendingQuery);
    rows.insert(rows.end(), std::make_move_iterator(sessions.begin()), std::make_move_iterator(sessions.end()));
    sink(std::move(rows), true);
}

void AgentSessionSearchProvider::BeginSearch(const std::string& query, ResultSink sink)
{
    const std::string trimmed = TrimWhitespace(query);
    if (!m_Sessions)
    {
        // Every row waits for the read, so Resume last shows its session's prompt.
        m_PendingQuery = trimmed;
        m_PendingSink = std::move(sink);
        return;
    }
    std::vector<SearchResultItem> rows = trimmed.empty() ? FixedRows() : std::vector<SearchResultItem>{};
    std::vector<SearchResultItem> sessions = SessionRows(trimmed);
    rows.insert(rows.end(), std::make_move_iterator(sessions.begin()), std::make_move_iterator(sessions.end()));
    sink(std::move(rows), true);
}

void AgentSessionSearchProvider::CancelSearch()
{
    m_PendingSink = nullptr;
    m_PendingQuery.clear();
}

std::string AgentSessionSearchProvider::FormatResultSummary(std::span<const SearchResultItem> results) const
{
    const auto count = std::count_if(results.begin(), results.end(), [](const SearchResultItem& row)
    {
        const auto* choice = std::any_cast<AgentSessionChoice>(&row.UserData);
        return choice && choice->Choice == AgentSessionChoice::Kind::Session;
    });
    return std::to_string(count) + (count == 1 ? " session" : " sessions");
}

std::vector<SearchResultItem> AgentSessionSearchProvider::FixedRows() const
{
    std::vector<SearchResultItem> rows;
    SearchResultItem newSession;
    newSession.Id = kNewSessionRow;
    newSession.Label = "New session";
    newSession.Detail = "Start an empty conversation";
    newSession.UserData = AgentSessionChoice{};
    rows.push_back(std::move(newSession));

    if (m_LastSessionId.empty())
        return rows;
    AgentSessionChoice resume{AgentSessionChoice::Kind::ResumeLast, {}};
    resume.Session.Id = m_LastSessionId;
    if (m_Sessions)
    {
        const auto& sessions = m_Sessions->Sessions;
        const auto listed = std::find_if(sessions.begin(), sessions.end(),
                                         [this](const CliSessionSummary& s) { return s.Id == m_LastSessionId; });
        if (listed != sessions.end())
            resume.Session = *listed;
    }
    SearchResultItem resumeLast;
    resumeLast.Id = kResumeLastRow;
    // A shortcut, not a second copy of a listed session: its title, and the session it
    // continues in the muted line beneath, date and count first so a long prompt is
    // what the row's ellipsis cuts.
    resumeLast.Label = "Resume last";
    resumeLast.Detail = resume.Session.Time == std::chrono::system_clock::time_point{}
                            ? std::string("The session this project used last")
                            : SessionDetail(resume.Session) + " \xC2\xB7 " + SessionLabel(resume.Session);
    resumeLast.UserData = std::move(resume);
    rows.push_back(std::move(resumeLast));
    return rows;
}

std::vector<SearchResultItem> AgentSessionSearchProvider::SessionRows(const std::string& query) const
{
    std::vector<SearchResultItem> rows;
    if (!m_Sessions)
        return rows;
    const std::string needle = ToLowerAscii(query);
    for (size_t index = 0; index < m_Sessions->Sessions.size(); ++index)
    {
        const CliSessionSummary& session = m_Sessions->Sessions[index];
        if (!needle.empty() && ToLowerAscii(session.FirstPrompt).find(needle) == std::string::npos)
            continue;
        SearchResultItem row;
        row.Id = kFirstSessionRow + index;
        row.Label = SessionLabel(session);
        row.Detail = SessionDetail(session);
        row.UserData = AgentSessionChoice{AgentSessionChoice::Kind::Session, session};
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace GameEngine
