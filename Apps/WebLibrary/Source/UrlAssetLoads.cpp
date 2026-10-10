#include "UrlAssetLoads.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Types/Fnv1a.h"

#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>

namespace GameEngine::WebLibrary
{

// The asset manager's answer, written by its completion callback and read by Advance.
struct UrlAssetLoads::LoadResult
{
    std::mutex Mutex;
    std::optional<Result<SharedPtr<Asset>, AssetError>> Value;
};

namespace
{

// Under the asset root: every URL's file lands in its own folder, named by the URL's hash.
constexpr std::string_view kUrlAssetFolder = "Web";

// The browser fetches every URL; the result waits in JavaScript until PollUrlFetch takes it, so
// nothing reaches the module between two of its calls.
EM_JS(int, StartUrlFetch, (const char* url), {
    var fetches = Module['geUrlFetches'] || (Module['geUrlFetches'] = { next: 1, pending: {} });
    var id = fetches.next++;
    var entry = { done: false, status: 0, bytes: null };
    fetches.pending[id] = entry;
    fetch(UTF8ToString(url))
        .then(function(response) {
            entry.status = response.status;
            return response.ok ? response.arrayBuffer() : null;
        })
        .then(function(buffer) {
            entry.bytes = buffer ? new Uint8Array(buffer) : null;
            entry.done = true;
        }, function() {
            entry.status = -1;
            entry.done = true;
        });
    return id;
});

// 0 while the fetch runs; 1 when the file arrived, its bytes at *outData (malloc) and their
// count at *outSize; 2 when it failed, with the HTTP status (or -1) at *outSize. A finished
// fetch is forgotten here.
EM_JS(int, PollUrlFetch, (int id, uint8_t** outData, int32_t* outSize), {
    var fetches = Module['geUrlFetches'];
    var entry = fetches && fetches.pending[id];
    if (!entry || !entry.done)
        return 0;
    delete fetches.pending[id];
    if (!entry.bytes) {
        HEAP32[outSize >> 2] = entry.status;
        return 2;
    }
    var data = _malloc(entry.bytes.length);
    HEAPU8.set(entry.bytes, data);
    HEAPU32[outData >> 2] = data;
    HEAP32[outSize >> 2] = entry.bytes.length;
    return 1;
});

EM_JS(void, DropUrlFetch, (int id), {
    var fetches = Module['geUrlFetches'];
    if (fetches)
        delete fetches.pending[id];
});

// The last path segment of the URL, without its query or fragment; empty when it has no
// extension, which the asset manager needs to pick the loader. A blob: URL (a file the page
// opened or had dropped on it) has no name in its path, so it carries the name as its fragment:
// blob:https://host/<uuid>#model.glb.
std::string FileNameOf(const std::string& url)
{
    std::string_view path = url;
    if (path.starts_with("blob:"))
    {
        const size_t fragment = path.find('#');
        path = fragment == std::string_view::npos ? std::string_view{} : path.substr(fragment + 1);
    }
    else if (const size_t end = path.find_first_of("?#"); end != std::string_view::npos)
    {
        path = path.substr(0, end);
    }
    const size_t slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (name.find('.') == std::string_view::npos || name.back() == '.')
        return {};
    std::string sanitized(name);
    for (char& c : sanitized)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             c == '.' || c == '-' || c == '_';
        if (!allowed)
            c = '_';
    }
    return sanitized;
}

std::filesystem::path AssetPathFor(const std::string& url, const std::string& fileName)
{
    const uint64_t hash = Hashing::Fnv1a64(url.data(), url.size());
    return std::filesystem::path(kUrlAssetFolder) / std::format("{:016x}", hash) / fileName;
}

bool WriteFile(const std::filesystem::path& file, const uint8_t* data, size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

} // namespace

UrlAssetLoads::~UrlAssetLoads()
{
    Clear();
}

uint32_t UrlAssetLoads::Start(const std::string& url, std::string& outError)
{
    // A blob: URL is absolute without a "://" when its origin is opaque (blob:nodedata:<uuid>).
    if (url.find("://") == std::string::npos && !url.starts_with("blob:"))
    {
        outError = std::format("ge_load_asset needs an absolute URL; '{}' is not one.", url);
        return 0;
    }
    if (FileNameOf(url).empty())
    {
        outError = std::format("ge_load_asset: the URL '{}' must end in a file name with its extension "
                               "(for example model.glb), which picks the loader; a blob: URL carries the "
                               "file's name after '#' (blob:...#model.glb).",
                               url);
        return 0;
    }
    // The file is written once and loaded once: writing it again would replace the bytes under
    // an asset whose textures live materials bind.
    if (const auto known = m_HandleByUrl.find(url); known != m_HandleByUrl.end())
    {
        const auto it = m_Loads.find(known->second);
        if (it != m_Loads.end() && it->second.Status != UrlAssetStatus::Failed)
            return known->second;
    }
    const uint32_t handle = m_NextHandle++;
    m_HandleByUrl[url] = handle;
    Load& load = m_Loads[handle];
    load.Url = url;
    load.FetchId = StartUrlFetch(url.c_str());
    return handle;
}

bool UrlAssetLoads::TryGetStatus(uint32_t handle, UrlAssetStatus& outStatus, std::string& outFailure) const
{
    const auto it = m_Loads.find(handle);
    if (it == m_Loads.end())
        return false;
    outStatus = it->second.Status;
    outFailure = it->second.Failure;
    return true;
}

SharedPtr<Asset> UrlAssetLoads::GetAsset(uint32_t handle, GUID& outGuid) const
{
    const auto it = m_Loads.find(handle);
    if (it == m_Loads.end() || it->second.Status != UrlAssetStatus::Ready)
        return nullptr;
    outGuid = it->second.AssetGuid;
    return it->second.LoadedAsset;
}

void UrlAssetLoads::Advance()
{
    for (auto& [handle, load] : m_Loads)
    {
        if (load.Status != UrlAssetStatus::Loading)
            continue;
        if (load.FetchId != 0)
            AdvanceFetch(load);
        if (load.Status == UrlAssetStatus::Loading && load.Result)
            AdvanceAssetLoad(load);
    }
}

void UrlAssetLoads::Clear()
{
    for (auto& [handle, load] : m_Loads)
    {
        if (load.FetchId != 0)
            DropUrlFetch(load.FetchId);
    }
    m_Loads.clear();
    m_HandleByUrl.clear();
}

void UrlAssetLoads::AdvanceFetch(Load& load)
{
    uint8_t* data = nullptr;
    int32_t size = 0;
    const int state = PollUrlFetch(load.FetchId, &data, &size);
    if (state == 0)
        return;
    load.FetchId = 0;
    if (state == 2)
    {
        load.Status = UrlAssetStatus::Failed;
        load.Failure = size > 0
                           ? std::format("could not fetch '{}': HTTP {}.", load.Url, size)
                           : std::format("could not fetch '{}': the request failed (a file on another origin "
                                         "loads only when its host sends CORS headers).",
                                         load.Url);
        return;
    }

    auto& engine = EngineCore::GetInstance();
    const std::filesystem::path relative = AssetPathFor(load.Url, FileNameOf(load.Url));
    const bool written = WriteFile(engine.GetResolvedAssetRoot() / relative, data, static_cast<size_t>(size));
    std::free(data);
    if (!written)
    {
        load.Status = UrlAssetStatus::Failed;
        load.Failure = std::format("could not store '{}' in the module's memory file system.", load.Url);
        return;
    }

    auto result = std::make_shared<LoadResult>();
    load.Result = result;
    AssetLoadHandle handle = engine.GetAssetManager().LoadAsset(
        relative,
        [result](Result<SharedPtr<Asset>, AssetError> outcome)
        {
            std::lock_guard<std::mutex> lock(result->Mutex);
            result->Value.emplace(std::move(outcome));
        });
    load.AssetGuid = handle.AssetGuid;
}

void UrlAssetLoads::AdvanceAssetLoad(Load& load)
{
    std::optional<Result<SharedPtr<Asset>, AssetError>> outcome;
    {
        std::lock_guard<std::mutex> lock(load.Result->Mutex);
        outcome.swap(load.Result->Value);
    }
    if (!outcome)
        return;
    load.Result.reset();
    if (outcome->IsOk() && outcome->Value())
    {
        load.LoadedAsset = outcome->Value();
        load.Status = UrlAssetStatus::Ready;
        return;
    }
    load.Status = UrlAssetStatus::Failed;
    load.Failure = std::format("the engine could not load '{}' ({}); the browser console has the engine's log.",
                               load.Url, outcome->IsOk() ? "no asset" : ToString(outcome->Error()));
}

} // namespace GameEngine::WebLibrary
