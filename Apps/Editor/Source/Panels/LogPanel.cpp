#include "Panels/LogPanel.h"

#include "Logger/Logger.h"
#include "Panels/BacktraceFrameView.h"
#include "Panels/SettingsPanel.h"
#include "Panels/LogView.h"
#include "UI/PanelSearchBar.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/SplitView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/UIEvents.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

#include <cstdio>
#include <string>

namespace GameEngine {

namespace {

void SyncLogFilterToggleLabels(Button& button, const std::string& levelText)
{
    if (button.GetChildren().size() < 2)
        return;
    UIElement* checkIcon = button.GetChildren()[0].get();
    auto* textLbl = dynamic_cast<Label*>(button.GetChildren()[1].get());
    if (!checkIcon || !textLbl)
        return;
    textLbl->SetText(levelText);
    // Keep icon in layout; hide when off via opacity (not display:none).
    checkIcon->Overrides().Set(Style::Opacity, button.HasClass("checked") ? 1.0f : 0.0f);
    checkIcon->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

} // namespace

// Static reference to the shared CallbackSink instance
// We use a static sink that gets added to the Logger once
static Logger::CallbackSink* s_LogCallbackSink = nullptr;
static int s_SinkRefCount = 0;

namespace {

Logger::CallbackSink* GetOrCreateCallbackSink()
{
    if (!s_LogCallbackSink)
    {
        Logger::CallbackSink::Config config;
        config.MinLevel = Logger::LogLevel::Trace;
        
        auto sink = std::make_unique<Logger::CallbackSink>(config);
        s_LogCallbackSink = sink.get();
        Logger::Log::AddSink(std::move(sink));
    }
    s_SinkRefCount++;
    return s_LogCallbackSink;
}

void ReleaseCallbackSink()
{
    if (s_SinkRefCount > 0)
    {
        s_SinkRefCount--;
    }
    // Note: We don't remove the sink from Logger because there's no
    // RemoveSink API. The sink will remain but with no callbacks.
}

} // namespace

LogPanel::LogPanel()
    : DockPanel("Log")
{
    // Toolbar: two rows when narrow (same idea as SettingsPanel width/aspect checks + layout change).
    {
        auto stack = std::make_unique<UIElement>();
        stack->AddClass("log-toolbar-stack");
        m_LogToolbarStack = stack.get();

        auto primary = std::make_unique<UIElement>();
        primary->AddClass("log-toolbar");
        primary->AddClass("log-toolbar-primary");
        m_LogToolbarPrimary = primary.get();

        auto filtersRow = std::make_unique<UIElement>();
        filtersRow->AddClass("log-toolbar-filters");
        UIElement* filters = filtersRow.get();

        auto addSeverityToggle = [this, filters](const char* label, const char* logLineColorClass,
                                                   const char* levelChromeClass, bool initialOn,
                                                   void (LogView::*setter)(bool)) {
            const std::string levelText(label);

            auto btn = std::make_unique<Button>();
            btn->AddClass("small");
            btn->AddClass("secondary");
            btn->AddClass("log-filter-toggle");
            btn->AddClass(levelChromeClass);
            if (initialOn)
                btn->AddClass("checked");

            // Replace default label: [checked icon] [level name] (Assets/Icons/checked.png).
            if (!btn->GetChildren().empty())
                btn->RemoveChild(btn->GetChildren()[0].get());

            auto checkIcon = std::make_unique<UIElement>();
            checkIcon->AddClass("log-filter-toggle-check");
            UI::Layout::SetBackgroundPath(*checkIcon, "Icons/checked.png");

            auto textLbl = std::make_unique<Label>();
            textLbl->AddClass(logLineColorClass);
            textLbl->AddClass("button-text");
            textLbl->AddClass("log-filter-toggle-label");
            textLbl->SetText(levelText);

            btn->AddChild(std::move(checkIcon));
            btn->AddChild(std::move(textLbl));
            SyncLogFilterToggleLabels(*btn, levelText);

            btn->RegisterEventHandler(kEventButtonClick, [this, setter, levelText](UIEvent& e) {
                Button& b = static_cast<Button&>(*e.CurrentTarget);
                if (!m_LogView)
                    return;
                const bool nowOn = !b.HasClass("checked");
                if (nowOn)
                    b.AddClass("checked");
                else
                    b.RemoveClass("checked");
                (m_LogView->*setter)(nowOn);
                SyncLogFilterToggleLabels(b, levelText);
            });
            filters->AddChild(std::move(btn));
        };

        addSeverityToggle("DEBUG", "log-debug", "log-filter-level-debug", true, &LogView::SetDebugFilterEnabled);
        addSeverityToggle("INFO", "log-info", "log-filter-level-info", true, &LogView::SetInfoFilterEnabled);
        addSeverityToggle("WARNING", "log-warning", "log-filter-level-warning", true, &LogView::SetWarningFilterEnabled);
        addSeverityToggle("ERROR", "log-error", "log-filter-level-error", true, &LogView::SetErrorFilterEnabled);

        primary->AddChild(std::move(filtersRow));

        auto copyFiltersGap = std::make_unique<UIElement>();
        copyFiltersGap->AddClass("log-toolbar-copy-filters-gap");
        primary->AddChild(std::move(copyFiltersGap));

        auto mainActionsRow = std::make_unique<UIElement>();
        mainActionsRow->AddClass("log-toolbar-main-actions");
        auto clearBtn = std::make_unique<Button>();
        m_ClearButton = clearBtn.get();
        clearBtn->AddClass("small");
        clearBtn->AddClass("secondary");
        clearBtn->AddClass("log-clear-button");
        clearBtn->SetText("Clear");
        clearBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { this->ClearLog(); });
        mainActionsRow->AddChild(std::move(clearBtn));

        auto pauseBtn = std::make_unique<Button>();
        m_PauseButton = pauseBtn.get();
        pauseBtn->AddClass("small");
        pauseBtn->AddClass("secondary");
        pauseBtn->AddClass("log-pause-button");
        pauseBtn->SetText("Pause");
        pauseBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            Button& btn = static_cast<Button&>(*e.CurrentTarget);
            if (!m_LogView)
                return;
            const bool nowPaused = !m_LogView->IsPaused();
            m_LogView->SetPaused(nowPaused);
            btn.SetText(nowPaused ? "Resume" : "Pause");
        });
        mainActionsRow->AddChild(std::move(pauseBtn));

        auto copyBtn = std::make_unique<Button>();
        m_CopyButton = copyBtn.get();
        copyBtn->AddClass("small");
        copyBtn->AddClass("secondary");
        copyBtn->AddClass("log-copy-button");
        copyBtn->SetText("Copy");
        copyBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { this->CopyToClipboard(); });
        mainActionsRow->AddChild(std::move(copyBtn));

        auto centerSlot = std::make_unique<UIElement>();
        centerSlot->AddClass("log-toolbar-center-slot");
        centerSlot->AddChild(std::move(mainActionsRow));
        m_LogCenterSlot = centerSlot.get();
        primary->AddChild(std::move(centerSlot));

        auto built = BuildPanelSearchBar(
            "log-search-field",
            []() { return SettingsPanel::GetSearchBarsVisible(); },
            [this](const std::string& value) { this->OnSearchTextChanged(value); },
            {},
            {{"all", "All fields"}, {"message", "Message"}, {"level", "Severity"},
             {"source", "Source"}, {"function", "Function"}},
            [this](const std::string& field) {
                if (m_LogView)
                    m_LogView->SetSearchField(field);
            });
        m_SearchBar = built.RootPtr;
        m_SearchField = built.FieldPtr;
        primary->AddChild(std::move(built.Root));
        SettingsPanel::RegisterSearchBar(m_SearchBar, built.IconPtr);

        auto secondary = std::make_unique<UIElement>();
        secondary->AddClass("log-toolbar");
        secondary->AddClass("log-toolbar-secondary");
        secondary->AddClass("log-toolbar-secondary-hidden");
        m_LogToolbarSecondary = secondary.get();

        stack->AddChild(std::move(primary));
        stack->AddChild(std::move(secondary));
        AddChild(std::move(stack));
    }

    auto splitBody = std::make_unique<SplitView>();
    splitBody->AddClass("log-split-body");
    m_SplitBody = splitBody.get();

    auto logPane = std::make_unique<WeightedPane>(1.0f);
    logPane->AddClass("pane");
    m_LogPane = logPane.get();

    auto logView = std::make_unique<LogView>();
    m_LogView = logView.get();

    m_LogView->SetOnLineClicked([this](const std::string& lineText) {
        UIManager* ui = GetOwnerManager();
        if (!ui)
            return;
        if (auto* platform = ui->GetPlatform())
            platform->SetClipboardText(lineText.c_str());
    });

    m_LogView->SetOnLineDoubleClicked([this](const std::string& lineText) {
        if (m_OnOpenInIDE)
            m_OnOpenInIDE(lineText);
    });

    m_LogView->SetOnMessageSelected([this](size_t idx, const LogView::StoredMessage* stored) {
        const bool hasStored = stored != nullptr;
        m_SelectedMessageIndex = hasStored ? idx : kNoSelection;
        PostSafeAction([this, idx, hasStored]() {
            if (!hasStored)
                HideDetailPane();
            else
                PopulateDetailPaneForIndex(idx);
        });
    });

    logPane->AddChild(std::move(logView));
    splitBody->AddChild(std::move(logPane));

    auto splitter = std::make_unique<Splitter>();
    splitter->AddClass("splitter");
    splitter->AddClass("col");
    splitter->AddClass("log-detail-splitter");
    splitter->AddClass("log-detail-pane--hidden");
    m_DetailSplitter = splitter.get();
    splitBody->AddChild(std::move(splitter));

    auto detailPane = std::make_unique<WeightedPane>(0.0f);
    detailPane->AddClass("pane");
    detailPane->AddClass("log-detail-pane--hidden");
    m_DetailWeightedPane = detailPane.get();
    BuildDetailPane(detailPane.get());
    splitBody->AddChild(std::move(detailPane));

    AddChild(std::move(splitBody));
    
    SetupLogCapture();
    m_Initialized = true;
    
    // Flush any early messages that were buffered before initialization
    for (const auto& msg : m_EarlyMessages)
    {
        if (m_LogView)
        {
            m_LogView->AddMessage(msg);
        }
    }
    m_EarlyMessages.clear();
}

LogPanel::~LogPanel()
{
    if (m_SearchBar)
        SettingsPanel::UnregisterSearchBar(m_SearchBar);
    TeardownLogCapture();
}

void LogPanel::Update()
{
    SyncLogToolbarLayoutMode();
    SyncLogDetailLayoutMode();
    if (m_LogView)
    {
        m_LogView->FlushPendingMessages();
    }
}

void LogPanel::SyncLogDetailLayoutMode()
{
    if (!m_SplitBody || !m_DetailSplitter)
        return;

    const float width = GetLayoutWidth();
    const float height = GetLayoutHeight();
    if (width <= 1.0f || height <= 1.0f)
        return;

    const float aspect = width / height;
    bool sideBySide = m_LogDetailSideBySide;
    if (!m_LogDetailLayoutInitialized)
        sideBySide = aspect > 1.0f;
    else if (m_LogDetailSideBySide)
        sideBySide = aspect >= 0.9f;
    else
        sideBySide = aspect > 1.1f;

    if (m_LogDetailLayoutInitialized && sideBySide == m_LogDetailSideBySide)
        return;

    m_LogDetailLayoutInitialized = true;
    m_LogDetailSideBySide = sideBySide;
    if (sideBySide)
    {
        m_SplitBody->AddClass("log-split-body--side-by-side");
        m_DetailSplitter->RemoveClass("col");
        m_DetailSplitter->AddClass("row");
    }
    else
    {
        m_SplitBody->RemoveClass("log-split-body--side-by-side");
        m_DetailSplitter->RemoveClass("row");
        m_DetailSplitter->AddClass("col");
    }

    m_SplitBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_SplitBody->RequestRelayout();
}

void LogPanel::RepairLogToolbarChromeIfNeeded()
{
    if (!m_LogToolbarStack || !m_LogToolbarPrimary || !m_LogToolbarSecondary || !m_LogCenterSlot || !m_SearchBar)
        return;

    UIElement* centerParent = m_LogCenterSlot->GetParent();
    UIElement* searchParent = m_SearchBar->GetParent();
    if (!centerParent || !searchParent)
        return;

    auto syncChromeVisibilityClasses = [this]() {
        if (m_LogToolbarCompact)
        {
            m_LogToolbarSecondary->RemoveClass("log-toolbar-secondary-hidden");
            m_LogToolbarStack->AddClass("log-toolbar-stack--compact");
        }
        else
        {
            m_LogToolbarSecondary->AddClass("log-toolbar-secondary-hidden");
            m_LogToolbarStack->RemoveClass("log-toolbar-stack--compact");
        }
        m_LogToolbarStack->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    };

    auto takeIfChild = [](UIElement* parent, UIElement* node) -> std::unique_ptr<UIElement> {
        if (!parent || !node || node->GetParent() != parent)
            return nullptr;
        return parent->TakeChild(node);
    };

    // Center and search must live on the same row; split parents means a half-applied transition.
    if (centerParent != searchParent)
    {
        std::unique_ptr<UIElement> center = takeIfChild(centerParent, m_LogCenterSlot);
        std::unique_ptr<UIElement> search = takeIfChild(searchParent, m_SearchBar);
        if (center)
            m_LogToolbarPrimary->AddChild(std::move(center));
        if (search)
            m_LogToolbarPrimary->AddChild(std::move(search));
        m_LogToolbarCompact = false;
        m_LogToolbarCompactStreak = 0;
        m_LogToolbarWideStreak = 0;
        syncChromeVisibilityClasses();
        return;
    }

    if (centerParent == m_LogToolbarPrimary && m_LogToolbarCompact)
    {
        m_LogToolbarCompact = false;
        syncChromeVisibilityClasses();
        return;
    }
    if (centerParent == m_LogToolbarSecondary && !m_LogToolbarCompact)
    {
        m_LogToolbarCompact = true;
        syncChromeVisibilityClasses();
        return;
    }

    // DOM matches the flag — fix visibility classes if they drifted (e.g. stale deferred work).
    if (centerParent == m_LogToolbarSecondary)
    {
        if (m_LogToolbarSecondary->HasClass("log-toolbar-secondary-hidden"))
        {
            m_LogToolbarSecondary->RemoveClass("log-toolbar-secondary-hidden");
            m_LogToolbarStack->AddClass("log-toolbar-stack--compact");
            m_LogToolbarStack->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    }
    else if (centerParent == m_LogToolbarPrimary)
    {
        if (!m_LogToolbarSecondary->HasClass("log-toolbar-secondary-hidden"))
        {
            m_LogToolbarSecondary->AddClass("log-toolbar-secondary-hidden");
            m_LogToolbarStack->RemoveClass("log-toolbar-stack--compact");
            m_LogToolbarStack->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
    }
}

void LogPanel::SyncLogToolbarLayoutMode()
{
    if (!m_LogToolbarStack || !m_LogToolbarPrimary || !m_LogToolbarSecondary || !m_LogCenterSlot || !m_SearchBar)
        return;

    RepairLogToolbarChromeIfNeeded();

    const float width = GetLayoutWidth();
    if (width <= 1.0f)
        return;

    static constexpr float kCompactEnterWidthPx = 760.0f;
    static constexpr float kCompactExitWidthPx = 840.0f;
    static constexpr int kLayoutStableFrames = 2;

    bool wantCompact = m_LogToolbarCompact;

    if (!m_LogToolbarCompact)
    {
        m_LogToolbarWideStreak = 0;
        if (width < kCompactEnterWidthPx)
        {
            if (++m_LogToolbarCompactStreak >= kLayoutStableFrames)
            {
                wantCompact = true;
                m_LogToolbarCompactStreak = 0;
            }
        }
        else
            m_LogToolbarCompactStreak = 0;
    }
    else
    {
        m_LogToolbarCompactStreak = 0;
        if (width > kCompactExitWidthPx)
        {
            if (++m_LogToolbarWideStreak >= kLayoutStableFrames)
            {
                wantCompact = false;
                m_LogToolbarWideStreak = 0;
            }
        }
        else
            m_LogToolbarWideStreak = 0;
    }

    if (wantCompact == m_LogToolbarCompact)
        return;

    if (wantCompact)
    {
        auto center = m_LogToolbarPrimary->TakeChild(m_LogCenterSlot);
        auto search = m_LogToolbarPrimary->TakeChild(m_SearchBar);
        if (!center || !search)
        {
            if (center)
                m_LogToolbarPrimary->AddChild(std::move(center));
            if (search)
                m_LogToolbarPrimary->AddChild(std::move(search));
            return;
        }
        m_LogToolbarSecondary->AddChild(std::move(center));
        m_LogToolbarSecondary->AddChild(std::move(search));
        m_LogToolbarSecondary->RemoveClass("log-toolbar-secondary-hidden");
        m_LogToolbarStack->AddClass("log-toolbar-stack--compact");
        m_LogToolbarCompact = true;
    }
    else
    {
        auto center = m_LogToolbarSecondary->TakeChild(m_LogCenterSlot);
        auto search = m_LogToolbarSecondary->TakeChild(m_SearchBar);
        if (!center || !search)
        {
            if (center)
                m_LogToolbarSecondary->AddChild(std::move(center));
            if (search)
                m_LogToolbarSecondary->AddChild(std::move(search));
            return;
        }
        m_LogToolbarPrimary->AddChild(std::move(center));
        m_LogToolbarPrimary->AddChild(std::move(search));
        m_LogToolbarSecondary->AddClass("log-toolbar-secondary-hidden");
        m_LogToolbarStack->RemoveClass("log-toolbar-stack--compact");
        m_LogToolbarCompact = false;
    }

    m_LogToolbarStack->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void LogPanel::ClearLog()
{
    if (m_LogView)
    {
        m_LogView->Clear();
    }
}

void LogPanel::SetupLogCapture()
{
    Logger::CallbackSink* sink = GetOrCreateCallbackSink();
    if (sink)
    {
        m_CallbackId = sink->RegisterCallback(
            [this](const Logger::LogMessage& msg) { OnLogMessage(msg); });
    }
}

void LogPanel::TeardownLogCapture()
{
    if (s_LogCallbackSink && m_CallbackId != 0)
    {
        s_LogCallbackSink->UnregisterCallback(m_CallbackId);
        m_CallbackId = 0;
    }
    ReleaseCallbackSink();
}

void LogPanel::OnLogMessage(const Logger::LogMessage& message)
{
    if (!m_Initialized)
    {
        // Buffer early messages
        m_EarlyMessages.push_back(message);
        return;
    }
    
    if (m_LogView)
    {
        m_LogView->AddMessage(message);
    }
}

void LogPanel::OnSearchTextChanged(const std::string& searchText)
{
    if (m_LogView)
        m_LogView->SetSearchFilter(searchText);
}

void LogPanel::BuildDetailPane(UIElement* parent)
{
    auto pane = std::make_unique<UIElement>();
    pane->AddClass("log-detail-pane");
    m_DetailPane = pane.get();

    auto header = std::make_unique<UIElement>();
    header->AddClass("log-detail-header");

    auto title = std::make_unique<Label>();
    title->AddClass("log-detail-title");
    title->SetText("Stack Trace");
    header->AddChild(std::move(title));

    auto sourceLbl = std::make_unique<Label>();
    sourceLbl->AddClass("log-detail-source");
    sourceLbl->SetText("");
    m_DetailSourceLabel = sourceLbl.get();
    header->AddChild(std::move(sourceLbl));

    pane->AddChild(std::move(header));

    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("log-detail-scroll");
    m_DetailFrameList = scroll->GetViewport();
    if (m_DetailFrameList)
        m_DetailFrameList->AddClass("log-detail-frames");

    pane->AddChild(std::move(scroll));
    parent->AddChild(std::move(pane));
}

void LogPanel::HideDetailPane()
{
    if (!m_DetailPane)
        return;
    const bool wasVisible = m_DetailWeightedPane &&
                            !m_DetailWeightedPane->HasClass("log-detail-pane--hidden");
    if (wasVisible && m_LogPane)
    {
        m_VisibleLogPaneWeight = m_LogPane->GetFlexWeight();
        m_VisibleDetailPaneWeight = m_DetailWeightedPane->GetFlexWeight();
    }
    if (m_DetailSplitter)
        m_DetailSplitter->AddClass("log-detail-pane--hidden");
    if (m_DetailWeightedPane)
    {
        m_DetailWeightedPane->AddClass("log-detail-pane--hidden");
        m_DetailWeightedPane->SetFlexWeight(0.0f);
    }
    if (m_LogPane)
        m_LogPane->SetFlexWeight(1.0f);
    if (m_DetailFrameList)
    {
        m_DetailFrameList->RemoveAllChildren();
    }
    if (m_DetailSourceLabel)
        m_DetailSourceLabel->SetText("");
    if (m_SplitBody)
        m_SplitBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void LogPanel::OpenSourceLocation(const std::string& file, int line)
{
    if (file.empty())
        return;

    if (m_OnOpenSourceFile)
    {
        m_OnOpenSourceFile(file, line);
        return;
    }

    // Fallback: format as "file:line" and route through the legacy IDE open hook.
    if (m_OnOpenInIDE)
    {
        std::string formatted = file;
        if (line > 0)
        {
            formatted += ":";
            formatted += std::to_string(line);
        }
        m_OnOpenInIDE(formatted);
    }
}

void LogPanel::PopulateDetailPaneForIndex(size_t messageIndex)
{
    if (!m_DetailPane || !m_DetailFrameList || !m_DetailSourceLabel || !m_LogView)
        return;

    const auto* stored = m_LogView->GetMessage(messageIndex);
    if (!stored)
    {
        HideDetailPane();
        return;
    }

    const bool hasBacktrace = stored->Backtrace && !stored->Backtrace->empty();
    if (!hasBacktrace)
    {
        HideDetailPane();
        return;
    }

    const bool hasSource = !stored->SourceFile.empty();

    if (m_LogPane)
        m_LogPane->SetFlexWeight(m_VisibleLogPaneWeight);
    if (m_DetailWeightedPane)
        m_DetailWeightedPane->SetFlexWeight(m_VisibleDetailPaneWeight);
    if (m_DetailSplitter)
        m_DetailSplitter->RemoveClass("log-detail-pane--hidden");
    if (m_DetailWeightedPane)
        m_DetailWeightedPane->RemoveClass("log-detail-pane--hidden");

    std::string sourceText;
    if (hasSource)
    {
        sourceText = stored->SourceFile;
        if (stored->SourceLine > 0)
        {
            sourceText += ":";
            sourceText += std::to_string(stored->SourceLine);
        }
        if (!stored->Function.empty())
        {
            sourceText += "  ";
            sourceText += stored->Function;
        }
    }
    else
    {
        sourceText = "(no source location)";
    }
    m_DetailSourceLabel->SetText(sourceText);

    m_DetailFrameList->RemoveAllChildren();

    if (hasSource)
    {
        auto srcRow = std::make_unique<Label>();
        srcRow->AddClass("log-detail-frame");
        srcRow->AddClass("log-detail-frame--source");
        std::string rowText = "source  ";
        rowText += stored->SourceFile;
        if (stored->SourceLine > 0)
        {
            rowText += ":";
            rowText += std::to_string(stored->SourceLine);
        }
        srcRow->SetText(rowText);

        std::string capturedFile = stored->SourceFile;
        int capturedLine = stored->SourceLine;
        srcRow->RegisterEventHandler(kEventMouseUp, [this, capturedFile, capturedLine](UIEvent&) {
            OpenSourceLocation(capturedFile, capturedLine);
        });
        m_DetailFrameList->AddChild(std::move(srcRow));
    }

    if (hasBacktrace)
    {
        const auto& frames = *stored->Backtrace;
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            m_DetailFrameList->AddChild(BuildBacktraceFrameRow(
                i, frames[i],
                [this](const std::string& file, int line) { OpenSourceLocation(file, line); }));
        }
    }

    m_DetailPane->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

void LogPanel::CopyToClipboard()
{
    if (!m_LogView)
        return;

    std::string logText = m_LogView->GetSelectedLinesAsText();
    if (logText.empty())
    {
        const bool useFiltered =
            (m_SearchField && !m_SearchField->GetValue().empty()) ||
            m_LogView->IsSeverityFilterActive();
        logText = m_LogView->GetAllMessagesAsText(useFiltered);
    }

    const bool detailVisible = m_DetailWeightedPane && !m_DetailWeightedPane->HasClass("log-detail-pane--hidden");
    if (detailVisible && m_SelectedMessageIndex != kNoSelection)
    {
        if (const auto* stored = m_LogView->GetMessage(m_SelectedMessageIndex))
        {
            const bool hasSource = !stored->SourceFile.empty();
            const bool hasBacktrace = stored->Backtrace && !stored->Backtrace->empty();
            if (hasSource || hasBacktrace)
            {
                if (!logText.empty() && logText.back() != '\n')
                    logText += '\n';
                logText += "\nStack Trace\n";
                if (hasSource)
                {
                    logText += "source  ";
                    logText += stored->SourceFile;
                    if (stored->SourceLine > 0)
                    {
                        logText += ':';
                        logText += std::to_string(stored->SourceLine);
                    }
                    if (!stored->Function.empty())
                    {
                        logText += "  ";
                        logText += stored->Function;
                    }
                    logText += '\n';
                }
                if (hasBacktrace)
                {
                    const auto& frames = *stored->Backtrace;
                    for (std::size_t i = 0; i < frames.size(); ++i)
                    {
                        const auto& f = frames[i];
                        char idxBuf[24];
                        std::snprintf(idxBuf, sizeof(idxBuf), "[%zu] ", i);
                        logText += idxBuf;
                        if (!f.Module.empty())
                        {
                            logText += f.Module;
                            logText += '!';
                        }
                        logText += f.Symbol.empty() ? std::string("<unknown>") : f.Symbol;
                        if (!f.File.empty())
                        {
                            logText += "  ";
                            logText += f.File;
                            if (f.Line > 0)
                            {
                                logText += ':';
                                logText += std::to_string(f.Line);
                            }
                        }
                        logText += '\n';
                    }
                }
            }
        }
    }

    if (logText.empty())
        return;

    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;

    if (auto* platform = ui->GetPlatform())
        platform->SetClipboardText(logText.c_str());
}

} // namespace GameEngine
