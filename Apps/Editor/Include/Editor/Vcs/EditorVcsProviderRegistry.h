#pragma once

#include "ECS/ModuleRegistration.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class IVCSIntegration;
class ScrollView;
}

namespace GameEngine::Editor
{

// Per-provider status-badge display preferences, loaded from the provider's
// settings. Pure data: the AssetsPanel renders badges identically for every
// provider — a provider only chooses which of the shared toggles apply.
struct VcsBadgeUiSettings
{
    bool ShowStatusIcons = true;
    bool ColorWholeText = false;
    bool ColorDotOnly = true;
    bool HideDotForClean = true;
};

// A provider-specific context-menu entry beyond the generic Add/Revert/Diff/
// Show Log/Commit/Update rows (e.g. Git's "Push"). LocalId is provider-local;
// the editor maps it into a reserved command range and routes activation back
// through the descriptor's HandleMenuCommand.
struct VcsMenuItem
{
    std::string Label; // shown under the provider's DisplayName menu root
    uint32_t LocalId = 0;
};

// What a VCS provider module registers to become discoverable by the editor —
// replaces the hard-wired per-provider if/else chains (detection, init,
// settings tabs, badges, context menus). Everything beyond the IVCSIntegration
// contract is expressed as data or a descriptor closure so the editor core
// carries zero provider names.
struct EditorVcsProviderDescriptor
{
    std::string TypeId;      // stable id, e.g. "git" — replace-forward key, log tag
    std::string DisplayName; // menu/settings label, e.g. "Git"

    // Tree-row CSS class for the provider's settings tab (chrome stylesheets —
    // package or editor — style the icon). Empty = no class.
    std::string SettingsRowClass;

    // AssetsPanel status-column title, e.g. "DV Status". Empty = "<DisplayName> Status".
    std::string StatusColumnTitle;

    // Detection runs ascending; ties break lexicographically by TypeId.
    // Package providers: git 0, svn 10, diversion 20, lore 30 (the legacy
    // built-in precedence, kept across the extraction).
    int DetectionOrder = 100;

    // Server-backed VCS (Diversion/Lore) also offer the update action on the
    // per-file menu; local-history VCS only offer it on the directory menu.
    bool ServerBacked = false;

    // Label for the generic IVCSIntegration::Update action ("Pull"/"Update"/
    // "Sync"). Empty = no update menu item.
    std::string UpdateActionLabel;

    // Cheap synchronous workspace probe (marker-directory check only — no
    // process spawns; this runs on project open for every registered provider).
    std::function<bool(const std::filesystem::path& projectRoot)> Detect;

    // The provider's integration instance (module-owned singleton; the editor
    // never destroys it — module lifetime is loud no-unload).
    std::function<IVCSIntegration&()> Integration;

    // Full editor bring-up: load the provider's settings, Initialize the
    // integration for projectRoot, apply configuration, and install
    // statusChanged as the integration's status callback.
    std::function<bool(const std::filesystem::path& projectRoot,
                       std::function<void()> statusChanged)>
        Initialize;

    // Committed/base file content for the internal diff panel. Empty result =
    // no base version available. Null = provider has no base-content primitive.
    std::function<std::string(const std::filesystem::path& filePath)> GetBaseContent;

    // External-diff fallback when no diff panel is bound (the provider owns
    // its external-tool setting). Optional.
    std::function<void(const std::filesystem::path& filePath)> OpenExternalDiff;

    // Badge display prefs from the provider's settings. Null = defaults.
    std::function<VcsBadgeUiSettings()> BadgeUiSettings;

    // Default message prefilled into the commit dialog when the caller has
    // none (the provider owns the setting). Null/empty = no prefill.
    std::function<std::string()> DefaultCommitMessage;

    // Provider-specific context-menu extras + their handler. Optional.
    std::function<void(bool isDirectory, std::vector<VcsMenuItem>& outItems)> CollectMenuItems;
    std::function<void(uint32_t localId, const std::filesystem::path& path)> HandleMenuCommand;

    // Settings-tab content, built into the Version Control section of the
    // Settings panel. Optional (a provider without a tab still detects).
    std::function<void(ScrollView& contentBody)> BuildSettingsContent;
};

// Registration is main-thread (module loads + editor startup), matching the
// other editor registries. Module lifetime is loud no-unload: a module rebuild
// re-registers under the same TypeId and the NEW descriptor wins
// (replace-forward); the observer receives the replaced integration so the
// owner can disconnect it loudly.
class EditorVcsProviderRegistry
{
public:
    static EditorVcsProviderRegistry& Get();

    void RegisterProvider(EditorVcsProviderDescriptor descriptor);

    // Descriptors in detection order (DetectionOrder, then TypeId).
    std::vector<EditorVcsProviderDescriptor> Snapshot() const;
    bool TryGet(std::string_view typeId, EditorVcsProviderDescriptor& outDescriptor) const;

    // First provider (detection order) whose Detect(projectRoot) hits.
    // Empty = no provider claims the workspace.
    std::string DetectWorkspace(const std::filesystem::path& projectRoot) const;

    // First provider (detection order) whose integration reports an active
    // repository. Null/empty/false when none is connected.
    IVCSIntegration* ActiveIntegration() const;
    std::string ActiveTypeId() const;
    bool TryGetActiveProvider(EditorVcsProviderDescriptor& outDescriptor) const;

    // Fired on every registration. replacedIntegration is the PREVIOUS
    // descriptor's integration on a same-TypeId re-registration (module
    // reload), null for first-time registrations. Setting the observer replays
    // existing registrations (with null) so attach order never matters.
    using RegistrationObserver =
        std::function<void(const EditorVcsProviderDescriptor& descriptor,
                           IVCSIntegration* replacedIntegration)>;
    void SetRegistrationObserver(RegistrationObserver observer);

    // Providers poke this when the user changes badge display settings; the
    // editor installs a handler that refreshes the asset views.
    void NotifyBadgeSettingsChanged() const;
    void SetBadgeSettingsChangedHandler(std::function<void()> handler);

    // C12 editor-kind unload refusal diagnostics: append a description of
    // every provider attributed to `moduleId` (descriptor callables + the
    // integration are module code and pin its images mapped).
    void AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const;

private:
    EditorVcsProviderRegistry() = default;

    std::vector<EditorVcsProviderDescriptor> m_Providers; // kept in detection order
    // Module stamp per provider TypeId (registry bookkeeping — the descriptor
    // stays a pure module-authored value).
    std::unordered_map<std::string, ECS::ModuleRegistrationStamp> m_ModuleOwners;
    RegistrationObserver m_Observer;
    std::function<void()> m_BadgeSettingsChangedHandler;
};

} // namespace GameEngine::Editor
