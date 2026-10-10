// Git VCS provider — registers the Git integration, its settings tab, badge
// preferences and context-menu extras with the editor's VCS provider
// registry. Lives in the git-vcs Editor-kind package module: the editor core
// carries no Git code.

#include "GitSettings.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsSettingsUi.h"
#include "GitCommandExecutor.h"
#include "GitIntegration.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/InfoCard.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace GameEngine::Editor
{
namespace
{

constexpr uint32_t kGitMenuPush = 1;

void BuildGitSettingsContent(ScrollView& contentBody)
{
    // Get Git integration instance
    auto& git = GitIntegration::GetInstance();
    auto* activeVCS = EditorVcsProviderRegistry::Get().ActiveIntegration();
    const bool isGit = activeVCS && EditorVcsProviderRegistry::Get().ActiveTypeId() == "git";
    
    // ========================================================================
    // Repository Status Section
    // ========================================================================
    VcsSettingsAddRepositoryStatusHeader(&contentBody, "Git");
    
    // VCS Type with Status Indicator (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Version Control");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto valueContainer = std::make_unique<UIElement>();
        valueContainer->AddClass("settings-row-value");
        VcsSettingsApplyValueContainerStyle(valueContainer.get());
        
        // Status indicator dot - only show for Git
        bool isGitActive = (isGit && git.IsRepository());
        auto statusDot = std::make_unique<UIElement>();
        const uint32_t statusDotColor = isGitActive ? 0xFF00FF00u : 0xFF888888u;
        VcsSettingsApplyStatusDotStyle(statusDot.get(), statusDotColor);
        valueContainer->AddChild(std::move(statusDot));
        
        auto value = std::make_unique<Label>();
        value->SetText("Git");
        valueContainer->AddChild(std::move(value));
        
        section->AddChild(std::move(valueContainer));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Current Branch (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Current Branch");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        if (isGit && git.IsRepository())
        {
            std::string branch = git.GetCurrentBranch();
            if (branch.empty())
                branch = "(unknown)";
            value->SetText(branch);
        }
        else
        {
            value->SetText("(not a git repository)");
        }
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Repository Root (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Repository Root");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        if (isGit && git.IsRepository())
        {
            value->SetText(git.GetRepositoryRoot().string());
        }
        else
        {
            value->SetText("(not a git repository)");
        }
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // ========================================================================
    // VCS Configuration Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Configuration");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Detected Git Executable (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Detected Git Executable");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        std::filesystem::path detectedExe;
        if (activeVCS && isGit)
        {
            detectedExe = activeVCS->GetExecutable();
        }
        else
        {
            detectedExe = git.GetExecutable();
        }
        
        value->SetText(detectedExe.empty() ? "(not found)" : detectedExe.string());
        section->AddChild(std::move(value));
        contentBody.AddContent(std::move(section));
    }
    
    // Git Executable Path (editable)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Git Executable Path");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        field->AddClass("settings-path-field");
        VcsSettingsApplyMinWidth(field.get(), 150.0f);
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        std::filesystem::path currentExe = gitSettings.GetGitExecutable();
        if (currentExe.empty() && activeVCS && isGit)
        {
            currentExe = activeVCS->GetExecutable();
        }
        field->SetValue(currentExe.empty() ? "(auto-detect)" : currentExe.string());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::GitSettings settings;
            settings.Load();
            if (value.empty() || value == "(auto-detect)")
            {
                settings.SetGitExecutable({});
            }
            else
            {
                settings.SetGitExecutable(std::filesystem::path(value));
            }
            settings.Save();
        });
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Description
    {
        auto description = std::make_unique<GameEngine::EditorUI::CollapsibleInfoCard>("Leave empty to auto-detect git executable from PATH.");
        contentBody.AddContent(std::move(description));
    }
    
    // External Diff Tool Path (Git only)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("External Diff Tool");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        field->AddClass("settings-path-field");
        VcsSettingsApplyMinWidth(field.get(), 150.0f);
        
        // Load current diff tool from settings
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        std::filesystem::path currentDiffTool = gitSettings.GetExternalDiffTool();
        field->SetValue(currentDiffTool.empty() ? "" : currentDiffTool.string());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetExternalDiffTool(value.empty() ? std::filesystem::path{} : std::filesystem::path(value));
            settings.Save();
        });
        
        section->AddChild(std::move(field));
        contentBody.AddContent(std::move(section));
        
        // Description
        {
            auto description = std::make_unique<GameEngine::EditorUI::CollapsibleInfoCard>("Path to external diff tool (e.g., /usr/local/bin/meld, /Applications/Beyond Compare.app/Contents/MacOS/bcomp). Leave empty to use git's configured difftool.");
            VcsSettingsApplyMaxWidth(description.get(), 280.0f);
            contentBody.AddContent(std::move(description));
        }
    }
    
    // ========================================================================
    // Display Options Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Display Options");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Show Status Icons toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetShowStatusIcons());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetShowStatusIcons(enabled);
            settings.Save();
            
            // Notify to refresh asset browser views
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Show Status Icons");
        label->SetTooltip("Show Status Icons in Asset Browser");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Color whole text toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetColorWholeText());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetColorWholeText(enabled);
            settings.Save();
            
            // Notify to refresh asset browser views
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Color Whole Text");
        label->SetTooltip("Color Whole Text (not just dot)");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Hide Dot for Clean Status
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetHideDotForClean());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetHideDotForClean(enabled);
            settings.Save();
            
            // Notify to refresh asset browser views
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Hide Dot for Clean Status");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // ========================================================================
    // Auto Operations Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Auto Operations");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Auto-fetch toggle (Git only)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetAutoFetch());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetAutoFetch(enabled);
            settings.Save();
            // Also update the GitIntegration instance
            GitIntegration::GetInstance().SetAutoFetch(enabled, settings.GetFetchInterval());
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-fetch Remote Changes");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Auto-add on create
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetAutoAddOnCreate());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetAutoAddOnCreate(enabled);
            settings.Save();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-add on Create");
        label->SetTooltip("Auto-add files when created");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Auto-remove on delete
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetAutoRemoveOnDelete());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetAutoRemoveOnDelete(enabled);
            settings.Save();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-remove on Delete");
        label->SetTooltip("Auto-remove files when deleted");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Auto-move on rename
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetAutoMoveOnRename());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetAutoMoveOnRename(enabled);
            settings.Save();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-move on Rename");
        label->SetTooltip("Auto-move files when renamed");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // ========================================================================
    // Status & Update Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Status & Update");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Status refresh interval
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Status Refresh Interval");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        int refreshInterval = gitSettings.GetStatusRefreshInterval();
        field->SetValue(std::to_string(refreshInterval));
        
        field->SetOnValueChanged([](const std::string& value) {
            try {
                int interval = std::stoi(value);
                if (interval > 0) {
                    Editor::GitSettings settings;
                    settings.Load();
                    settings.SetStatusRefreshInterval(interval);
                    settings.Save();
                }
            } catch (...) {
                // Invalid input, ignore
            }
        });
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
    }
    
    // ========================================================================
    // Commit Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Commit");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Prompt before commit
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        toggle->SetChecked(gitSettings.GetPromptBeforeCommit());
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetPromptBeforeCommit(enabled);
            settings.Save();
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Prompt before committing");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Default commit message
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Default Commit Message");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        Editor::GitSettings gitSettings;
        gitSettings.Load();
        field->SetValue(gitSettings.GetDefaultCommitMessage());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::GitSettings settings;
            settings.Load();
            settings.SetDefaultCommitMessage(value);
            settings.Save();
        });
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
    }
}

} // namespace

void RegisterGitVcsProvider()
{
    EditorVcsProviderDescriptor descriptor;
    descriptor.TypeId = "git";
    descriptor.DisplayName = "Git";
    descriptor.SettingsRowClass = "git-row";
    descriptor.StatusColumnTitle = "Git Status";
    descriptor.DetectionOrder = 0;
    descriptor.ServerBacked = false;
    descriptor.UpdateActionLabel = "Pull";
    descriptor.Detect = [](const std::filesystem::path& projectRoot) {
        std::error_code ec;
        const std::filesystem::path marker = projectRoot / ".git";
        // A .git FILE is a valid worktree/submodule marker.
        return std::filesystem::exists(marker, ec) &&
               (std::filesystem::is_directory(marker, ec) ||
                std::filesystem::is_regular_file(marker, ec));
    };
    descriptor.Integration = []() -> IVCSIntegration& { return GitIntegration::GetInstance(); };
    descriptor.Initialize = [](const std::filesystem::path& projectRoot,
                               std::function<void()> statusChanged) {
        auto& git = GitIntegration::GetInstance();
        GitSettings settings;
        (void)settings.Load();

        const std::filesystem::path exe = settings.GetGitExecutable();
        const bool ok = exe.empty() ? git.Initialize(projectRoot) : git.Initialize(projectRoot, exe);
        if (!ok)
            return false;

        if (settings.GetAutoFetch())
            git.SetAutoFetch(true, settings.GetFetchInterval());
        git.SetStatusRefreshInterval(settings.GetStatusRefreshInterval());
        git.SetStatusChangedCallback(std::move(statusChanged));
        return true;
    };
    descriptor.GetBaseContent = [](const std::filesystem::path& filePath) -> std::string {
        auto& git = GitIntegration::GetInstance();
        if (!git.IsRepository())
            return {};

        std::error_code ec;
        const std::filesystem::path relPath =
            std::filesystem::relative(filePath, git.GetRepositoryRoot(), ec);
        if (ec)
            return {};

        const std::vector<std::string> showArgs = {"show", "HEAD:" + relPath.generic_string()};
        auto result = GitCommandExecutor::Execute(git.GetExecutable(), git.GetRepositoryRoot(),
                                                  showArgs, true);
        if (!result.success)
        {
            Logger::Log::Warning("Git: show failed for {}", relPath.string());
            return {};
        }
        return result.output;
    };
    descriptor.OpenExternalDiff = [](const std::filesystem::path& filePath) {
        auto& git = GitIntegration::GetInstance();
        if (!git.IsRepository())
            return;
        GitSettings settings;
        (void)settings.Load();
        git.OpenDiff(filePath, settings.GetExternalDiffTool());
    };
    descriptor.BadgeUiSettings = []() {
        GitSettings settings;
        (void)settings.Load();
        VcsBadgeUiSettings out;
        out.ShowStatusIcons = settings.GetShowStatusIcons();
        out.ColorWholeText = settings.GetColorWholeText();
        out.ColorDotOnly = settings.GetColorDotOnly();
        out.HideDotForClean = settings.GetHideDotForClean();
        return out;
    };
    descriptor.DefaultCommitMessage = []() {
        GitSettings settings;
        (void)settings.Load();
        return settings.GetDefaultCommitMessage();
    };
    descriptor.CollectMenuItems = [](bool isDirectory, std::vector<VcsMenuItem>& outItems) {
        if (isDirectory)
            outItems.push_back({"Push", kGitMenuPush});
    };
    descriptor.HandleMenuCommand = [](uint32_t localId, const std::filesystem::path&) {
        if (localId != kGitMenuPush)
            return;
        auto& git = GitIntegration::GetInstance();
        if (git.IsRepository())
        {
            git.Push();
            Logger::Log::Info("Git: Pushed changes");
        }
    };
    descriptor.BuildSettingsContent = [](ScrollView& contentBody) {
        BuildGitSettingsContent(contentBody);
    };
    EditorVcsProviderRegistry::Get().RegisterProvider(std::move(descriptor));
}

} // namespace GameEngine::Editor
