#pragma once

#include <functional>
#include <memory>
#include <string>
#include "Logger/CallbackSink.h"
#include "UI/Controls/DockPanel.h"

namespace GameEngine {

class LogView;
class Label;
class TextField;
class UIElement;
class Button;
class Splitter;
class WeightedPane;

/**
 * @brief Editor panel that displays engine log messages.
 * 
 * LogPanel provides a dockable panel that shows log output with color coding
 * based on log severity. It automatically captures log messages via a
 * CallbackSink and displays them in real-time.
 */
class LogPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-log-icon"; }

    LogPanel();
    ~LogPanel() override;

    /**
     * @brief Update the panel (flush pending log messages).
     * Should be called each frame from the main thread.
     */
    void Update();

    /**
     * @brief Clear all log messages from the panel.
     */
    void ClearLog();

    /**
     * @brief Get the log view component.
     * @return Pointer to the LogView (owned by UI tree).
     */
    LogView* GetLogView() const { return m_LogView; }

    /**
     * @brief Set callback for opening log entries in IDE.
     * @param callback Function receiving the log message text.
     */
    void SetOnOpenInIDE(std::function<void(const std::string&)> callback) { m_OnOpenInIDE = std::move(callback); }

    /**
     * @brief Set callback for opening a specific source file at a line (for stack frames).
     * @param callback Function receiving the file path and 1-based line.
     */
    void SetOnOpenSourceFile(std::function<void(const std::string&, int)> callback)
    {
        m_OnOpenSourceFile = std::move(callback);
    }

private:
    void SetupLogCapture();
    void TeardownLogCapture();
    void OnLogMessage(const Logger::LogMessage& message);
    void OnSearchTextChanged(const std::string& searchText);
    void CopyToClipboard();
    void BuildDetailPane(UIElement* parent);
    void PopulateDetailPaneForIndex(size_t messageIndex);
    void HideDetailPane();
    void OpenSourceLocation(const std::string& file, int line);
    /** Stack detail below the log in tall docks and beside it in wide docks. */
    void SyncLogDetailLayoutMode();
    /** Like SettingsPanel aspect checks: react to width with hysteresis; move Clear/Pause/Copy + search to a second row when narrow. */
    void SyncLogToolbarLayoutMode();
    /** If center/search were reparented out of sync with m_LogToolbarCompact (e.g. deferred work), restore a valid tree. */
    void RepairLogToolbarChromeIfNeeded();

    LogView* m_LogView = nullptr;
    UIElement* m_SplitBody = nullptr;
    WeightedPane* m_LogPane = nullptr;
    Splitter* m_DetailSplitter = nullptr;
    WeightedPane* m_DetailWeightedPane = nullptr;
    UIElement* m_DetailPane = nullptr;
    Label* m_DetailSourceLabel = nullptr;
    UIElement* m_DetailFrameList = nullptr;
    UIElement* m_LogToolbarStack = nullptr;
    UIElement* m_LogToolbarPrimary = nullptr;
    UIElement* m_LogToolbarSecondary = nullptr;
    UIElement* m_LogCenterSlot = nullptr;
    UIElement* m_SearchBar = nullptr;
    TextField* m_SearchField = nullptr;
    Button* m_ClearButton = nullptr;
    Button* m_PauseButton = nullptr;
    Button* m_CopyButton = nullptr;
    
    // Callback registration ID for unregistering on destruction
    Logger::uint64 m_CallbackId = 0;
    
    // Buffer messages that arrive before UI is ready
    std::vector<Logger::LogMessage> m_EarlyMessages;
    bool m_Initialized = false;

    // Callback for opening log entries in IDE
    std::function<void(const std::string&)> m_OnOpenInIDE;
    // Callback for opening a source file at a specific line (for stack frames)
    std::function<void(const std::string&, int)> m_OnOpenSourceFile;

    bool m_LogToolbarCompact = false;
    int m_LogToolbarCompactStreak = 0;
    int m_LogToolbarWideStreak = 0;
    bool m_LogDetailLayoutInitialized = false;
    bool m_LogDetailSideBySide = false;
    float m_VisibleLogPaneWeight = 0.7f;
    float m_VisibleDetailPaneWeight = 0.3f;

    static constexpr size_t kNoSelection = static_cast<size_t>(-1);
    size_t m_SelectedMessageIndex = kNoSelection;
};

} // namespace GameEngine
