#pragma once

#include "Assets/Packages/PackageCodeDiscovery.h"
#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageResolver.h"
#include "Types/Types.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace GameEngine
{
class PackageManagerPanel;

namespace Editor
{

/// Owns the editor's project package state: asset mounts resolved from
/// <project>/Packages/manifest.json, the code modules those packages carry,
/// and the Package Manager panel's snapshot/action wiring.
///
/// Mounts follow the manifest only on the next project open (drift model) —
/// panel actions edit the manifest/lock and re-resolve for display, and the
/// panel surfaces per-row drift against what was mounted at open.
class PackageManagerController
{
  public:
    struct Dependencies
    {
        // Package Manager panels to wire (app-lifetime panel storage; evaluated
        // lazily, so panels may be created after Initialize).
        std::function<std::vector<PackageManagerPanel*>()> GetPackageManagerPanels;
    };

    void Initialize(Dependencies deps);

    /// Resolve + mount the project's packages after the project source binds
    /// (startup + project switch), hand the collected code modules to the
    /// ScriptManager, and rewire the panels.
    void MountForProject(const std::filesystem::path& projectRoot);

    /// Unmount every package asset source and clear code-module state. The
    /// kept aliases let a project switch unmount the previous project's
    /// packages before rebinding. Each source's assets drain first, so
    /// `onUnmounted` runs when the last one is gone — possibly inside this
    /// call, possibly several frames later.
    void UnmountAll(Function<void()> onUnmounted = {});

    /// (Re)wire every Package Manager panel's snapshot provider and actions.
    void WirePanels();

    // P1 package code modules, collected at mount time (editor context:
    // Editor-kind modules included). C# modules feed the ScriptManager
    // immediately; Cpp modules are consumed by the native-scripting wiring,
    // which runs on workspace-root change — always after the mount for that
    // root.
    const std::vector<PackageCodeModule>& GetCodeModules() const { return m_CodeModules; }
    const std::vector<std::string>& GetPackageDefines() const { return m_Defines; }

  private:
    // Re-run PackageResolver::Resolve for panel display only (no mount, code
    // module, or script changes). Warm/locked resolves are offline.
    void RefreshResolution();

    // Record the code modules every resolved package delivers, enabled or not
    // (ModulesDeliveredBy). Runs with the resolution, never per frame: the
    // panel reads the result instead of walking while it draws.
    void DiscoverResolvedModules();

    // The code modules a resolved package delivers right now; empty when it
    // delivers none or is not in the current resolution.
    const std::vector<DiscoveredPackageModule>& ModulesOf(const std::string& packageName) const;

    Dependencies m_Deps;
    std::vector<std::string> m_MountedAliases;
    std::vector<PackageCodeModule> m_CodeModules;
    std::vector<std::string> m_Defines;

    // P3 Package Manager panel data: the CURRENT resolution (re-resolved after
    // every panel action) and the manifest the panel's actions edit.
    PackageResolution m_Resolution;
    std::filesystem::path m_ManifestFile;
    // What each mounted package pointed at when it mounted, so a re-pin or
    // relocate shows as per-row drift.
    std::map<std::string, std::filesystem::path> m_MountedRoots;
    // Code modules each resolved package delivers right now, keyed by package
    // name. Disabled packages are included: the panel reports what a package
    // would bring if it were enabled.
    std::map<std::string, std::vector<DiscoveredPackageModule>> m_ResolvedModules;
    // Module set each enabled package carried when the project was opened (the
    // module analogue of m_MountedRoots). Code modules, like asset mounts,
    // only (un)load on project open, so a modules-only package with no asset
    // mount still reads as mounted-equivalent when its current module set
    // matches what was applied at open — otherwise Enabled != the asset-only
    // Mounted flag would pin a permanent REOPEN badge on it.
    std::map<std::string, std::vector<DiscoveredPackageModule>> m_OpenTimeModules;
};

} // namespace Editor
} // namespace GameEngine
