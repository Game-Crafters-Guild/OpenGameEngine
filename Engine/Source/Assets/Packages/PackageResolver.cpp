#include "Assets/Packages/PackageResolver.h"

#include "Assets/Packages/PackageGitFetcher.h"
#include "Assets/Packages/PackageGitSource.h"
#include "Assets/Packages/PackagesLockFile.h"
#include "Assets/Packages/ProjectPackagesManifest.h"
#include "AssetCore/PathNormalization.h"
#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "Logger/Logger.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace GameEngine
{

namespace
{

constexpr std::string_view kFileSpecPrefix = "file:";
constexpr std::string_view kEmbeddedSpec = "embedded";
constexpr std::string_view kReservedAliases[] = {"project", "editor"};

struct LocatedPackage
{
    PackageManifest Manifest;
    std::filesystem::path RootDir;
    std::filesystem::path AssetsDir;
    std::filesystem::path AuthoringRootDir;
    std::string Alias;
    PackageSourceKind SourceKind = PackageSourceKind::Embedded;
    std::string GitCommit;
    bool Disabled = false;
    // Set when the package (or a transitive dependency) failed validation.
    bool Skipped = false;
};

// Resolve a dependency spec to the package directory. Relative "file:" paths
// resolve against the manifest's own directory (<project>/Packages/), the
// same anchoring npm and UPM use.
bool TryResolvePackageDir(const std::string& name,
                          const std::string& spec,
                          const std::filesystem::path& packagesDir,
                          std::filesystem::path& outDir,
                          PackageSourceKind& outKind,
                          std::string& outError)
{
    if (spec == kEmbeddedSpec)
    {
        outDir = AssetPaths::NormalizeMountRoot(packagesDir / name);
        outKind = PackageSourceKind::Embedded;
        return true;
    }
    if (spec.rfind(kFileSpecPrefix, 0) == 0)
    {
        const std::filesystem::path raw(spec.substr(kFileSpecPrefix.size()));
        if (raw.empty())
        {
            outError = "package '" + name + "': empty path in spec '" + spec + "'";
            return false;
        }
        outDir = AssetPaths::NormalizeMountRoot(raw.is_absolute() ? raw : packagesDir / raw);
        outKind = PackageSourceKind::File;
        return true;
    }
    outError = "package '" + name + "': unsupported spec '" + spec +
               "' (supported: \"file:<path>\", \"embedded\", \"git+https://...#ref\")";
    return false;
}

// Git acquisition state shared across one Resolve pass: the lock file loads
// lazily on the first git dependency, and the git executable is probed once.
struct GitResolveState
{
    std::filesystem::path LockFile;
    std::filesystem::path CacheRoot;
    PackagesLock Lock;
    bool LockLoaded = false;
    bool LockDirty = false;
    std::string GitExe;
};

// Locate a git dependency: locked fast-path (unchanged spec + warm cache = no
// git, no network), else acquire through PackageGitFetcher and relock.
bool TryResolveGitPackageDir(const std::string& name,
                             const std::string& spec,
                             GitResolveState& state,
                             std::filesystem::path& outDir,
                             std::string& outCommit,
                             std::vector<std::string>& outWarnings,
                             std::string& outError)
{
    GitPackageSpec parsed;
    if (!TryParseGitPackageSpec(name, spec, parsed, outError))
        return false;

    if (!state.LockLoaded)
    {
        state.LockLoaded = true;
        std::string lockError;
        if (!TryLoadPackagesLock(state.LockFile, state.Lock, lockError))
        {
            // A malformed lock must not brick resolution: re-acquire and rewrite.
            outWarnings.push_back(lockError + "; re-resolving git dependencies");
            state.Lock = PackagesLock{};
        }
    }

    // Honor the lock: the same manifest spec text resolves to the locked
    // commit as long as its cache entry is still populated.
    if (const auto lockIt = state.Lock.Packages.find(name); lockIt != state.Lock.Packages.end())
    {
        const PackagesLockEntry& entry = lockIt->second;
        if (entry.Spec == spec)
        {
            const std::filesystem::path entryDir =
                state.CacheRoot / PackageCacheEntryName(name, entry.Version, entry.Commit);
            std::error_code ec;
            if (std::filesystem::exists(entryDir / "package.json", ec))
            {
                outDir = AssetPaths::NormalizeMountRoot(entryDir);
                outCommit = entry.Commit;
                return true;
            }
            // Cache evicted/cleared: fall through and re-acquire the SAME pin.
        }
        else
        {
            Logger::Log::Info("Packages: '{}' spec changed ('{}' -> '{}'); re-resolving", name,
                              entry.Spec, spec);
        }
    }

    if (state.GitExe.empty() && !TryFindGitExecutable(state.GitExe, outError))
    {
        outError = "package '" + name + "': " + outError;
        return false;
    }

    // Re-acquiring an unchanged spec keeps the locked pin even when the ref
    // moved: fetch the locked commit, not the ref (reproducible cache refill).
    GitPackageSpec fetchSpec = parsed;
    if (const auto lockIt = state.Lock.Packages.find(name);
        lockIt != state.Lock.Packages.end() && lockIt->second.Spec == spec)
        fetchSpec.Ref = lockIt->second.Commit;

    GitPackageAcquisition acquired;
    if (!AcquireGitPackage(state.GitExe, name, fetchSpec, state.CacheRoot, acquired, outError))
        return false;

    PackagesLockEntry entry;
    entry.Spec = spec;
    entry.Url = parsed.Url;
    entry.Ref = parsed.Ref;
    entry.Commit = acquired.Commit;
    entry.Version = acquired.Version;
    entry.Integrity = acquired.Integrity;
    const auto existing = state.Lock.Packages.find(name);
    if (existing == state.Lock.Packages.end() || existing->second.Commit != entry.Commit ||
        existing->second.Spec != entry.Spec)
        state.LockDirty = true;
    state.Lock.Packages[name] = std::move(entry);

    outDir = AssetPaths::NormalizeMountRoot(acquired.PackageDir);
    outCommit = acquired.Commit;
    return true;
}

// The repository tree a developer build staged its engine packages from, read
// from the marker the build drops beside them. Empty in a shipped or packaged
// tree, which is what keeps those trees' packages read-only. A marker naming a
// directory that is no longer there reads as absent rather than as an error:
// the staged tree still runs, it just stops being authorable.
std::filesystem::path ReadEnginePackageAuthoringRoot(const std::filesystem::path& engineRoot)
{
    std::ifstream in(engineRoot / kEnginePackageAuthoringRootFile);
    std::string line;
    if (!in || !std::getline(in, line))
        return {};
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    if (line.empty())
        return {};
    std::error_code ec;
    const std::filesystem::path root(line);
    if (!std::filesystem::is_directory(root, ec))
        return {};
    return AssetPaths::NormalizeMountRoot(root);
}

} // namespace

std::string SanitizePackageAlias(std::string_view packageName)
{
    std::string alias;
    alias.reserve(packageName.size());
    for (char c : packageName)
    {
        if (c == '@')
            continue;
        if (c == '/' || c == '.' || c == '_')
        {
            alias.push_back('-');
            continue;
        }
        alias.push_back(c);
    }
    return alias;
}

std::string PackageDefine(std::string_view packageName)
{
    constexpr std::string_view kPackageDefinePrefix = "GE_PACKAGE_";

    const std::string alias = SanitizePackageAlias(packageName);
    std::string out(kPackageDefinePrefix);
    out.reserve(out.size() + alias.size());
    for (char c : alias)
    {
        if (c == '-')
            c = '_';
        else if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        out.push_back(c);
    }
    return out;
}

PackageResolution PackageResolver::Resolve(const std::filesystem::path& projectRoot,
                                           const std::filesystem::path& enginePackagesRoot)
{
    PackageResolution result;

    const std::filesystem::path packagesDir = projectRoot / "Packages";
    const std::filesystem::path manifestFile = packagesDir / "manifest.json";

    ProjectPackagesManifest projectManifest;
    std::string manifestError;
    if (!TryLoadProjectPackagesManifest(manifestFile, projectManifest, manifestError))
    {
        result.Errors.push_back(std::move(manifestError));
        return result;
    }

    // Implicit engine packages: every staged <engine root>/<dir>/package.json
    // joins the set as if the project manifest declared it. A project
    // declaration of the same NAME shadows the engine copy (package-dev
    // checkouts); "disabled" opts out per project like any other package.
    std::map<std::string, std::filesystem::path> enginePackageDirs; // manifest name -> package dir
    std::filesystem::path enginePackageAuthoringRoot;
    {
        const std::filesystem::path engineRoot =
            enginePackagesRoot.empty() ? PathUtils::GetExecutableDirectory() / kStagedEnginePackagesDirName
                                       : enginePackagesRoot;
        enginePackageAuthoringRoot = ReadEnginePackageAuthoringRoot(engineRoot);
        std::error_code ec;
        if (std::filesystem::is_directory(engineRoot, ec))
        {
            for (const auto& entry : std::filesystem::directory_iterator(engineRoot, ec))
            {
                std::error_code entryEc;
                if (!entry.is_directory(entryEc))
                    continue;
                const std::filesystem::path pkgManifestFile = entry.path() / "package.json";
                if (!std::filesystem::exists(pkgManifestFile, entryEc))
                    continue;
                PackageManifest manifest;
                std::string error;
                if (!TryLoadPackageManifest(pkgManifestFile, manifest, error))
                {
                    result.Errors.push_back("engine package '" + entry.path().filename().string() +
                                            "': " + error);
                    continue;
                }
                if (projectManifest.Dependencies.count(manifest.Name) != 0)
                    continue;
                enginePackageDirs.emplace(manifest.Name, AssetPaths::NormalizeMountRoot(entry.path()));
            }
        }
    }

    if (projectManifest.Dependencies.empty() && enginePackageDirs.empty())
        return result;

    // ------------------------------------------------------------------
    // Locate + parse each declared package.
    // ------------------------------------------------------------------
    std::map<std::string, LocatedPackage> located; // name -> package (map: deterministic order)
    // Alias uniqueness is define uniqueness: PackageDefine is the alias
    // rewritten one-for-one (PackageResolver.h), so one alias per package is
    // also one GE_PACKAGE_ macro per package.
    std::map<std::string, std::string> aliasOwners; // alias -> name

    GitResolveState gitState;
    gitState.LockFile = packagesDir / kPackagesLockFileName;
    gitState.CacheRoot = GlobalPackageCacheRoot();

    for (const auto& [name, spec] : projectManifest.Dependencies)
    {
        std::filesystem::path pkgDir;
        PackageSourceKind sourceKind = PackageSourceKind::Embedded;
        std::string gitCommit;
        std::string error;
        bool locatedOk = false;
        if (IsGitPackageSpec(spec))
        {
            sourceKind = PackageSourceKind::Git;
            locatedOk = TryResolveGitPackageDir(name, spec, gitState, pkgDir, gitCommit,
                                                result.Warnings, error);
        }
        else
        {
            locatedOk = TryResolvePackageDir(name, spec, packagesDir, pkgDir, sourceKind, error);
        }
        if (!locatedOk)
        {
            result.Errors.push_back(std::move(error));
            continue;
        }

        const std::filesystem::path pkgManifestFile = pkgDir / "package.json";
        std::error_code ec;
        if (!std::filesystem::exists(pkgManifestFile, ec))
        {
            result.Errors.push_back("package '" + name + "': no package.json at '" +
                                    pkgManifestFile.generic_string() + "' (spec '" + spec + "')");
            continue;
        }

        LocatedPackage pkg;
        if (!TryLoadPackageManifest(pkgManifestFile, pkg.Manifest, error))
        {
            result.Errors.push_back(std::move(error));
            continue;
        }
        if (pkg.Manifest.Name != name)
        {
            result.Errors.push_back("package '" + name + "': dependency key does not match manifest name '" +
                                    pkg.Manifest.Name + "' (" + pkgManifestFile.generic_string() + ")");
            continue;
        }

        pkg.Alias = SanitizePackageAlias(name);
        const bool reserved =
            std::find(std::begin(kReservedAliases), std::end(kReservedAliases), pkg.Alias) !=
            std::end(kReservedAliases);
        if (reserved)
        {
            result.Errors.push_back("package '" + name + "': mount alias '" + pkg.Alias +
                                    "' is reserved; rename the package");
            continue;
        }
        if (const auto ownerIt = aliasOwners.find(pkg.Alias); ownerIt != aliasOwners.end())
        {
            result.Errors.push_back("package '" + name + "': mount alias '" + pkg.Alias +
                                    "' collides with package '" + ownerIt->second +
                                    "' after sanitization; rename one of them");
            continue;
        }

        pkg.RootDir = pkgDir;
        // An explicit empty manifest "assets" declares a code-only package;
        // AssetsDir stays empty so the mount pass skips it silently.
        pkg.AssetsDir = pkg.Manifest.AssetsDir.empty()
                            ? std::filesystem::path{}
                            : AssetPaths::NormalizeMountRoot(pkgDir / pkg.Manifest.AssetsDir);
        pkg.SourceKind = sourceKind;
        pkg.GitCommit = std::move(gitCommit);
        pkg.Disabled = projectManifest.Disabled.count(name) != 0;
        aliasOwners.emplace(pkg.Alias, name);
        located.emplace(name, std::move(pkg));
    }

    // Implicit engine packages (name shadowing already applied above). The
    // staged dir was parsed during discovery; re-parse here is deliberate —
    // TryLoadPackageManifest is cheap and keeps one code path for validation.
    for (const auto& [name, pkgDir] : enginePackageDirs)
    {
        LocatedPackage pkg;
        std::string error;
        if (!TryLoadPackageManifest(pkgDir / "package.json", pkg.Manifest, error))
        {
            result.Errors.push_back(std::move(error));
            continue;
        }

        pkg.Alias = SanitizePackageAlias(name);
        const bool reserved =
            std::find(std::begin(kReservedAliases), std::end(kReservedAliases), pkg.Alias) !=
            std::end(kReservedAliases);
        if (reserved)
        {
            result.Errors.push_back("engine package '" + name + "': mount alias '" + pkg.Alias +
                                    "' is reserved; rename the package");
            continue;
        }
        if (const auto ownerIt = aliasOwners.find(pkg.Alias); ownerIt != aliasOwners.end())
        {
            result.Errors.push_back("engine package '" + name + "': mount alias '" + pkg.Alias +
                                    "' collides with package '" + ownerIt->second +
                                    "' after sanitization; rename one of them");
            continue;
        }

        pkg.RootDir = pkgDir;
        // An explicit empty manifest "assets" declares a code-only package;
        // AssetsDir stays empty so the mount pass skips it silently.
        pkg.AssetsDir = pkg.Manifest.AssetsDir.empty()
                            ? std::filesystem::path{}
                            : AssetPaths::NormalizeMountRoot(pkgDir / pkg.Manifest.AssetsDir);
        pkg.SourceKind = PackageSourceKind::Engine;
        // Where this package is committed, when the staged tree names an
        // authoring tree and still holds this package's directory in it.
        if (!enginePackageAuthoringRoot.empty())
        {
            std::error_code authoringEc;
            const std::filesystem::path authored =
                enginePackageAuthoringRoot / pkgDir.filename();
            if (std::filesystem::is_directory(authored, authoringEc))
                pkg.AuthoringRootDir = AssetPaths::NormalizeMountRoot(authored);
        }
        pkg.Disabled = projectManifest.Disabled.count(name) != 0;
        aliasOwners.emplace(pkg.Alias, name);
        located.emplace(name, std::move(pkg));
    }

    // Lock hygiene: entries whose dependency left the manifest (or stopped
    // being a git spec) are dead — drop them with the next write.
    if (gitState.LockLoaded)
    {
        for (auto it = gitState.Lock.Packages.begin(); it != gitState.Lock.Packages.end();)
        {
            const auto depIt = projectManifest.Dependencies.find(it->first);
            const bool stillGit =
                depIt != projectManifest.Dependencies.end() && IsGitPackageSpec(depIt->second);
            if (!stillGit)
            {
                gitState.LockDirty = true;
                it = gitState.Lock.Packages.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (gitState.LockDirty && !SavePackagesLock(gitState.LockFile, gitState.Lock))
            result.Warnings.push_back("could not write " + gitState.LockFile.generic_string() +
                                      "; git pins will re-resolve next open");
    }

    for (const auto& disabledName : projectManifest.Disabled)
    {
        if (projectManifest.Dependencies.count(disabledName) == 0 &&
            enginePackageDirs.count(disabledName) == 0)
            result.Warnings.push_back("disabled entry '" + disabledName +
                                      "' is not a declared dependency; ignored");
    }

    // ------------------------------------------------------------------
    // Flat semver validation over the local set. Each package's declared
    // dependencies must be satisfiable by the resolved set (one version per
    // name locally, so "highest compatible wins" reduces to satisfiability).
    // Failures are per-package: the package is skipped, others continue.
    // ------------------------------------------------------------------
    for (auto& [name, pkg] : located)
    {
        for (const auto& [depName, range] : pkg.Manifest.Dependencies)
        {
            const auto depIt = located.find(depName);
            if (depIt == located.end())
            {
                result.Errors.push_back("package '" + name + "': missing dependency '" + depName + "@" +
                                        range.ToString() + "' (not in the project package set); package skipped");
                pkg.Skipped = true;
                continue;
            }
            const PackageVersion& depVersion = depIt->second.Manifest.Version;
            if (!range.Matches(depVersion))
            {
                result.Errors.push_back("package '" + name + "': dependency conflict — requires '" + depName +
                                        "@" + range.ToString() + "' but the resolved set has " + depName + "@" +
                                        depVersion.ToString() + "; package skipped");
                pkg.Skipped = true;
            }
        }
    }

    // Propagate skips through dependents: a package whose dependency is
    // skipped (error) or disabled (warning) cannot mount either. Fixpoint
    // iteration — the set only shrinks, so this terminates.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (auto& [name, pkg] : located)
        {
            if (pkg.Skipped || pkg.Disabled)
                continue;
            for (const auto& [depName, range] : pkg.Manifest.Dependencies)
            {
                const auto depIt = located.find(depName);
                if (depIt == located.end())
                    continue; // already reported above
                if (depIt->second.Disabled)
                {
                    result.Warnings.push_back("package '" + name + "': dependency '" + depName +
                                              "' is disabled in the project manifest; package skipped");
                    pkg.Skipped = true;
                    changed = true;
                    break;
                }
                if (depIt->second.Skipped)
                {
                    result.Errors.push_back("package '" + name + "': dependency '" + depName +
                                            "' failed to resolve; package skipped");
                    pkg.Skipped = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Topological order (Kahn) over the surviving enabled set, dependencies
    // first. std::map iteration keeps the order deterministic (alphabetical
    // among peers). Leftover nodes are on (or downstream of) a cycle.
    // ------------------------------------------------------------------
    std::map<std::string, int> indegree;
    std::map<std::string, std::vector<std::string>> dependents; // dep -> dependents
    for (const auto& [name, pkg] : located)
    {
        if (pkg.Skipped || pkg.Disabled)
            continue;
        indegree.emplace(name, 0);
    }
    for (const auto& [name, pkg] : located)
    {
        if (pkg.Skipped || pkg.Disabled)
            continue;
        for (const auto& [depName, range] : pkg.Manifest.Dependencies)
        {
            if (indegree.count(depName) == 0)
                continue;
            ++indegree[name];
            dependents[depName].push_back(name);
        }
    }

    std::vector<std::string> topoOrder;
    topoOrder.reserve(indegree.size());
    std::set<std::string> ready;
    for (const auto& [name, degree] : indegree)
    {
        if (degree == 0)
            ready.insert(name);
    }
    while (!ready.empty())
    {
        const std::string name = *ready.begin();
        ready.erase(ready.begin());
        topoOrder.push_back(name);
        for (const auto& dependent : dependents[name])
        {
            if (--indegree[dependent] == 0)
                ready.insert(dependent);
        }
    }

    if (topoOrder.size() < indegree.size())
    {
        // Name one concrete cycle: walk dependency edges inside the leftover
        // set until a node repeats.
        std::set<std::string> leftover;
        for (const auto& [name, degree] : indegree)
        {
            if (degree > 0)
                leftover.insert(name);
        }
        std::vector<std::string> walk;
        std::set<std::string> walkSeen;
        std::string current = *leftover.begin();
        while (walkSeen.insert(current).second)
        {
            walk.push_back(current);
            const LocatedPackage& pkg = located.at(current);
            for (const auto& [depName, range] : pkg.Manifest.Dependencies)
            {
                if (leftover.count(depName) != 0)
                {
                    current = depName;
                    break;
                }
            }
        }
        walk.push_back(current); // closes the loop
        std::string cycleText;
        const auto cycleStart = std::find(walk.begin(), walk.end(), current);
        for (auto it = cycleStart; it != walk.end(); ++it)
        {
            if (!cycleText.empty())
                cycleText += " -> ";
            cycleText += *it;
        }
        std::string skippedText;
        for (const auto& name : leftover)
        {
            located.at(name).Skipped = true;
            if (!skippedText.empty())
                skippedText += ", ";
            skippedText += name;
        }
        result.Errors.push_back("dependency cycle: " + cycleText + "; skipped packages: " + skippedText);
    }

    // ------------------------------------------------------------------
    // Priorities: base + topo index (dependencies lower, dependents higher —
    // see kPackageMountPriorityBase). Clamp at the band max.
    // ------------------------------------------------------------------
    bool bandOverflowWarned = false;
    int32_t nextPriority = kPackageMountPriorityBase;
    for (const auto& name : topoOrder)
    {
        LocatedPackage& pkg = located.at(name);
        if (pkg.Skipped)
            continue;

        ResolvedPackage resolved;
        resolved.Manifest = std::move(pkg.Manifest);
        resolved.RootDir = std::move(pkg.RootDir);
        resolved.AssetsDir = std::move(pkg.AssetsDir);
        resolved.AuthoringRootDir = std::move(pkg.AuthoringRootDir);
        resolved.Alias = std::move(pkg.Alias);
        resolved.SourceKind = pkg.SourceKind;
        resolved.GitCommit = std::move(pkg.GitCommit);
        resolved.Priority = std::min(nextPriority, kPackageMountPriorityMax);
        if (nextPriority > kPackageMountPriorityMax && !bandOverflowWarned)
        {
            result.Warnings.push_back(
                "more packages than the priority band 30..69 holds; later packages share priority 69");
            bandOverflowWarned = true;
        }
        ++nextPriority;
        result.MountOrder.push_back(std::move(resolved));
    }

    for (auto& [name, pkg] : located)
    {
        if (!pkg.Disabled || pkg.Skipped)
            continue;
        ResolvedPackage resolved;
        resolved.Manifest = std::move(pkg.Manifest);
        resolved.RootDir = std::move(pkg.RootDir);
        resolved.AssetsDir = std::move(pkg.AssetsDir);
        resolved.AuthoringRootDir = std::move(pkg.AuthoringRootDir);
        resolved.Alias = std::move(pkg.Alias);
        resolved.SourceKind = pkg.SourceKind;
        resolved.GitCommit = std::move(pkg.GitCommit);
        result.Disabled.push_back(std::move(resolved));
    }

    return result;
}

} // namespace GameEngine
