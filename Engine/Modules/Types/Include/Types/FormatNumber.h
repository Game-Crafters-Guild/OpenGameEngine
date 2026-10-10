#pragma once

#include <charconv>
#include <string>
#include <system_error>

namespace GameEngine
{

// Locale-independent number formatting (std::to_chars) — the write-side
// counterpart of ParseNumber.h. An ostream would apply the global locale
// (e.g. comma decimal separators), which ParseFloat/ParseDouble could not
// read back.
//
// Default is the shortest representation that parses back to exactly the
// same value (serializer form; -0.0 keeps its sign). significantDigits > 0
// instead caps the width printf-%g-style — trailing zeros trimmed,
// scientific notation for extreme magnitudes — for display uses.

inline void AppendFloat(std::string& out, float value, int significantDigits = 0)
{
    char buf[32];
    const auto res = significantDigits > 0
        ? std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general, significantDigits)
        : std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec == std::errc{})
        out.append(buf, res.ptr);
    else
        out.push_back('0');
}

inline std::string FormatFloat(float value, int significantDigits = 0)
{
    std::string out;
    AppendFloat(out, value, significantDigits);
    return out;
}

inline void AppendDouble(std::string& out, double value)
{
    char buf[40];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec == std::errc{})
        out.append(buf, res.ptr);
    else
        out.push_back('0');
}

inline std::string FormatDouble(double value)
{
    std::string out;
    AppendDouble(out, value);
    return out;
}

// Fixed notation with at most `decimals` digits after the point, trailing zeros and a bare
// point trimmed, for display: (12.5, 3) -> "12.5", (3.0, 2) -> "3", (-0.0001, 3) -> "0".
// Infinity and NaN read "inf", "-inf" and "nan".
inline std::string FormatFixed(double value, int decimals)
{
    char buf[512];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, decimals);
    if (res.ec != std::errc{})
        return "0";
    std::string out(buf, res.ptr);
    if (out.find('.') != std::string::npos)
    {
        out.erase(out.find_last_not_of('0') + 1);
        if (out.back() == '.')
            out.pop_back();
    }
    return out == "-0" ? "0" : out;
}

} // namespace GameEngine
