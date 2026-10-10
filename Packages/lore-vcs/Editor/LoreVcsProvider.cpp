// Lore VCS provider — registers the Lore integration, its settings tab and
// badge preferences with the editor's VCS provider registry. Lives in the
// lore-vcs Editor-kind package module: the editor core carries no Lore code.

#include "LoreIntegration.h"
#include "LoreSettings.h"

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

void BuildLoreSettingsContent(ScrollView& contentBody)
{
    auto& lore = LoreIntegration::GetInstance();
    auto* activeVCS = EditorVcsProviderRegistry::Get().ActiveIntegration();
    const bool isLore = activeVCS && EditorVcsProviderRegistry::Get().ActiveTypeId() == "lore";

    // ========================================================================
    // Repository Status Section
    // ========================================================================
    VcsSettingsAddRepositoryStatusHeader(&contentBody, "Lore");

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

        auto statusDot = std::make_unique<UIElement>();
        const uint32_t statusDotColor = (isLore && lore.IsRepository()) ? 0xFF00FF00u : 0xFF888888u;
        VcsSettingsApplyStatusDotStyle(statusDot.get(), statusDotColor);
        valueContainer->AddChild(std::move(statusDot));

        auto value = std::make_unique<Label>();
        value->SetText("Lore");
        valueContainer->AddChild(std::move(value));

        section->AddChild(std::move(valueContainer));

        contentBody.AddContent(std::move(section));
    }

    // Current Branch / Revision (read-only display)
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

        if (isLore && lore.IsRepository())
        {
            std::string branch = lore.GetCurrentBranch();
            value->SetText(branch.empty() ? "Unknown" : branch);
        }
        else
        {
            value->SetText("Not a Lore repository");
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

        if (isLore && lore.IsRepository())
        {
            std::filesystem::path repoRoot = lore.GetRepositoryRoot();
            value->SetText(repoRoot.empty() ? "Unknown" : repoRoot.string());
        }
        else
        {
            value->SetText("Not a Lore repository");
        }
        section->AddChild(std::move(value));

        contentBody.AddContent(std::move(section));
    }

    // Remote URL (read-only, from .lore/config.toml)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Remote URL");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);

        std::string remoteUrl = (isLore && lore.IsRepository()) ? lore.GetRemoteUrl() : "";
        value->SetText(remoteUrl.empty() ? "(none)" : remoteUrl);
        section->AddChild(std::move(value));

        contentBody.AddContent(std::move(section));
    }

    // Commit Identity (read-only, from .lore/config.toml)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Commit Identity");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);

        std::string identity = (isLore && lore.IsRepository()) ? lore.GetIdentity() : "";
        value->SetText(identity.empty() ? "(unset)" : identity);
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

    // Detected Lore Executable (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Detected Lore Executable");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);

        std::filesystem::path detectedExe = (activeVCS && isLore) ? activeVCS->GetExecutable() : lore.GetExecutable();
        value->SetText(detectedExe.empty() ? "(not found)" : detectedExe.string());
        section->AddChild(std::move(value));

        contentBody.AddContent(std::move(section));
    }

    // Lore Executable Path (editable)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Lore Executable Path");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        field->AddClass("settings-path-field");
        VcsSettingsApplyMinWidth(field.get(), 150.0f);

        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        std::filesystem::path currentExe = loreSettings.GetLoreExecutable();
        if (currentExe.empty() && activeVCS && isLore)
        {
            currentExe = activeVCS->GetExecutable();
        }
        field->SetValue(currentExe.empty() ? "(auto-detect)" : currentExe.string());

        field->SetOnValueChanged([](const std::string& value) {
            Editor::LoreSettings settings;
            settings.Load();
            if (value.empty() || value == "(auto-detect)")
            {
                settings.SetLoreExecutable({});
            }
            else
            {
                settings.SetLoreExecutable(std::filesystem::path(value));
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

        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        std::filesystem::path currentDiffTool = loreSettings.GetExternalDiffTool();
        field->SetValue(currentDiffTool.empty() ? "" : currentDiffTool.string());

        field->SetOnValueChanged([](const std::string& value) {
            Editor::LoreSettings settings;
            settings.Load();
            settings.SetExternalDiffTool(value.empty() ? std::filesystem::path{} : std::filesystem::path(value));
            settings.Save();
        });

        section->AddChild(std::move(field));
        contentBody.AddContent(std::move(section));

        {
            auto description = std::make_unique<GameEngine::EditorUI::CollapsibleInfoCard>("Path to external diff tool (e.g., /usr/local/bin/meld). Leave empty to use lore diff.");
            VcsSettingsApplyMaxWidth(description.get(), 280.0f);
            contentBody.AddContent(std::move(description));
        }
    }

    // ========================================================================
    // Authentication Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Authentication");
        sectionHeader->AddClass("settings-section-header");
        contentBody.AddContent(std::move(sectionHeader));
    }

    // Login button (runs `lore login <remote_url>` — the CLI owns the credential store)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Sign In");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        auto button = std::make_unique<Button>();
        button->SetText("Login");
        button->AddClass("small");
        button->AddClass("secondary");
        button->AddClass("settings-button");
        button->RegisterEventHandler(kEventButtonClick, [](UIEvent&) {
            auto& loreInstance = LoreIntegration::GetInstance();
            if (loreInstance.IsRepository())
            {
                loreInstance.Login();
            }
        });
        section->AddChild(std::move(button));

        contentBody.AddContent(std::move(section));

        {
            auto description = std::make_unique<GameEngine::EditorUI::CollapsibleInfoCard>("Lore manages its own credentials. Login launches the CLI sign-in flow for the repository's remote.");
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
        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        toggle->SetChecked(loreSettings.GetShowStatusIcons());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::LoreSettings settings;
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
        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        toggle->SetChecked(loreSettings.GetColorWholeText());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::LoreSettings settings;
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
        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        toggle->SetChecked(loreSettings.GetColorDotOnly());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::LoreSettings settings;
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
        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        toggle->SetChecked(loreSettings.GetHideDotForClean());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::LoreSettings settings;
            settings.Load();
            settings.SetHideDotForClean(enabled);
            settings.Save();
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
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

        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        int refreshInterval = loreSettings.GetStatusRefreshInterval();
        field->SetValue(std::to_string(refreshInterval));

        field->SetOnValueChanged([](const std::string& value) {
            try
            {
                int interval = std::stoi(value);
                if (interval > 0)
                {
                    Editor::LoreSettings settings;
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
        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        toggle->SetChecked(loreSettings.GetPromptBeforeCommit());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::LoreSettings settings;
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

        Editor::LoreSettings loreSettings;
        loreSettings.Load();
        field->SetValue(loreSettings.GetDefaultCommitMessage());

        field->SetOnValueChanged([](const std::string& value) {
            Editor::LoreSettings settings;
            settings.Load();
            settings.SetDefaultCommitMessage(value);
            settings.Save();
        });
        section->AddChild(std::move(field));

        contentBody.AddContent(std::move(section));
    }
}

} // namespace

void RegisterLoreVcsProvider()
{
    EditorVcsProviderDescriptor descriptor;
    descriptor.TypeId = "lore";
    descriptor.DisplayName = "Lore";
    descriptor.SettingsRowClass = "lore-row";
    descriptor.StatusColumnTitle = "Lore Status";
    descriptor.DetectionOrder = 30;
    descriptor.ServerBacked = true;
    descriptor.UpdateActionLabel = "Sync";
    descriptor.Detect = [](const std::filesystem::path& projectRoot) {
        // The CLI opens both marker names; `.urc` is the pre-rename spelling
        // that existing working trees still carry.
        std::error_code ec;
        return std::filesystem::is_directory(projectRoot / ".lore", ec) ||
               std::filesystem::is_directory(projectRoot / ".urc", ec);
    };
    descriptor.Integration = []() -> IVCSIntegration& { return LoreIntegration::GetInstance(); };
    descriptor.Initialize = [](const std::filesystem::path& projectRoot,
                               std::function<void()> statusChanged) {
        auto& lore = LoreIntegration::GetInstance();
        LoreSettings settings;
        (void)settings.Load();

        const std::filesystem::path exe = settings.GetLoreExecutable();
        const bool ok = exe.empty() ? lore.Initialize(projectRoot) : lore.Initialize(projectRoot, exe);
        if (!ok)
            return false;

        lore.SetStatusRefreshInterval(settings.GetStatusRefreshInterval());
        lore.SetStatusChangedCallback(std::move(statusChanged));
        return true;
    };
    descriptor.GetBaseContent = [](const std::filesystem::path& filePath) -> std::string {
        return LoreIntegration::GetInstance().ReadCommittedContent(filePath);
    };
    descriptor.OpenExternalDiff = [](const std::filesystem::path& filePath) {
        auto& lore = LoreIntegration::GetInstance();
        if (!lore.IsRepository())
            return;
        LoreSettings settings;
        (void)settings.Load();
        lore.OpenDiff(filePath, settings.GetExternalDiffTool());
    };
    descriptor.BadgeUiSettings = []() {
        LoreSettings settings;
        (void)settings.Load();
        VcsBadgeUiSettings out;
        out.ShowStatusIcons = settings.GetShowStatusIcons();
        out.ColorWholeText = settings.GetColorWholeText();
        out.ColorDotOnly = settings.GetColorDotOnly();
        out.HideDotForClean = settings.GetHideDotForClean();
        return out;
    };
    descriptor.DefaultCommitMessage = []() {
        LoreSettings settings;
        (void)settings.Load();
        return settings.GetDefaultCommitMessage();
    };
    descriptor.BuildSettingsContent = [](ScrollView& contentBody) {
        BuildLoreSettingsContent(contentBody);
    };
    EditorVcsProviderRegistry::Get().RegisterProvider(std::move(descriptor));
}

} // namespace GameEngine::Editor
