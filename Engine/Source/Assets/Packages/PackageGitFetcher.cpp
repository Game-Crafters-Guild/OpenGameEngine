#include "Assets/Packages/PackageGitFetcher.h"

#include "AssetCore/SharedFileRead.h"

#include "Assets/Packages/PackageManifest.h"
#include "Assets/Packages/PackagesLockFile.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>

namespace GameEngine
{

namespace
{

namespace fs = std::filesystem;

// Network fetches of small package repos finish in seconds; five minutes is a
// hung remote, not a slow one.
constexpr std::chrono::milliseconds kGitTimeout{300000};

std::string Trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    return text.substr(start);
}

bool RunGit(const std::string& gitExe,
            const std::vector<std::string>& args,
            std::string& outOutput,
            std::string& outError)
{
    const ShellProcessResult result = RunProcessCaptured(gitExe, args, kGitTimeout);
    outOutput = result.output;
    if (result.exitCode == 0 && !result.cancelled)
        return true;

    std::string commandText = "git";
    for (const std::string& arg : args)
    {
        commandText += ' ';
        commandText += arg;
    }
    outError = "'" + commandText + "' failed (exit " + std::to_string(result.exitCode) + ")";
    const std::string trimmedOutput = Trimmed(result.output);
    if (!trimmedOutput.empty())
        outError += ": " + trimmedOutput;
    return false;
}

// remove_all refuses read-only files on Windows, and .git objects are exactly
// that — clear the read-only bit first so temp clones always clean up.
void RemoveTreeForce(const fs::path& root)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
        return;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code permEc;
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, permEc);
    }
    fs::remove_all(root, ec);
    if (ec)
        Logger::Log::Warning("Packages: could not clean temp dir '{}': {}", root.string(),
                             ec.message());
}

fs::path MakeTempWorkDir(const fs::path& cacheRoot, std::string& outError)
{
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex rngMutex;
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(rngMutex);
        token = rng();
    }
    std::ostringstream name;
    name << "fetch-" << std::hex << token;
    const fs::path dir = cacheRoot / ".tmp" / name.str();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        outError = "cannot create temp dir '" + dir.generic_string() + "': " + ec.message();
        return {};
    }
    return dir;
}

bool CopyPackagePayload(const fs::path& sourceDir, const fs::path& destDir, std::string& outError)
{
    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec)
    {
        outError = "cannot create '" + destDir.generic_string() + "': " + ec.message();
        return false;
    }
    for (fs::directory_iterator it(sourceDir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->path().filename() == ".git")
            continue;
        std::error_code copyEc;
        fs::copy(it->path(), destDir / it->path().filename(),
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, copyEc);
        if (copyEc)
        {
            outError = "cannot copy '" + it->path().generic_string() + "': " + copyEc.message();
            return false;
        }
    }
    if (ec)
    {
        outError = "cannot enumerate '" + sourceDir.generic_string() + "': " + ec.message();
        return false;
    }
    return true;
}

} // namespace

bool TryFindGitExecutable(std::string& outExe, std::string& outError)
{
    // One probe per process: the answer cannot change mid-session, and resolve
    // may ask once per git dependency.
    static std::mutex probeMutex;
    static bool probed = false;
    static bool found = false;
    static std::string cachedError;

    std::lock_guard<std::mutex> lock(probeMutex);
    if (!probed)
    {
        probed = true;
        const ShellProcessResult result =
            RunProcessCaptured("git", {"--version"}, std::chrono::milliseconds(15000));
        found = result.exitCode == 0 && !result.cancelled;
        if (!found)
            cachedError = "git is required for git package dependencies but was not found on "
                          "PATH (running 'git --version' failed) — install git or remove the "
                          "git dependencies from Packages/manifest.json";
    }
    if (!found)
    {
        outError = cachedError;
        return false;
    }
    outExe = "git";
    return true;
}

bool AcquireGitPackage(const std::string& gitExe,
                       const std::string& packageName,
                       const GitPackageSpec& spec,
                       const fs::path& cacheRoot,
                       GitPackageAcquisition& out,
                       std::string& outError)
{
    out = GitPackageAcquisition{};

    const fs::path workDir = MakeTempWorkDir(cacheRoot, outError);
    if (workDir.empty())
        return false;
    struct TempCleanup
    {
        const fs::path& Dir;
        ~TempCleanup() { RemoveTreeForce(Dir); }
    } cleanup{workDir};

    const std::string workDirText = workDir.generic_string();
    std::string output;

    // init + fetch <ref> handles tags, branches, AND commit shas uniformly —
    // `clone --branch` cannot take a sha. Shallow: exactly one commit's tree.
    if (!RunGit(gitExe, {"init", "-q", workDirText}, output, outError))
        return false;
    if (!RunGit(gitExe, {"-C", workDirText, "remote", "add", "origin", spec.Url}, output, outError))
        return false;
    if (!RunGit(gitExe, {"-C", workDirText, "fetch", "-q", "--depth", "1", "origin", spec.Ref},
                output, outError))
    {
        outError = "package '" + packageName + "': cannot fetch ref '" + spec.Ref + "' from '" +
                   spec.Url + "' — " + outError;
        return false;
    }
    if (!RunGit(gitExe,
                {"-C", workDirText, "-c", "advice.detachedHead=false", "checkout", "-q",
                 "--detach", "FETCH_HEAD"},
                output, outError))
        return false;
    if (!RunGit(gitExe, {"-C", workDirText, "rev-parse", "HEAD"}, output, outError))
        return false;
    const std::string commit = Trimmed(output);
    if (commit.size() < 12)
    {
        outError = "package '" + packageName + "': git rev-parse returned no commit sha";
        return false;
    }

    // Read the package manifest out of the checkout (monorepo subdir or root).
    fs::path payloadDir = workDir;
    if (!spec.Subdir.empty())
        payloadDir /= fs::path(spec.Subdir);
    const fs::path manifestFile = payloadDir / "package.json";
    String manifestText;
    if (!ReadFileTextShared(manifestFile, manifestText))
    {
        outError = "package '" + packageName + "': no package.json at '" +
                   (spec.Subdir.empty() ? std::string("<repo root>") : spec.Subdir) +
                   "' in " + spec.Url + "#" + spec.Ref;
        return false;
    }

    PackageManifest manifest;
    if (!TryParsePackageManifest(manifestText, manifestFile, manifest, outError))
        return false;
    if (!packageName.empty() && manifest.Name != packageName)
    {
        outError = "package '" + packageName + "': manifest at " + spec.Url + "#" + spec.Ref +
                   (spec.Subdir.empty() ? "" : "&path=" + spec.Subdir) + " names '" +
                   manifest.Name + "' — dependency key and package name must match";
        return false;
    }

    // Populate the content-addressed entry. A concurrent/prior populate of the
    // same commit is reused verbatim — entries are immutable, so the existing
    // payload IS this payload.
    const std::string entryName =
        PackageCacheEntryName(manifest.Name, manifest.Version.ToString(), commit);
    const fs::path entryDir = cacheRoot / entryName;
    std::error_code ec;
    if (!fs::exists(entryDir, ec))
    {
        const fs::path stagingDir = workDir / ".entry-staging";
        if (!CopyPackagePayload(payloadDir, stagingDir, outError))
            return false;
        fs::rename(stagingDir, entryDir, ec);
        if (ec)
        {
            // Lost a populate race (or rename across volumes failed): if the
            // entry exists now, someone else finished it; otherwise fail.
            std::error_code existsEc;
            if (!fs::exists(entryDir, existsEc))
            {
                outError = "package '" + packageName + "': cannot move cache entry into '" +
                           entryDir.generic_string() + "': " + ec.message();
                return false;
            }
        }
        Logger::Log::Info("Packages: cached '{}' {} from {}#{} as '{}'", manifest.Name,
                          manifest.Version.ToString(), spec.Url, spec.Ref, entryName);
    }

    out.PackageDir = entryDir;
    out.Name = manifest.Name;
    out.Commit = commit;
    out.Version = manifest.Version.ToString();
    out.Integrity = HashPackageManifestIntegrity(manifestText);
    return true;
}

bool QueryGitRemoteRefCommit(const std::string& gitExe,
                             const std::string& url,
                             const std::string& ref,
                             std::string& outCommit,
                             std::string& outError)
{
    outCommit.clear();

    const bool isFullSha =
        ref.size() == 40 && std::all_of(ref.begin(), ref.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (isFullSha)
    {
        outCommit = ref;
        return true;
    }

    // Ask for the ref and its peeled form: annotated tags list the tag object
    // first and the commit as "<ref>^{}" — the peeled sha is what a fetch of
    // the ref checks out, so prefer it when present.
    std::string output;
    if (!RunGit(gitExe, {"ls-remote", url, ref, ref + "^{}"}, output, outError))
        return false;

    std::string plainSha;
    std::string peeledSha;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab < 40)
            continue;
        const std::string sha = line.substr(0, 40);
        const std::string refName = Trimmed(line.substr(tab + 1));
        if (refName.size() >= 3 && refName.compare(refName.size() - 3, 3, "^{}") == 0)
            peeledSha = sha;
        else
            plainSha = sha;
    }

    outCommit = peeledSha.empty() ? plainSha : peeledSha;
    if (outCommit.empty())
    {
        outError = "ref '" + ref + "' not found on remote '" + url + "'";
        return false;
    }
    return true;
}

} // namespace GameEngine
