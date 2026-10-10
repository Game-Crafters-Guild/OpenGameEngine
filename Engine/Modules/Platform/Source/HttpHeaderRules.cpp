#include "HttpHeaderRules.h"

#include "Types/StringUtils.h"

#include <algorithm>
#include <array>

namespace GameEngine
{
namespace
{

constexpr std::array<std::string_view, 6> kCredentialHeaders = {
    "Authorization", "Proxy-Authorization", "Cookie", "X-Api-Key", "Api-Key", "X-Goog-Api-Key"};

// CR and LF end a header line; NUL ends the C string the backend hands on.
constexpr std::string_view kLineBreakingCharacters("\r\n\0", 3);

// RFC 9110 token characters besides letters and digits.
constexpr std::string_view kTokenSymbols = "!#$%&'*+-.^_`|~";

// A header name is an RFC 9110 token. Anything else (a space before the
// colon, a tab, a non-ASCII byte) is a different name to every comparison
// here, yet the backend still sends it.
bool IsToken(std::string_view name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               kTokenSymbols.find(c) != std::string_view::npos;
    });
}

// The backends hand the URL on as a C string, so a NUL cuts it short of the
// host read here, and a space or control character can make the transport's
// URL parser read a different host than UrlHost does.
bool HasUnsafeUrlCharacter(std::string_view url)
{
    return std::any_of(url.begin(), url.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte <= 0x20 || byte == 0x7F;
    });
}

bool IsCredentialHeader(std::string_view name)
{
    return std::any_of(kCredentialHeaders.begin(), kCredentialHeaders.end(),
                       [name](std::string_view credential) { return EqualsIgnoreCase(name, credential); });
}

// The host as written in the URL: no userinfo, no port; an IPv6 literal keeps
// its brackets. A URL without "scheme://" is read from its first character,
// the way the backends read it.
std::string_view UrlHost(std::string_view url)
{
    const size_t schemeEnd = url.find("://");
    std::string_view authority = schemeEnd == std::string_view::npos ? url : url.substr(schemeEnd + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));

    const size_t userInfoEnd = authority.rfind('@');
    if (userInfoEnd != std::string_view::npos)
        authority = authority.substr(userInfoEnd + 1);

    if (!authority.empty() && authority.front() == '[')
        return authority.substr(0, authority.find(']') + 1);
    return authority.substr(0, authority.find(':'));
}

// 127.0.0.0/8 written as a dotted-decimal literal of two to four parts, each
// 0-255 without a leading zero ("127.1" is 127.0.0.1). Anything else, such as
// "127.example.com" or "127.0.0.999", goes to a resolver, which can answer
// with any address.
bool IsLoopbackIPv4Literal(std::string_view host)
{
    constexpr size_t kMaxParts = 4;
    constexpr size_t kMaxPartDigits = 3;
    constexpr int kMaxPartValue = 255;
    if (!host.starts_with("127."))
        return false;

    size_t parts = 0;
    for (;;)
    {
        const size_t dot = host.find('.');
        const std::string_view part = host.substr(0, dot);
        const bool decimal = std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (part.empty() || part.size() > kMaxPartDigits || !decimal || (part.size() > 1 && part.front() == '0'))
            return false;
        int value = 0;
        for (const char c : part)
            value = value * 10 + (c - '0');
        if (value > kMaxPartValue || ++parts > kMaxParts)
            return false;
        if (dot == std::string_view::npos)
            return true;
        host.remove_prefix(dot + 1);
    }
}

bool IsLoopbackHost(std::string_view host)
{
    return EqualsIgnoreCase(host, "localhost") || host == "[::1]" || IsLoopbackIPv4Literal(host);
}

} // namespace

std::string ValidateHeaders(std::string_view url, const std::unordered_map<std::string, std::string>& headers)
{
    if (HasUnsafeUrlCharacter(url))
        return "The URL contains a space, a control character or NUL; percent-encode it";

    for (const auto& [name, value] : headers)
    {
        if (!IsToken(name))
        {
            return "An HTTP header name is empty or holds a character outside the HTTP token set (letters, "
                   "digits and !#$%&'*+-.^_`|~); remove spaces, separators and control characters from it";
        }
        if (value.find_first_of(kLineBreakingCharacters) != std::string::npos)
        {
            return "The value of HTTP header '" + name +
                   "' contains CR, LF or NUL; remove the line break from the value";
        }
        if (IsCredentialHeader(name) && !StartsWithIgnoreCase(url, "https://") && !IsLoopbackHost(UrlHost(url)))
        {
            return "HTTP header '" + name +
                   "' carries a credential and is sent only over https or to a loopback host; use an "
                   "https:// URL";
        }
    }
    return {};
}

} // namespace GameEngine
