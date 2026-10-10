#include "Assets/Packages/PackageManagementActions.h"

#include "Assets/Packages/PackageGitFetcher.h"
#include "Assets/Packages/PackageGitSource.h"
#include "Assets/Packages/PackageManifest.h"
#include "Assets/Packages/PackagesLockFile.h"
#include "Assets/Packages/ProjectPackagesManifest.h"
#include "AssetCore/PathNormalization.h"

#include <system_error>

namespace GameEngine
{

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view kManagementFileSpecPrefix = "file:";

fs::path PackagesDir(const fs::path& projectRoot) { return projectRoot / "Packages"; }
fs::path ManifestFile(const fs::path& projectRoot) { return PackagesDir(projectRoot) / "manifest.json"; }
fs::path LockFile(const fs::path& projectRoot)
{
    return PackagesDir(projectRoot) / kPackagesLockFileName;
}

// True when `dir` is <packagesDir>/<name>, the embedded location. Roots keep
// the caller's spelling, so the comparison runs in the identity domain: a
// path typed in another case or drive-letter spelling still names the same
// directory on a case-insensitive filesystem.
bool IsEmbeddedPackageDir(const fs::path& dir, const fs::path& packagesDir, const std::string& name)
{
    return AssetPaths::NormalizeForRegistryKey(dir) ==
           AssetPaths::NormalizeForRegistryKey(packagesDir / name);
}

// The directory must contain a parseable package.json; its manifest comes out.
bool TryLoadPackageDirManifest(const fs::path& dir, PackageManifest& out, std::string& outError)
{
    const fs::path manifestFile = dir / "package.json";
    std::error_code ec;
    if (!fs::exists(manifestFile, ec))
    {
        outError = "no package.json at '" + dir.generic_string() + "'";
        return false;
    }
    return TryLoadPackageManifest(manifestFile, out, outError);
}

bool IsDeclaredInManifest(const fs::path& projectRoot,
                          const std::string& packageName,
                          std::string& outSpec,
                          std::string& outError)
{
    ProjectPackagesManifest manifest;
    if (!TryLoadProjectPackagesManifest(ManifestFile(projectRoot), manifest, outError))
        return false;
    const auto it = manifest.Dependencies.find(packageName);
    if (it == manifest.Dependencies.end())
    {
        outError = "package '" + packageName + "' is not declared in Packages/manifest.json";
        return false;
    }
    outSpec = it->second;
    return true;
}

} // namespace

bool IsGitAddInput(const std::string& input) { return IsGitPackageSpec(input); }

bool AddLocalPackageToProject(const fs::path& projectRoot,
                              const std::string& inputPath,
                              PackageAddResult& out,
                              std::string& outError)
{
    out = PackageAddResult{};
    outError.clear();

    std::string raw = inputPath;
    if (raw.rfind(kManagementFileSpecPrefix, 0) == 0)
        raw = raw.substr(kManagementFileSpecPrefix.size());
    if (raw.empty())
    {
        outError = "empty package path";
        return false;
    }

    const fs::path typed(raw);
    const fs::path packagesDir = PackagesDir(projectRoot);
    const fs::path resolvedDir =
        AssetPaths::NormalizeMountRoot(typed.is_absolute() ? typed : packagesDir / typed);

    PackageManifest pkgManifest;
    if (!TryLoadPackageDirManifest(resolvedDir, pkgManifest, outError))
        return false;

    ProjectPackagesManifest projectManifest;
    if (!TryLoadProjectPackagesManifest(ManifestFile(projectRoot), projectManifest, outError))
        return false;
    if (projectManifest.Dependencies.count(pkgManifest.Name) != 0)
    {
        outError = "package '" + pkgManifest.Name + "' is already declared in Packages/manifest.json";
        return false;
    }

    // <project>/Packages/<name> is the embedded location — canonical spec, no path.
    std::string spec;
    if (IsEmbeddedPackageDir(resolvedDir, packagesDir, pkgManifest.Name))
        spec = "embedded";
    else
        spec = std::string(kManagementFileSpecPrefix) + typed.generic_string();

    if (!AddPackageToProjectManifest(ManifestFile(projectRoot), pkgManifest.Name, spec, outError))
        return false;

    out.Name = pkgManifest.Name;
    out.Spec = spec;
    return true;
}

bool AddGitPackageToProject(const fs::path& projectRoot,
                            const std::string& spec,
                            PackageAddResult& out,
                            std::string& outError)
{
    out = PackageAddResult{};
    outError.clear();

    GitPackageSpec parsed;
    if (!TryParseGitPackageSpec("(new)", spec, parsed, outError))
        return false;

    std::string gitExe;
    if (!TryFindGitExecutable(gitExe, outError))
        return false;

    // Acquire first: the name comes from the fetched package.json, and any
    // URL/ref/manifest failure surfaces here — inline — instead of as a
    // resolver error on the next project open.
    GitPackageAcquisition acquired;
    if (!AcquireGitPackage(gitExe, /*packageName=*/"", parsed, GlobalPackageCacheRoot(), acquired,
                           outError))
        return false;

    ProjectPackagesManifest projectManifest;
    if (!TryLoadProjectPackagesManifest(ManifestFile(projectRoot), projectManifest, outError))
        return false;
    if (projectManifest.Dependencies.count(acquired.Name) != 0)
    {
        outError = "package '" + acquired.Name + "' is already declared in Packages/manifest.json";
        return false;
    }

    if (!AddPackageToProjectManifest(ManifestFile(projectRoot), acquired.Name, spec, outError))
        return false;

    // Pin what was just fetched so the follow-up resolve is offline.
    PackagesLockEntry entry;
    entry.Spec = spec;
    entry.Url = parsed.Url;
    entry.Ref = parsed.Ref;
    entry.Commit = acquired.Commit;
    entry.Version = acquired.Version;
    entry.Integrity = acquired.Integrity;
    if (!UpsertPackagesLockEntry(LockFile(projectRoot), acquired.Name, entry, outError))
    {
        outError += " (dependency was added; the next resolve re-pins)";
        return false;
    }

    out.Name = acquired.Name;
    out.Spec = spec;
    return true;
}

bool RemovePackageFromProject(const fs::path& projectRoot,
                              const std::string& packageName,
                              std::string& outError)
{
    outError.clear();
    if (!RemovePackageFromProjectManifest(ManifestFile(projectRoot), packageName, outError))
        return false;
    // The resolver's lock hygiene only runs when some other git dependency
    // loads the lock — drop the dead pin now, not "eventually".
    if (!RemovePackagesLockEntry(LockFile(projectRoot), packageName, outError))
    {
        outError += " (dependency was removed from the manifest)";
        return false;
    }
    return true;
}

bool RepinGitPackageToRef(const fs::path& projectRoot,
                          const std::string& packageName,
                          const std::string& newRef,
                          std::string& outError)
{
    outError.clear();
    if (newRef.empty() || newRef.find('#') != std::string::npos ||
        newRef.find('&') != std::string::npos || newRef.find(' ') != std::string::npos)
    {
        outError = "package '" + packageName + "': '" + newRef +
                   "' is not a valid ref (tag, branch, or commit sha)";
        return false;
    }

    std::string currentSpec;
    if (!IsDeclaredInManifest(projectRoot, packageName, currentSpec, outError))
        return false;
    if (!IsGitPackageSpec(currentSpec))
    {
        outError = "package '" + packageName + "' is not a git dependency (spec '" + currentSpec + "')";
        return false;
    }
    GitPackageSpec parsed;
    if (!TryParseGitPackageSpec(packageName, currentSpec, parsed, outError))
        return false;

    // Keep the URL exactly as the user wrote it (incl. the git+ prefix style);
    // swap only the fragment.
    const size_t hash = currentSpec.find('#');
    std::string newSpec = currentSpec.substr(0, hash) + "#" + newRef;
    if (!parsed.Subdir.empty())
        newSpec += "&path=" + parsed.Subdir;

    if (!SetPackageSpecInProjectManifest(ManifestFile(projectRoot), packageName, newSpec, outError))
        return false;

    // Drop the pin unconditionally: re-pinning to the SAME ref text must still
    // re-acquire (that is the "update to latest on ref" action — the resolver
    // honors an unchanged spec's pin by design).
    if (!RemovePackagesLockEntry(LockFile(projectRoot), packageName, outError))
    {
        outError += " (manifest ref was updated)";
        return false;
    }
    return true;
}

bool RelocatePackageInProject(const fs::path& projectRoot,
                              const std::string& packageName,
                              const fs::path& newDir,
                              std::string& outError)
{
    outError.clear();

    std::string currentSpec;
    if (!IsDeclaredInManifest(projectRoot, packageName, currentSpec, outError))
        return false;
    if (IsGitPackageSpec(currentSpec))
    {
        outError = "package '" + packageName + "' is a git dependency — re-pin it instead of relocating";
        return false;
    }

    const fs::path resolvedDir = AssetPaths::NormalizeMountRoot(newDir);
    PackageManifest pkgManifest;
    if (!TryLoadPackageDirManifest(resolvedDir, pkgManifest, outError))
        return false;
    if (pkgManifest.Name != packageName)
    {
        outError = "package.json at '" + resolvedDir.generic_string() + "' names '" +
                   pkgManifest.Name + "' — expected '" + packageName + "'; not relocated";
        return false;
    }

    // Inside the project => keep the manifest portable with a relative path
    // (anchored at Packages/, like every relative file: spec). Outside =>
    // absolute.
    std::string spec;
    const fs::path packagesDir = AssetPaths::NormalizeMountRoot(PackagesDir(projectRoot));
    if (IsEmbeddedPackageDir(resolvedDir, packagesDir, packageName))
    {
        spec = "embedded";
    }
    else
    {
        const fs::path relToRoot =
            resolvedDir.lexically_relative(AssetPaths::NormalizeMountRoot(projectRoot));
        const bool insideProject =
            !relToRoot.empty() && relToRoot.begin()->generic_string() != "..";
        const fs::path relToPackages = resolvedDir.lexically_relative(packagesDir);
        spec = std::string(kManagementFileSpecPrefix) +
               (insideProject && !relToPackages.empty() ? relToPackages.generic_string()
                                                        : resolvedDir.generic_string());
    }

    return SetPackageSpecInProjectManifest(ManifestFile(projectRoot), packageName, spec, outError);
}

bool CheckGitPackageForUpdates(const fs::path& projectRoot,
                               const std::string& packageName,
                               std::string& outStatus,
                               std::string& outError)
{
    outStatus.clear();
    outError.clear();

    std::string currentSpec;
    if (!IsDeclaredInManifest(projectRoot, packageName, currentSpec, outError))
        return false;
    if (!IsGitPackageSpec(currentSpec))
    {
        outError = "package '" + packageName + "' is not a git dependency";
        return false;
    }
    GitPackageSpec parsed;
    if (!TryParseGitPackageSpec(packageName, currentSpec, parsed, outError))
        return false;

    std::string gitExe;
    if (!TryFindGitExecutable(gitExe, outError))
        return false;
    std::string remoteCommit;
    if (!QueryGitRemoteRefCommit(gitExe, parsed.Url, parsed.Ref, remoteCommit, outError))
        return false;

    PackagesLock lock;
    std::string lockError;
    std::string lockedCommit;
    if (TryLoadPackagesLock(LockFile(projectRoot), lock, lockError))
    {
        const auto it = lock.Packages.find(packageName);
        if (it != lock.Packages.end())
            lockedCommit = it->second.Commit;
    }

    const std::string remoteShort = remoteCommit.substr(0, 12);
    if (lockedCommit.empty())
        outStatus = packageName + ": no lock pin yet — next resolve fetches '" + parsed.Ref +
                    "' (currently " + remoteShort + ")";
    else if (lockedCommit == remoteCommit)
        outStatus = packageName + ": up to date — locked " + lockedCommit.substr(0, 12) +
                    " is the latest on '" + parsed.Ref + "'";
    else
        outStatus = packageName + ": update available — '" + parsed.Ref + "' is now " + remoteShort +
                    " (locked " + lockedCommit.substr(0, 12) + "); use Update to take it";
    return true;
}

} // namespace GameEngine
