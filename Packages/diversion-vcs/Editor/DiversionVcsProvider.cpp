// Diversion VCS provider — registers the Diversion integration, its settings
// tab and badge preferences with the editor's VCS provider registry. Lives in
// the diversion-vcs Editor-kind package module: the editor core carries no
// Diversion code.

#include "DiversionCommandExecutor.h"
#include "DiversionIntegration.h"
#include "DiversionSettings.h"

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsSettingsUi.h"
#include "Input/KeyCodes.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/StyleProperties.h"
#include "UI/InfoCard.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

// The token field displays a stored token as bullets, and the widget hands that
// display string back on change. Mask characters are therefore presentation and
// never token data. Strip them wherever the field's value is read: matching the
// whole placeholder, or a run of four bullets, lets a single leftover bullet
// through, and the server rejects the result with invalid_grant.
const std::string kDiversionTokenMask = "\xE2\x80\xA2"; // U+2022 BULLET
const std::string kDiversionTokenNotSet = "(not set)";

std::string CleanDiversionToken(const std::string& raw)
{
    const size_t maskLength = kDiversionTokenMask.size();
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (size_t i = 0; i < raw.size();)
    {
        if (raw.compare(i, maskLength, kDiversionTokenMask) == 0)
        {
            i += maskLength;
            continue;
        }
        cleaned.push_back(raw[i]);
        ++i;
    }

    if (cleaned == kDiversionTokenNotSet)
        cleaned.clear();

    const size_t first = cleaned.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = cleaned.find_last_not_of(" \t\r\n");
    return cleaned.substr(first, last - first + 1);
}

void BuildDiversionSettingsContent(ScrollView& contentBody)
{
    // Get Diversion integration instance
    auto& diversion = DiversionIntegration::GetInstance();
    auto* activeVCS = EditorVcsProviderRegistry::Get().ActiveIntegration();
    const bool isDiversion = activeVCS && EditorVcsProviderRegistry::Get().ActiveTypeId() == "diversion";
    
    // ========================================================================
    // Repository Status Section
    // ========================================================================
    VcsSettingsAddRepositoryStatusHeader(&contentBody, "Diversion");
    
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
        const uint32_t statusDotColor = (isDiversion && diversion.IsRepository()) ? 0xFF00FF00u : 0xFF888888u;
        VcsSettingsApplyStatusDotStyle(statusDot.get(), statusDotColor);
        valueContainer->AddChild(std::move(statusDot));
        
        auto value = std::make_unique<Label>();
        value->SetText("Diversion");
        valueContainer->AddChild(std::move(value));
        
        section->AddChild(std::move(valueContainer));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Status (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Status");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        // Check if token is set
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        std::string token = dvSettings.GetRefreshToken();
        
        if (token.empty())
        {
            value->SetText("configure!");
        }
        else if (isDiversion && diversion.IsRepository())
        {
            value->SetText("active");
        }
        else
        {
            value->SetText("not active");
        }
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Current Workspace (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Current Workspace");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        if (isDiversion && diversion.IsRepository())
        {
            std::string workspaceId = diversion.GetWorkspaceId();
            value->SetText(workspaceId.empty() ? "Unknown" : workspaceId);
        }
        else
        {
            value->SetText("Not a Diversion repository");
        }
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Repository ID (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Repository ID");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        if (isDiversion && diversion.IsRepository())
        {
            std::string repoId = diversion.GetRepoId();
            value->SetText(repoId.empty() ? "Unknown" : repoId);
        }
        else
        {
            value->SetText("Not a Diversion repository");
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
        
        if (isDiversion && diversion.IsRepository())
        {
            std::filesystem::path repoRoot = diversion.GetRepositoryRoot();
            value->SetText(repoRoot.empty() ? "Unknown" : repoRoot.string());
        }
        else
        {
            value->SetText("Not a Diversion repository");
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
    
    // Detected Diversion Executable (read-only display)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Detected Diversion CLI");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));
        
        auto value = std::make_unique<Label>();
        value->AddClass("settings-row-value");
        VcsSettingsApplyMinWidth(value.get(), 150.0f);
        
        std::filesystem::path detectedExe;
        if (activeVCS && isDiversion)
        {
            detectedExe = activeVCS->GetExecutable();
        }
        else
        {
            detectedExe = diversion.GetExecutable();
        }
        
        value->SetText(detectedExe.empty() ? "(not found)" : detectedExe.string());
        section->AddChild(std::move(value));
        
        contentBody.AddContent(std::move(section));
    }
    
    // Status Refresh Interval
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Status Refresh Interval");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));
        
        auto slider = std::make_unique<Slider>();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(5.0f);
        slider->SetMax(120.0f);
        slider->SetStep(1.0f);
        
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        float interval = static_cast<float>(dvSettings.GetStatusRefreshInterval());
        slider->SetValue(interval);
        
        auto applyInterval = [](float value) {
            Editor::DiversionSettings settings;
            settings.Load();
            settings.SetStatusRefreshInterval(static_cast<int>(value));
            settings.Save();
            auto& diversionIntegration = DiversionIntegration::GetInstance();
            diversionIntegration.SetStatusRefreshInterval(static_cast<int>(value));
        };
        slider->SetOnValueChanged([applyInterval](const float& value) { applyInterval(value); });
        
        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(interval);
        FloatField* valueFieldPtr = valueField.get();
        slider->SetOnValueChanging([valueFieldPtr](const float& v) { valueFieldPtr->SetValue(v); });
        slider->SetOnValueChanged([valueFieldPtr, applyInterval](const float& value) {
            valueFieldPtr->SetValue(value);
            applyInterval(value);
        });
        valueField->SetOnValueChanged([sliderPtr = slider.get(), valueFieldPtr, applyInterval](const float& value) {
            float clamped = std::max(5.0f, std::min(120.0f, value));
            sliderPtr->SetValue(clamped);
            valueFieldPtr->SetValue(clamped);
            applyInterval(clamped);
        });

        InspectorDrag::SetupLabelDragSlider(
            labelPtr, slider.get(), nullptr, nullptr, 10.0f);
        
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
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
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        std::filesystem::path currentDiffTool = dvSettings.GetExternalDiffTool();
        field->SetValue(currentDiffTool.empty() ? "" : currentDiffTool.string());
        
        field->SetOnValueChanged([](const std::string& value) {
            Editor::DiversionSettings settings;
            settings.Load();
            settings.SetExternalDiffTool(value.empty() ? std::filesystem::path{} : std::filesystem::path(value));
            settings.Save();
        });
        
        section->AddChild(std::move(field));
        contentBody.AddContent(std::move(section));
        
        // Description
        {
            auto description = std::make_unique<GameEngine::EditorUI::CollapsibleInfoCard>("Path to external diff tool (e.g., /usr/local/bin/meld, /Applications/Beyond Compare.app/Contents/MacOS/bcomp). Leave empty to use dv diff.");
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
    
    // Integration Token (refresh token)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto labelContainer = std::make_unique<UIElement>();
        {
            labelContainer->Overrides()
                .Set(Style::Display, DisplayMode::Flex)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(8.0f));
        }
        
        // Check if token is set (format validation only - no blocking CLI call)
        // Check DiversionIntegration instance FIRST (it's set immediately when user types)
        // Then check settings (for persistence across sessions)
        auto& diversionIntegration = DiversionIntegration::GetInstance();
        std::string instanceToken = diversionIntegration.GetRefreshToken();
        std::string token = instanceToken;
        
        // If instance token is empty, check settings
        if (token.empty())
        {
            Editor::DiversionSettings dvSettings;
            dvSettings.Load();
            token = dvSettings.GetRefreshToken();
            
            // If we found a token in settings but not in instance, sync it
            if (!token.empty() && instanceToken.empty())
            {
                diversionIntegration.SetRefreshToken(token);
            }
        }
        
        // Trim whitespace from token
        if (!token.empty())
        {
            size_t start = token.find_first_not_of(" \t\r\n");
            if (start != std::string::npos)
            {
                token = token.substr(start);
            }
            size_t end = token.find_last_not_of(" \t\r\n");
            if (end != std::string::npos && end < token.length() - 1)
            {
                token = token.substr(0, end + 1);
            }
        }
        
        // Quick format validation only (no CLI call to avoid blocking UI)
        // Token should be non-empty and have reasonable length (at least 10 chars)
        bool hasValidFormat = !token.empty() && token.length() >= 10;
        
        Logger::Log::Debug("Diversion Settings: Token check - length: {}, hasValidFormat: {} (instance token length: {})", 
            token.length(), hasValidFormat, instanceToken.length());
        
        // Status indicator dot (blue if valid, red if invalid, gray if not set)
        // Note: Full validation happens in background, this is just format check
        auto statusDot = std::make_unique<UIElement>();
        uint32_t dotColor = 0xFF888888u;
        std::string dotTitle;
        if (token.empty())
        {
            // Not set - gray
            dotColor = 0xFF888888u;
            dotTitle = "Integration token not set";
        }
        else if (hasValidFormat)
        {
            // Valid - blue
            dotColor = 0xFF549BFFu;
            dotTitle = "Integration token is set and valid";
        }
        else
        {
            // Invalid - red
            dotColor = 0xFFFF4444u;
            dotTitle = "Integration token is invalid (too short)";
        }
        VcsSettingsApplyStatusDotStyle(statusDot.get(), dotColor);
        UIElement* statusDotPtr = statusDot.get(); // Store pointer for later updates
        labelContainer->AddChild(std::move(statusDot));
        
        auto label = std::make_unique<Label>();
        label->SetText("Integration Token");
        label->AddClass("settings-row-label");
        labelContainer->AddChild(std::move(label));
        
        section->AddChild(std::move(labelContainer));
        
        auto field = std::make_unique<TextField>();
        field->AddClass("settings-row-field");
        VcsSettingsApplyMinWidth(field.get(), 150.0f);
        
        field->SetValue(token.empty() ? "(not set)" : "••••••••••••••••");
        
        // Lambda to update status dot based on token value
        // This validates the token by actually trying to exchange it for an access token
        auto updateStatusDot = [statusDotPtr](const std::string& tokenValue, bool validateAsync = false) {
            (void)validateAsync;
            if (!statusDotPtr)
                return;
            
            const std::string trimmed = CleanDiversionToken(tokenValue);
            
            // Determine dot color and title based on token state
            uint32_t dotColor = 0xFF888888u;
            std::string dotTitle;
            
            if (trimmed.empty())
            {
                // Not set - gray
                dotColor = 0xFF888888u;
                dotTitle = "Integration token not set";
            }
            else
            {
                // Token is set, validate it by checking if API returns successfully
                // The token should already be set in DiversionIntegration by SetOnValueChanged
                // Try to exchange token for access token (this validates it works)
                auto& diversion = DiversionIntegration::GetInstance();
                
                // Log token length for debugging
                Logger::Log::Debug("Diversion: Validating token (length: {} characters)", trimmed.length());
                
                // Show "validating" state (yellow) while checking
                VcsSettingsApplyStatusDotStyle(statusDotPtr, 0xFFFFAA00u);
                
                // Try to refresh access token - this will validate the token by attempting API exchange
                bool isValid = diversion.RefreshAccessToken();
                
                if (isValid)
                {
                    // Valid - blue (API returned successfully)
                    dotColor = 0xFF549BFFu;
                    dotTitle = "Integration token is valid";
                    Logger::Log::Debug("Diversion: Token validation succeeded (length: {} characters)", trimmed.length());
                    
                    // Trigger immediate status update to refresh asset panel
                    // This will clear cache, update status, and trigger the callback to refresh UI
                    diversion.TriggerStatusUpdate();
                }
                else
                {
                    // Invalid - red (API call failed)
                    dotColor = 0xFFFF4444u;
                    dotTitle = "Integration token failed to authenticate";
                    Logger::Log::Warning("Diversion: Token validation failed (length: {} characters)", trimmed.length());
                }
                
                // Update dot style with final result
                VcsSettingsApplyStatusDotStyle(statusDotPtr, dotColor);
                return; // Early return since we already updated the dot
            }
            
            // Update dot style (for empty case)
            VcsSettingsApplyStatusDotStyle(statusDotPtr, dotColor);
        };
        
        field->SetOnValueChanged([updateStatusDot](const std::string& value) {
            const std::string token = CleanDiversionToken(value);

            // A value that held only mask characters is the field redrawing
            // itself, not the user clearing the token. Only an actually empty
            // field clears it.
            if (token.empty() && !value.empty())
            {
                return;
            }

            // Save the token immediately.
            Editor::DiversionSettings settings;
            settings.Load();
            settings.SetRefreshToken(token);
            bool saved = settings.Save();

            auto& diversion = DiversionIntegration::GetInstance();
            diversion.SetRefreshToken(token);

            Logger::Log::Info("Diversion: Integration token saved (length: {}, saved: {})",
                token.length(), saved);

            // Update status dot immediately (with validation)
            updateStatusDot(token, false);
        });
        
        // Store field pointer before moving
        TextField* fieldPtr = field.get();
        
        // Handle Enter key to refresh status dot
        field->RegisterEventHandler(kEventKeyDown, [fieldPtr, updateStatusDot](UIEvent& e) {
            if (e.Key == Input::kKeyCode_Enter)
            {
                const std::string token = CleanDiversionToken(fieldPtr->GetValue());
                if (!token.empty())
                {
                    // Update status dot based on current value (with validation)
                    updateStatusDot(token, false);
                }
            }
        });
        
        section->AddChild(std::move(field));
        
        contentBody.AddContent(std::move(section));
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
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        toggle->SetChecked(dvSettings.GetShowStatusIcons());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::DiversionSettings settings;
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
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        toggle->SetChecked(dvSettings.GetColorWholeText());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::DiversionSettings settings;
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
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        toggle->SetChecked(dvSettings.GetColorDotOnly());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::DiversionSettings settings;
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
        Editor::DiversionSettings dvSettings;
        dvSettings.Load();
        toggle->SetChecked(dvSettings.GetHideDotForClean());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::DiversionSettings settings;
            settings.Load();
            settings.SetHideDotForClean(enabled);
            settings.Save();
            EditorVcsProviderRegistry::Get().NotifyBadgeSettingsChanged();
        });
        section->AddChild(std::move(toggle));
        section->InsertChild(0, std::move(label));
        
        contentBody.AddContent(std::move(section));
    }
}

} // namespace

void RegisterDiversionVcsProvider()
{
    EditorVcsProviderDescriptor descriptor;
    descriptor.TypeId = "diversion";
    descriptor.DisplayName = "Diversion";
    descriptor.SettingsRowClass = "diversion-row";
    descriptor.StatusColumnTitle = "DV Status";
    descriptor.DetectionOrder = 20;
    descriptor.ServerBacked = true;
    descriptor.UpdateActionLabel = "Sync";
    descriptor.Detect = [](const std::filesystem::path& projectRoot) {
        std::error_code ec;
        const std::filesystem::path marker = projectRoot / ".diversion";
        return std::filesystem::exists(marker, ec) && std::filesystem::is_directory(marker, ec);
    };
    descriptor.Integration = []() -> IVCSIntegration& { return DiversionIntegration::GetInstance(); };
    descriptor.Initialize = [](const std::filesystem::path& projectRoot,
                               std::function<void()> statusChanged) {
        auto& diversion = DiversionIntegration::GetInstance();
        DiversionSettings settings;
        (void)settings.Load();

        const std::filesystem::path exe = settings.GetDiversionExecutable();
        const bool ok =
            exe.empty() ? diversion.Initialize(projectRoot) : diversion.Initialize(projectRoot, exe);
        if (!ok)
            return false;

        diversion.SetStatusRefreshInterval(settings.GetStatusRefreshInterval());
        if (!settings.GetRepoId().empty())
            diversion.SetRepoId(settings.GetRepoId());
        if (!settings.GetWorkspaceId().empty())
            diversion.SetWorkspaceId(settings.GetWorkspaceId());
        if (!settings.GetRefreshToken().empty())
            diversion.SetRefreshToken(settings.GetRefreshToken());
        diversion.SetStatusChangedCallback(std::move(statusChanged));
        return true;
    };
    descriptor.GetBaseContent = [](const std::filesystem::path& filePath) -> std::string {
        auto& diversion = DiversionIntegration::GetInstance();
        if (!diversion.IsRepository())
            return {};

        std::error_code ec;
        const std::filesystem::path relPath =
            std::filesystem::relative(filePath, diversion.GetRepositoryRoot(), ec);
        if (ec)
            return {};

        std::string normalizedPath = relPath.generic_string();
        std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
        if (!normalizedPath.empty() && normalizedPath.front() == '/')
            normalizedPath = normalizedPath.substr(1);

        auto result = DiversionCommandExecutor::Execute(
            diversion.GetExecutable(), diversion.GetRepositoryRoot(), {"cat", normalizedPath}, true);
        if (!result.success)
        {
            Logger::Log::Warning("Diversion: cat failed for {}", normalizedPath);
            return {};
        }
        return result.output;
    };
    descriptor.OpenExternalDiff = [](const std::filesystem::path& filePath) {
        auto& diversion = DiversionIntegration::GetInstance();
        if (!diversion.IsRepository())
            return;
        DiversionSettings settings;
        (void)settings.Load();
        diversion.OpenDiff(filePath, settings.GetExternalDiffTool());
    };
    descriptor.BadgeUiSettings = []() {
        DiversionSettings settings;
        (void)settings.Load();
        VcsBadgeUiSettings out;
        out.ShowStatusIcons = settings.GetShowStatusIcons();
        out.ColorWholeText = settings.GetColorWholeText();
        out.ColorDotOnly = settings.GetColorDotOnly();
        out.HideDotForClean = settings.GetHideDotForClean();
        return out;
    };
    descriptor.DefaultCommitMessage = []() {
        DiversionSettings settings;
        (void)settings.Load();
        return settings.GetDefaultCommitMessage();
    };
    descriptor.BuildSettingsContent = [](ScrollView& contentBody) {
        BuildDiversionSettingsContent(contentBody);
    };
    EditorVcsProviderRegistry::Get().RegisterProvider(std::move(descriptor));
}

} // namespace GameEngine::Editor
