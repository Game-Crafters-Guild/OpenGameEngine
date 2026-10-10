#include "Projects/ProjectAcquisition.h"

#include "Editor/EditorPaths.h"
#include "Editor/Settings/SettingsStore.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Projects/SubprocessStream.h"
#include "Platform/HttpClient.h"
#include "Projects/ZipArchive.h"
#include "Core/Application.h"
#include "JobSystem/JobChannel.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace GameEngine::Editor
{

namespace fs = std::filesystem;

namespace
{

#if defined(_WIN32)
constexpr const char* kGitExeName = "git.exe";
constexpr char kPathListSeparator = ';';
#else
constexpr const char* kGitExeName = "git";
constexpr char kPathListSeparator = ':';
#endif

// Downloaded project archives are buffered in memory before extraction.
constexpr size_t kMaxProjectArchiveBytes = 512ull * 1024 * 1024;

fs::path FindGitExecutable()
{
    if (const char* pathEnv = std::getenv("PATH"))
    {
        std::string remaining = pathEnv;
        size_t start = 0;
        while (start <= remaining.size())
        {
            const size_t end = remaining.find(kPathListSeparator, start);
            const std::string dir =
                remaining.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty())
            {
                std::error_code ec;
                const fs::path candidate = fs::path(dir) / kGitExeName;
                if (fs::exists(candidate, ec))
                    return candidate;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }

#if defined(_WIN32)
    for (const char* fallback :
         {"C:/Program Files/Git/cmd/git.exe", "C:/Program Files (x86)/Git/cmd/git.exe"})
    {
        std::error_code ec;
        if (fs::exists(fallback, ec))
            return fallback;
    }
#endif
    return {};
}

int CurrentProcessId()
{
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

// git marks pack files read-only; fs::remove_all refuses those on Windows.
// nofollow: a hostile repo could otherwise route the chmod through a symlink
// to a file outside the tree.
void RemoveTreeForce(const fs::path& root)
{
    std::error_code ec;
    if (!fs::exists(root, ec))
        return;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code permEc;
        fs::permissions(it->path(), fs::perms::owner_write,
                        fs::perm_options::add | fs::perm_options::nofollow, permEc);
    }
    fs::remove_all(root, ec);
    if (ec)
        Logger::Log::Warning("Project acquisition: could not remove '{}': {}",
                             root.generic_string(), ec.message());
}

bool RunGit(const fs::path& git, const std::vector<std::string>& args, std::string& outError)
{
    const SubprocessResult result = RunSubprocessStreaming(git, args, [](const std::string&) {});
    if (!result.Spawned)
    {
        outError = "Could not start git.";
        return false;
    }
    if (result.ExitCode != 0)
    {
        outError = result.StderrTail.empty()
            ? "git exited with code " + std::to_string(result.ExitCode)
            : result.StderrTail;
        return false;
    }
    return true;
}

bool CopyTemplatePayload(const fs::path& templateDir, const fs::path& destination,
                         std::string& outError)
{
    std::error_code ec;
    if (!fs::is_directory(templateDir, ec))
    {
        outError = "Template folder not found: " + templateDir.generic_string();
        return false;
    }
    for (fs::directory_iterator it(templateDir, ec), end; !ec && it != end; it.increment(ec))
    {
        // The per-template thumbnail is catalog art, not project content.
        std::error_code entryEc;
        if (it->is_regular_file(entryEc) && it->path().stem().string() == "thumbnail")
            continue;
        // Streamed, not fs::copy: the template is a preloaded read-only tree and
        // the destination may be OPFS, which has no permission bits to preserve.
        if (!FileSystem::CopyTree(it->path(), destination / it->path().filename()))
        {
            outError = "Copy failed: " + it->path().filename().generic_string();
            return false;
        }
    }
    if (ec)
    {
        outError = "Copy failed: " + ec.message();
        return false;
    }
    return true;
}

// Move the acquired content into the destination; fs::rename when possible
// (same volume), recursive copy otherwise.
bool MoveTree(const fs::path& from, const fs::path& to, std::string& outError)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec)
    {
        outError = "Move failed: " + ec.message();
        return false;
    }
    RemoveTreeForce(from);
    return true;
}

void StampProjectSettings(const ProjectAcquireRequest& request, const fs::path& settingsFile)
{
    SettingsStore settings(settingsFile);
    std::string err;
    (void)settings.Load(&err);
    if (!request.DisplayName.empty())
        settings.SetString("project.displayName", request.DisplayName);
    const bool remoteOrigin = (request.Entry.Source.Type == ProjectSourceType::Git ||
                               request.Entry.Source.Type == ProjectSourceType::Zip) &&
                              !request.Entry.Source.Url.empty();
    if (remoteOrigin)
    {
        settings.SetString("project.origin.manifestId", request.Entry.Id);
        settings.SetString("project.origin.url", request.Entry.Source.Url);
        if (!request.Entry.Source.Ref.empty())
            settings.SetString("project.origin.ref", request.Entry.Source.Ref);
        if (!request.Entry.Source.Path.empty())
            settings.SetString("project.origin.path", request.Entry.Source.Path);
    }
    else if (request.Entry.Source.Type == ProjectSourceType::None ||
             request.Entry.Source.Type == ProjectSourceType::Local)
    {
        settings.SetString("project.template", request.Entry.Id);
    }
    if (!settings.Save(&err))
        Logger::Log::Warning("Project acquisition: could not save project settings: {}", err);
}

} // namespace

ProjectAcquisition::ProjectAcquisition(JobSystem::WorkStealingThreadPool& jobSystem)
    : m_Channel(std::make_unique<JobSystem::JobChannel>(
          jobSystem, JobSystem::JobChannelDesc{.Name = "Project acquisition", .MaxRunning = 1}))
{
}

ProjectAcquisition::~ProjectAcquisition() = default;

bool ProjectAcquisition::IsGitAvailable()
{
    return !FindGitExecutable().empty();
}

bool ProjectAcquisition::Start(ProjectAcquireRequest request)
{
    bool expected = false;
    if (!m_Shared->Running.compare_exchange_strong(expected, true))
        return false;

    {
        std::lock_guard<std::mutex> lock(m_Shared->Mutex);
        m_Shared->Status = {ProjectAcquireState::Running, request.Destination, {}};
        m_Shared->StatusDirty = true;
    }

    // Resolve everything environment-dependent on the calling thread:
    // editor paths / preferences / getenv are not synchronized for
    // cross-thread access.
    WorkerContext context;
    context.StagingRoot = GetEditorGlobalPaths().communityCacheRoot / "staging";
    context.SettingsFile = GetEditorProjectPaths(request.Destination).projectSettingsFile;
    if (request.Entry.Source.Type == ProjectSourceType::Git)
        context.GitExecutable = FindGitExecutable();

    // A channel job, not a thread of its own: the shared state keeps the status
    // alive past this object, and a platform without transient threads
    // (Platform::SupportsTransientThreads) has no other way to run it.
    (void)m_Channel->Submit(
        [state = m_Shared, request = std::move(request),
         context = std::move(context)]() mutable {
            RunWorker(state, std::move(request), std::move(context));
        });
    return true;
}

bool ProjectAcquisition::Poll(ProjectAcquireStatus& outStatus)
{
    std::lock_guard<std::mutex> lock(m_Shared->Mutex);
    if (!m_Shared->StatusDirty)
        return false;
    m_Shared->StatusDirty = false;
    outStatus = m_Shared->Status;
    return true;
}

void ProjectAcquisition::PublishTo(SharedState& state, ProjectAcquireState status,
                                   const fs::path& destination, std::string error)
{
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.Status = {status, destination, std::move(error)};
        state.StatusDirty = true;
    }
    if (status != ProjectAcquireState::Running)
        state.Running.store(false);
}

void ProjectAcquisition::RunWorker(const std::shared_ptr<SharedState>& state,
                                   ProjectAcquireRequest request, WorkerContext context)
{
    const fs::path& destination = request.Destination;

    // Only ever delete a destination this worker created — an "already
    // exists" verdict from the UI thread can go stale (or come back false on
    // IO error), and force-deleting a folder we didn't make is data loss.
    std::error_code ec;
    const bool destinationExisted = fs::exists(destination, ec);
    if (destinationExisted && !fs::is_empty(destination, ec))
    {
        PublishTo(*state, ProjectAcquireState::Failed, destination,
                "The destination folder already exists and is not empty.");
        return;
    }

    auto fail = [&](std::string error) {
        if (!destinationExisted)
            RemoveTreeForce(destination);
        PublishTo(*state, ProjectAcquireState::Failed, destination, std::move(error));
    };

    fs::create_directories(destination, ec);
    if (ec)
    {
        PublishTo(*state, ProjectAcquireState::Failed, destination,
                "Could not create the project folder: " + ec.message());
        return;
    }

    std::string error;
    switch (request.Entry.Source.Type)
    {
    case ProjectSourceType::None:
        break;

    case ProjectSourceType::Local:
        if (!CopyTemplatePayload(request.CatalogRoot / request.Entry.Source.Path, destination,
                                 error))
        {
            fail(std::move(error));
            return;
        }
        break;

    case ProjectSourceType::Git:
    {
        if (context.GitExecutable.empty())
        {
            fail("Cloning community projects requires Git. Install it from git-scm.com and retry.");
            return;
        }

        // Never let this git invocation block on an auth prompt: no terminal
        // prompt, no credential helper GUI. Community sources are public
        // http(s) repos; auth failures should fail fast with git's error.
#if defined(_WIN32)
        (void)_putenv("GIT_TERMINAL_PROMPT=0");
#else
        (void)setenv("GIT_TERMINAL_PROMPT", "0", 1);
#endif
        const std::vector<std::string> gitBase = {
            "-c", "core.longpaths=true", "-c", "credential.helper="};

        const std::string& subfolder = request.Entry.Source.Path;
        // Short path (git pack names inside can exceed MAX_PATH fast) that is
        // unique per editor instance so two editors can't clear each other's
        // in-progress clone.
        const fs::path staging = context.StagingRoot /
            (request.Entry.Id + "-" + std::to_string(CurrentProcessId()));
        RemoveTreeForce(staging);
        fs::create_directories(staging.parent_path(), ec);

        std::vector<std::string> cloneArgs = gitBase;
        cloneArgs.insert(cloneArgs.end(), {"clone", "--depth", "1"});
        if (!subfolder.empty())
        {
            cloneArgs.push_back("--filter=blob:none");
            cloneArgs.push_back("--sparse");
        }
        if (!request.Entry.Source.Ref.empty())
        {
            cloneArgs.push_back("--branch");
            cloneArgs.push_back(request.Entry.Source.Ref);
        }
        // "--" so a hostile url/ref can never be parsed as a git option; the
        // parser already restricts urls to http(s).
        cloneArgs.push_back("--");
        cloneArgs.push_back(request.Entry.Source.Url);
        cloneArgs.push_back(PathArgUtf8(staging));

        if (!RunGit(context.GitExecutable, cloneArgs, error))
        {
            RemoveTreeForce(staging);
            fail("Clone failed: " + error);
            return;
        }
        if (!subfolder.empty())
        {
            std::vector<std::string> sparseArgs = gitBase;
            sparseArgs.insert(sparseArgs.end(),
                              {"-C", PathArgUtf8(staging), "sparse-checkout", "set", "--",
                               subfolder});
            if (!RunGit(context.GitExecutable, sparseArgs, error))
            {
                RemoveTreeForce(staging);
                fail("Sparse checkout failed: " + error);
                return;
            }
        }

        RemoveTreeForce(staging / ".git");
        const fs::path contentRoot = subfolder.empty() ? staging : staging / subfolder;
        if (!fs::is_directory(contentRoot, ec) || fs::is_empty(contentRoot, ec))
        {
            RemoveTreeForce(staging);
            fail(subfolder.empty()
                     ? "The repository is empty."
                     : "The repository has no folder '" + subfolder + "'.");
            return;
        }

        // Replace the (empty) destination with the acquired tree; if the
        // remove fails the copy fallback in MoveTree merges into it instead.
        fs::remove(destination, ec);
        ec.clear();
        if (!MoveTree(contentRoot, destination, error))
        {
            RemoveTreeForce(staging);
            fail(std::move(error));
            return;
        }
        RemoveTreeForce(staging);
        break;
    }

    case ProjectSourceType::Zip:
    {
        fs::path zipFile = request.LocalArchivePath;
        fs::path downloadedZip;
        if (zipFile.empty())
        {
            const HttpClient::HttpResponse response =
                HttpClient::Get(request.Entry.Source.Url, {}, kMaxProjectArchiveBytes);
            if (!response.success || response.statusCode != 200 || response.body.empty())
            {
                fail("Download failed: " +
                     (response.error.empty() ? "HTTP " + std::to_string(response.statusCode)
                                             : response.error));
                return;
            }
            downloadedZip = context.StagingRoot /
                (request.Entry.Id + "-" + std::to_string(CurrentProcessId()) + ".zip");
            fs::create_directories(downloadedZip.parent_path(), ec);
            std::ofstream out(downloadedZip, std::ios::binary | std::ios::trunc);
            if (!out ||
                !out.write(response.body.data(),
                           static_cast<std::streamsize>(response.body.size()))
                     .good())
            {
                fail("Could not write the downloaded archive.");
                return;
            }
            out.close();
            zipFile = downloadedZip;
        }

        const bool extracted =
            ExtractZipArchive(zipFile, destination, request.Entry.Source.Path, &error);
        if (!downloadedZip.empty())
        {
            fs::remove(downloadedZip, ec);
            ec.clear();
        }
        if (!extracted)
        {
            fail(std::move(error));
            return;
        }

        // Zips commonly wrap everything in one root folder (GitHub archive
        // downloads always do); collapse it so the project root is the
        // destination itself. Names can't collide — the wrapper was the
        // destination's only child.
        if (request.Entry.Source.Path.empty())
        {
            fs::directory_iterator it(destination, ec), end;
            if (!ec && it != end)
            {
                const fs::path onlyChild = it->path();
                const bool singleDirectory =
                    it->is_directory(ec) && (it.increment(ec), it == end);
                if (singleDirectory)
                {
                    std::vector<fs::path> children;
                    for (fs::directory_iterator childIt(onlyChild, ec), childEnd;
                         !ec && childIt != childEnd; childIt.increment(ec))
                        children.push_back(childIt->path());
                    for (const fs::path& child : children)
                    {
                        fs::rename(child, destination / child.filename(), ec);
                        if (ec)
                        {
                            fail("Could not restructure the extracted archive: " + ec.message());
                            return;
                        }
                    }
                    fs::remove(onlyChild, ec);
                    ec.clear();
                }
            }
        }
        break;
    }

    case ProjectSourceType::Unsupported:
        fail("This project's source type is not supported by this build.");
        return;
    }

    fs::create_directories(destination / "Assets", ec);
    StampProjectSettings(request, context.SettingsFile);
    PublishTo(*state, ProjectAcquireState::Succeeded, destination, {});
}

} // namespace GameEngine::Editor
