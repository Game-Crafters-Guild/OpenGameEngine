#include "VersionControl/Ui/VcsLogViewer.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include <sstream>

namespace GameEngine::Editor {

VcsLogViewer::VcsLogViewer() = default;
VcsLogViewer::~VcsLogViewer() = default;

void VcsLogViewer::Show(GameEngine::UIManager* uiManager, GameEngine::IVCSIntegration* vcs,
                        const std::string& vcsDisplayName, const std::filesystem::path& filePath)
{
    if (!uiManager)
    {
        Logger::Log::Error("VcsLogViewer: UIManager is null");
        return;
    }
    if (!vcs)
    {
        Logger::Log::Error("VcsLogViewer: no active VCS integration");
        return;
    }

    Hide(); // re-Show replaces the panel instead of stacking a second one

    m_UIManager = uiManager;
    m_Vcs = vcs;
    m_VcsDisplayName = vcsDisplayName;
    m_FilePath = filePath;
    m_IsVisible = true;

    BuildUI(uiManager);
    UpdateLogEntries();
}

void VcsLogViewer::Hide()
{
    m_IsVisible = false;
    if (m_PanelRoot && m_UIManager)
    {
        if (auto* root = m_UIManager->GetRootElement())
        {
            root->RemoveChild(m_PanelRoot);
        }
    }
    m_PanelRoot = nullptr;
    m_UIManager = nullptr;
    m_Vcs = nullptr;
}

void VcsLogViewer::Refresh()
{
    if (m_IsVisible)
    {
        UpdateLogEntries();
    }
}

void VcsLogViewer::BuildUI(GameEngine::UIManager* uiManager)
{
    // Root is a full-screen backdrop that centers a bounded window (the
    // editor's modal idiom; see VcsDialog.css). Without the stylesheet the
    // root would flow as an unstyled full-width strip.
    auto panelRoot = std::make_unique<GameEngine::UIElement>("div");
    m_PanelRoot = panelRoot.get();
    m_PanelRoot->SetId("vcs-log-viewer");
    m_PanelRoot->AddClass("vcs-modal");
    m_PanelRoot->AddClass("vcs-log-viewer");
    m_PanelRoot->RequestSubtreeStyleAssetPath("UI/controls/VcsDialog.css", "editor");
    m_PanelRoot->SetFocusable(true);
    m_PanelRoot->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            Hide();
        }
    });

    // Bounded window that holds the header and the (scrolling) entry list.
    auto window = std::make_unique<GameEngine::UIElement>("div");
    auto* windowPtr = window.get();
    windowPtr->AddClass("modal-window");
    windowPtr->AddClass("vcs-modal-window");

    // Header row: title on the left, actions (Refresh / Close) on the right.
    auto header = std::make_unique<GameEngine::UIElement>("div");
    auto* headerPtr = header.get();
    headerPtr->AddClass("vcs-modal-header");

    auto title = std::make_unique<GameEngine::Label>();
    title->SetId("log-title");
    title->SetText((m_VcsDisplayName.empty() ? "VCS" : m_VcsDisplayName) + " Log" +
                   (m_FilePath.empty() ? "" : (" - " + m_FilePath.filename().string())));
    title->AddClass("vcs-modal-title");
    headerPtr->AddChild(std::move(title));

    auto actions = std::make_unique<GameEngine::UIElement>("div");
    auto* actionsPtr = actions.get();
    actionsPtr->AddClass("vcs-modal-header-actions");

    // Refresh button
    auto refreshBtn = std::make_unique<GameEngine::Button>();
    refreshBtn->SetText("Refresh");
    refreshBtn->SetId("refresh-button");
    refreshBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Refresh(); });
    actionsPtr->AddChild(std::move(refreshBtn));

    // Close button (same dismiss idiom as VcsCommitDialog's cancel)
    auto closeBtn = std::make_unique<GameEngine::Button>();
    closeBtn->SetText("Close");
    closeBtn->SetId("close-button");
    closeBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Hide(); });
    actionsPtr->AddChild(std::move(closeBtn));

    headerPtr->AddChild(std::move(actions));
    windowPtr->AddChild(std::move(header));

    // Log entries container
    auto container = std::make_unique<GameEngine::UIElement>("div");
    container->SetId("log-entries-container");
    container->AddClass("log-entries");
    windowPtr->AddChild(std::move(container));

    m_PanelRoot->AddChild(std::move(window));

    // Add to UI root (the root owns the element; m_PanelRoot stays as the
    // non-owning handle for UpdateLogEntries/Hide).
    if (auto* root = uiManager->GetRootElement())
    {
        root->AddChild(std::move(panelRoot));
        uiManager->PostToUI([uiManager, this]()
        {
            if (m_PanelRoot && m_UIManager == uiManager)
                uiManager->FocusElement(m_PanelRoot);
        });
    }
    else
    {
        m_PanelRoot = nullptr;
    }
}

void VcsLogViewer::UpdateLogEntries()
{
    if (!m_PanelRoot || !m_UIManager)
    {
        return;
    }

    if (!m_Vcs || !m_Vcs->IsRepository())
    {
        return;
    }

    auto entries = m_Vcs->GetLog(m_FilePath, 100);

    // Find container
    if (auto* container = m_PanelRoot->FindById("log-entries-container"))
    {
        // Clear existing entries
        container->RemoveAllChildren();

        // Add log entries
        for (const auto& entry : entries)
        {
            auto entryEl = std::make_unique<GameEngine::UIElement>("div");
            entryEl->AddClass("log-entry");

            // Revision
            auto hashLabel = std::make_unique<GameEngine::Label>();
            hashLabel->SetText(entry.revision.substr(0, 8));
            hashLabel->AddClass("log-hash");
            entryEl->AddChild(std::move(hashLabel));

            // Author and date
            auto metaLabel = std::make_unique<GameEngine::Label>();
            metaLabel->SetText(entry.author + " - " + entry.date);
            metaLabel->AddClass("log-meta");
            entryEl->AddChild(std::move(metaLabel));

            // Message
            auto msgLabel = std::make_unique<GameEngine::Label>();
            msgLabel->SetText(entry.message);
            msgLabel->AddClass("log-message");
            entryEl->AddChild(std::move(msgLabel));

            container->AddChild(std::move(entryEl));
        }
    }
}

} // namespace GameEngine::Editor
