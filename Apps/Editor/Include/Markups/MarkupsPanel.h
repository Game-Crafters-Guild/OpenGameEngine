#pragma once

#include "ECS/ECS.h"
#include "EditorChangeNotifications.h"
#include "UI/AssetBoundDockPanel.h"
#include "UI/UIManagerRef.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Dropdown;
class Label;
class ScrollView;
class TextField;
} // namespace GameEngine

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// The Mark-ups panel (dock id "Markups"): the open scene's mark-ups and, on its second
// tab, the activity of every thread. Its frame is UI/panels/MarkupsPanel.uxml; the rows
// (MarkupsPanelRows) are rebuilt on a change notification, when the editor's selection
// changed (checked at the UI's periodic refresh), and once a minute while the panel shows so
// the relative times stay true; never per frame.
//
// Mark-ups tab: a row per mark-up, newest change first, with its status pill, title,
// author, last change and a dot when the agent changed it since the viewer's last look; the
// rows of the selected mark-ups show as selected; hover outlines it in the Scene View, a click
// selects it, a double-click frames it,
// the eye hides or shows it (a view state, not a scene edit).
// Activity tab: every thread entry of every mark-up, newest first; a click selects and
// frames the mark-up.
class MarkupsPanel final : public AssetBoundDockPanel
{
public:
    static constexpr const char* kPanelId = "Markups";

    enum class Tab
    {
        Markups,
        Activity,
    };

    explicit MarkupsPanel(MarkupEditorBridge& bridge);
    ~MarkupsPanel() override;

    void ShowTab(Tab tab);
    Tab GetTab() const { return m_Tab; }

    // The panel the dock holds, or null when none is open.
    static MarkupsPanel* TryGetOpen();

private:
    void OnLayoutBound() override;
    void OnLayoutReconciled() override;
    // Resolves the frame's elements and wires each one this panel has not wired yet (a
    // reconcile recreates the elements a .uxml edit changed).
    void BindFrame();
    void RefreshTimesIfShown();
    // Whether the editor's selection differs from the one the rows were last built with.
    bool SelectionChangedSinceRebuild() const;
    // Show all / Hide all: every mark-up of the open world in one viewer-state write.
    void SetAllHidden(bool hidden);
    void RequestRebuild();
    void Rebuild();
    void BuildMarkupRows(UIElement& list);
    void BuildActivityRows(UIElement& list);
    // A click selects; a second click on the same row within the system's double-click
    // interval also frames.
    void OnRowClicked(ECS::EntityHandle entity);
    void OnActivityRowClicked(ECS::EntityHandle entity);

    MarkupEditorBridge& m_Bridge;
    EditorChangeNotifications::SubscriptionToken m_ComponentToken;
    EditorChangeNotifications::SubscriptionToken m_StructureToken;
    bool m_RebuildPending = false;
    Tab m_Tab = Tab::Markups;

    WeakRef<Button> m_MarkupsTabButton;
    WeakRef<Button> m_ActivityTabButton;
    WeakRef<Button> m_ShowAllButton;
    WeakRef<Button> m_HideAllButton;
    WeakRef<UIElement> m_SearchHost;
    WeakRef<Dropdown> m_StatusFilterDropdown;
    WeakRef<TextField> m_Search;
    WeakRef<ScrollView> m_Scroll;
    WeakRef<UIElement> m_List;
    WeakRef<Label> m_Empty;
    std::string m_StatusFilter; // a status tag id; empty lists every status

    ECS::EntityHandle m_LastClicked{};
    std::chrono::steady_clock::time_point m_LastClickTime{};

    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
    std::chrono::steady_clock::time_point m_LastRebuildTime{};
    std::vector<ECS::EntityHandle> m_ShownSelection; // the selection the rows were built with
};

} // namespace GameEngine::Editor
