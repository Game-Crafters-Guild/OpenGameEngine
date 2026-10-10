#include "Panels/LogView.h"

#include "Platform/SystemMetrics.h"
#include "Types/StringUtils.h"

#include <chrono>
#include <cctype>
#include <deque>
#include <iomanip>
#include <sstream>
#include <algorithm>

#include "Logger/LogLevel.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"

namespace GameEngine {

namespace {

LogView::StoredMessage MakeStoredMessage(const Logger::LogMessage& msg)
{
    LogView::StoredMessage stored;
    stored.Level = msg.Level;
    stored.Text = msg.Message;
    stored.TimestampRaw = msg.Timestamp;
    stored.SourceFile = msg.SourceFile;
    stored.SourceLine = msg.SourceLine;
    stored.Function = msg.Function;
    stored.Backtrace = msg.Backtrace;
    return stored;
}

std::string FormatTimestamp(const Logger::TimePoint& timestamp)
{
    auto timeT = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &timeT);
#else
    localtime_r(&timeT, &tm);
#endif

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << tm.tm_hour << ':'
        << std::setw(2) << tm.tm_min << ':'
        << std::setw(2) << tm.tm_sec << '.'
        << std::setw(3) << ms.count();
    return oss.str();
}

} // namespace

// Lazy accessors: compute once, cache on the message. See header comment for rationale.
const std::string& LogView::GetDisplayText(const StoredMessage& m)
{
    if (m.DisplayTextCache.empty())
    {
        m.DisplayTextCache.reserve(m.Text.size() + 32);
        m.DisplayTextCache.assign("[");
        m.DisplayTextCache += FormatTimestamp(m.TimestampRaw);
        m.DisplayTextCache += "] [";
        m.DisplayTextCache += Logger::LogLevelToString(m.Level);
        m.DisplayTextCache += "] ";
        m.DisplayTextCache += m.Text;
    }
    return m.DisplayTextCache;
}

const std::string& LogView::GetDisplayLower(const StoredMessage& m)
{
    if (m.DisplayLowerCache.empty())
    {
        m.DisplayLowerCache = ToLowerAscii(GetDisplayText(m));
    }
    return m.DisplayLowerCache;
}

void LogView::InvalidateDisplayCaches()
{
    for (auto& m : m_Messages)
    {
        m.DisplayTextCache.clear();
        m.DisplayLowerCache.clear();
    }
}

// LogDataProvider implementation
int LogView::LogDataProvider::GetItemCount() const
{
    if (m_Owner->IsViewFiltered())
        return static_cast<int>(m_Owner->m_FilteredIndices.size());
    return static_cast<int>(m_Owner->m_Messages.size());
}

ListId LogView::LogDataProvider::GetItemId(int index) const
{
    // Use the underlying message index + 1 as a stable ID (0 is reserved for "no selection").
    size_t messageIndex = static_cast<size_t>(index);
    if (m_Owner->IsViewFiltered())
    {
        if (index < 0 || index >= static_cast<int>(m_Owner->m_FilteredIndices.size()))
            return 0;
        messageIndex = m_Owner->m_FilteredIndices[static_cast<size_t>(index)];
    }
    return static_cast<ListId>(messageIndex + 1);
}

float LogView::LogDataProvider::GetItemHeight(int /*index*/) const
{
    return m_Owner->m_RowHeight;
}

LogView::LogView()
{
    AddClass("log-view");
    SetId("LogView");

    m_DataProvider = std::make_unique<LogDataProvider>(this);
    SetupListView();
}

LogView::~LogView() = default;

void LogView::SetupListView()
{
    auto listView = std::make_unique<ListView>();
    m_ListView = listView.get();
    m_ListView->SetId("LogListView");
    m_ListView->AddClass("log-list");
    m_ListView->SetFocusable(true);
    m_ListView->SetDataProvider(m_DataProvider.get());

    m_SelectionModel = std::make_unique<UI::Interaction::SelectionModel>();
    m_ListView->SetSelectionModel(m_SelectionModel.get());

    m_SelectionModel->SetOnChanged([this]() {
        if (!m_OnMessageSelected)
            return;
        auto anchor = m_SelectionModel ? m_SelectionModel->GetAnchor() : 0;
        if (anchor == 0)
        {
            m_OnMessageSelected(0, nullptr);
            return;
        }
        const size_t idx = static_cast<size_t>(anchor - 1);
        if (idx >= m_Messages.size())
        {
            m_OnMessageSelected(0, nullptr);
            return;
        }
        m_OnMessageSelected(idx, &m_Messages[idx]);
    });

    // Set up item factory - creates Label elements for log entries
    m_ListView->SetItemFactory([](ListId /*id*/, IListDataProvider* /*provider*/) {
        auto label = std::make_unique<Label>();
        label->AddClass("log-line");
        return label;
    });

    // Set up item binder - binds data to the Label
    m_ListView->SetItemBinder([this](UIElement* element, ListId /*id*/, int index, IListDataProvider* /*provider*/) {
        auto* label = static_cast<Label*>(element);
        if (!label)
            return;

        size_t messageIndex = static_cast<size_t>(index);
        if (IsViewFiltered())
        {
            if (index < 0 || index >= static_cast<int>(m_FilteredIndices.size()))
                return;
            messageIndex = m_FilteredIndices[static_cast<size_t>(index)];
        }
        if (messageIndex >= m_Messages.size())
            return;

        const auto& stored = m_Messages[messageIndex];

        // Remove old level classes and add new one
        label->RemoveClass("log-trace");
        label->RemoveClass("log-debug");
        label->RemoveClass("log-info");
        label->RemoveClass("log-warning");
        label->RemoveClass("log-error");
        label->RemoveClass("log-critical");
        label->AddClass(GetLogLevelClass(stored.Level));

        label->SetText(GetDisplayText(stored));

        auto& rowState = m_RowState[label];
        rowState.MessageIndex = messageIndex;

        // Wire up click handlers (only once per pooled element)
        if (!rowState.ClickWired)
        {
            rowState.ClickWired = true;
            rowState.LastClickTimeMs = 0;

            // Mouse up - detect single click vs double click
            label->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
                UIElement* target = e.Target;
                if (!target)
                    return;

                auto it = m_RowState.find(target);
                if (it == m_RowState.end())
                    return;
                const size_t idx = it->second.MessageIndex;

                if (idx >= m_Messages.size())
                    return;

                // Check for double click (within 400ms)
                auto now = std::chrono::steady_clock::now();
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

                const bool isDoubleClick =
                    std::chrono::milliseconds(nowMs - it->second.LastClickTimeMs) <
                    GameEngine::Platform::GetDoubleClickInterval();
                it->second.LastClickTimeMs = nowMs;

                // Selection is updated by ListView on MouseDown (Shift/Ctrl multi-select).
                // We only handle copy and double-click here.
                if (isDoubleClick)
                {
                    // Double click - open in IDE
                    if (m_OnLineDoubleClicked)
                        m_OnLineDoubleClicked(m_Messages[idx].Text);
                }
                else
                {
                    // Single click - copy all selected lines to clipboard
                    if (m_OnLineClicked)
                    {
                        std::string toCopy = GetSelectedLinesAsText();
                        if (toCopy.empty())
                            toCopy = GetDisplayText(m_Messages[idx]);
                        m_OnLineClicked(toCopy);
                    }
                }
            });
        }
    });

    AddChild(std::move(listView));
}

void LogView::Clear()
{
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        m_PendingMessages.clear();
    }
    m_Messages.clear();
    m_FilteredIndices.clear();

    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

size_t LogView::GetMessageCount() const
{
    return m_Messages.size();
}

void LogView::AddMessage(const Logger::LogMessage& message)
{
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    // Cap the staging deque so the drain thread can't grow it unboundedly
    // when FlushPendingMessages is called less often (panel hidden, UI
    // pause, burst arrival). Drop oldest pending messages on overflow —
    // the visible LogView already preserves the most recent m_MaxMessages
    // anyway, so dropping pre-flush avoids paying for messages that would
    // immediately be trimmed by m_MaxMessages on flush.
    constexpr std::size_t kMaxPending = 16384;
    if (m_PendingMessages.size() >= kMaxPending)
    {
        m_PendingMessages.pop_front();
    }
    m_PendingMessages.push_back(MakeStoredMessage(message));
}

void LogView::FlushPendingMessages()
{
    if (m_Paused)
        return;

    // Drain up to m_MaxFlushPerFrame messages. Excess stays queued so the next
    // frame can continue absorbing without any single frame paying the full
    // cost of a burst.
    std::deque<StoredMessage> pending;
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);
        if (m_PendingMessages.empty())
            return;
        if (m_PendingMessages.size() <= m_MaxFlushPerFrame)
        {
            pending.swap(m_PendingMessages);
        }
        else
        {
            const auto end = m_PendingMessages.begin() + static_cast<std::ptrdiff_t>(m_MaxFlushPerFrame);
            pending.assign(std::make_move_iterator(m_PendingMessages.begin()),
                           std::make_move_iterator(end));
            m_PendingMessages.erase(m_PendingMessages.begin(), end);
        }
    }

    if (pending.empty())
        return;

    int appendedCount = 0;
    int appendedVisible = 0;
    const bool filtering = IsViewFiltered();
    const size_t prevFilteredCount = m_FilteredIndices.size();

    // Append raw messages only — no per-message string formatting. DisplayText
    // and DisplayLower are produced lazily by the accessors if/when the ListView
    // or search filter asks for them. SourceFile/Function were already copied
    // into owned strings at AddMessage.
    for (auto& stored : pending)
    {
        m_Messages.push_back(std::move(stored));
        ++appendedCount;
    }

    // Batch-trim in one shot instead of per-message pop_front. Important when a
    // burst overflows m_MaxMessages by thousands — the old loop did N separate
    // erases plus marked trimmed=true after the first.
    bool trimmed = false;
    if (m_Messages.size() > m_MaxMessages)
    {
        const size_t excess = m_Messages.size() - m_MaxMessages;
        m_Messages.erase(m_Messages.begin(),
                         m_Messages.begin() + static_cast<std::ptrdiff_t>(excess));
        trimmed = true;
    }

    // Maintain filtered indices (search text and/or severity toggles).
    if (filtering)
    {
        if (trimmed || IsSeverityFilterActive())
        {
            RebuildFilteredIndices();
        }
        else if (!m_SearchFilterLower.empty())
        {
            // Search-only: append matching indices for new tail rows.
            // GetDisplayLower() populates the message's cache on first use.
            const size_t start = m_Messages.size() - static_cast<size_t>(appendedCount);
            for (size_t i = start; i < m_Messages.size(); ++i)
            {
                if (MatchesSearch(m_Messages[i]))
                    m_FilteredIndices.push_back(i);
            }
        }
        appendedVisible = static_cast<int>(m_FilteredIndices.size() - prevFilteredCount);
    }

    // Update the ListView efficiently
    if (m_ListView)
    {
        if (trimmed || filtering)
        {
            // Trim shifts indices; filtering changes the effective view size.
            m_ListView->RefreshFromProvider();
        }
        else
        {
            // Only appended items - use fast path
            m_ListView->NotifyItemsAppended(appendedCount);
        }
    }

    // Auto-scroll to bottom
    if (m_AutoScroll && (!filtering || appendedVisible > 0))
        ScrollToBottom();
}

void LogView::ScrollToBottom()
{
    if (!m_ListView)
        return;

    m_ListView->ScrollToEnd();
}

const char* LogView::GetLogLevelClass(Logger::LogLevel level)
{
    switch (level)
    {
    case Logger::LogLevel::Trace:    return "log-trace";
    case Logger::LogLevel::Debug:    return "log-debug";
    case Logger::LogLevel::Info:     return "log-info";
    case Logger::LogLevel::Warning:  return "log-warning";
    case Logger::LogLevel::Error:    return "log-error";
    case Logger::LogLevel::Critical: return "log-critical";
    default:                         return "log-info";
    }
}

bool LogView::IsSeverityFilterActive() const
{
    return !(m_ShowDebug && m_ShowInfo && m_ShowWarning && m_ShowError);
}

bool LogView::IsViewFiltered() const
{
    return !m_SearchFilterLower.empty() || IsSeverityFilterActive();
}

bool LogView::PassesLevelFilter(const StoredMessage& stored) const
{
    if (!IsSeverityFilterActive())
        return true;
    switch (stored.Level)
    {
    case Logger::LogLevel::Debug:
        return m_ShowDebug;
    case Logger::LogLevel::Info:
        return m_ShowInfo;
    case Logger::LogLevel::Warning:
        return m_ShowWarning;
    case Logger::LogLevel::Error:
        return m_ShowError;
    default:
        return true;
    }
}

void LogView::RebuildFilteredIndices()
{
    m_FilteredIndices.clear();
    if (!IsViewFiltered())
        return;
    for (size_t i = 0; i < m_Messages.size(); ++i)
    {
        if (!PassesLevelFilter(m_Messages[i]))
            continue;
        if (!MatchesSearch(m_Messages[i]))
            continue;
        m_FilteredIndices.push_back(i);
    }
}

void LogView::SetDebugFilterEnabled(bool enabled)
{
    if (m_ShowDebug == enabled)
        return;
    m_ShowDebug = enabled;
    RebuildFilteredIndices();
    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

void LogView::SetInfoFilterEnabled(bool enabled)
{
    if (m_ShowInfo == enabled)
        return;
    m_ShowInfo = enabled;
    RebuildFilteredIndices();
    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

void LogView::SetWarningFilterEnabled(bool enabled)
{
    if (m_ShowWarning == enabled)
        return;
    m_ShowWarning = enabled;
    RebuildFilteredIndices();
    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

void LogView::SetErrorFilterEnabled(bool enabled)
{
    if (m_ShowError == enabled)
        return;
    m_ShowError = enabled;
    RebuildFilteredIndices();
    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

void LogView::SetSearchFilter(const std::string& filterText)
{
    m_SearchFilterLower = ToLowerAscii(filterText);
    RebuildFilteredIndices();

    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

void LogView::SetSearchField(const std::string& field)
{
    m_SearchField = field;
    RebuildFilteredIndices();
    if (m_ListView)
        m_ListView->RefreshFromProvider();
}

bool LogView::MatchesSearch(const StoredMessage& stored) const
{
    if (m_SearchFilterLower.empty())
        return true;

    std::string searchable;
    if (m_SearchField == "message")
        searchable = stored.Text;
    else if (m_SearchField == "level")
        searchable = Logger::LogLevelToString(stored.Level);
    else if (m_SearchField == "source")
        searchable = stored.SourceFile + ":" + std::to_string(stored.SourceLine);
    else if (m_SearchField == "function")
        searchable = stored.Function;
    else
        return GetDisplayLower(stored).find(m_SearchFilterLower) != std::string::npos ||
               ToLowerAscii(stored.SourceFile).find(m_SearchFilterLower) != std::string::npos ||
               ToLowerAscii(stored.Function).find(m_SearchFilterLower) != std::string::npos;

    return ToLowerAscii(searchable).find(m_SearchFilterLower) != std::string::npos;
}

std::string LogView::GetAllMessagesAsText(bool includeFiltered) const
{
    std::string out;

    const bool useFilters = includeFiltered && IsViewFiltered();
    if (useFilters)
    {
        for (size_t idx : m_FilteredIndices)
        {
            if (idx >= m_Messages.size())
                continue;
            out += GetDisplayText(m_Messages[idx]);
            out += '\n';
        }
        return out;
    }

    for (const auto& msg : m_Messages)
    {
        out += GetDisplayText(msg);
        out += '\n';
    }
    return out;
}

std::string LogView::GetSelectedLinesAsText() const
{
    if (!m_SelectionModel)
        return {};
    auto ids = m_SelectionModel->GetSelection();
    if (ids.empty())
        return {};
    std::vector<size_t> indices;
    indices.reserve(ids.size());
    for (UI::Interaction::ItemId id : ids)
    {
        if (id == 0)
            continue;
        size_t msgIdx = static_cast<size_t>(id - 1);
        if (msgIdx < m_Messages.size())
            indices.push_back(msgIdx);
    }
    if (indices.empty())
        return {};
    std::sort(indices.begin(), indices.end());
    std::string out;
    for (size_t idx : indices)
    {
        out += GetDisplayText(m_Messages[idx]);
        out += '\n';
    }
    return out;
}

} // namespace GameEngine
