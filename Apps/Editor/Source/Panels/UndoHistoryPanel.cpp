#include "Panels/UndoHistoryPanel.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/UIElement.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"

#include <chrono>
#include <ctime>
#include <string>

namespace GameEngine {

namespace {

std::string FormatTimestamp(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{})
        return {};
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[9]; // "HH:MM:SS\0"
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace

UndoHistoryPanel::UndoHistoryPanel()
    : DockPanel("Undo History")
{
    BuildUI();
}

UndoHistoryPanel::~UndoHistoryPanel() = default;

void UndoHistoryPanel::SetUndoRedoService(Editor::UndoRedoService* undo)
{
    if (m_Undo == undo)
        return;
    m_Undo = undo;
    Rebuild();
}

void UndoHistoryPanel::BuildUI()
{
    auto root = std::make_unique<UIElement>();
    root->AddClass("undo-history-panel");

    // Header strip: nav arrows + title.
    auto header = std::make_unique<UIElement>();
    header->AddClass("undo-history-header");

    auto historyNav = std::make_unique<UIElement>();
    historyNav->AddClass("inspector-header-history");
    historyNav->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(2.0f))
        .Set(Style::FlexShrink, 0.0f);

    auto backBtn = std::make_unique<Button>();
    backBtn->AddClass("icon-button");
    backBtn->AddClass("inspector-header-history-icon");
    backBtn->AddClass("inspector-history-back-btn");
    backBtn->SetText("");
    backBtn->SetTooltip("Undo");
    backBtn->Overrides().Set(Style::FlexShrink, 0.0f);
    backBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_Undo)
            m_Undo->Undo();
    });
    m_BackBtn = backBtn.get();
    historyNav->AddChild(std::move(backBtn));

    auto fwdBtn = std::make_unique<Button>();
    fwdBtn->AddClass("icon-button");
    fwdBtn->AddClass("inspector-header-history-icon");
    fwdBtn->AddClass("inspector-history-forward-btn");
    fwdBtn->SetText("");
    fwdBtn->SetTooltip("Redo");
    fwdBtn->Overrides().Set(Style::FlexShrink, 0.0f);
    fwdBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_Undo)
            m_Undo->Redo();
    });
    m_ForwardBtn = fwdBtn.get();
    historyNav->AddChild(std::move(fwdBtn));

    header->AddChild(std::move(historyNav));

    auto title = std::make_unique<Label>();
    title->AddClass("inspector-header");
    title->AddClass("undo-history-title");
    title->SetText("Undo History");
    header->AddChild(std::move(title));

    root->AddChild(std::move(header));

    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("undo-history-scroll");
    m_ScrollView = scroll.get();

    auto list = std::make_unique<UIElement>();
    list->AddClass("undo-history-list");
    m_List = list.get();

    scroll->AddContent(std::move(list));
    root->AddChild(std::move(scroll));

    // Footer strip: count label + clear button.
    auto footer = std::make_unique<UIElement>();
    footer->AddClass("undo-history-footer");

    auto countLabel = std::make_unique<Label>();
    countLabel->AddClass("undo-history-count");
    m_CountLabel = countLabel.get();
    footer->AddChild(std::move(countLabel));

    auto clearBtn = std::make_unique<Button>();
    clearBtn->AddClass("small");
    clearBtn->AddClass("secondary");
    clearBtn->AddClass("undo-history-clear-button");
    clearBtn->SetText("Clear");
    clearBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_Undo)
            m_Undo->Clear();
    });
    footer->AddChild(std::move(clearBtn));

    root->AddChild(std::move(footer));
    AddChild(std::move(root));
}

void UndoHistoryPanel::Rebuild()
{
    if (!m_List)
        return;

    {
        std::vector<UIElement*> toRemove;
        for (const auto& child : m_List->GetChildren())
            toRemove.push_back(child.get());
        for (UIElement* child : toRemove)
            m_List->RemoveChild(child);
    }

    const std::size_t undoCount = m_Undo ? m_Undo->GetUndoCount() : 0;
    const std::size_t redoCount = m_Undo ? m_Undo->GetRedoCount() : 0;

    // Topmost row: always shows "Current State" (non-clickable).
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("undo-history-item");
        row->AddClass("current-state");
        auto label = std::make_unique<Label>();
        label->AddClass("undo-history-label");
        label->SetText("Current State");
        row->AddChild(std::move(label));
        m_List->AddChild(std::move(row));
    }

    // Undo entries: newest first so it sits just below "Current State".
    // GetUndoNameAt(undoCount-1) = newest (next to be undone).
    // GetUndoNameAt(0)           = oldest.
    for (std::size_t k = 0; k < undoCount; ++k)
    {
        const std::size_t idx = undoCount - 1 - k; // k=0 → newest
        const char* name = m_Undo->GetUndoNameAt(idx);
        if (!name)
            name = "(unnamed)";

        auto row = std::make_unique<UIElement>();
        row->AddClass("undo-history-item");
        if (k == 0)
            row->AddClass("most-recent");

        auto label = std::make_unique<Label>();
        label->AddClass("undo-history-label");
        label->SetText(name);
        row->AddChild(std::move(label));

        const std::string ts = FormatTimestamp(m_Undo->GetUndoTimestampAt(idx));
        if (!ts.empty())
        {
            auto tsLabel = std::make_unique<Label>();
            tsLabel->AddClass("undo-history-timestamp");
            tsLabel->SetText(ts);
            row->AddChild(std::move(tsLabel));
        }

        // Clicking seeks back: undo (k+1) steps to reach state before this action.
        const std::size_t steps = k + 1;
        row->RegisterEventHandler(kEventMouseUp, [this, steps](UIEvent& e) {
            if (e.Button != 0 || !m_Undo)
                return;
            for (std::size_t step = 0; step < steps; ++step)
                m_Undo->Undo();
            e.Stop();
        });

        m_List->AddChild(std::move(row));
    }

    // Redo entries below, dimmed. Next-to-redo (index 0) first.
    // GetRedoNameAt(0) = most recently undone (next to be redone).
    for (std::size_t i = 0; i < redoCount; ++i)
    {
        const char* name = m_Undo->GetRedoNameAt(i);
        if (!name)
            name = "(unnamed)";

        auto row = std::make_unique<UIElement>();
        row->AddClass("undo-history-item");
        row->AddClass("redo-item");

        auto label = std::make_unique<Label>();
        label->AddClass("undo-history-label");
        label->SetText(name);
        row->AddChild(std::move(label));

        const std::size_t stepsForward = i + 1;
        row->RegisterEventHandler(kEventMouseUp, [this, stepsForward](UIEvent& e) {
            if (e.Button != 0 || !m_Undo)
                return;
            for (std::size_t step = 0; step < stepsForward; ++step)
                m_Undo->Redo();
            e.Stop();
        });

        m_List->AddChild(std::move(row));
    }

    if (m_CountLabel)
    {
        const std::size_t total = undoCount + redoCount;
        m_CountLabel->SetText(std::to_string(total) + (total == 1 ? " Action" : " Actions"));
    }

    if (m_BackBtn)
        m_BackBtn->SetEnabled(m_Undo && m_Undo->CanUndo());
    if (m_ForwardBtn)
        m_ForwardBtn->SetEnabled(m_Undo && m_Undo->CanRedo());

    if (m_ScrollView)
        m_ScrollView->SetScrollY(0.0f);
}

} // namespace GameEngine
