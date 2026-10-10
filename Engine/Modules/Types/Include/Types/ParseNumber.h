#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Detect whether std::from_chars supports float/double on this platform.
// MSVC and GCC 11+ (libstdc++) provide full charconv; Apple Clang does not.
#include <charconv>
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    #define GE_FLOAT_CHARCONV_AVAILABLE 1
#elif defined(_MSC_VER) && _MSC_VER >= 1924
    #define GE_FLOAT_CHARCONV_AVAILABLE 1
#endif

#ifndef GE_FLOAT_CHARCONV_AVAILABLE
    #include <cerrno>
    #include <cstdlib>
    #include <cstring>
    #ifdef __APPLE__
        #include <xlocale.h>
    #else
        #include <locale.h>
    #endif
#endif

namespace GameEngine
{

// Locale-independent float parse. Returns the value on success, nullopt on
// failure. If charsConsumed is non-null, stores the number of characters that
// were part of the parsed number (callers can use this to inspect trailing
// suffixes like "px" or "%").
inline std::optional<float> ParseFloat(std::string_view s, size_t* charsConsumed = nullptr)
{
    if (s.empty())
        return std::nullopt;

#ifdef GE_FLOAT_CHARCONV_AVAILABLE
    float result = 0.0f;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec != std::errc{})
        return std::nullopt;
    if (charsConsumed)
        *charsConsumed = static_cast<size_t>(ptr - s.data());
    return result;
#else
    constexpr size_t kBufSize = 128;
    char buf[kBufSize];
    const char* str;
    std::string heap;
    if (s.size() < kBufSize)
    {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        str = buf;
    }
    else
    {
        heap.assign(s.data(), s.size());
        str = heap.c_str();
    }

    static locale_t cLocale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    char* end = nullptr;
    errno = 0;
    float result = strtof_l(str, &end, cLocale);
    if (end == str || errno == ERANGE)
        return std::nullopt;
    if (charsConsumed)
        *charsConsumed = static_cast<size_t>(end - str);
    return result;
#endif
}

inline std::optional<double> ParseDouble(std::string_view s, size_t* charsConsumed = nullptr)
{
    if (s.empty())
        return std::nullopt;

#ifdef GE_FLOAT_CHARCONV_AVAILABLE
    double result = 0.0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec != std::errc{})
        return std::nullopt;
    if (charsConsumed)
        *charsConsumed = static_cast<size_t>(ptr - s.data());
    return result;
#else
    constexpr size_t kBufSize = 128;
    char buf[kBufSize];
    const char* str;
    std::string heap;
    if (s.size() < kBufSize)
    {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = '\0';
        str = buf;
    }
    else
    {
        heap.assign(s.data(), s.size());
        str = heap.c_str();
    }

    static locale_t cLocale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    char* end = nullptr;
    errno = 0;
    double result = strtod_l(str, &end, cLocale);
    if (end == str || errno == ERANGE)
        return std::nullopt;
    if (charsConsumed)
        *charsConsumed = static_cast<size_t>(end - str);
    return result;
#endif
}

// Strict decimal parse: the intersection of what std::from_chars and strtod_l
// accept, so a string parses identically on every platform. ParseFloat and
// ParseDouble above differ across platforms at the edges — strtod_l takes hex
// ("0x10") and inf/nan, from_chars does not — and both stop at the first
// character they cannot use. This one refuses hex, a leading '+', inf and nan,
// refuses anything left over after the number, and refuses a non-finite
// result. Surrounding ASCII space is allowed and trimmed.
//
// Use it wherever a string has to mean the same number on Windows and macOS:
// authored values, serialized fields, anything a user typed.
inline bool ParseStrictDecimal(std::string_view text, double& outValue)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    std::string_view number = text.substr(begin, end - begin);
    if (number.empty())
        return false;

    if (number[0] == '+')
        number.remove_prefix(1);
    // A second '+' would be left over, and from_chars would stop before it.
    if (number.empty() || number[0] == '+')
        return false;

    std::string_view hexProbe = number;
    if (hexProbe[0] == '-')
        hexProbe.remove_prefix(1);
    if (hexProbe.size() >= 2 && hexProbe[0] == '0' && (hexProbe[1] == 'x' || hexProbe[1] == 'X'))
        return false;

    std::size_t consumed = 0;
    const std::optional<double> parsed = ParseDouble(number, &consumed);
    // isfinite rejects the inf and nan that strtod_l would have taken.
    if (!parsed || consumed != number.size() || !std::isfinite(*parsed))
        return false;
    outValue = *parsed;
    return true;
}

} // namespace GameEngine
