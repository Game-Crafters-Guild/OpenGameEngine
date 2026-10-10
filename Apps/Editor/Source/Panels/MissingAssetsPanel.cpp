#include "Panels/MissingAssetsPanel.h"

#include "EditorPanelIds.h"
#include "MissingAssetTracker.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/DockTab.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>

namespace GameEngine
{

MissingAssetsPanel::MissingAssetsPanel()
    : DockPanel("Missing Assets")
{
    AddClass("missing-assets-panel");

    auto root = std::make_unique<UIElement>();
    root->AddClass("missing-assets-root");

    auto header = std::make_unique<UIElement>();
    header->AddClass("missing-assets-header");

    auto status = std::make_unique<Label>();
    status->AddClass("missing-assets-status");
    status->SetText("No missing assets detected.");
    m_StatusLabel = status.get();
    header->AddChild(std::move(status));

    auto refresh = std::make_unique<Button>();
    refresh->AddClass("small");
    refresh->AddClass("secondary");
    refresh->AddClass("missing-assets-refresh");
    refresh->SetText("Refresh");
    refresh->SetTooltip("Rescan the active scene for unresolved asset references");
    refresh->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnRefreshClicked(); });
    // Icon as a real child rather than the button's background image: the
    // button is a flex row, so the icon takes its own track and cannot land
    // under the label. Index 0 — Button's constructor already made the label
    // child 0, and the icon belongs ahead of it.
    auto refreshIcon = std::make_unique<UIElement>();
    refreshIcon->AddClass("missing-assets-refresh-icon");
    refresh->InsertChild(0, std::move(refreshIcon));
    m_RefreshButton = refresh.get();
    header->AddChild(std::move(refresh));

    root->AddChild(std::move(header));

    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("missing-assets-scroll");

    auto list = std::make_unique<UIElement>();
    list->AddClass("missing-assets-list");
    m_ListContainer = list.get();

    scroll->AddContent(std::move(list));
    root->AddChild(std::move(scroll));

    AddChild(std::move(root));
}

MissingAssetsPanel::~MissingAssetsPanel()
{
    DetachTracker();
}

void MissingAssetsPanel::DetachTracker()
{
    // Unsubscribing does not join a rescan callback already running, and the
    // action it posted outlives this panel in the dispatcher queue, so the flag
    // — not the unsubscribe — is what keeps a rebuild off a destroyed panel.
    if (m_SubscriptionAlive)
        m_SubscriptionAlive->store(false, std::memory_order_release);
    m_SubscriptionAlive.reset();
    m_TrackerSubscription.Reset();
}

void MissingAssetsPanel::SetTracker(MissingAssetTracker* tracker)
{
    if (m_Tracker == tracker)
        return;
    DetachTracker();
    m_Tracker = tracker;
    if (m_Tracker)
    {
        m_SubscriptionAlive = std::make_shared<std::atomic_bool>(true);
        // Defer rebuild via PostAction so we don't mutate the UI tree from a
        // listener that may itself fire during a UI-tree walk.
        m_TrackerSubscription = m_Tracker->AddListener([this, alive = m_SubscriptionAlive]()
        {
            if (!alive->load(std::memory_order_acquire))
                return;
            this->PostAction([this, alive]()
            {
                if (!alive->load(std::memory_order_acquire))
                    return;
                this->RebuildList();
            });
        });
    }
    RebuildList();
}

void MissingAssetsPanel::OnRefreshClicked()
{
    if (!m_Tracker)
    {
        Logger::Log::Info("MissingAssetsPanel: no tracker bound; refresh skipped.");
        return;
    }
    m_Tracker->RescanActive();
}

void MissingAssetsPanel::RebuildList()
{
    if (!m_ListContainer)
        return;
    // Drain children one-by-one — UIElement has no clear-all helper.
    m_ListContainer->RemoveAllChildren();

    if (!m_Tracker)
    {
        if (m_StatusLabel)
            m_StatusLabel->SetText("Tracker not bound.");
        return;
    }

    const auto entries = m_Tracker->GetMissing();
    if (m_StatusLabel)
    {
        if (entries.empty())
            m_StatusLabel->SetText("No missing assets detected.");
        else
            m_StatusLabel->SetText(std::to_string(entries.size()) + " missing asset reference(s):");
    }

    // Push count badge into the dock tab. The tab is a child of the docking
    // root, looked up by id "tab:<panelId>" — there's no automatic title→tab
    // sync today, so we mutate the tab's Label directly. Falls back silently
    // if the tab hasn't materialized yet (panel not docked / not opened).
    const std::string baseTitle = "Missing Assets";
    const std::string tabText = entries.empty()
                                    ? baseTitle
                                    : baseTitle + " (" + std::to_string(entries.size()) + ")";
    SetTitle(tabText);
    if (auto* mgr = GetOwnerManager())
    {
        if (auto* root = mgr->GetRootElement())
        {
            if (auto* tab = dynamic_cast<DockTab*>(root->FindById(std::string("tab:") + EditorPanelIds::MissingAssets)))
                tab->SetText(tabText);
        }
    }

    for (const auto& e : entries)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("missing-assets-row");

        // Header: entity name + component.property
        std::string header = e.EntityTag.empty() ? std::string("<entity>") : e.EntityTag;
        header += "  •  ";
        header += e.ComponentName;
        if (!e.PropertyName.empty())
            header += "." + e.PropertyName;
        auto headerLabel = std::make_unique<Label>();
        headerLabel->AddClass("missing-assets-row-title");
        headerLabel->SetText(header);
        row->AddChild(std::move(headerLabel));

        // Path line: what the reference pointed at, best source first. The
        // authored path comes from the scene text (only schemas that keep it
        // supply one); the last-known path comes from the registry's tombstone
        // for a GUID this project once had. Neither exists for a reference
        // authored in another project — say so rather than showing a blank row.
        std::string pathText;
        if (!e.AuthoredPath.empty())
            pathText = "path: " + e.AuthoredPath;
        else if (!e.LastKnownPath.empty())
            pathText = "last known path: " + e.LastKnownPath;
        else
            pathText = "last known path: unknown (no registry record)";
        auto pathLabel = std::make_unique<Label>();
        pathLabel->AddClass("missing-assets-row-path");
        pathLabel->SetText(pathText);
        pathLabel->SetTooltip(pathText);
        row->AddChild(std::move(pathLabel));

        auto guidLabel = std::make_unique<Label>();
        guidLabel->AddClass("missing-assets-row-guid");
        guidLabel->SetText("guid: " + e.Guid.ToString());
        row->AddChild(std::move(guidLabel));

        m_ListContainer->AddChild(std::move(row));
    }
}

} // namespace GameEngine
