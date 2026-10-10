#include "Engine/Build/PackageBuildStaging.h"

#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackagesIndex.h"
#include "Engine/Build/NativeScriptStaging.h"
#include "Logger/Logger.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/PrebuiltModuleBinaries.h"
#include "NativeScripting/ToolchainFingerprint.h"

#include <algorithm>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace GameEngine
{

namespace
{

// The Cpp runtime modules of one resolved package (P1 allows at most one, but
// iterate defensively). Matched by package name — module AssemblyNames are the
// deterministic package mapping, not the manifest record names.
std::vector<const PackageCodeModule*> CppRuntimeModulesOf(
    const std::vector<PackageCodeModule>& runtimeModules, const std::string& packageName)
{
    std::vector<const PackageCodeModule*> out;
    for (const PackageCodeModule& module : runtimeModules)
    {
        if (module.PackageName == packageName &&
            module.Lang == PackageModuleRecord::ModuleLang::Cpp &&
            module.Kind == PackageModuleRecord::ModuleKind::Runtime)
            out.push_back(&module);
    }
    return out;
}

// P3: stage a package's SHIPPED prebuilt binary instead of the editor build
// cache when it is provably valid for the shipping engine: host toolchain
// fingerprint dir present AND an engine_abi marker that matches the current
// digest. A marker-less prebuilt cannot be verified at build time, so it
// falls back to the source-built cache (loud INFO) — the dev-time loader can
// still use it, where the DLL's own exported handshake gates the load.
bool TryStagePrebuiltPackageModule(const fs::path& stagedPackageRoot,
                                   const PackageCodeModule& module,
                                   const EngineAbiDigestSet& engineAbiDigests, bool includeDebugSymbols)
{
    namespace ns = NativeScripting;
    if (module.PrebuiltDir.empty())
        return false;

    const ns::PrebuiltModuleLookup lookup =
        ns::FindPrebuiltModule(module.PrebuiltDir, ns::HostToolchainFingerprint(),
                               module.AssemblyName, engineAbiDigests.Current);
    if (lookup.Dll.empty())
    {
        Logger::Log::Info("Build: package '{}' prebuilt binaries unusable — {}; staging the "
                          "editor-built module instead",
                          module.PackageName, lookup.Reason);
        return false;
    }
    if (ns::ReadEngineAbiMarker(lookup.PlatformDir).empty())
    {
        Logger::Log::Info("Build: package '{}' prebuilt '{}' ships no engine_abi marker — cannot "
                          "verify it against the shipping engine at build time; staging the "
                          "editor-built module instead",
                          module.PackageName, lookup.Dll.generic_string());
        return false;
    }

    const std::string dllName = lookup.Dll.filename().string();
    const fs::path destDir = stagedPackageRoot / "NativeScripts";
    std::error_code ec;
    fs::create_directories(destDir, ec);
    fs::copy_file(lookup.Dll, destDir / dllName, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        Logger::Log::Warning("Build: failed to stage prebuilt '{}' ({}); staging the editor-built "
                             "module instead",
                             lookup.Dll.generic_string(), ec.message());
        return false;
    }
    const fs::path pdb = lookup.PlatformDir / (module.AssemblyName + ".pdb");
    if (includeDebugSymbols && fs::exists(pdb, ec))
    {
        std::error_code pdbEc;
        fs::copy_file(pdb, destDir / pdb.filename(), fs::copy_options::overwrite_existing, pdbEc);
    }

    // Same relocatable record + marker contract as the editor-built staging
    // path (LoadPrebuiltUserModule reads both). The full digest is synthetic —
    // a packaged game never recomputes it.
    if (!ns::WriteBuildCacheRecord(destDir,
                                   ns::BuildCacheRecord{"prebuilt", "NativeScripts/" + dllName,
                                                        engineAbiDigests.Current,
                                                        /*engineBuildId=*/{}}) ||
        !ns::WriteEngineAbiMarker(destDir, engineAbiDigests.Current))
    {
        Logger::Log::Warning("Build: failed to write the prebuilt record for package '{}'; "
                             "staging the editor-built module instead",
                             module.PackageName);
        return false;
    }

    Logger::Log::Info("Build: packaged prebuilt native module '{}' for package '{}' (no build)",
                      dllName, module.PackageName);
    return true;
}

// GUID -> "package-name (alias/rel/path)" over a package's (unmounted) asset
// tree, derived exactly like a mounted derived-identity source would. Lets the
// validator attribute a dangling scene reference to the disabled package that
// owns it.
void MapPackageTreeGuids(const ResolvedPackage& package,
                         std::unordered_map<GUID, std::string>& outOwners)
{
    std::error_code ec;
    if (!fs::is_directory(package.AssetsDir, ec))
        return;
    for (auto it = fs::recursive_directory_iterator(package.AssetsDir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc))
            continue;
        const fs::path rel = fs::relative(it->path(), package.AssetsDir, entryEc);
        if (entryEc || rel.empty())
            continue;
        const GUID guid =
            AssetRegistry::DeriveGuidForSourcePath(package.Alias, rel.generic_string());
        if (!guid.IsNull())
            outOwners.emplace(guid,
                              package.Manifest.Name + "' (" + package.Alias + "/" +
                                  rel.generic_string() + ")");
    }
}

bool IsMounted(const std::vector<std::string>& sessionMountedPackageAliases,
               const std::string& alias)
{
    return std::find(sessionMountedPackageAliases.begin(), sessionMountedPackageAliases.end(),
                     alias) != sessionMountedPackageAliases.end();
}

} // namespace

PackageResolution ResolveBuildTimePackages(
    const fs::path& projectRoot, const std::vector<std::string>& sessionMountedPackageAliases)
{
    PackageResolution resolution = PackageResolver::Resolve(projectRoot);

    // Enabled/disabled drift vs the live session: mid-session manifest edits
    // don't touch mounts (those update on the next project open), so the build
    // can legitimately disagree with what the session sees. One loud line per
    // drifted package tells the user what THIS build is about to do.
    for (const ResolvedPackage& package : resolution.MountOrder)
    {
        // Code-only packages (empty AssetsDir) never mount an asset source,
        // so "not mounted" is their steady state, not drift.
        if (package.AssetsDir.empty())
            continue;
        if (!IsMounted(sessionMountedPackageAliases, package.Alias))
            Logger::Log::Warning(
                "Build: package resolution drift — '{}' is enabled in Packages/manifest.json but "
                "not mounted in this session; the build ships it while the session cannot see it "
                "(mounts update on project open)",
                package.Manifest.Name);
    }
    for (const ResolvedPackage& package : resolution.Disabled)
    {
        if (IsMounted(sessionMountedPackageAliases, package.Alias))
            Logger::Log::Warning(
                "Build: package resolution drift — '{}' is disabled in Packages/manifest.json but "
                "still mounted in this session; the build excludes it while the session still "
                "uses it (mounts update on project open)",
                package.Manifest.Name);
    }
    return resolution;
}

bool ValidatePackagedBuild(const AssetManifest& manifest,
                           const PackageResolution& resolution,
                           const std::vector<PackageCodeModule>& runtimeModules,
                           const std::vector<std::string>& sessionMountedPackageAliases,
                           std::vector<std::string>& errors,
                           std::vector<std::string>& warnings)
{
    const size_t errorsBefore = errors.size();

    // --- DISABLED packages still mounted in this session ---------------------
    // The mid-session disable shape. The session still resolves the package's
    // GUIDs and compiled scripts with its defines, while THIS build excludes it
    // — its assets silently vanish from staging with no collected
    // entry or unresolved GUID for the reference nets below to attribute, and
    // the first failure would otherwise be the native ABI define gate, long
    // after the drop and far less actionable. Fail here, attributed, before
    // any asset is copied.
    for (const ResolvedPackage& package : resolution.Disabled)
    {
        if (IsMounted(sessionMountedPackageAliases, package.Alias))
        {
            errors.push_back("package '" + package.Manifest.Name +
                             "' is DISABLED in Packages/manifest.json but still mounted in this "
                             "session — the build would drop its assets and defines while open "
                             "scenes and compiled scripts still use them; reopen the project so "
                             "the disable takes effect (or re-enable the package), then build "
                             "again");
        }
    }

    // --- collected assets owned by DISABLED packages -------------------------
    // Ownership is asked directly, entry by entry: in a live editor session a
    // package disabled in Packages/manifest.json is usually STILL MOUNTED
    // (mount changes apply on the next resolve/restart), so its GUIDs resolve,
    // the collector emits perfectly normal entries for them, and the
    // unresolved-dependency net below never fires. The resolver output knows
    // the disabled set; any collected entry whose owning source alias belongs
    // to it fails the build with attribution.
    if (!resolution.Disabled.empty())
    {
        std::unordered_map<std::string, const ResolvedPackage*> disabledByAlias;
        for (const ResolvedPackage& package : resolution.Disabled)
            disabledByAlias.emplace(package.Alias, &package);

        for (const AssetManifestEntry& entry : manifest.entries)
        {
            const auto it = disabledByAlias.find(entry.sourceAlias);
            if (it == disabledByAlias.end())
                continue;
            // outputPath is "Packages/<alias>/Assets/<rel>" for package
            // entries; strip the mount prefix for the attribution.
            std::string rel = entry.outputPath.generic_string();
            const std::string prefix = "Packages/" + entry.sourceAlias + "/Assets/";
            if (rel.rfind(prefix, 0) == 0)
                rel.erase(0, prefix.size());
            errors.push_back("Build references asset of DISABLED package '" +
                             it->second->Manifest.Name + "' (" + entry.sourceAlias + "/" + rel +
                             ", still mounted in this session) — enable the package in "
                             "Packages/manifest.json or remove the reference before packaging");
        }
    }

    // --- referenced assets owned by DISABLED packages (unmounted net) --------
    // Secondary net for the restart/never-mounted case: the disabled package's
    // mount is absent, so its references surface as unresolved dependencies and
    // are attributed by deriving GUIDs over the package's on-disk tree.
    if (!manifest.unresolvedDependencies.empty())
    {
        std::unordered_map<GUID, std::string> disabledOwners;
        for (const ResolvedPackage& package : resolution.Disabled)
            MapPackageTreeGuids(package, disabledOwners);

        size_t unattributed = 0;
        for (const GUID& guid : manifest.unresolvedDependencies)
        {
            const auto it = disabledOwners.find(guid);
            if (it != disabledOwners.end())
            {
                errors.push_back("Build references asset of DISABLED package '" + it->second +
                                 "' — enable the package in Packages/manifest.json or remove "
                                 "the reference before packaging");
            }
            else
            {
                ++unattributed;
            }
        }

        // A package that failed resolution never mounted, so its references are
        // unresolved AND its tree cannot be attributed — surface the resolver's
        // own errors instead of shipping dangling references silently.
        if (unattributed > 0 && !resolution.Errors.empty())
        {
            std::string msg = std::to_string(unattributed) +
                              " referenced asset(s) could not be resolved while package "
                              "resolution failed:";
            for (const std::string& err : resolution.Errors)
                msg += "\n  - " + err;
            errors.push_back(std::move(msg));
        }
        else if (unattributed > 0)
        {
            warnings.push_back(std::to_string(unattributed) +
                               " referenced asset(s) have no metadata in any mounted source and "
                               "will not ship (see AssetCollector warnings for their GUIDs)");
        }
    }

    // --- runtime C# modules must not depend on editor assemblies ------------
    for (const PackageCodeModule& module : runtimeModules)
    {
        if (module.Lang != PackageModuleRecord::ModuleLang::CSharp ||
            module.Kind != PackageModuleRecord::ModuleKind::Runtime)
            continue;
        for (const std::string& dep : module.DependencyAssemblies)
        {
            const bool editorDep =
                dep.size() > 7 && dep.compare(dep.size() - 7, 7, ".Editor") == 0;
            if (editorDep)
            {
                errors.push_back("package '" + module.PackageName + "' runtime C# module '" +
                                 module.AssemblyName + "' references editor assembly '" + dep +
                                 ".dll' — editor assemblies never ship in a packaged game");
            }
        }
    }

    // --- native runtime modules need sources or prebuilt binaries -----------
    for (const PackageCodeModule& module : runtimeModules)
    {
        if (module.Lang != PackageModuleRecord::ModuleLang::Cpp)
            continue;
        std::error_code ec;
        const bool hasSources = !module.RootDir.empty() && fs::is_directory(module.RootDir, ec);
        const bool hasPrebuilt =
            !module.PrebuiltDir.empty() && fs::is_directory(module.PrebuiltDir, ec);
        if (!hasSources && !hasPrebuilt)
        {
            errors.push_back("package '" + module.PackageName + "' native module '" +
                             module.AssemblyName + "' has neither sources ('" +
                             module.RootDir.generic_string() +
                             "') nor prebuilt binaries for this platform — nothing to package");
        }
    }

    // --- shipping rationale per enabled package ------------------------------
    for (const ResolvedPackage& package : resolution.MountOrder)
    {
        // Code-only packages (empty AssetsDir) stage no assets by design; the
        // zero-staged warning below would cry wolf with a remediation that
        // cannot apply (there is no source to mount). Their shipped footprint
        // is their modules, staged and validated by the module passes above.
        if (package.AssetsDir.empty())
        {
            Logger::Log::Info("Build: package '{}' is code-only — no assets to stage",
                              package.Manifest.Name);
            continue;
        }

        size_t staged = 0;
        for (const AssetManifestEntry& entry : manifest.entries)
            if (entry.sourceAlias == package.Alias)
                ++staged;

        if (staged == 0 && !IsMounted(sessionMountedPackageAliases, package.Alias))
        {
            // An asset-carrying package whose source is not mounted in this
            // session (mid-session enable — mounts update on project open) had
            // no registry to read: its Shaders/ folder, its runtimeAssets and
            // every asset a scene references in it are missing from the build.
            warnings.push_back("package '" + package.Manifest.Name +
                               "' is enabled but staged no assets — its source is not mounted "
                               "in this session; reopen the project so the package mounts, "
                               "then build again");
        }
        else if (staged == 0)
        {
            // Mounted, and the build reaches nothing of it: editor-only content,
            // or assets no built scene uses. Nothing is missing, so no warning.
            Logger::Log::Info("Build: package '{}' ships no assets — the build reaches none of them",
                              package.Manifest.Name);
        }
        else
        {
            Logger::Log::Info("Build: package '{}' ships {} asset(s)",
                              package.Manifest.Name, staged);
        }
    }

    return errors.size() == errorsBefore;
}

bool StagePackageNativeModules(
    const fs::path& contentRoot,
    const PackageResolution& resolution,
    const std::vector<PackageCodeModule>& runtimeModules,
    const std::function<EngineAbiDigestSet(const PackageCodeModule&)>& engineAbiDigestsForModule,
    std::vector<std::string>& errors, bool includeDebugSymbols)
{
    bool ok = true;
    for (const ResolvedPackage& package : resolution.MountOrder)
    {
        const auto cppModules = CppRuntimeModulesOf(runtimeModules, package.Manifest.Name);
        if (cppModules.empty())
            continue;

        // Same layout contract as the project module: the staged package dir is
        // the "project root" LoadPrebuiltUserModule resolves the relative record
        // against; the editor's build cache lives under the module's writable
        // cache dir (the package's .Cache, or the managed .native cache under
        // TEMP on Windows for git and engine entries). The digest is per-module — its
        // defines are digest inputs.
        const fs::path stagedPackageRoot = contentRoot / kPackagesStagingDirName / package.Alias;
        if (TryStagePrebuiltPackageModule(stagedPackageRoot, *cppModules.front(),
                                          engineAbiDigestsForModule(*cppModules.front()), includeDebugSymbols))
            continue;
        const NativeScriptStageOutcome outcome = StageNativeUserScriptsForPackage(
            cppModules.front()->CacheDir, stagedPackageRoot,
            engineAbiDigestsForModule(*cppModules.front()), errors, includeDebugSymbols);

        if (outcome == NativeScriptStageOutcome::Failed)
        {
            ok = false;
        }
        else if (outcome == NativeScriptStageOutcome::NoScripts)
        {
            // The manifest declares a runtime Cpp module but the editor never
            // built it — shipping without the package's gameplay is not success.
            errors.push_back("package '" + package.Manifest.Name +
                             "' declares native module '" + cppModules.front()->AssemblyName +
                             "' but no built module was found under '" +
                             (cppModules.front()->CacheDir / "NativeScripts").generic_string() +
                             "' — open the project in the editor (which builds package native "
                             "modules) and package again");
            ok = false;
        }
    }
    return ok;
}

bool StagePackageManagedAssemblies(const fs::path& assembliesPackagesDir,
                                   const fs::path& managedOutDir,
                                   const std::vector<PackageCodeModule>& runtimeModules,
                                   std::vector<std::string>& errors, bool includeDebugSymbols)
{
    bool any = false;
    for (const PackageCodeModule& module : runtimeModules)
    {
        if (module.Lang == PackageModuleRecord::ModuleLang::CSharp &&
            module.Kind == PackageModuleRecord::ModuleKind::Runtime)
        {
            any = true;
            break;
        }
    }
    if (!any)
        return true;

    std::error_code ec;
    const fs::path destDir = managedOutDir / kPackagesStagingDirName;
    fs::create_directories(destDir, ec);
    if (ec)
    {
        errors.push_back("Failed to create staged package assembly dir '" +
                         destDir.generic_string() + "': " + ec.message());
        return false;
    }

    bool ok = true;
    for (const PackageCodeModule& module : runtimeModules)
    {
        if (module.Lang != PackageModuleRecord::ModuleLang::CSharp ||
            module.Kind != PackageModuleRecord::ModuleKind::Runtime)
            continue;

        const std::string dllName = module.AssemblyName + ".dll";
        const fs::path src = assembliesPackagesDir / dllName;
        if (!fs::exists(src, ec) && module.RootDir.empty())
        {
            // No sources: the editor stages the declared prebuilt assembly
            // there when the package mounts (ScriptManager::SetPackageCodeModules).
            errors.push_back("package '" + module.PackageName + "' runtime assembly '" + dllName +
                             "' has no prebuilt assembly staged (expected at '" + src.generic_string() +
                             "', copied from '" +
                             (module.PrebuiltDir / dllName).generic_string() +
                             "') — put the DLL in the package's declared prebuilt directory, open the "
                             "project in the editor so it is staged, then package again");
            ok = false;
            continue;
        }
        if (!fs::exists(src, ec))
        {
            errors.push_back("package '" + module.PackageName + "' runtime assembly '" + dllName +
                             "' was never compiled (expected at '" + src.generic_string() +
                             "') — open the project in the editor so package scripts compile, "
                             "then package again");
            ok = false;
            continue;
        }
        fs::copy_file(src, destDir / dllName, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errors.push_back("Failed to stage package assembly '" + dllName +
                             "': " + ec.message());
            ok = false;
            continue;
        }
        // Symbols ship only for configurations that request them.
        const fs::path srcPdb = assembliesPackagesDir / (module.AssemblyName + ".pdb");
        std::error_code pdbEc;
        if (includeDebugSymbols && fs::exists(srcPdb, pdbEc))
            fs::copy_file(srcPdb, destDir / srcPdb.filename(),
                          fs::copy_options::overwrite_existing, pdbEc);
        Logger::Log::Info("Build: staged package runtime assembly Managed/Packages/{}", dllName);
    }
    return ok;
}

bool StagePackagesIndex(const fs::path& contentRoot,
                        const AssetManifest& manifest,
                        const PackageResolution& resolution,
                        const std::vector<PackageCodeModule>& runtimeModules,
                        std::vector<std::string>& errors)
{
    // Aliases that actually staged assets (their Packages/<alias>/Assets exists).
    std::unordered_set<std::string> aliasesWithAssets;
    for (const AssetManifestEntry& entry : manifest.entries)
    {
        if (!entry.sourceAlias.empty() && entry.sourceAlias != kAssetSourceAliasProject &&
            entry.sourceAlias != kAssetSourceAliasEditor)
            aliasesWithAssets.insert(entry.sourceAlias);
    }

    PackagesIndex index;
    for (const ResolvedPackage& package : resolution.MountOrder)
    {
        PackagesIndexEntry entry;
        entry.Name = package.Manifest.Name;
        entry.Alias = package.Alias;
        entry.Version = package.Manifest.Version.ToString();
        entry.Priority = package.Priority;
        for (const PackageCodeModule* module :
             CppRuntimeModulesOf(runtimeModules, package.Manifest.Name))
            entry.NativeModules.push_back(module->AssemblyName);

        // Only packages with a staged footprint get mounted by the Player.
        if (aliasesWithAssets.count(entry.Alias) == 0 && entry.NativeModules.empty())
            continue;
        index.Packages.push_back(std::move(entry));
    }

    if (index.Packages.empty())
        return true; // no packages staged, no index — the Player's quiet path

    const fs::path indexFile =
        contentRoot / kPackagesStagingDirName / kPackagesIndexFileName;
    if (!SavePackagesIndex(indexFile, index))
    {
        errors.push_back("Failed to write '" + indexFile.generic_string() +
                         "' — the packaged game would mount no packages");
        return false;
    }
    Logger::Log::Info("Build: wrote {} ({} package(s))", indexFile.generic_string(),
                      index.Packages.size());
    return true;
}

} // namespace GameEngine
