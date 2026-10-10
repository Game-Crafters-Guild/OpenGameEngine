// SVN VCS provider — registers the SVN integration, its settings tab and
// badge preferences with the editor's VCS provider registry. Lives in the
// svn-vcs Editor-kind package module: the editor core carries no SVN code.

#include "SVNCommandExecutor.h"
#include "SVNIntegration.h"
#include "SVNSettings.h"

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsSettingsUi.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
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

void BuildSvnSettingsContent(ScrollView& contentBody)
{
    // Get SVN integration instance
    auto& svn = SVNIntegration::GetInstance();
    auto* activeVCS = EditorVcsProviderRegistry::Get().ActiveIntegration();
    const bool isSVN = activeVCS && EditorVcsProviderRegistry::Get().ActiveTypeId() == "svn";
    
    // ========================================================================
    // Repository Status Section
    // ========================================================================
    VcsSettingsAddRepositoryStatusHeader(&contentBody, "SVN");
    
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
        
        // Status indicator dot
        auto statusDot = std::make_unique<UIElement>();
        const uint32_t statusDotColor = (isSVN && svn.IsRepository()) ? 0xFF00FF00u : 0xFF888888u;
        VcsSettingsApplyStatusDotStyle(statusDot.get(), statusDotColor);
        valueContainer->AddChild(std::move(statusDot));
        
        auto value = std::make_unique<Label>();
        value->SetText("SVN");
        valueContainer->AddChild(std::move(value));
        
        section->AddChild(std::move(valueContainer));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Current Revision (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Current Revision");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        if (isSVN && svn.IsRepository())
        {
            std::string revision = svn.GetCurrentBranch();
            value->SetText(revision.empty() ? "Unknown" : revision);
        }
        else
        {
            value->SetText("Not an SVN repository");
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
        
        if (isSVN && svn.IsRepository())
        {
            std::filesystem::path repoRoot = svn.GetRepositoryRoot();
            value->SetText(repoRoot.empty() ? "Unknown" : repoRoot.string());
        }
        else
        {
            value->SetText("Not an SVN repository");
        }
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // ========================================================================
    // Configuration Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Configuration");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Detected SVN Executable (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Detected SVN Executable");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        std::filesystem::path detectedExe;
        if (activeVCS && isSVN)
        {
            detectedExe = activeVCS->GetExecutable();
        }
        else
        {
            // Try to find SVN executable even if not initialized
            detectedExe = svn.GetExecutable();
        }
        
        value->SetText(detectedExe.empty() ? "(not found)" : detectedExe.string());
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // SVN Executable Path (editable)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("SVN Executable Path");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        field->AddClass("settings-path-field");
        VcsSettingsApplyMinWidth(field.get(), 150.0f);
        
        // Load current executable from settings
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        std::filesystem::path currentExe = svnSettings.GetSVNExecutable();
        if (currentExe.empty() && activeVCS && isSVN)
        {
            currentExe = activeVCS->GetExecutable();
        }
        field->SetValue(currentExe.empty() ? "(auto-detect)" : currentExe.string());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::SVNSettings settings;
            settings.Load();
            if (value.empty() || value == "(auto-detect)")
            {
                settings.SetSVNExecutable({});
            }
            else
            {
                settings.SetSVNExecutable(std::filesystem::path(value));
            }
            settings.Save();
        });
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
    }
    
    // External Diff Tool Path
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
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        std::filesystem::path currentDiffTool = svnSettings.GetExternalDiffTool();
        field->SetValue(currentDiffTool.empty() ? "" : currentDiffTool.string());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetExternalDiffTool(value.empty() ? std::filesystem::path{} : std::filesystem::path(value));
            settings.Save();
        });
        
        section->AddChild(std::move(field));
        contentBody.AddContent(std::move(section));
        
        // Description
        {
            auto description = std::make_unique<GameEngine::EditorUI::CollapsibleInfoCard>("Path to external diff tool (e.g., /usr/local/bin/meld, /Applications/Beyond Compare.app/Contents/MacOS/bcomp). Leave empty to use svn diff.");
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
    
    // Show Status Icons
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Show Status Icons");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetShowStatusIcons());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetShowStatusIcons(enabled);
            settings.Save();
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Color Whole Text
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Color Whole Text");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetColorWholeText());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetColorWholeText(enabled);
            settings.Save();
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Color Dot Only
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Color Dot Only");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetColorDotOnly());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetColorDotOnly(enabled);
            settings.Save();
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Hide Dot for Clean Status
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Hide Dot for Clean");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetHideDotForClean());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetHideDotForClean(enabled);
            settings.Save();
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        section->AddChild(std::move(toggle));
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
    
    // Auto Add
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-add on Create");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetAutoAddOnCreate());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetAutoAddOnCreate(enabled);
            settings.Save();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Auto Remove
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-remove on Delete");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetAutoRemoveOnDelete());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetAutoRemoveOnDelete(enabled);
            settings.Save();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Auto Move
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Auto-move on Rename");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetAutoMoveOnRename());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetAutoMoveOnRename(enabled);
            settings.Save();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Status Refresh Interval
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Status Refresh Interval");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        VcsSettingsApplyMinWidth(field.get(), 80.0f);
        
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        int refreshInterval = svnSettings.GetStatusRefreshInterval();
        field->SetValue(std::to_string(refreshInterval));
        
        field->SetOnValueChanged([](const std::string& value) {
            try
            {
                int interval = std::stoi(value);
                if (interval > 0)
                {
                    Editor::SVNSettings settings;
                    settings.Load();
                    settings.SetStatusRefreshInterval(interval);
                    settings.Save();
                }
            }
            catch (...)
            {
                // Invalid input, ignore
            }
        });
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
    }
    
    // ========================================================================
    // Commit Settings Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Commit Settings");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }
    
    // Prompt Before Commit
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Prompt Before Commit");
        label->AddClass("settings-row-label");
        
        auto toggle = std::make_unique<Toggle>();
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        toggle->SetChecked(svnSettings.GetPromptBeforeCommit());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetPromptBeforeCommit(enabled);
            settings.Save();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Default Commit Message
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Default Commit Message");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        VcsSettingsApplyMinWidth(field.get(), 200.0f);
        
        Editor::SVNSettings svnSettings;
        svnSettings.Load();
        field->SetValue(svnSettings.GetDefaultCommitMessage());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::SVNSettings settings;
            settings.Load();
            settings.SetDefaultCommitMessage(value);
            settings.Save();
        });
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
    }
}

} // namespace

void RegisterSvnVcsProvider()
{
    EditorVcsProviderDescriptor descriptor;
    descriptor.TypeId = "svn";
    descriptor.DisplayName = "SVN";
    descriptor.SettingsRowClass = "svn-row";
    descriptor.StatusColumnTitle = "SVN Status";
    descriptor.DetectionOrder = 10;
    descriptor.ServerBacked = false;
    descriptor.UpdateActionLabel = "Update";
    descriptor.Detect = [](const std::filesystem::path& projectRoot) {
        std::error_code ec;
        const std::filesystem::path marker = projectRoot / ".svn";
        return std::filesystem::exists(marker, ec) && std::filesystem::is_directory(marker, ec);
    };
    descriptor.Integration = []() -> IVCSIntegration& { return SVNIntegration::GetInstance(); };
    descriptor.Initialize = [](const std::filesystem::path& projectRoot,
                               std::function<void()> statusChanged) {
        auto& svn = SVNIntegration::GetInstance();
        SVNSettings settings;
        (void)settings.Load();

        const std::filesystem::path exe = settings.GetSVNExecutable();
        const bool ok = exe.empty() ? svn.Initialize(projectRoot) : svn.Initialize(projectRoot, exe);
        if (!ok)
            return false;

        svn.SetStatusRefreshInterval(settings.GetStatusRefreshInterval());
        svn.SetStatusChangedCallback(std::move(statusChanged));
        return true;
    };
    descriptor.GetBaseContent = [](const std::filesystem::path& filePath) -> std::string {
        auto& svn = SVNIntegration::GetInstance();
        if (!svn.IsRepository())
            return {};

        std::error_code ec;
        const std::filesystem::path relPath =
            std::filesystem::relative(filePath, svn.GetRepositoryRoot(), ec);
        if (ec)
            return {};

        const std::vector<std::string> catArgs = {"cat", "-r", "BASE", relPath.generic_string()};
        auto result = SVNCommandExecutor::Execute(svn.GetExecutable(), svn.GetRepositoryRoot(),
                                                  catArgs, true);
        if (!result.success)
        {
            Logger::Log::Warning("SVN: cat failed for {}", relPath.string());
            return {};
        }
        return result.output;
    };
    descriptor.OpenExternalDiff = [](const std::filesystem::path& filePath) {
        auto& svn = SVNIntegration::GetInstance();
        if (!svn.IsRepository())
            return;
        SVNSettings settings;
        (void)settings.Load();
        svn.OpenDiff(filePath, settings.GetExternalDiffTool());
    };
    descriptor.BadgeUiSettings = []() {
        SVNSettings settings;
        (void)settings.Load();
        VcsBadgeUiSettings out;
        out.ShowStatusIcons = settings.GetShowStatusIcons();
        out.ColorWholeText = settings.GetColorWholeText();
        out.ColorDotOnly = settings.GetColorDotOnly();
        out.HideDotForClean = settings.GetHideDotForClean();
        return out;
    };
    descriptor.DefaultCommitMessage = []() {
        SVNSettings settings;
        (void)settings.Load();
        return settings.GetDefaultCommitMessage();
    };
    descriptor.BuildSettingsContent = [](ScrollView& contentBody) {
        BuildSvnSettingsContent(contentBody);
    };
    EditorVcsProviderRegistry::Get().RegisterProvider(std::move(descriptor));
}

} // namespace GameEngine::Editor
