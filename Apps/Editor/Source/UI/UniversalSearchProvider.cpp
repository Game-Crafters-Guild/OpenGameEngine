#include "UI/UniversalSearchProvider.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetSearchProvider.h"
#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "ECS/ECS.h"
#include "ECS/ComponentConcepts.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr std::size_t kMaxResults = 100;
constexpr std::size_t kMaxIndexedFileBytes = 2u * 1024u * 1024u;
constexpr std::size_t kMaxCommentEntries = 12000;

bool IsCodeKind(UniversalSearchKind kind)
{
    return kind == UniversalSearchKind::File || kind == UniversalSearchKind::Class ||
           kind == UniversalSearchKind::Function || kind == UniversalSearchKind::Symbol ||
           kind == UniversalSearchKind::Comment;
}

int KindPriority(UniversalSearchKind kind)
{
    switch (kind)
    {
    case UniversalSearchKind::Command: return 70;
    case UniversalSearchKind::Panel: return 60;
    case UniversalSearchKind::Setting: return 50;
    case UniversalSearchKind::Entity: return 40;
    case UniversalSearchKind::Asset: return 35;
    case UniversalSearchKind::Class: return 32;
    case UniversalSearchKind::Function: return 30;
    case UniversalSearchKind::Symbol: return 26;
    case UniversalSearchKind::File: return 24;
    case UniversalSearchKind::Comment: return 12;
    }
    return 0;
}

std::string Trim(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        --end;
    return std::string(value.substr(begin, end - begin));
}

int DamerauDistance(std::string_view a, std::string_view b, int cutoff)
{
    // Optimal-string-alignment Damerau-Levenshtein distance. In addition to
    // insert/delete/replace, an adjacent letter inversion is one edit, so fast
    // typing such as "brnach" still resolves to "branch".
    if (std::abs(static_cast<int>(a.size()) - static_cast<int>(b.size())) > cutoff)
        return cutoff + 1;
    // This runs for every query-word/candidate-word pair. Reuse per-thread
    // scratch buffers so fuzzy matching does not perform thousands of small
    // heap allocations on each keystroke.
    thread_local std::vector<int> previous;
    thread_local std::vector<int> current;
    thread_local std::vector<int> beforePrevious;
    previous.resize(b.size() + 1);
    current.resize(b.size() + 1);
    beforePrevious.resize(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        previous[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        current[0] = static_cast<int>(i);
        int rowBest = current[0];
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                current[j] = std::min(current[j], beforePrevious[j - 2] + 1);
            rowBest = std::min(rowBest, current[j]);
        }
        if (rowBest > cutoff)
            return cutoff + 1;
        beforePrevious.swap(previous);
        previous.swap(current);
    }
    return previous[b.size()];
}

int SubsequenceScore(std::string_view query, std::string_view candidate)
{
    std::size_t qi = 0;
    int score = 0;
    int run = 0;
    for (std::size_t ci = 0; ci < candidate.size() && qi < query.size(); ++ci)
    {
        if (query[qi] != candidate[ci])
        {
            run = 0;
            continue;
        }
        const bool boundary = ci == 0 || candidate[ci - 1] == ' ';
        score += 7 + (boundary ? 10 : 0) + std::min(run * 4, 16);
        ++run;
        ++qi;
    }
    return qi == query.size() ? score - static_cast<int>(candidate.size() - query.size()) : -1;
}

bool IsIgnoredDirectory(std::string_view name)
{
    static constexpr std::string_view ignored[] = {
        ".git", ".svn", ".hg", ".Editor", ".Cache", "build", "build-xcode", "build-macos",
        "node_modules", "dependencies", "ThirdParty", "DerivedData", "Library", "Temp", "obj", "bin"};
    return std::find(std::begin(ignored), std::end(ignored), name) != std::end(ignored) ||
           name.starts_with("build-");
}

bool IsCodeFile(const std::filesystem::path& path)
{
    static constexpr std::string_view extensions[] = {
        ".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".cs",
        ".glsl", ".vert", ".frag", ".comp", ".py", ".js", ".ts", ".lua", ".cmake"};
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(std::begin(extensions), std::end(extensions), ext) != std::end(extensions) ||
           path.filename() == "CMakeLists.txt";
}

std::string LastIdentifierBefore(std::string_view line, std::size_t end)
{
    while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0)
        --end;
    std::size_t begin = end;
    while (begin > 0)
    {
        const unsigned char c = static_cast<unsigned char>(line[begin - 1]);
        if (std::isalnum(c) == 0 && c != '_' && c != '~')
            break;
        --begin;
    }
    return std::string(line.substr(begin, end - begin));
}
} // namespace

/// Immutable view of every searchable entry, published to the lexical search job.
/// Layers are shared with the provider and never mutated after publication.
struct UniversalSearchProvider::SearchSnapshot
{
    std::uint64_t Generation = 0;
    std::vector<EntryLayer> Layers;
};

/// Latest-wins lexical search queue. Schedule keeps one pending request and
/// starts a drain job when none is running; the drain job runs requests until
/// none is pending, so at most one search runs at a time.
struct UniversalSearchProvider::LexicalState
{
    struct Request
    {
        std::string Query;
        std::shared_ptr<const SearchSnapshot> Snapshot;
        ResultSink Sink;
        std::uint64_t Version = 0;
    };

    explicit LexicalState(JobSystem::WorkStealingThreadPool& jobSystem)
        : Jobs(jobSystem)
    {
    }

    ~LexicalState() { StopAndWait(); }

    /// Drops the pending request, refuses new ones and blocks until the drain
    /// job has returned. Idempotent.
    void StopAndWait()
    {
        JobSystem::TaskHandle drain;
        {
            std::lock_guard lock(Mutex);
            Stop = true;
            Pending.reset();
            Version.fetch_add(1, std::memory_order_release);
            drain = Drain;
        }
        if (drain.IsValid() && !drain.IsDone())
            drain.Wait();
    }

    void Schedule(std::string query,
                  std::shared_ptr<const SearchSnapshot> snapshot,
                  ResultSink sink)
    {
        Request request;
        request.Query = std::move(query);
        request.Snapshot = std::move(snapshot);
        request.Sink = std::move(sink);
        request.Version = Version.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard lock(Mutex);
            if (Stop)
                return;
            Pending = std::move(request);
            if (Draining)
                return;
            Draining = true;
        }
        // Drain is stored under a second lock, after the job may already run.
        // Schedule and StopAndWait run only on the provider's owning thread, so
        // StopAndWait never runs between this Submit and the store below and
        // always finds the handle it must wait for.
        JobSystem::TaskHandle drain = Jobs.Submit([this]() { DrainPending(); });
        std::lock_guard lock(Mutex);
        // The pool refuses work once it is shutting down (invalid or cancelled
        // handle); clear Draining so a later request can submit again.
        if (!drain.IsValid() || drain.HasFailed())
        {
            Logger::Log::Error("UniversalSearchProvider: the job system refused the search job; the query is dropped");
            Pending.reset();
            Draining = false;
            return;
        }
        Drain = std::move(drain);
    }

    void Cancel()
    {
        Version.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard lock(Mutex);
        Pending.reset();
    }

    bool IsCurrent(std::uint64_t version) const
    {
        return Version.load(std::memory_order_acquire) == version;
    }

    void DrainPending()
    {
        std::unique_lock lock(Mutex);
        while (!Stop && Pending.has_value())
        {
            Request request = std::move(*Pending);
            Pending.reset();
            lock.unlock();
            if (request.Snapshot && IsCurrent(request.Version))
                RunRequest(request);
            lock.lock();
        }
        Draining = false;
    }

    // Keeps Draining truthful: an exception must not end the drain job early.
    void RunRequest(Request& request)
    {
        try
        {
            RunLexicalSearch(*this, request.Query, request.Snapshot, std::move(request.Sink), request.Version);
        }
        catch (const std::exception& e)
        {
            Logger::Log::Error("UniversalSearchProvider: search for '{}' failed: {}", request.Query, e.what());
        }
    }

    // Query-result cache for the snapshot generation in CacheGeneration.
    // Touched only by the drain job, and at most one runs at a time, so no locking.
    std::vector<SearchResultItem>* FindCached(const SearchSnapshot& snapshot, const std::string& query)
    {
        if (snapshot.Generation != CacheGeneration)
        {
            Cache.clear();
            CacheOrder.clear();
            CacheGeneration = snapshot.Generation;
            return nullptr;
        }
        const auto found = Cache.find(query);
        return found != Cache.end() ? &found->second : nullptr;
    }

    void StoreCached(std::string query, std::vector<SearchResultItem> results)
    {
        constexpr std::size_t kMaxCachedQueries = 64;
        if (Cache.size() >= kMaxCachedQueries && !Cache.contains(query))
        {
            Cache.erase(CacheOrder.front());
            CacheOrder.pop_front();
        }
        if (Cache.insert_or_assign(query, std::move(results)).second)
            CacheOrder.push_back(std::move(query));
    }

    JobSystem::WorkStealingThreadPool& Jobs;
    std::atomic<std::uint64_t> Version{0};
    std::mutex Mutex;
    // Guarded by Mutex: the pending request, the stop flag, whether a drain job
    // is running, and that job's handle.
    std::optional<Request> Pending;
    bool Stop = false;
    bool Draining = false;
    JobSystem::TaskHandle Drain;
    std::uint64_t CacheGeneration = 0;
    std::unordered_map<std::string, std::vector<SearchResultItem>> Cache;
    std::deque<std::string> CacheOrder;
};

UniversalSearchProvider::UniversalSearchProvider(JobSystem::WorkStealingThreadPool& jobSystem)
    : m_JobSystem(jobSystem)
    , m_Lexical(std::make_unique<LexicalState>(jobSystem))
{
}

UniversalSearchProvider::~UniversalSearchProvider()
{
    // Both jobs are joined here, before any member is destroyed.
    m_Lexical->StopAndWait();
    m_CodeCancel.store(true, std::memory_order_release);
    if (m_CodeTask.IsValid() && !m_CodeTask.IsDone())
        m_CodeTask.Wait();
}

void UniversalSearchProvider::AddEntry(UniversalSearchEntry entry)
{
    m_Registered.push_back(MakeIndexed(std::move(entry)));
    m_SearchSnapshot.reset();
}

void UniversalSearchProvider::SetScriptEntries(std::vector<UniversalSearchEntry> entries)
{
    m_Scripts.clear();
    m_Scripts.reserve(entries.size());
    for (auto& entry : entries)
        m_Scripts.push_back(MakeIndexed(std::move(entry)));
    m_SearchSnapshot.reset();
}

void UniversalSearchProvider::SetSettingsEntries(std::vector<UniversalSearchEntry> entries)
{
    m_Settings.clear();
    m_Settings.reserve(entries.size());
    for (auto& entry : entries)
        m_Settings.push_back(MakeIndexed(std::move(entry)));
    m_SearchSnapshot.reset();
}

void UniversalSearchProvider::SetOnAssetSelected(std::function<void(const std::filesystem::path&)> callback)
{
    m_OnAssetSelected = std::move(callback);
}

void UniversalSearchProvider::SetOnEntitySelected(std::function<void(ECS::EntityHandle)> callback)
{
    m_OnEntitySelected = std::move(callback);
}

void UniversalSearchProvider::SetOnCodeSelected(
    std::function<void(const std::filesystem::path&, std::size_t)> callback)
{
    m_OnCodeSelected = std::move(callback);
}

std::string UniversalSearchProvider::NormalizeForSearch(std::string_view text)
{
    std::string result;
    result.reserve(text.size() + 8);
    bool previousSpace = true;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool isUpper = std::isupper(c) != 0;
        const bool previousLowerOrDigit = i > 0 &&
            (std::islower(static_cast<unsigned char>(text[i - 1])) != 0 ||
             std::isdigit(static_cast<unsigned char>(text[i - 1])) != 0);
        const bool acronymBoundary = isUpper && i > 0 && i + 1 < text.size() &&
            std::isupper(static_cast<unsigned char>(text[i - 1])) != 0 &&
            std::islower(static_cast<unsigned char>(text[i + 1])) != 0;
        if ((isUpper && previousLowerOrDigit) || acronymBoundary)
        {
            if (!previousSpace)
                result.push_back(' ');
            previousSpace = true;
        }
        if (std::isalnum(c) != 0)
        {
            result.push_back(static_cast<char>(std::tolower(c)));
            previousSpace = false;
        }
        else if (!previousSpace)
        {
            result.push_back(' ');
            previousSpace = true;
        }
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

int UniversalSearchProvider::FuzzyScore(std::string_view query, std::string_view candidate)
{
    if (query.empty())
        return 0;
    if (candidate.empty())
        return -1;
    if (query == candidate)
        return 1200;
    if (candidate.starts_with(query))
        return 1000 - static_cast<int>(candidate.size() - query.size());
    if (const std::size_t at = candidate.find(query); at != std::string_view::npos)
        return 800 - static_cast<int>(at * 4 + candidate.size() - query.size());

    int total = 0;
    std::size_t queryAt = 0;
    while (queryAt < query.size())
    {
        while (queryAt < query.size() && query[queryAt] == ' ')
            ++queryAt;
        const std::size_t queryBegin = queryAt;
        while (queryAt < query.size() && query[queryAt] != ' ')
            ++queryAt;
        const std::string_view queryWord = query.substr(queryBegin, queryAt - queryBegin);
        if (queryWord.empty())
            continue;

        int best = -1;
        std::size_t candidateAt = 0;
        while (candidateAt < candidate.size())
        {
            while (candidateAt < candidate.size() && candidate[candidateAt] == ' ')
                ++candidateAt;
            const std::size_t candidateBegin = candidateAt;
            while (candidateAt < candidate.size() && candidate[candidateAt] != ' ')
                ++candidateAt;
            const std::string_view candidateWord = candidate.substr(
                candidateBegin, candidateAt - candidateBegin);
            if (candidateWord.empty())
                continue;

            if (candidateWord.starts_with(queryWord))
                best = std::max(best, 180 - static_cast<int>(candidateWord.size() - queryWord.size()));
            const int cutoff = queryWord.size() <= 5 ? 1 : 2;
            const int distance = DamerauDistance(queryWord, candidateWord, cutoff);
            if (distance <= cutoff)
                best = std::max(best, 140 - distance * 35 - std::abs(static_cast<int>(candidateWord.size() - queryWord.size())));
            best = std::max(best, SubsequenceScore(queryWord, candidateWord));
        }
        if (best < 0)
            return -1;
        total += best;
    }
    return total;
}

const char* UniversalSearchProvider::KindName(UniversalSearchKind kind)
{
    switch (kind)
    {
    case UniversalSearchKind::Command: return "Command";
    case UniversalSearchKind::Panel: return "Panel";
    case UniversalSearchKind::Setting: return "Setting";
    case UniversalSearchKind::Asset: return "Asset";
    case UniversalSearchKind::Entity: return "Entity";
    case UniversalSearchKind::File: return "File";
    case UniversalSearchKind::Class: return "Class";
    case UniversalSearchKind::Function: return "Function";
    case UniversalSearchKind::Symbol: return "Symbol";
    case UniversalSearchKind::Comment: return "Comment";
    }
    return "Result";
}

UniversalSearchProvider::ParsedQuery UniversalSearchProvider::ParseQuery(std::string_view query)
{
    ParsedQuery parsed;
    const std::string trimmed = Trim(query);
    if (trimmed.empty())
        return parsed;

    struct Prefix { std::string_view Text; UniversalSearchKind Kind; };
    static constexpr Prefix prefixes[] = {
        {"asset:", UniversalSearchKind::Asset}, {"file:", UniversalSearchKind::File},
        {"class:", UniversalSearchKind::Class}, {"function:", UniversalSearchKind::Function},
        {"symbol:", UniversalSearchKind::Symbol}, {"comment:", UniversalSearchKind::Comment},
    };
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto& prefix : prefixes)
    {
        if (lower.starts_with(prefix.Text))
        {
            parsed.Kind = prefix.Kind;
            parsed.Text = Trim(trimmed.substr(prefix.Text.size()));
            return parsed;
        }
    }
    if (lower.starts_with("code:"))
    {
        parsed.CodeKindsOnly = true;
        parsed.Text = Trim(trimmed.substr(5));
        return parsed;
    }
    const char first = trimmed[0];
    if (first == '>') parsed.Kind = UniversalSearchKind::Command;
    else if (first == '@') parsed.Kind = UniversalSearchKind::Entity;
    else if (first == '/') parsed.Kind = UniversalSearchKind::Panel;
    else if (first == ':') parsed.Kind = UniversalSearchKind::Setting;
    if (parsed.Kind)
        parsed.Text = Trim(trimmed.substr(1));
    else
        parsed.Text = trimmed;
    return parsed;
}

UniversalSearchProvider::IndexedEntry UniversalSearchProvider::MakeIndexed(UniversalSearchEntry entry)
{
    IndexedEntry indexed;
    indexed.NormalizedLabel = NormalizeForSearch(entry.Label);
    indexed.NormalizedSearchText = indexed.NormalizedLabel;
    if (!entry.Keywords.empty())
    {
        indexed.NormalizedSearchText.push_back(' ');
        indexed.NormalizedSearchText += NormalizeForSearch(entry.Keywords);
    }
    if (!entry.Detail.empty())
    {
        indexed.NormalizedSearchText.push_back(' ');
        indexed.NormalizedSearchText += NormalizeForSearch(entry.Detail);
    }
    indexed.Entry = std::move(entry);
    return indexed;
}

void UniversalSearchProvider::RefreshRuntimeEntries()
{
    PollCodeIndex();
    RebuildAssetEntries();
    RebuildEntityEntries();
    RefreshSearchSnapshot();
}

void UniversalSearchProvider::RefreshSearchSnapshot()
{
    auto snapshot = std::make_shared<SearchSnapshot>();
    snapshot->Generation = ++m_SnapshotGeneration;
    // Registered/script entries are small and mutate incrementally, so they are
    // copied into a fresh layer; the large asset/entity/code layers are already
    // immutable and shared without copying.
    auto statics = std::make_shared<std::vector<IndexedEntry>>();
    statics->reserve(m_Registered.size() + m_Scripts.size() + m_Settings.size());
    statics->insert(statics->end(), m_Registered.begin(), m_Registered.end());
    statics->insert(statics->end(), m_Scripts.begin(), m_Scripts.end());
    statics->insert(statics->end(), m_Settings.begin(), m_Settings.end());
    snapshot->Layers.push_back(std::move(statics));
    if (m_Assets && !m_Assets->empty())
        snapshot->Layers.push_back(m_Assets);
    if (m_Entities && !m_Entities->empty())
        snapshot->Layers.push_back(m_Entities);
    if (m_Code && !m_Code->empty())
        snapshot->Layers.push_back(m_Code);
    m_SearchSnapshot = std::move(snapshot);
}

void UniversalSearchProvider::RebuildAssetEntries()
{
    auto assets = std::make_shared<std::vector<IndexedEntry>>();
    m_Assets = assets;
    if (!m_AssetRegistry)
        return;
    // *assets is filled before the layer is published to a snapshot, and never
    // mutated afterwards.
    const Vector<AssetIndexRecord> snapshot = m_AssetRegistry->GetAssetIndexSnapshot();
    assets->reserve(snapshot.size());
    for (const AssetIndexRecord& metadata : snapshot)
    {
        UniversalSearchEntry entry;
        entry.Kind = UniversalSearchKind::Asset;
        entry.Label = metadata.Name.empty() ? metadata.Path.filename().string() : metadata.Name;
        entry.Detail = metadata.Path.generic_string();
        entry.Keywords = AssetTypeToString(metadata.Type);
        entry.Provider = "Asset Registry";
        entry.Icon = SearchIcon::FromClass(AssetSearchProvider::GetIconClassForType(metadata.Type));
        if (metadata.Type == AssetType::Texture)
        {
            // Image assets can be sampled directly, so their previews are
            // available on the first palette open without thumbnail work.
            entry.Icon.ImagePath = metadata.Path.string();
        }
        const std::filesystem::path path = metadata.Path;
        entry.Execute = [callback = m_OnAssetSelected, path]() { if (callback) callback(path); };
        assets->push_back(MakeIndexed(std::move(entry)));
    }
}

void UniversalSearchProvider::RebuildEntityEntries()
{
    auto entities = std::make_shared<std::vector<IndexedEntry>>();
    m_Entities = entities;
    if (!m_World)
        return;
    auto query = m_World->Query<ECS::Read<Components::Name>>();
    // Search finds what the hierarchy shows, and the hierarchy shows disabled
    // entities.
    query.IncludeDisabled();
    query.Each([this, &entities](ECS::EntityHandle entity, const Components::Name& name)
    {
        UniversalSearchEntry entry;
        entry.Kind = UniversalSearchKind::Entity;
        entry.Label = name.value[0] != '\0' ? name.View() : "Unnamed Entity";
        entry.Detail = "Scene entity";
        entry.Keywords = "object actor node hierarchy";
        entry.Provider = "Scene Hierarchy";
        entry.Icon = SearchIcon::FromClass("inspector-section-icon-default");
        entry.Execute = [callback = m_OnEntitySelected, entity]() { if (callback) callback(entity); };
        entities->push_back(MakeIndexed(std::move(entry)));
    });
}

void UniversalSearchProvider::StartCodeIndex(const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return;
    if (m_CodeTask.IsValid() && !m_CodeTask.IsDone())
    {
        // Already indexing this root; let the in-flight scan finish instead of
        // cancelling and restarting it on every palette open.
        if (projectRoot == m_CodeRoot)
            return;
        m_QueuedCodeRoot = projectRoot;
        m_CodeCancel.store(true, std::memory_order_release);
        return;
    }
    PollCodeIndex();
    if (m_CodeRoot == projectRoot && m_Code && !m_Code->empty())
        return;
    m_CodeRoot = projectRoot;
    m_CodeCancel.store(false, std::memory_order_release);
    m_CodeTask = m_JobSystem.Submit(
        [root = projectRoot, onOpen = m_OnCodeSelected, cancel = &m_CodeCancel]()
        { return BuildCodeIndexLayer(root, onOpen, cancel); },
        JobSystem::JobPriority::Background);
}

void UniversalSearchProvider::RestartCodeIndex(const std::filesystem::path& projectRoot)
{
    m_Code.reset();
    m_CodeRoot.clear();
    StartCodeIndex(projectRoot);
}

UniversalSearchProvider::EntryLayer UniversalSearchProvider::BuildCodeIndexLayer(
    const std::filesystem::path& root,
    const std::function<void(const std::filesystem::path&, std::size_t)>& onCodeSelected,
    std::atomic<bool>* cancel)
{
    try
    {
        return std::make_shared<const std::vector<IndexedEntry>>(BuildCodeIndex(root, onCodeSelected, cancel));
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("UniversalSearchProvider: indexing project code under '{}' failed: {}", root.string(),
                           e.what());
        return nullptr;
    }
}

void UniversalSearchProvider::PollCodeIndex()
{
    if (!m_CodeTask.IsValid() || !m_CodeTask.IsDone())
        return;
    EntryLayer completed;
    const bool built = m_CodeTask.TryGetResult(completed);
    m_CodeTask = JobSystem::TaskHandle{};
    if (built && completed && !m_CodeCancel.load(std::memory_order_acquire))
        m_Code = std::move(completed);
    if (!m_QueuedCodeRoot.empty())
    {
        const auto queued = std::exchange(m_QueuedCodeRoot, {});
        m_CodeRoot.clear();
        StartCodeIndex(queued);
    }
}

std::vector<UniversalSearchProvider::IndexedEntry> UniversalSearchProvider::BuildCodeIndex(
    const std::filesystem::path& root,
    const std::function<void(const std::filesystem::path&, std::size_t)>& onCodeSelected,
    std::atomic<bool>* cancel)
{
    std::vector<IndexedEntry> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return result;
    // Shared by every entry lambda; a per-entry std::function copy would cost an
    // allocation for each of the (potentially very many) indexed lines.
    const auto onOpen =
        std::make_shared<const std::function<void(const std::filesystem::path&, std::size_t)>>(onCodeSelected);
    std::size_t commentCount = 0;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    while (it != end)
    {
        if (cancel && cancel->load(std::memory_order_acquire))
            return {};
        const auto path = it->path();
        if (it->is_directory(ec))
        {
            if (IsIgnoredDirectory(path.filename().string()))
                it.disable_recursion_pending();
            it.increment(ec);
            continue;
        }
        if (!it->is_regular_file(ec) || !IsCodeFile(path) || it->file_size(ec) > kMaxIndexedFileBytes)
        {
            it.increment(ec);
            continue;
        }
        const std::string relative = path.lexically_relative(root).generic_string();
        // One shared path per file: every entry lambda for this file references
        // it instead of holding its own copy.
        const auto sharedPath = std::make_shared<const std::filesystem::path>(path);
        UniversalSearchEntry fileEntry;
        fileEntry.Kind = UniversalSearchKind::File;
        fileEntry.Label = path.filename().string();
        fileEntry.Detail = relative;
        fileEntry.Keywords = path.stem().string();
        fileEntry.Provider = "Project Code";
        fileEntry.Icon = SearchIcon::FromClass("icon-script");
        fileEntry.Execute = [onOpen, sharedPath]()
        {
            if (*onOpen) (*onOpen)(*sharedPath, 1);
        };
        result.push_back(MakeIndexed(std::move(fileEntry)));

        std::ifstream stream(path);
        if (!stream)
        {
            it.increment(ec);
            continue;
        }
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            const std::string trimmed = Trim(line);
            if (trimmed.empty())
                continue;
            UniversalSearchKind kind = UniversalSearchKind::Symbol;
            std::string label;
            std::string keywords;
            const auto makeDetail = [&]() { return relative + ":" + std::to_string(lineNumber); };

            if ((trimmed.starts_with("//") || trimmed.starts_with("/*") || trimmed.starts_with("* ")) &&
                commentCount < kMaxCommentEntries)
            {
                std::string text = trimmed;
                while (!text.empty() && (text.front() == '/' || text.front() == '*' || std::isspace(static_cast<unsigned char>(text.front())) != 0))
                    text.erase(text.begin());
                if (text.size() >= 8)
                {
                    kind = UniversalSearchKind::Comment;
                    label = text.substr(0, 120);
                    ++commentCount;
                }
            }
            if (label.empty())
            {
                static constexpr std::string_view declarations[] = {"class ", "struct ", "enum ", "interface ", "record "};
                for (std::string_view declaration : declarations)
                {
                    const std::size_t at = trimmed.find(declaration);
                    if (at == std::string::npos)
                        continue;
                    std::size_t begin = at + declaration.size();
                    std::size_t finish = begin;
                    while (finish < trimmed.size() &&
                           (std::isalnum(static_cast<unsigned char>(trimmed[finish])) != 0 || trimmed[finish] == '_'))
                        ++finish;
                    label = trimmed.substr(begin, finish - begin);
                    kind = UniversalSearchKind::Class;
                    keywords = std::string(declaration);
                    break;
                }
            }
            if (label.empty())
            {
                const std::size_t paren = trimmed.find('(');
                const std::size_t close = paren == std::string::npos ? std::string::npos : trimmed.find(')', paren);
                if (paren != std::string::npos && close != std::string::npos &&
                    (trimmed.find('{', close) != std::string::npos || trimmed.ends_with("const") || trimmed.ends_with("override")))
                {
                    label = LastIdentifierBefore(trimmed, paren);
                    static constexpr std::string_view control[] = {"if", "for", "while", "switch", "catch"};
                    if (!label.empty() &&
                        std::find(std::begin(control), std::end(control), label) == std::end(control))
                        kind = UniversalSearchKind::Function;
                    else
                        label.clear();
                }
            }
            if (label.empty() && (trimmed.starts_with("#define ") || trimmed.starts_with("using ") ||
                                  trimmed.starts_with("typedef ") || trimmed.find("constexpr ") != std::string::npos))
            {
                label = trimmed.substr(0, std::min<std::size_t>(trimmed.size(), 120));
                kind = UniversalSearchKind::Symbol;
            }
            if (!label.empty())
            {
                UniversalSearchEntry entry;
                entry.Kind = kind;
                entry.Label = std::move(label);
                entry.Detail = makeDetail();
                entry.Keywords = std::move(keywords);
                entry.Provider = "Project Code";
                entry.Icon = SearchIcon::FromClass("icon-script");
                entry.Execute = [onOpen, sharedPath, lineNumber]()
                {
                    if (*onOpen) (*onOpen)(*sharedPath, lineNumber);
                };
                result.push_back(MakeIndexed(std::move(entry)));
            }
        }
        it.increment(ec);
    }
    return result;
}

void UniversalSearchProvider::BeginSearch(const std::string& rawQuery, ResultSink sink)
{
    if (!m_SearchSnapshot)
        RefreshSearchSnapshot();
    m_Lexical->Schedule(rawQuery, m_SearchSnapshot, std::move(sink));
}

void UniversalSearchProvider::RunLexicalSearch(
    LexicalState& lexical,
    const std::string& rawQuery,
    const std::shared_ptr<const SearchSnapshot>& snapshot,
    ResultSink sink,
    std::uint64_t version)
{
    const auto isCurrent = [&lexical, version]()
    {
        return lexical.IsCurrent(version);
    };
    const ParsedQuery parsed = ParseQuery(rawQuery);
    const std::string normalized = NormalizeForSearch(parsed.Text);
    if (const auto* cached = lexical.FindCached(*snapshot, rawQuery))
    {
        if (isCurrent())
            sink(*cached, true);
        return;
    }

    struct Match { const IndexedEntry* Indexed = nullptr; int Score = 0; };
    std::vector<Match> matches;
    auto collect = [&](const std::vector<IndexedEntry>& entries)
    {
        for (const auto& indexed : entries)
        {
            if (!isCurrent())
                return;
            const auto kind = indexed.Entry.Kind;
            if (parsed.Kind && kind != *parsed.Kind)
                continue;
            if (parsed.CodeKindsOnly && !IsCodeKind(kind))
                continue;
            if (normalized.empty())
            {
                if (!parsed.Kind && kind != UniversalSearchKind::Command && kind != UniversalSearchKind::Panel)
                    continue;
                matches.push_back({&indexed, KindPriority(kind) + indexed.Entry.Priority});
                continue;
            }
            int score = FuzzyScore(normalized, indexed.NormalizedLabel);
            const int broad = FuzzyScore(normalized, indexed.NormalizedSearchText);
            score = std::max(score, broad < 0 ? -1 : broad - 80);
            if (score >= 0)
                matches.push_back({&indexed, score + KindPriority(kind) + indexed.Entry.Priority});
        }
    };
    for (const EntryLayer& layer : snapshot->Layers)
        collect(*layer);

    if (!isCurrent())
        return;

    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b)
    {
        if (a.Score != b.Score)
            return a.Score > b.Score;
        if (a.Indexed->Entry.Kind != b.Indexed->Entry.Kind)
            return KindPriority(a.Indexed->Entry.Kind) > KindPriority(b.Indexed->Entry.Kind);
        return a.Indexed->NormalizedLabel < b.Indexed->NormalizedLabel;
    });

    std::vector<SearchResultItem> results;
    results.reserve(std::min(matches.size(), kMaxResults));
    std::unordered_set<std::string> dedupe;
    for (const Match& match : matches)
    {
        if (!isCurrent())
            return;
        const auto& entry = match.Indexed->Entry;
        const std::string key = std::to_string(static_cast<int>(entry.Kind)) + ":" +
                                match.Indexed->NormalizedLabel + ":" + entry.Detail;
        if (!dedupe.insert(key).second)
            continue;
        SearchResultItem item;
        item.Id = static_cast<SearchItemId>(results.size() + 1);
        item.Label = entry.Label;
        item.Detail = std::string(KindName(entry.Kind)) + " · " + entry.Provider;
        if (!entry.Detail.empty())
            item.Detail += " · " + entry.Detail;
        item.TypeKey = KindName(entry.Kind);
        item.Icon = entry.Icon;
        item.UserData = entry.Execute;
        results.push_back(std::move(item));
        if (results.size() >= kMaxResults)
            break;
    }

    lexical.StoreCached(rawQuery, results);
    if (isCurrent())
        sink(std::move(results), true);
}

void UniversalSearchProvider::CancelSearch()
{
    m_Lexical->Cancel();
}

std::string UniversalSearchProvider::GetPlaceholderText() const
{
    return "Search everything…  > commands  @ entities  / panels  : settings";
}

} // namespace GameEngine
