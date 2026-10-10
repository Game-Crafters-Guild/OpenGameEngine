#pragma once

#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Projects/ProjectAcquisition.h"
#include "Projects/ProjectCatalogService.h"
#include "UI/UIElement.h"

namespace GameEngine {
class TextField;
class Label;
class Button;
namespace FileSystem {
class TreeRemoval;
}
}

namespace GameEngine {

// Modal dialog for selecting a project folder on editor launch
class ProjectFolderPickerModal : public UIElement {
public:
    using OnFolderSelectedCallback = std::function<void(const std::filesystem::path&)>;
    using OnCancelledCallback = std::function<void()>;

    ProjectFolderPickerModal();
    ~ProjectFolderPickerModal() override;

    void SetOnFolderSelected(OnFolderSelectedCallback cb) { m_OnFolderSelected = std::move(cb); }
    void SetOnCancelled(OnCancelledCallback cb) { m_OnCancelled = std::move(cb); }

    void Show();
    void Hide();
    bool IsVisible() const { return m_Visible; }

    /// Native WKWebView/WebView2 sits above GPU UI; keep it off-screen while this modal is visible or finishing its hide fade.
    bool ShouldSuppressNativeWebView() const { return m_Visible || m_SuppressNativeWebViewAfterHide; }
    
    void ShowLoading();
    void HideLoading();
    
    // Update the displayed path in the modal
    void SetPath(const std::filesystem::path& path);
    
    // Set whether to show buttons (hide if no existing project can be returned to)
    // showCancel: show cancel button when a non-default project is active
    // showOpen: show open button (only when non-default project selected)
    void SetShowButtons(bool showCancel, bool showOpen = false);
    
    // Call this from Update() to process deferred folder loading
    void Update();
    
    // Add a project to the recent projects list
    void AddToRecentProjects(const std::filesystem::path& path);

    // Write the current recent-projects list back to editor preferences.
    void PersistRecentProjects();

    // Remove a recent project: delete its files when it lives under the root the
    // editor owns (the platform's project library, else the managed projects
    // root), otherwise just forget it, then refresh the list.
    void DeleteRecentProject(const std::filesystem::path& path);

    // Load recent projects from preferences
    void LoadRecentProjects();
    
    // Refresh the recent projects list display
    void RefreshRecentProjectsList();

private:
    enum class ImportSourceMode { Folder, Archive, GitUrl };

    void OnOwnerManagerChanged(UIManager* owner) override;
    /// Builds the modal's element tree once, on the first UI tick after attach
    /// or on the first Show(), whichever comes first. Every widget accessor in
    /// this class tolerates a missing tree, so calls that land before the build
    /// record their state and get re-applied at the end of BuildTree().
    void EnsureTreeBuilt();
    void BuildTree();

    void OnBrowseClicked();
    void OnSelectClicked();
    void OnCancelClicked();
    void OnRecentProjectClicked(const std::filesystem::path& path);
    void OnNewProjectClicked();
    void OnNewProjectBrowseClicked();
    void OnCreateProjectClicked();
    void OnImportProjectClicked();
    void OnImportArchiveBrowseClicked();
    void UpdateImportProjectEnabled();
    void ShowLibraryView();
    void ShowNewProjectView();
    void ShowImportProjectView();
    void UpdateImportProjectMode();
    void UpdateNewProjectTabView();
    void UpdateTemplateSelection();
    void BuildTemplateCards();
    void RebuildCommunityCards();
    void UpdateCommunityStatusView();
    void OnCommunityCardClicked(int index);
    void UpdateCreateProjectEnabled();
    Button* AddCatalogCard(UIElement& row, const std::string& elementId,
                           const Editor::ProjectCatalogEntry& entry, const std::string& subtitle,
                           uint32_t fallbackTint, UIElement::EventHandler onClick);

    std::string DefaultNewProjectName() const;
    std::filesystem::path ResolveHomePath() const;
    std::filesystem::path NormalizePath(const std::filesystem::path& path) const;
    std::string NormalizePathKey(const std::filesystem::path& path) const;
    bool IsDefaultProjectPath(const std::filesystem::path& path) const;
    void UpdateButtonsAndLogo(const std::filesystem::path& workspacePath);
    void UpdateLayoutForRecentProjects();
    void SetRecentProjectsVisible(bool visible, bool aggressiveHide);

    bool m_TreeBuilt = false;
    bool m_BuildScheduled = false;
    bool m_Visible = false;
    bool m_SuppressNativeWebViewAfterHide = false;
    float m_HideTimeSecondsForNativeWebView = 0.0f;
    bool m_Loading = false;
    bool m_PendingFolderLoad = false;
    bool m_FadeInPending = false;
    bool m_ShowingNewProject = false;
    bool m_ShowingImportProject = false;
    bool m_ShowingCommunityProjects = false;
    bool m_ShowCancelButton = false;
    int m_SelectedTemplate = 1;
    std::filesystem::path m_SelectedPath;
    std::filesystem::path m_PendingPath;
    OnFolderSelectedCallback m_OnFolderSelected;
    OnCancelledCallback m_OnCancelled;
    
    std::vector<std::filesystem::path> m_RecentProjects;
    std::unordered_map<std::string, int64_t> m_RecentProjectLastOpened;
    // Resolved card-thumbnail path per project (keyed by NormalizePathKey). Filled
    // once per picker session; every refresh after the first — search keystrokes,
    // and the post-delete refresh — reuses it, so the frame loop never repeats the
    // per-card OPFS stats FindProjectSceneThumbnail does on web. Cleared on load.
    std::unordered_map<std::string, std::string> m_ProjectThumbnailPathCache;
    bool m_RecentsLoaded = false; /* Defer LoadRecentProjects until first Show() to avoid blocking startup on Windows/Linux */
    bool m_RefreshRecentsPosted = false;
    UIElement* m_RecentProjectsContainer = nullptr;
    UIElement* m_RecentProjectsScroll = nullptr;
    UIElement* m_ProjectsHeader = nullptr;
    UIElement* m_NewProjectView = nullptr;
    UIElement* m_ImportProjectView = nullptr;
    UIElement* m_TemplateProjectsView = nullptr;
    UIElement* m_CommunityProjectsView = nullptr;
    UIElement* m_Title = nullptr;
    Label* m_ProjectCountLabel = nullptr;
    UIElement* m_SearchPlaceholder = nullptr;
    UIElement* m_BottomPanel = nullptr;
    UIElement* m_Footer = nullptr;
    UIElement* m_LoadingLabel = nullptr;
    UIElement* m_ProjectLogo = nullptr;
    UIElement* m_ButtonContainer = nullptr;
    UIElement* m_CancelButton = nullptr;
    UIElement* m_NewProjectActions = nullptr;
    UIElement* m_ImportProjectActions = nullptr;
    UIElement* m_ImportFolderView = nullptr;
    UIElement* m_ImportArchiveView = nullptr;
    UIElement* m_ImportGitView = nullptr;
    TextField* m_SearchField = nullptr;
    TextField* m_NewProjectNameField = nullptr;
    TextField* m_NewProjectLocationField = nullptr;
    Label* m_NewProjectErrorLabel = nullptr;
    Button* m_CreateProjectButton = nullptr;
    Button* m_TemplatesTabButton = nullptr;
    Button* m_CommunityTabButton = nullptr;
    Button* m_ImportFolderTabButton = nullptr;
    Button* m_ImportArchiveTabButton = nullptr;
    Button* m_ImportGitTabButton = nullptr;
    Button* m_ImportProjectButton = nullptr;
    TextField* m_ImportGitUrlField = nullptr;
    Label* m_ImportGitHint = nullptr;
    Button* m_ImportArchiveDrop = nullptr;
    Label* m_ImportErrorLabel = nullptr;
    UIElement* m_ImportDestinationRow = nullptr;
    TextField* m_ImportDestinationField = nullptr;
    std::filesystem::path m_ImportArchivePath;
    ImportSourceMode m_ImportSourceMode = ImportSourceMode::Folder;
    std::vector<Button*> m_TemplateButtons;

    Editor::ProjectCatalog m_TemplateCatalog;
    std::filesystem::path m_TemplateCatalogRoot;
    Editor::ProjectCatalogService m_CatalogService;
    Editor::ProjectAcquisition m_Acquisition;
    // Deletes the files of a removed editor-owned project; created on the
    // first removal.
    std::unique_ptr<FileSystem::TreeRemoval> m_TreeRemoval;
    Editor::CommunityCatalogSnapshot m_Community;
    int m_SelectedCommunity = -1;
    bool m_CommunityFetchRequested = false;
    bool m_AcquireInFlight = false;
    std::string m_LastAutoFilledName;
    std::vector<Button*> m_CommunityButtons;
    UIElement* m_CommunityGrid = nullptr;
    UIElement* m_CommunityStatusView = nullptr;
    Label* m_CommunityStatusTitle = nullptr;
    Label* m_CommunityStatusMessage = nullptr;
    Button* m_CommunityRetryButton = nullptr;
    UIElement* m_CommunityOfflineBanner = nullptr;
    Label* m_CommunityOfflineLabel = nullptr;
    UIElement* m_StackedScrim = nullptr;

    std::string m_SearchQuery;
    size_t m_FilteredProjectCount = 0;
    float m_LogoHueAnimationTime = 0.0f;
    float m_LastLogoUpdateTime = -1.0f;
};

} // namespace GameEngine
