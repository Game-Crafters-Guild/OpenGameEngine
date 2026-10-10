#pragma once

#include "MissingAssetTracker.h"
#include "UI/Controls/DockPanel.h"

#include <atomic>
#include <memory>

namespace GameEngine
{

class Label;
class Button;

// Lists scene asset references whose GUID is unknown to the AssetRegistry.
// Repopulates whenever the tracker fires a listener event (typically after a
// scene load or an explicit Refresh click).
class MissingAssetsPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "alert-circle-icon"; }

    MissingAssetsPanel();
    ~MissingAssetsPanel() override;

    // Called by EditorApplication during panel construction. The tracker
    // outlives all panels (owned by EditorApplication).
    void SetTracker(MissingAssetTracker* tracker);

  private:
    // Drop the tracker subscription and disarm any callback already in flight.
    void DetachTracker();
    void OnRefreshClicked();
    void RebuildList();

    MissingAssetTracker* m_Tracker = nullptr;
    MissingAssetTracker::Subscription m_TrackerSubscription;
    // Cleared before this panel dies. Unsubscribing does not join a rescan
    // callback already running, and the action it posts outlives the panel in
    // the dispatcher queue, so this — not the unsubscribe — is what keeps a
    // rebuild off a destroyed panel.
    std::shared_ptr<std::atomic_bool> m_SubscriptionAlive;

    Label*    m_StatusLabel = nullptr;
    Button*   m_RefreshButton = nullptr;
    UIElement* m_ListContainer = nullptr;
};

} // namespace GameEngine
