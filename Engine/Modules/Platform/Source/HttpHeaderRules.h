#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{

/// The request rule every HttpClient call applies before a byte goes on the
/// wire, on every backend:
/// - the URL holds no space, control character or NUL, so the host read here
///   is the host the transport connects to;
/// - a header name is an RFC 9110 token (letters, digits and
///   !#$%&'*+-.^_`|~) and a value holds no CR, LF or NUL, so no header can end
///   early and smuggle another, and no credential header passes under a
///   variant spelling;
/// - a credential header (Authorization, Proxy-Authorization, Cookie,
///   X-Api-Key, Api-Key, X-Goog-Api-Key; any case) is sent only over https or
///   to a loopback host (localhost, a dotted-decimal 127.x.x.x literal,
///   [::1]), so a key never crosses a network in clear text.
///
/// The rule reads the request URL only; where a redirect leads is the
/// caller's concern. A key carried in the URL query is not covered: a caller
/// that puts one there checks the URL itself.
///
/// Returns an empty string when the request may be sent; otherwise the reason,
/// phrased with the fix. The message never repeats a header value: a refused
/// value may be a key.
std::string ValidateHeaders(std::string_view url, const std::unordered_map<std::string, std::string>& headers);

} // namespace GameEngine
