#include "Projects/ProjectCatalogService.h"

#include "Editor/EditorPaths.h"
#include "Editor/Settings/SettingsStore.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Platform/HttpClient.h"
#include "JobSystem/JobChannel.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace GameEngine::Editor
{

namespace fs = std::filesystem;

namespace
{

constexpr const char* kManifestFileName = "manifest.json";
constexpr const char* kDefaultCommunityManifestUrl =
    "https://raw.githubusercontent.com/Game-Crafters-Guild/OpenEngine-Community-Projects/main/manifest.json";
constexpr const char* kCommunityManifestSourcePrefKey = "communityManifestSource";

bool IsHttpSource(const std::string& source)
{
    return source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
}

// Directory (local) or URL prefix (remote) the manifest's relative thumbnail
// references resolve against.
std::string SourceBaseOf(const std::string& source)
{
    const size_t slash = source.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : source.substr(0, slash);
}

bool ReadFileToString(const fs::path& path, std::string& outText)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    outText = buffer.str();
    return true;
}

// Temp-file + rename so a concurrent reader (second editor instance) never
// sees a torn file.
bool WriteStringToFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out.good())
            return false;
    }
    return GameEngine::FileSystem::PublishFile(temp, path);
}

std::string ThumbnailExtensionOf(const std::string& url)
{
    const size_t query = url.find_first_of("?#");
    const std::string clean = query == std::string::npos ? url : url.substr(0, query);
    const size_t dot = clean.find_last_of('.');
    const size_t slash = clean.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return ".png";
    const std::string ext = clean.substr(dot);
    for (const char* allowed : {".png", ".jpg", ".jpeg", ".webp", ".svg"})
    {
        if (ext == allowed)
            return ext;
    }
    return ".png";
}

// FNV-1a, hex. Folded into cached thumbnail names so a changed URL re-downloads
// and entries with the same id from different catalog sources can't collide.
std::string UrlHashOf(const std::string& url)
{
    uint64_t hash = 14695981039346656037ull;
    for (const char c : url)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

// Rewrite each entry's remote thumbnail URL to a file in the cache dir,
// downloading it when allowed and not already present. Entries whose
// thumbnail can't be produced fall back to an empty ref (placeholder art).
// A fresh fetch (allowDownload) also prunes cache files no longer referenced,
// so re-keyed or removed thumbnails don't accumulate.
void ResolveThumbnailsToCache(ProjectCatalog& catalog, const fs::path& thumbnailCacheDir,
                              bool allowDownload)
{
    std::vector<fs::path> referenced;
    for (ProjectCatalogEntry& entry : catalog.Entries)
    {
        if (entry.ThumbnailRef.empty())
            continue;
        if (!IsHttpSource(entry.ThumbnailRef))
        {
            std::error_code ec;
            if (!fs::exists(entry.ThumbnailRef, ec))
                entry.ThumbnailRef.clear();
            continue;
        }

        const fs::path cached = thumbnailCacheDir /
            (entry.Id + "-" + UrlHashOf(entry.ThumbnailRef) +
             ThumbnailExtensionOf(entry.ThumbnailRef));
        referenced.push_back(cached.filename());
        std::error_code ec;
        if (fs::exists(cached, ec))
        {
            entry.ThumbnailRef = cached.generic_string();
            continue;
        }
        if (!allowDownload)
        {
            entry.ThumbnailRef.clear();
            continue;
        }

        const HttpClient::HttpResponse response = HttpClient::Get(entry.ThumbnailRef);
        if (response.success && response.statusCode == 200 && !response.body.empty() &&
            WriteStringToFile(cached, response.body))
        {
            entry.ThumbnailRef = cached.generic_string();
        }
        else
        {
            Logger::Log::Warning("Community catalog: thumbnail fetch failed for '{}' ({})",
                                 entry.Id, response.error.empty() ? std::to_string(response.statusCode)
                                                                  : response.error);
            entry.ThumbnailRef.clear();
        }
    }

    if (!allowDownload)
        return;
    std::error_code iterEc;
    for (fs::directory_iterator it(thumbnailCacheDir, iterEc), end; !iterEc && it != end;
         it.increment(iterEc))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc))
            continue;
        if (std::find(referenced.begin(), referenced.end(), it->path().filename()) ==
            referenced.end())
        {
            std::error_code removeEc;
            fs::remove(it->path(), removeEc);
        }
    }
}

} // namespace

ProjectCatalogService::ProjectCatalogService(JobSystem::WorkStealingThreadPool& jobSystem)
    : m_Channel(std::make_unique<JobSystem::JobChannel>(
          jobSystem, JobSystem::JobChannelDesc{.Name = "Project catalog", .MaxRunning = 1}))
{
}

ProjectCatalogService::~ProjectCatalogService()
{
    // The fetch writes into this object until it ends.
    if (m_Fetch.IsValid())
        m_Fetch.Wait();
}

bool ProjectCatalogService::LoadTemplateCatalog(ProjectCatalog& outCatalog,
                                                fs::path& outRootDir,
                                                std::string* outError)
{
    outRootDir = GetEditorGlobalPaths().installTemplatesRoot;
    const fs::path manifestPath = outRootDir / kManifestFileName;

    std::string jsonText;
    if (!ReadFileToString(manifestPath, jsonText))
    {
        if (outError)
            *outError = "Template manifest not found: " + manifestPath.generic_string();
        return false;
    }
    return ParseProjectCatalog(jsonText, outRootDir.generic_string(), outCatalog, outError);
}

std::string ProjectCatalogService::GetCommunityManifestSource()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    std::string source;
    if (prefs.TryGetString(kCommunityManifestSourcePrefKey, source) && !source.empty())
        return source;
    return kDefaultCommunityManifestUrl;
}

void ProjectCatalogService::RequestCommunityFetch()
{
    bool expected = false;
    if (!m_FetchInFlight.compare_exchange_strong(expected, true))
        return;

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Snapshot.State = CommunityCatalogState::Fetching;
        m_SnapshotDirty = true;
    }

    // Resolve source + cache root on the calling thread: preferences and
    // editor paths are not synchronized for cross-thread access.
    std::string source = GetCommunityManifestSource();
    fs::path cacheRoot = GetEditorGlobalPaths().communityCacheRoot;
    m_Fetch = m_Channel->Submit([this, source = std::move(source), cacheRoot = std::move(cacheRoot)]() {
        FetchWorker(source, cacheRoot);
    });
    // Refused once the job system is shutting down: the fetch never runs.
    if (!m_Fetch.IsValid())
        m_FetchInFlight.store(false);
}

bool ProjectCatalogService::PollCommunity(CommunityCatalogSnapshot& outSnapshot)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_SnapshotDirty)
        return false;
    m_SnapshotDirty = false;
    outSnapshot = m_Snapshot;
    return true;
}

void ProjectCatalogService::Publish(CommunityCatalogSnapshot snapshot)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Snapshot = std::move(snapshot);
    m_SnapshotDirty = true;
}

void ProjectCatalogService::FetchWorker(std::string source, fs::path cacheRoot)
{
    const fs::path cachedManifest = cacheRoot / kManifestFileName;
    const fs::path thumbnailCacheDir = cacheRoot / "thumbnails";

    std::string jsonText;
    std::string fetchError;
    if (IsHttpSource(source))
    {
        const HttpClient::HttpResponse response = HttpClient::Get(source);
        if (response.success && response.statusCode == 200)
            jsonText = response.body;
        else
            fetchError = response.error.empty()
                ? "HTTP " + std::to_string(response.statusCode)
                : response.error;
    }
    else if (!ReadFileToString(source, jsonText))
    {
        fetchError = "Could not read manifest file: " + source;
    }

    CommunityCatalogSnapshot snapshot;
    if (fetchError.empty())
    {
        std::string parseError;
        if (ParseProjectCatalog(jsonText, SourceBaseOf(source), snapshot.Catalog, &parseError))
        {
            if (!WriteStringToFile(cachedManifest, jsonText))
                Logger::Log::Warning("Community catalog: could not write cache manifest");
            ResolveThumbnailsToCache(snapshot.Catalog, thumbnailCacheDir, /*allowDownload=*/true);
            snapshot.State = CommunityCatalogState::Ready;
            Publish(std::move(snapshot));
            m_FetchInFlight.store(false);
            return;
        }
        fetchError = parseError;
    }

    // Fetch or parse failed — fall back to the last-good cached manifest so
    // offline sessions still see the catalog they saw last time.
    snapshot.Error = fetchError;
    std::string cachedText;
    std::string cachedParseError;
    if (ReadFileToString(cachedManifest, cachedText) &&
        ParseProjectCatalog(cachedText, SourceBaseOf(source), snapshot.Catalog, &cachedParseError))
    {
        ResolveThumbnailsToCache(snapshot.Catalog, thumbnailCacheDir, /*allowDownload=*/false);
        snapshot.State = CommunityCatalogState::Ready;
        snapshot.FromCache = true;
    }
    else
    {
        snapshot.State = CommunityCatalogState::Failed;
    }
    Logger::Log::Warning("Community catalog fetch failed ({}): {}", source, fetchError);
    Publish(std::move(snapshot));
    m_FetchInFlight.store(false);
}

} // namespace GameEngine::Editor
