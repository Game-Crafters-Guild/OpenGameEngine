#include "Packages/PackageManagerController.h"

#include "Assets/Packages/PackageGitSource.h"
#include "Assets/Packages/PackageManagementActions.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/ProjectPackagesManifest.h"
#include "Core/Engine.h"
#include "Panels/PackageManagerPanel.h"
#include "Platform/Shell.h"
#include "Scripting/ScriptManager.h"

#include <algorithm>
#include <set>
#include <system_error>
#include <utility>

namespace GameEngine
{
namespace Editor
{

void PackageManagerController::Initialize(Dependencies deps)
{
    m_Deps = std::move(deps);
}

void PackageManagerController::MountForProject(const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const PackageResolution resolution = PackageResolver::Resolve(projectRoot);
    m_MountedAliases = MountResolvedPackages(am, resolution, EngineCore::GetInstance().GetAssetDbCacheRoot());
    m_Resolution = resolution;
    m_ManifestFile = projectRoot / "Packages" / "manifest.json";

    // What each mounted package pointed at when it mounted: the panel compares
    // this against the CURRENT resolution to show re-pin/relocate drift.
    m_MountedRoots.clear();
    for (const ResolvedPackage& pkg : resolution.MountOrder)
    {
        if (std::find(m_MountedAliases.begin(), m_MountedAliases.end(), pkg.Alias) !=
            m_MountedAliases.end())
            m_MountedRoots.emplace(pkg.Manifest.Name, pkg.RootDir);
    }

    // Module set each enabled package carried at open (the module analogue of
    // the asset-root record above). MountOrder is the enabled, resolved set
    // whose code modules CollectPackageCodeModules applies below, so this is
    // exactly the module state a reopen reproduces for unchanged package
    // sources. The panel compares the CURRENT discovery against it so a
    // code-only package with no asset mount reads as mounted-equivalent when
    // unchanged.
    DiscoverResolvedModules();
    m_OpenTimeModules.clear();
    for (const ResolvedPackage& pkg : resolution.MountOrder)
    {
        const auto it = m_ResolvedModules.find(pkg.Manifest.Name);
        if (it != m_ResolvedModules.end() && !it->second.empty())
            m_OpenTimeModules.emplace(pkg.Manifest.Name, it->second);
    }

    // P1 code modules: flatten the resolution for the compile graphs. C# modules
    // go straight to the ScriptManager (the next compile pass — hot-reload or
    // initial build — picks them up); Cpp modules are stashed for the native
    // wiring, which runs when the workspace root binds.
    m_CodeModules = CollectPackageCodeModules(resolution, /*editorContext=*/true);
    m_Defines = CollectAllPackageDefines(resolution);
    auto& scriptManager = EngineCore::GetInstance().GetScriptManager();
    scriptManager.SetPackageCodeModules(m_CodeModules, m_Defines);

    // Package C# modules must be compiled + loaded without waiting for a script
    // edit: kick a whole-graph reload (the same pass every hot-reload uses —
    // packages compile in topo order, then project scripts, then one ALC swap).
    const bool hasCSharpModules =
        std::any_of(m_CodeModules.begin(), m_CodeModules.end(),
                    [](const PackageCodeModule& m) {
                        return m.Lang == PackageModuleRecord::ModuleLang::CSharp;
                    });
    if (hasCSharpModules && scriptManager.IsInitialized())
        scriptManager.ReloadScriptAssemblies();

    WirePanels();
}

void PackageManagerController::UnmountAll(Function<void()> onUnmounted)
{
    if (m_MountedAliases.empty())
    {
        if (onUnmounted)
            onUnmounted();
        return;
    }
    auto& am = EngineCore::GetInstance().GetAssetManager();
    // The local state is cleared now, not in the completion: these aliases are
    // no longer this controller's to mount, and a project switch starts its
    // rebind from the completion below.
    std::vector<std::string> aliases;
    aliases.swap(m_MountedAliases);
    m_MountedRoots.clear();
    m_ResolvedModules.clear();
    m_OpenTimeModules.clear();
    m_CodeModules.clear();
    m_Defines.clear();
    m_Resolution = PackageResolution{};
    EngineCore::GetInstance().GetScriptManager().SetPackageCodeModules({}, {});
    UnmountPackageSources(am, aliases, std::move(onUnmounted));
}

void PackageManagerController::RefreshResolution()
{
    if (m_ManifestFile.empty())
        return;
    const std::filesystem::path projectRoot = m_ManifestFile.parent_path().parent_path();
    m_Resolution = PackageResolver::Resolve(projectRoot);
    DiscoverResolvedModules();
}

void PackageManagerController::DiscoverResolvedModules()
{
    m_ResolvedModules.clear();
    for (const std::vector<ResolvedPackage>* packages : {&m_Resolution.MountOrder, &m_Resolution.Disabled})
        for (const ResolvedPackage& pkg : *packages)
            m_ResolvedModules.emplace(pkg.Manifest.Name, ModulesDeliveredBy(pkg));
}

const std::vector<DiscoveredPackageModule>& PackageManagerController::ModulesOf(
    const std::string& packageName) const
{
    static const std::vector<DiscoveredPackageModule> kNone;
    const auto it = m_ResolvedModules.find(packageName);
    return it == m_ResolvedModules.end() ? kNone : it->second;
}

void PackageManagerController::WirePanels()
{
    // Snapshot provider: the CURRENT resolution (kept fresh by
    // RefreshResolution after every panel action) merged with the
    // CURRENT manifest — pending edits show immediately, with per-row drift
    // flags exposing "applies on reopen" against the live mounts.
    auto provider = [this]() {
        PackageManagerSnapshot snapshot;
        ProjectPackagesManifest manifest;
        std::string manifestError;
        (void)TryLoadProjectPackagesManifest(m_ManifestFile, manifest, manifestError);

        std::vector<std::string> unclaimedErrors = m_Resolution.Errors;
        std::set<std::string> resolvedNames;

        const auto specRef = [](const std::string& spec) {
            GitPackageSpec parsed;
            std::string error;
            return TryParseGitPackageSpec("", spec, parsed, error) ? parsed.Ref : std::string();
        };

        // True when a package's current module set is identical to the set
        // applied when the project was opened (recorded in m_OpenTimeModules).
        // Element-wise: both come from the same discovery over the same root,
        // so unchanged sources yield an identical order.
        const auto modulesUnchangedSinceOpen = [this](const ResolvedPackage& pkg) {
            const auto it = m_OpenTimeModules.find(pkg.Manifest.Name);
            if (it == m_OpenTimeModules.end())
                return false;
            const std::vector<DiscoveredPackageModule>& openModules = it->second;
            const std::vector<DiscoveredPackageModule>& nowModules = ModulesOf(pkg.Manifest.Name);
            if (openModules.size() != nowModules.size())
                return false;
            for (size_t i = 0; i < nowModules.size(); ++i)
            {
                const DiscoveredPackageModule& a = openModules[i];
                const DiscoveredPackageModule& b = nowModules[i];
                if (a.Kind != b.Kind || a.Lang != b.Lang || a.RootDir != b.RootDir)
                    return false;
            }
            return true;
        };

        const auto appendEntries = [&](const std::vector<ResolvedPackage>& packages) {
            for (const ResolvedPackage& pkg : packages)
            {
                PackageManagerEntry entry;
                entry.Name = pkg.Manifest.Name;
                entry.Version = pkg.Manifest.Version.ToString();
                switch (pkg.SourceKind)
                {
                    case PackageSourceKind::Embedded: entry.SourceKind = "embedded"; break;
                    case PackageSourceKind::File:     entry.SourceKind = "file"; break;
                    case PackageSourceKind::Git:      entry.SourceKind = "git"; break;
                    case PackageSourceKind::Engine:   entry.SourceKind = "engine"; break;
                }
                if (const auto specIt = manifest.Dependencies.find(pkg.Manifest.Name);
                    specIt != manifest.Dependencies.end())
                    entry.Spec = specIt->second;
                entry.GitCommit = pkg.GitCommit;
                if (pkg.SourceKind == PackageSourceKind::Git)
                    entry.GitRef = specRef(entry.Spec);
                entry.Enabled = manifest.Disabled.count(pkg.Manifest.Name) == 0;
                entry.Mounted = std::find(m_MountedAliases.begin(), m_MountedAliases.end(),
                                          pkg.Alias) != m_MountedAliases.end();
                // The Mounted flag above only tracks asset mounts. A package
                // with no assets dir (MountResolvedPackages skips those) mounts
                // nothing asset-side at open, so it reads as mounted-equivalent
                // — a reopen applies nothing new — when EITHER it has no code
                // modules (#634) OR its modules were applied at open and are
                // unchanged. Without the module check, a modules-only package
                // wore a permanent REOPEN badge because Enabled != Mounted.
                if (!entry.Mounted && entry.Enabled)
                {
                    std::error_code contentEc;
                    const bool hasAssetsDir = std::filesystem::is_directory(pkg.AssetsDir, contentEc);
                    if (!hasAssetsDir &&
                        (ModulesOf(pkg.Manifest.Name).empty() || modulesUnchangedSinceOpen(pkg)))
                        entry.Mounted = true;
                }
                if (const auto rootIt = m_MountedRoots.find(pkg.Manifest.Name);
                    rootIt != m_MountedRoots.end())
                    entry.MountDrift = rootIt->second != pkg.RootDir;
                entry.Priority = pkg.Priority;
                entry.ModuleCount = static_cast<int>(ModulesOf(pkg.Manifest.Name).size());
                resolvedNames.insert(entry.Name);
                snapshot.Entries.push_back(std::move(entry));
            }
        };
        appendEntries(m_Resolution.MountOrder);
        appendEntries(m_Resolution.Disabled);

        // Declared dependencies that failed to resolve (dead file: path, bad
        // spec, cold git cache offline): synthesize MISSING rows so the panel
        // offers Locate / re-pin / Remove instead of hiding them. Each row
        // claims its own resolver error out of the general issue list.
        for (const auto& [name, spec] : manifest.Dependencies)
        {
            if (resolvedNames.count(name) != 0)
                continue;
            PackageManagerEntry entry;
            entry.Name = name;
            entry.Version = "?";
            entry.Spec = spec;
            if (IsGitPackageSpec(spec))
            {
                entry.SourceKind = "git";
                entry.GitRef = specRef(spec);
            }
            else
            {
                entry.SourceKind = spec == "embedded" ? "embedded" : "file";
            }
            entry.Enabled = manifest.Disabled.count(name) == 0;
            entry.Mounted = m_MountedRoots.count(name) != 0;
            entry.Missing = true;
            const std::string needle = "'" + name + "'";
            const auto errIt = std::find_if(
                unclaimedErrors.begin(), unclaimedErrors.end(),
                [&](const std::string& e) { return e.find(needle) != std::string::npos; });
            if (errIt != unclaimedErrors.end())
            {
                entry.Error = *errIt;
                unclaimedErrors.erase(errIt);
            }
            snapshot.Entries.push_back(std::move(entry));
        }

        // Mounted packages that left the manifest entirely: still mounted
        // until reopen — the status line says so.
        for (const auto& [name, rootDir] : m_MountedRoots)
        {
            if (resolvedNames.count(name) == 0 && manifest.Dependencies.count(name) == 0)
                ++snapshot.StaleMountCount;
        }

        snapshot.Errors = std::move(unclaimedErrors);
        snapshot.Warnings = m_Resolution.Warnings;
        return snapshot;
    };

    const auto projectRootOf = [this]() {
        return m_ManifestFile.parent_path().parent_path();
    };

    // Every action edits the manifest/lock, then re-resolves for display.
    // Mounts stay untouched until the next project open — live remount would
    // eject loaded assets under the open scene and swap script ALCs, which is
    // exactly the churn the project-switch path exists to contain.
    PackageManagerActions actions;
    actions.Toggle = [this](const std::string& packageName, bool disable, std::string& outError) {
        if (!SetPackageDisabledInProjectManifest(m_ManifestFile, packageName, disable, outError))
            return false;
        RefreshResolution();
        return true;
    };
    actions.Add = [this, projectRootOf](const std::string& input, std::string& outName,
                                        std::string& outError) {
        PackageAddResult result;
        const bool ok = IsGitAddInput(input)
                            ? AddGitPackageToProject(projectRootOf(), input, result, outError)
                            : AddLocalPackageToProject(projectRootOf(), input, result, outError);
        if (!ok)
            return false;
        outName = result.Name;
        RefreshResolution();
        return true;
    };
    actions.AddBrowse = [this, projectRootOf](std::string& outName, std::string& outError,
                                              bool& outCancelled) {
        outCancelled = false;
        const std::filesystem::path picked = Platform::SelectFolder(projectRootOf());
        if (picked.empty())
        {
            outCancelled = true;
            return true;
        }
        PackageAddResult result;
        if (!AddLocalPackageToProject(projectRootOf(), picked.string(), result, outError))
            return false;
        outName = result.Name;
        RefreshResolution();
        return true;
    };
    actions.Remove = [this, projectRootOf](const std::string& packageName, std::string& outError) {
        if (!RemovePackageFromProject(projectRootOf(), packageName, outError))
            return false;
        RefreshResolution();
        return true;
    };
    actions.Refresh = [this](std::string& outError) {
        (void)outError;
        RefreshResolution();
        return true;
    };
    actions.CheckUpdates = [projectRootOf](const std::string& packageName,
                                           std::string& outStatus, std::string& outError) {
        return CheckGitPackageForUpdates(projectRootOf(), packageName, outStatus, outError);
    };
    actions.Repin = [this, projectRootOf](const std::string& packageName, const std::string& ref,
                                          std::string& outError) {
        if (!RepinGitPackageToRef(projectRootOf(), packageName, ref, outError))
            return false;
        RefreshResolution();
        return true;
    };
    actions.Relocate = [this, projectRootOf](const std::string& packageName,
                                             const std::string& path, std::string& outError) {
        if (path.empty())
        {
            outError = "package '" + packageName + "': type the package's new folder path";
            return false;
        }
        if (!RelocatePackageInProject(projectRootOf(), packageName, std::filesystem::path(path),
                                      outError))
            return false;
        RefreshResolution();
        return true;
    };
    actions.RelocateBrowse = [this, projectRootOf](const std::string& packageName,
                                                   std::string& outError, bool& outCancelled) {
        outCancelled = false;
        const std::filesystem::path picked = Platform::SelectFolder(projectRootOf());
        if (picked.empty())
        {
            outCancelled = true;
            return true;
        }
        if (!RelocatePackageInProject(projectRootOf(), packageName, picked, outError))
            return false;
        RefreshResolution();
        return true;
    };

    if (m_Deps.GetPackageManagerPanels)
    {
        for (PackageManagerPanel* panel : m_Deps.GetPackageManagerPanels())
        {
            if (panel)
                panel->SetDataSource(provider, actions);
        }
    }
}

} // namespace Editor
} // namespace GameEngine
