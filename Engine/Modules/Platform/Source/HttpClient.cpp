#include "Platform/HttpClient.h"
#include "HttpHeaderRules.h"
#include "Logger/Logger.h"

#include <curl/curl.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <sstream>
#include <cstring>
#include <mutex>

namespace
{

using GameEngine::HttpClient;

std::once_flag curlInitFlag;
void InitializeCurl()
{
    std::call_once(curlInitFlag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

struct WriteState
{
    explicit WriteState(std::string& data, size_t maxBytes = 0) : Data(&data), MaxBytes(maxBytes) {}

    std::string* Data;
    size_t MaxBytes = 0; // 0 = unlimited
    bool Overflowed = false;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, WriteState* state)
{
    size_t totalSize = size * nmemb;
    // CURLOPT_WRITEDATA takes a void*, so passing the wrong type here compiles
    // silently and lands as a garbage WriteState. Fail the transfer instead of
    // writing through it.
    if (state == nullptr || state->Data == nullptr)
    {
        Logger::Log::Error("HttpClient: write callback has no destination buffer; aborting transfer");
        return 0;
    }
    if (state->MaxBytes != 0 && state->Data->size() + totalSize > state->MaxBytes)
    {
        // Returning a short count makes curl abort with CURLE_WRITE_ERROR.
        state->Overflowed = true;
        return 0;
    }
    state->Data->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

bool IsCancelled(const std::atomic<bool>* cancellation)
{
    return cancellation != nullptr && cancellation->load(std::memory_order_relaxed);
}

// PostJson and PostJsonStream's receive side. OnChunk is null when the body is
// buffered; when set, a 2xx body streams to it and any other status buffers so
// the caller reads the error body as text.
struct JsonPostTransfer
{
    CURL* Curl = nullptr;
    std::string* Body = nullptr;
    const HttpClient::ChunkCallback* OnChunk = nullptr;
    const std::atomic<bool>* Cancellation = nullptr;
    size_t MaxBytes = 0;
    size_t ReceivedBytes = 0;
    bool Overflowed = false;
    bool ModeDecided = false;
    bool Streaming = false;
    // An exception from OnChunk must not unwind through curl's C frames: it is
    // held here, the transfer aborts, and the caller rethrows it after cleanup.
    std::exception_ptr ChunkException;
};

size_t JsonPostWriteCallback(char* contents, size_t size, size_t nmemb, void* userData)
{
    auto* transfer = static_cast<JsonPostTransfer*>(userData);
    // Returning a short count makes curl abort with CURLE_WRITE_ERROR; the
    // caller tells cancellation and overflow apart afterwards.
    if (IsCancelled(transfer->Cancellation))
        return 0;

    // curl documents size as always 1.
    const size_t totalSize = size * nmemb;
    if (totalSize > transfer->MaxBytes - transfer->ReceivedBytes)
    {
        transfer->Overflowed = true;
        return 0;
    }
    transfer->ReceivedBytes += totalSize;

    if (!transfer->ModeDecided)
    {
        // The status line has been received before the first body byte.
        long httpCode = 0;
        curl_easy_getinfo(transfer->Curl, CURLINFO_RESPONSE_CODE, &httpCode);
        transfer->Streaming = transfer->OnChunk != nullptr && httpCode >= 200 && httpCode < 300;
        transfer->ModeDecided = true;
    }

    if (!transfer->Streaming)
    {
        transfer->Body->append(contents, totalSize);
        return totalSize;
    }
    try
    {
        (*transfer->OnChunk)(std::string_view(contents, totalSize));
    }
    catch (...)
    {
        transfer->ChunkException = std::current_exception();
        return 0;
    }
    return totalSize;
}

struct CurlEasyDeleter
{
    void operator()(CURL* curl) const { curl_easy_cleanup(curl); }
};

struct CurlHeaderListDeleter
{
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

// curl calls this at least once a second while a transfer is open, so a
// cancellation is noticed even when the server sends nothing.
int CancellationProgressCallback(void* clientData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return IsCancelled(static_cast<const std::atomic<bool>*>(clientData)) ? 1 : 0;
}

// Shared transport hardening: URLs come from manifests and user input, so
// never let curl talk anything but http(s) (directly or via redirect), never
// rely on signals for timeouts (worker threads), and bound the connect phase.
void ApplyCommonTransportOptions(CURL* curl)
{
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
}

size_t HeaderCallback(char* buffer, size_t size, size_t nitems, std::unordered_map<std::string, std::string>* headers)
{
    size_t totalSize = size * nitems;
    std::string headerLine(buffer, totalSize);

    size_t colonPos = headerLine.find(':');
    if (colonPos != std::string::npos)
    {
        std::string key = headerLine.substr(0, colonPos);
        std::string value = headerLine.substr(colonPos + 1);

        while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
            key.erase(0, 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r' || key.back() == '\n'))
            key.pop_back();

        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(0, 1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
            value.pop_back();

        if (!key.empty())
            (*headers)[key] = value;
    }

    return totalSize;
}

HttpClient::HttpResponse PerformJsonPost(const std::string& url,
                                         const std::unordered_map<std::string, std::string>& headers,
                                         const std::string& jsonBody,
                                         const HttpClient::PostOptions& options,
                                         const HttpClient::ChunkCallback* onChunk)
{
    InitializeCurl();

    HttpClient::HttpResponse response;
    response.error = GameEngine::ValidateHeaders(url, headers);
    if (!response.error.empty())
        return response;
    if (options.Timeout.count() <= 0)
    {
        response.error = "PostOptions::Timeout must be positive; leave it at its default or set a duration";
        return response;
    }
    if (IsCancelled(options.Cancellation))
    {
        response.cancelled = true;
        response.error = "Request cancelled";
        return response;
    }

    const std::unique_ptr<CURL, CurlEasyDeleter> handle(curl_easy_init());
    CURL* curl = handle.get();
    if (!curl)
    {
        response.error = "Failed to initialize curl";
        return response;
    }

    JsonPostTransfer transfer;
    transfer.Curl = curl;
    transfer.Body = &response.body;
    transfer.OnChunk = onChunk;
    transfer.Cancellation = options.Cancellation;
    transfer.MaxBytes = options.MaxResponseBytes;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(jsonBody.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, JsonPostWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    if (options.Cancellation)
    {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CancellationProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, options.Cancellation);
    }

    // Refuses a declared Content-Length over the cap before any body arrives;
    // the write callback still bounds a body of unknown length.
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(options.MaxResponseBytes));

    const std::unique_ptr<curl_slist, CurlHeaderListDeleter> headerList(
        curl_slist_append(nullptr, "Content-Type: application/json"));
    if (!headerList)
    {
        response.error = "Failed to allocate the request headers";
        return response;
    }
    for (const auto& [key, value] : headers)
    {
        // Appending to a non-empty list keeps its head, which headerList owns.
        const std::string header = key + ": " + value;
        curl_slist_append(headerList.get(), header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList.get());

    ApplyCommonTransportOptions(curl);
    // A followed redirect would replay every custom header to the new
    // location, and curl withholds only Authorization and Cookie from another
    // host: an X-Api-Key would travel wherever the redirect points, over http
    // too. curl would also turn the POST into a GET on a 301-303.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    // A stream is bounded by stalls, not duration: it fails once curl's
    // averaged rate stays under 1 byte/s for Timeout (whole seconds, curl's
    // unit), so an answer that keeps arriving is never cut off. A buffered
    // POST is bounded whole.
    constexpr long long kMillisecondsPerSecond = 1000;
    const long long timeoutMs = std::min<long long>(options.Timeout.count(), std::numeric_limits<long>::max());
    const long long stallSeconds = std::max<long long>(1, (timeoutMs + kMillisecondsPerSecond - 1) / kMillisecondsPerSecond);
    if (onChunk != nullptr)
    {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(stallSeconds));
    }
    else
    {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    }

    const CURLcode res = curl_easy_perform(curl);
    if (transfer.ChunkException)
        std::rethrow_exception(transfer.ChunkException);

    if (res == CURLE_OK)
    {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.success = (httpCode >= 200 && httpCode < 300);
    }
    else
    {
        // A failed request returns no partial body, so nothing half-received
        // can be mistaken for an answer.
        response.body.clear();
        if (IsCancelled(options.Cancellation))
        {
            response.cancelled = true;
            response.error = "Request cancelled";
        }
        else if (transfer.Overflowed || res == CURLE_FILESIZE_EXCEEDED)
        {
            response.overflowed = true;
            response.error = "Response exceeded the size limit (" + std::to_string(options.MaxResponseBytes) +
                             " bytes); raise PostOptions::MaxResponseBytes if the endpoint legitimately "
                             "returns more";
        }
        else if (res == CURLE_OPERATION_TIMEDOUT && onChunk != nullptr)
        {
            response.error = "Request timed out: the server stalled for at least " + std::to_string(stallSeconds) +
                             " s; raise PostOptions::Timeout if the endpoint legitimately pauses longer";
        }
        else if (res == CURLE_OPERATION_TIMEDOUT)
        {
            response.error = "Request timed out after " + std::to_string(options.Timeout.count()) +
                             " ms; raise PostOptions::Timeout if the endpoint legitimately takes longer";
        }
        else
        {
            response.error = curl_easy_strerror(res);
        }
    }

    return response;
}

} // namespace

namespace GameEngine
{

std::string HttpClient::UrlEncode(const std::string& str)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return str;

    char* encoded = curl_easy_escape(curl, str.c_str(), static_cast<int>(str.length()));
    std::string result = encoded ? encoded : str;
    curl_free(encoded);
    curl_easy_cleanup(curl);

    return result;
}

HttpClient::HttpResponse HttpClient::PostForm(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& formData,
    const std::unordered_map<std::string, std::string>& headers)
{
    InitializeCurl();

    HttpResponse response;
    response.error = ValidateHeaders(url, headers);
    if (!response.error.empty())
        return response;

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        response.error = "Failed to initialize curl";
        return response;
    }

    std::ostringstream formStream;
    bool first = true;
    for (const auto& [key, value] : formData)
    {
        if (!first)
            formStream << "&";
        formStream << UrlEncode(key) << "=" << UrlEncode(value);
        first = false;
    }
    std::string formDataStr = formStream.str();

    WriteState writeState(response.body);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeState);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    struct curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/x-www-form-urlencoded");
    for (const auto& [key, value] : headers)
    {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    ApplyCommonTransportOptions(curl);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK)
    {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.success = (httpCode >= 200 && httpCode < 300);
    }
    else
    {
        response.error = curl_easy_strerror(res);
        response.success = false;
    }

    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return response;
}

HttpClient::HttpResponse HttpClient::PostJson(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& jsonBody,
    const PostOptions& options)
{
    return PerformJsonPost(url, headers, jsonBody, options, nullptr);
}

HttpClient::HttpResponse HttpClient::PostJsonStream(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& jsonBody,
    const PostOptions& options,
    const ChunkCallback& onChunk)
{
    if (!onChunk)
    {
        HttpResponse response;
        response.error = "PostJsonStream needs a chunk callback; use PostJson to buffer the body";
        return response;
    }
    return PerformJsonPost(url, headers, jsonBody, options, &onChunk);
}

HttpClient::HttpResponse HttpClient::Get(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    size_t maxBodyBytes)
{
    InitializeCurl();

    HttpResponse response;
    response.error = ValidateHeaders(url, headers);
    if (!response.error.empty())
        return response;

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        response.error = "Failed to initialize curl";
        return response;
    }

    WriteState writeState(response.body, maxBodyBytes);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeState);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    struct curl_slist* headerList = nullptr;
    for (const auto& [key, value] : headers)
    {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    ApplyCommonTransportOptions(curl);
    // No CURLOPT_TIMEOUT: GET fetches project archives whose duration scales
    // with size and bandwidth. A stall abort bounds dead transfers instead.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK)
    {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.success = (httpCode >= 200 && httpCode < 300);
    }
    else
    {
        response.overflowed = writeState.Overflowed;
        response.error = writeState.Overflowed
            ? "Response exceeded the size limit (" + std::to_string(maxBodyBytes) + " bytes)"
            : curl_easy_strerror(res);
        response.success = false;
    }

    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return response;
}

bool HttpClient::IsAvailable()
{
    return true;
}

} // namespace GameEngine
