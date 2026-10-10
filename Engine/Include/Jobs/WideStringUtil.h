#pragma once

#include <string>

namespace GameEngine
{
// Small shared helpers used by Jobs/ compile-server plumbing.
//
// Note: these are defined in a header (with include guards) so Unity builds
// don't hit duplicate function definitions when multiple .cpp files are
// compiled into a single translation unit.

#ifdef _WIN32
// Helper to convert UTF-8 std::string to std::wstring (simple ASCII-range conversion).
// This intentionally keeps behavior identical to the previous per-file helpers.
inline std::wstring ToWide(const std::string& s)
{
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s)
        w.push_back((wchar_t)c);
    return w;
}
#endif

} // namespace GameEngine


