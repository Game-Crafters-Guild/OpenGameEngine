#include "Platform/HttpClient.h"
#include "HttpHeaderRules.h"

#include "Logger/Logger.h"

#ifndef GE_WASM_SINGLE_THREAD
#include <emscripten/fetch.h>
#include <emscripten/threading.h>
#endif

#include <cstdio>
#include <sstream>
#include <string_view>
#include <vector>

namespace GameEngine
{

#ifdef GE_WASM_SINGLE_THREAD

// Single-threaded wasm has no backend: a blocking request needs a thread the
// browser lets block, and this ABI runs everything on the main browser thread,
// where synchronous XHR is forbidden. Requests fail soft with a clear error so
// callers exercise their failure paths instead of retrying forever.
namespace
{
    HttpClient::HttpResponse Unsupported(const char* what)
    {
        HttpClient::HttpResponse response;
        response.success = false;
        response.error = "HttpClient is not available in the single-threaded wasm build "
                         "(blocking requests need a background thread)";
        Logger::Log::Warning("HttpClient::{}: {}", what, response.error);
        return response;
    }
} // namespace

HttpClient::HttpResponse HttpClient::PostForm(const std::string&,
                                              const std::unordered_map<std::string, std::string>&,
                                              const std::unordered_map<std::string, std::string>&)
{
    return Unsupported("PostForm");
}

HttpClient::HttpResponse HttpClient::PostJson(const std::string&,
                                              const std::unordered_map<std::string, std::string>&,
                                              const std::string&,
                                              const PostOptions&)
{
    return Unsupported("PostJson");
}

HttpClient::HttpResponse HttpClient::PostJsonStream(const std::string&,
                                                    const std::unordered_map<std::string, std::string>&,
                                                    const std::string&,
                                                    const PostOptions&,
                                                    const ChunkCallback&)
{
    return Unsupported("PostJsonStream");
}

HttpClient::HttpResponse HttpClient::Get(const std::string&,
                                         const std::unordered_map<std::string, std::string>&,
                                         size_t)
{
    return Unsupported("Get");
}

bool HttpClient::IsAvailable()
{
    return false;
}

#else // threaded wasm: synchronous emscripten_fetch on the calling pthread

namespace
{

// getAllResponseHeaders() text: CRLF-separated "name: value" lines, names
// lowercased by the browser. Same trimming as the desktop backend's
// HeaderCallback so both populate the map identically.
void ParseResponseHeaders(const std::string& text,
                          std::unordered_map<std::string, std::string>& outHeaders)
{
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        const size_t colonPos = line.find(':');
        if (colonPos == std::string::npos)
            continue;

        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        const auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.erase(0, 1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
                s.pop_back();
        };
        trim(key);
        trim(value);

        if (!key.empty())
            outHeaders[key] = value;
    }
}

// Blocking request on the calling pthread. Synchronous emscripten_fetch runs a
// synchronous XHR directly on this thread's worker and invokes nothing on any
// other thread, so it is legal anywhere except the main browser thread.
// Cross-origin servers must grant CORS (Access-Control-Allow-Origin): under the
// COOP/COEP isolation a threaded build runs with, an unapproved response is
// surfaced as a network error with status 0.
HttpClient::HttpResponse Perform(const char* method,
                                 const std::string& url,
                                 const std::unordered_map<std::string, std::string>& headers,
                                 const std::string* body,
                                 const char* contentType,
                                 size_t maxBodyBytes,
                                 std::string_view sizeLimitAdvice)
{
    HttpClient::HttpResponse response;

    if (emscripten_is_main_browser_thread())
    {
        response.error = "HttpClient blocks and must be called from a background thread on wasm "
                         "(the main browser thread cannot run synchronous XHR)";
        Logger::Log::Warning("HttpClient::{}: {}", method, response.error);
        return response;
    }
    response.error = ValidateHeaders(url, headers);
    if (!response.error.empty())
        return response;

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::snprintf(attr.requestMethod, sizeof(attr.requestMethod), "%s", method);
    // REPLACE opts out of the IndexedDB cache path: this is a network client,
    // and the browser's own HTTP cache already applies. No timeoutMSecs — a
    // synchronous XHR cannot carry a timeout; the browser's network stack
    // bounds dead connections.
    attr.attributes =
        EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;

    // emscripten_fetch wants {key1, value1, ..., nullptr}; the storage vector
    // owns the strings for the duration of the (synchronous) call.
    std::vector<std::string> headerStorage;
    std::vector<const char*> headerPointers;
    headerStorage.reserve(headers.size() * 2 + 2);
    if (contentType)
    {
        headerStorage.emplace_back("Content-Type");
        headerStorage.emplace_back(contentType);
    }
    for (const auto& [key, value] : headers)
    {
        headerStorage.emplace_back(key);
        headerStorage.emplace_back(value);
    }
    if (!headerStorage.empty())
    {
        headerPointers.reserve(headerStorage.size() + 1);
        for (const std::string& s : headerStorage)
            headerPointers.push_back(s.c_str());
        headerPointers.push_back(nullptr);
        attr.requestHeaders = headerPointers.data();
    }

    if (body && !body->empty())
    {
        attr.requestData = body->data();
        attr.requestDataSize = body->size();
    }

    emscripten_fetch_t* fetch = emscripten_fetch(&attr, url.c_str());
    if (!fetch)
    {
        response.error = "emscripten_fetch failed to start";
        Logger::Log::Warning("HttpClient: {} {} failed to start", method, url.substr(0, 96));
        return response;
    }

    response.statusCode = fetch->status;
    response.success = (fetch->status >= 200 && fetch->status < 300);

    // Header getters are only valid on the fetch's own thread and before
    // close — both hold here by construction.
    const size_t headersLength = emscripten_fetch_get_response_headers_length(fetch);
    if (headersLength > 0)
    {
        std::string headersText(headersLength + 1, '\0');
        emscripten_fetch_get_response_headers(fetch, headersText.data(), headersLength + 1);
        headersText.resize(headersLength);
        ParseResponseHeaders(headersText, response.headers);
    }

    // A synchronous XHR cannot be aborted mid-transfer, so the body cap is
    // enforced after the fact: the bytes were downloaded, but an over-cap
    // response is dropped rather than returned.
    if (maxBodyBytes != 0 && fetch->numBytes > maxBodyBytes)
    {
        response.success = false;
        response.overflowed = true;
        response.error =
            "Response exceeded the size limit (" + std::to_string(maxBodyBytes) + " bytes)" + std::string(sizeLimitAdvice);
    }
    else if (fetch->data && fetch->numBytes > 0)
    {
        response.body.assign(fetch->data, static_cast<size_t>(fetch->numBytes));
    }

    if (!response.success && response.error.empty())
    {
        if (fetch->status == 0)
        {
            // The browser reports blocked cross-origin responses and transport
            // failures identically; CORS is the usual culprit for a reachable
            // host.
            response.error = "network error or CORS rejection (status 0) — a cross-origin server "
                             "must send Access-Control-Allow-Origin";
        }
        else
        {
            response.error = "HTTP " + std::to_string(fetch->status);
            if (fetch->statusText[0] != '\0')
                response.error += std::string(" ") + fetch->statusText;
        }
    }

    emscripten_fetch_close(fetch);
    return response;
}

bool IsCancelled(const std::atomic<bool>* cancellation)
{
    return cancellation != nullptr && cancellation->load(std::memory_order_relaxed);
}

HttpClient::HttpResponse Cancelled()
{
    HttpClient::HttpResponse response;
    response.cancelled = true;
    response.error = "Request cancelled";
    return response;
}

// The synchronous request can be neither interrupted nor streamed, so the
// options apply around it: cancellation before and after, the size cap on the
// downloaded body, and a 2xx body handed to onChunk whole. Timeout is not
// enforced (see Perform). Error texts match the desktop backend's.
HttpClient::HttpResponse PerformJsonPost(const std::string& url,
                                         const std::unordered_map<std::string, std::string>& headers,
                                         const std::string& jsonBody,
                                         const HttpClient::PostOptions& options,
                                         const HttpClient::ChunkCallback* onChunk)
{
    if (options.Timeout.count() <= 0)
    {
        HttpClient::HttpResponse response;
        response.error = "PostOptions::Timeout must be positive; leave it at its default or set a duration";
        return response;
    }
    if (IsCancelled(options.Cancellation))
        return Cancelled();

    constexpr std::string_view kSizeLimitAdvice =
        "; raise PostOptions::MaxResponseBytes if the endpoint legitimately returns more";
    // Perform reads a cap of 0 as unlimited, while here 0 admits no body at
    // all: that one case is caught by the check below, after the copy.
    HttpClient::HttpResponse response =
        Perform("POST", url, headers, &jsonBody, "application/json", options.MaxResponseBytes, kSizeLimitAdvice);

    if (IsCancelled(options.Cancellation))
        return Cancelled();
    if (response.body.size() > options.MaxResponseBytes)
    {
        response.success = false;
        response.overflowed = true;
        response.body.clear();
        response.error = "Response exceeded the size limit (" + std::to_string(options.MaxResponseBytes) +
                         " bytes)" + std::string(kSizeLimitAdvice);
        return response;
    }
    if (onChunk != nullptr && response.success)
    {
        (*onChunk)(response.body);
        response.body.clear();
    }
    return response;
}

} // namespace

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

HttpClient::HttpResponse HttpClient::PostForm(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& formData,
    const std::unordered_map<std::string, std::string>& headers)
{
    std::ostringstream formStream;
    bool first = true;
    for (const auto& [key, value] : formData)
    {
        if (!first)
            formStream << "&";
        formStream << UrlEncode(key) << "=" << UrlEncode(value);
        first = false;
    }
    const std::string body = formStream.str();
    return Perform("POST", url, headers, &body, "application/x-www-form-urlencoded", 0, {});
}

HttpClient::HttpResponse HttpClient::Get(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    size_t maxBodyBytes)
{
    return Perform("GET", url, headers, nullptr, nullptr, maxBodyBytes, {});
}

bool HttpClient::IsAvailable()
{
    return true;
}

#endif // GE_WASM_SINGLE_THREAD

std::string HttpClient::UrlEncode(const std::string& str)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(str.size());
    for (const char c : str)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved)
        {
            encoded += c;
        }
        else
        {
            const auto byte = static_cast<unsigned char>(c);
            encoded += '%';
            encoded += kHex[byte >> 4];
            encoded += kHex[byte & 0x0F];
        }
    }
    return encoded;
}

} // namespace GameEngine
