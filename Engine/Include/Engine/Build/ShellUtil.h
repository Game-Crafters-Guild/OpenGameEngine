#pragma once

#include <filesystem>
#include <string>

namespace GameEngine {

// Single-quote a string for safe interpolation into a POSIX /bin/sh command line.
// Everything inside single quotes is literal (no $, backtick, or backslash expansion),
// and an embedded single quote is emitted as the standard '\'' escape. Use this for
// EVERY path/value interpolated into a shell-out (iconutil, sips, codesign,
// install_name_tool, ditto, PlistBuddy, …) — double quotes are NOT safe because sh
// still expands $(...), backticks, and \ inside them, which turns an attacker- or
// typo-influenced build path into arbitrary command execution.
inline std::string ShellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

inline std::string ShellQuote(const std::filesystem::path& p)
{
    return ShellQuote(p.string());
}

} // namespace GameEngine
