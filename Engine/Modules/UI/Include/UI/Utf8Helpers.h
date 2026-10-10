#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine { namespace Utf8 {

inline std::string Encode(uint32_t cp)
{
    std::string out;
    if (cp <= 0x7F)
        out.push_back((char)cp);
    else if (cp <= 0x7FF)
    {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    return out;
}

inline bool IsTextInputCodepoint(uint32_t cp)
{
    if (cp == '\t' || cp == '\n' || cp == '\r')
        return true;
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))
        return false;
    if (cp > 0x10FFFF)
        return false;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return false;
    return true;
}

// Start of the character before byte offset i.
inline size_t Prev(std::string_view s, size_t i)
{
    if (i == 0 || i > s.size())
        return 0;
    --i;
    while (i > 0 && ((s[i] & 0xC0) == 0x80))
        --i;
    return i;
}

// Start of the character after the one at byte offset i.
inline size_t Next(std::string_view s, size_t i)
{
    if (i >= s.size())
        return s.size();
    ++i;
    while (i < s.size() && ((s[i] & 0xC0) == 0x80))
        ++i;
    return i;
}

}} // namespace GameEngine::Utf8
