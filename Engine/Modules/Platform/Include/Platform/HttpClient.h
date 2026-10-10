#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{

/// Simple blocking HTTP client. Desktop backends use libcurl (thread-safe: each
/// call creates its own curl easy handle; requests only speak http(s) —
/// including across redirects — connect within 30 s, and GET aborts when the
/// transfer stalls (< 1 KiB/s for 30 s) rather than capping total duration, so
/// large downloads on slow links still finish). Threaded wasm uses a
/// synchronous emscripten_fetch, which is why every call must come from a
/// background thread there — the main browser thread cannot block.
///
/// Every call checks the request URL and headers first and fails without
/// sending anything when the URL holds a space, control character or NUL, a
/// header name is not an HTTP token, a value holds CR, LF or NUL, or a
/// credential header (Authorization, Proxy-Authorization, Cookie, X-Api-Key,
/// Api-Key, X-Goog-Api-Key) would leave over plain http to a host that is not
/// loopback. The check reads the request URL only. PostJson and PostJsonStream
/// do not follow redirects; Get and PostForm do, and curl replays every custom
/// header except Authorization and Cookie to the redirect target, so a key in
/// another header can follow a redirect to another host, plain http included.
/// A key carried in the URL query is not checked.
class HttpClient
{
public:
    struct HttpResponse
    {
        int statusCode = 0;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
        bool success = false;
        std::string error;
        /// True when PostOptions::Cancellation stopped the request; success is
        /// then false and body is empty.
        bool cancelled = false;
        /// True when the body passed the response cap (PostOptions::MaxResponseBytes,
        /// or Get's maxBodyBytes); success is then false and error says so.
        bool overflowed = false;
    };

    /// Receives a streamed response body in arrival order. Chunk boundaries are
    /// wherever the transport split the bytes, not where the payload's own
    /// records end: the caller frames them. An exception thrown here stops the
    /// transfer and is rethrown from PostJsonStream.
    using ChunkCallback = std::function<void(std::string_view chunk)>;

    static constexpr std::chrono::milliseconds kDefaultPostTimeout = std::chrono::minutes(10);
    static constexpr size_t kDefaultMaxResponseBytes = size_t{16} * 1024 * 1024;

    struct PostOptions
    {
        /// PostJson: the whole-request limit, connect to last byte.
        /// PostJsonStream: how long the server may stall, rounded up to whole
        /// seconds, with no limit on total duration, so a long answer that
        /// keeps arriving is never cut off. curl averages the transfer rate
        /// over a few seconds, so right after a burst a stall can run several
        /// seconds past Timeout before it fails. The default is long because a
        /// hosted model can take minutes to answer; a call that expects a quick
        /// answer sets a shorter one.
        std::chrono::milliseconds Timeout = kDefaultPostTimeout;
        /// The request fails, naming this cap, once the response body would
        /// exceed it or declares a larger Content-Length. The default is far
        /// above a JSON API answer and stops a misbehaving endpoint before the
        /// body fills memory.
        size_t MaxResponseBytes = kDefaultMaxResponseBytes;
        /// Set to true from any thread to stop the request mid-transfer; the
        /// call then returns with HttpResponse::cancelled within about a
        /// second. Must outlive the call.
        const std::atomic<bool>* Cancellation = nullptr;
    };

    /// POST request with form data.
    static HttpResponse PostForm(const std::string& url,
                                 const std::unordered_map<std::string, std::string>& formData,
                                 const std::unordered_map<std::string, std::string>& headers = {});

    /// POST request with a JSON body (Content-Type: application/json), the
    /// response body buffered into HttpResponse::body. Redirects are not
    /// followed: the 3xx response is returned as is, so a credential header
    /// never travels to wherever a redirect points.
    ///
    /// Threaded wasm: the browser request is synchronous, so cancellation is
    /// observed only before it starts and after it ends (a result that
    /// completes after cancellation is discarded), the size cap is applied
    /// after the body has been downloaded, Timeout is not enforced (a
    /// synchronous request carries none; the browser bounds dead connections),
    /// and the browser follows redirects under its own header rules.
    static HttpResponse PostJson(const std::string& url,
                                 const std::unordered_map<std::string, std::string>& headers,
                                 const std::string& jsonBody,
                                 const PostOptions& options);

    /// PostJson that hands a 2xx response body to onChunk, on the calling
    /// thread, as the bytes arrive; HttpResponse::body stays empty. A non-2xx
    /// response is buffered into body instead, so the caller reads an error the
    /// same way as from PostJson. Streamed bytes count against
    /// MaxResponseBytes. Chunks already delivered stand when the request then
    /// fails (cancelled, timed out, over the cap): the caller discards what it
    /// assembled when success is false.
    ///
    /// Threaded wasm cannot stream (synchronous browser request): the whole
    /// body arrives as one chunk after the request completes.
    static HttpResponse PostJsonStream(const std::string& url,
                                       const std::unordered_map<std::string, std::string>& headers,
                                       const std::string& jsonBody,
                                       const PostOptions& options,
                                       const ChunkCallback& onChunk);

    /// GET request. maxBodyBytes > 0 aborts the transfer (success=false) once
    /// the response body exceeds the cap — callers fetching untrusted URLs
    /// should always set it, since the body is buffered in memory.
    static HttpResponse Get(const std::string& url,
                            const std::unordered_map<std::string, std::string>& headers = {},
                            size_t maxBodyBytes = 0);

    /// Percent-encode a string for use in URL query or path (RFC 3986).
    static std::string UrlEncode(const std::string& str);

    /// Whether this build can make HTTP requests at all.
    ///
    /// False is a property of the platform, not of the network: single-threaded
    /// wasm has no thread the browser lets block, so every request fails
    /// identically and for good. Callers that retry transient failures must
    /// check this first — retrying a capability that does not exist is an
    /// infinite loop that also floods the log.
    static bool IsAvailable();
};

} // namespace GameEngine
