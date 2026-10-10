#pragma once

#include <array>
#include <string>
#include <string_view>

namespace GameEngine::GraphPortLabels
{

/* THE RULE for port names on graph nodes: author full names in the catalog —
 * the inspector and the port tooltip always show them verbatim. On the node
 * itself, a label that fits the port-label column renders unchanged; one that
 * does not gets each word shortened through the standard-abbreviation table
 * below, and whatever still overflows ellipsizes in CSS. New names follow the
 * rule automatically; when a new long word ellipsizes, add its abbreviation
 * to the table rather than renaming the port. */

/* ~60 graph units of label column at ~7 units per glyph. */
inline constexpr int kMaxNodeLabelChars = 9;

struct WordAbbreviation
{
    std::string_view Word;
    std::string_view Short;
};

inline constexpr std::array<WordAbbreviation, 15> kWordAbbreviations = {{
    {"Seconds", "Sec"},
    {"Comparison", "Compare"},
    {"Second", "Sec"},
    {"Minutes", "Min"},
    {"Position", "Pos"},
    {"Rotation", "Rot"},
    {"Smoothing", "Smooth"},
    {"Direction", "Dir"},
    {"Velocity", "Vel"},
    {"Distance", "Dist"},
    {"Duration", "Dur"},
    {"Maximum", "Max"},
    {"Minimum", "Min"},
    {"Amount", "Amt"},
    {"Multiplier", "Mult"},
}};

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb)
            return false;
    }
    return true;
}

/* Parameter ids are camelCase; labels read as words: "fadeSeconds" ->
   "Fade Seconds". Names that already contain spaces pass through. */
inline std::string PrettifyParamKey(std::string_view key)
{
    if (key.find(' ') != std::string_view::npos)
        return std::string(key);
    std::string out;
    out.reserve(key.size() + 4);
    for (size_t i = 0; i < key.size(); ++i)
    {
        char c = key[i];
        if (i == 0 && c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 32);
        else if (c >= 'A' && c <= 'Z' && i > 0)
            out += ' ';
        out += c;
    }
    return out;
}

inline std::string AbbreviateForNode(std::string_view name, int maxChars = kMaxNodeLabelChars)
{
    if (static_cast<int>(name.size()) <= maxChars)
        return std::string(name);

    std::string out;
    out.reserve(name.size());
    size_t pos = 0;
    while (pos <= name.size())
    {
        const size_t space = name.find(' ', pos);
        const size_t end = space == std::string_view::npos ? name.size() : space;
        std::string_view word = name.substr(pos, end - pos);
        for (const WordAbbreviation& abbr : kWordAbbreviations)
        {
            if (EqualsIgnoreCase(word, abbr.Word))
            {
                word = abbr.Short;
                break;
            }
        }
        if (!out.empty())
            out += ' ';
        out.append(word);
        if (space == std::string_view::npos)
            break;
        pos = space + 1;
    }
    return out;
}

} // namespace GameEngine::GraphPortLabels
