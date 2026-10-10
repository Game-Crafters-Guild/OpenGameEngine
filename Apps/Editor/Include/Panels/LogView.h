#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Logger/Backtrace.h"
#include "Logger/LogSink.h"
#include "UI/UIElement.h"
#include "UI/Controls/ListView.h"
#include "UI/Interaction/Selection.h"

namespace GameEngine {

class ScrollView;

/**
 * @brief UIElement that displays log messages with color coding and auto-scroll.
 *
 * LogView captures log messages via a callback sink and displays them with
 * appropriate color coding based on log level. It supports:
 * - Auto-scrolling to newest messages (virtualized via ListView)
 * - Message limits to prevent unbounded memory growth
 * - Thread-safe message capture
 * - High performance virtualization for large log volumes
 */
class LogView : public UIElement {
public:
    static constexpr size_t kDefaultMaxMessages = 1000;
    static constexpr float kDefaultRowHeight = 18.0f;

    LogView();
    ~LogView() override;

    /**
     * @brief Set maximum number of messages to retain.
     * @param maxMessages Maximum message count (oldest are discarded when exceeded).
     */
    void SetMaxMessages(size_t maxMessages) { m_MaxMessages = maxMessages; }

    /**
     * @brief Enable or disable auto-scroll to bottom.
     * @param autoScroll True to automatically scroll to newest messages.
     */
    void SetAutoScroll(bool autoScroll) { m_AutoScroll = autoScroll; }

    /**
     * @brief Clear all messages from the view.
     */
    void Clear();

    /**
     * @brief Get the number of messages currently displayed.
     * @return Current message count.
     */
    size_t GetMessageCount() const;

    /**
     * @brief Add a log message to the view.
     * Thread-safe: can be called from any thread.
     * @param message The log message to add.
     */
    void AddMessage(const Logger::LogMessage& message);

    /**
     * @brief Flush pending messages from the queue to the UI.
     * Should be called on the main thread during update.
     */
    void FlushPendingMessages();

    // Filter the visible list (case-insensitive substring match).
    // Empty filter shows all messages.
    void SetSearchFilter(const std::string& filterText);
    void SetSearchField(const std::string& field);

    // Show/hide messages by severity (Debug / Info / Warning / Error). Trace and Critical are always shown.
    // Default: all four enabled.
    void SetDebugFilterEnabled(bool enabled);
    void SetInfoFilterEnabled(bool enabled);
    void SetWarningFilterEnabled(bool enabled);
    void SetErrorFilterEnabled(bool enabled);
    bool IsDebugFilterEnabled() const { return m_ShowDebug; }
    bool IsInfoFilterEnabled() const { return m_ShowInfo; }
    bool IsWarningFilterEnabled() const { return m_ShowWarning; }
    bool IsErrorFilterEnabled() const { return m_ShowError; }
    bool IsSeverityFilterActive() const;

    // Collect log lines into a single string (for copy/export).
    // If includeFiltered == true, the current filter (if any) is applied.
    std::string GetAllMessagesAsText(bool includeFiltered) const;

    // Returns the text of all currently selected lines (order preserved). Empty if none selected.
    std::string GetSelectedLinesAsText() const;

    /**
     * @brief Get the CSS class name for a log level.
     * @param level The log level.
     * @return CSS class name for styling.
     */
    static const char* GetLogLevelClass(Logger::LogLevel level);

    /**
     * @brief Set the row height for log entries.
     * @param height Height in pixels.
     */
    void SetRowHeight(float height) { m_RowHeight = height; }

    /**
     * @brief Set callback for when a log line is clicked (single click copies line).
     * @param callback Function receiving the log line text.
     */
    void SetOnLineClicked(std::function<void(const std::string&)> callback) { m_OnLineClicked = std::move(callback); }

    /**
     * @brief Set callback for when a log line is double-clicked (opens in IDE).
     * @param callback Function receiving the log line text.
     */
    void SetOnLineDoubleClicked(std::function<void(const std::string&)> callback) { m_OnLineDoubleClicked = std::move(callback); }

    /** When paused, the view stops updating; messages keep queuing and are applied when unpaused. */
    void SetPaused(bool paused) { m_Paused = paused; }
    bool IsPaused() const { return m_Paused; }

    // Message storage (public so the LogPanel detail pane can consume it).
    //
    // Only raw data is stored eagerly. Formatted display text and its lowercase
    // form for search are computed lazily on first access and memoized on the
    // message itself — see GetDisplayText() / GetDisplayLower() below. This
    // keeps ingestion cheap regardless of message volume.
    struct StoredMessage {
        Logger::LogLevel Level;
        std::string Text;
        std::chrono::system_clock::time_point TimestampRaw;
        std::string SourceFile;
        int SourceLine = 0;
        std::string Function;
        std::shared_ptr<const std::vector<Logger::BacktraceFrame>> Backtrace;

        // Lazy caches — filled on first call to the accessors below. mutable so
        // const consumers can memoize. When m_Messages trims a front entry the
        // caches are freed along with it.
        mutable std::string DisplayTextCache;
        mutable std::string DisplayLowerCache;
    };

    // Format-once accessor for the human-readable line shown in the ListView.
    // First call builds the cached string; subsequent calls are O(1).
    static const std::string& GetDisplayText(const StoredMessage& m);

    // Format-once accessor for the case-folded form used by the search filter.
    // Only called when the search filter is active, so unused messages never
    // pay the cost.
    static const std::string& GetDisplayLower(const StoredMessage& m);

    // Invalidate lazy caches across all stored messages. Call when anything
    // that affects formatting (e.g. timestamp format) changes at runtime.
    void InvalidateDisplayCaches();

    /** Access a stored message by internal index (for detail pane lookup). */
    const StoredMessage* GetMessage(size_t index) const
    {
        return index < m_Messages.size() ? &m_Messages[index] : nullptr;
    }

    /**
     * @brief Set callback invoked when the selection anchor changes.
     * Receives the internal message index and a pointer to the stored message
     * (or nullptr when selection was cleared).
     */
    void SetOnMessageSelected(std::function<void(size_t, const StoredMessage*)> cb)
    {
        m_OnMessageSelected = std::move(cb);
    }

private:
    // Data provider for ListView virtualization
    class LogDataProvider : public ListChangeTrackingProvider {
    public:
        LogDataProvider(LogView* owner) : m_Owner(owner) {}
        int GetItemCount() const override;
        ListId GetItemId(int index) const override;
        float GetItemHeight(int index) const override;
    private:
        LogView* m_Owner;
    };

    void SetupListView();
    void ScrollToBottom();
    bool IsViewFiltered() const;
    void RebuildFilteredIndices();
    bool MatchesSearch(const StoredMessage& stored) const;

    ListView* m_ListView = nullptr;
    std::unique_ptr<LogDataProvider> m_DataProvider;
    std::unique_ptr<UI::Interaction::SelectionModel> m_SelectionModel;

    std::deque<StoredMessage> m_Messages;

    bool PassesLevelFilter(const StoredMessage& stored) const;

    // Filter state (case-insensitive). We keep a cached lowercase filter string and
    // indices into m_Messages for the filtered view.
    std::string m_SearchFilterLower;
    std::string m_SearchField{"all"};
    std::vector<size_t> m_FilteredIndices;
    bool m_ShowDebug = true;
    bool m_ShowInfo = true;
    bool m_ShowWarning = true;
    bool m_ShowError = true;

    // Thread-safe pending queue. SourceFile/Function are owned strings on
    // LogMessage; we copy them at ingest so a later flush never sees a
    // borrowed C string.
    std::deque<StoredMessage> m_PendingMessages;
    mutable std::mutex m_PendingMutex;

    size_t m_MaxMessages = kDefaultMaxMessages;
    // Cap the number of pending messages ingested per frame to keep the UI
    // responsive during bursts (e.g. asset indexing emitting thousands of
    // Debug lines). Remaining messages stay queued for the next frame.
    size_t m_MaxFlushPerFrame = 2000;
    float m_RowHeight = kDefaultRowHeight;
    bool m_AutoScroll = true;
    bool m_NeedsRefresh = false;
    bool m_Paused = false;

    struct RowState
    {
        size_t MessageIndex = 0;
        std::int64_t LastClickTimeMs = 0;
        bool ClickWired = false;
    };
    std::unordered_map<UIElement*, RowState> m_RowState;

    // Callbacks for line interaction
    std::function<void(const std::string&)> m_OnLineClicked;
    std::function<void(const std::string&)> m_OnLineDoubleClicked;
    std::function<void(size_t, const StoredMessage*)> m_OnMessageSelected;
};

} // namespace GameEngine
