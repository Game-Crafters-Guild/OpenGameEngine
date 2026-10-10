#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace GameEngine
{

// The text in a fixed char buffer up to its first null byte, or the whole buffer
// when it holds none. Component string fields are such buffers, and a raw component
// write (GE_ECSABI_SetComponentBytes) can fill every byte, so never read one as a C string.
template <std::size_t Capacity>
[[nodiscard]] std::string_view FixedStringView(const char (&chars)[Capacity])
{
    const void* terminator = std::memchr(chars, '\0', Capacity);
    const std::size_t length =
        terminator ? static_cast<std::size_t>(static_cast<const char*>(terminator) - chars) : Capacity;
    return std::string_view(chars, length);
}

/// `count` followed by the noun that agrees with it: "1 entity", "0 entities", "3 entities".
/// English plurals are irregular, so the caller spells both forms.
inline std::string FormatCount(std::size_t count, std::string_view singular, std::string_view plural)
{
    std::string text = std::to_string(count);
    text += ' ';
    text += count == 1 ? singular : plural;
    return text;
}

/// `value` in decimal with a comma between each group of three digits: "4,194,304". For counts a
/// person reads (samples, bytes), where an ungrouped "4194304" has to be counted by eye.
inline std::string FormatGroupedInteger(unsigned long long value)
{
    const std::string digits = std::to_string(value);
    std::string text;
    text.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i != 0 && (digits.size() - i) % 3 == 0)
            text += ',';
        text += digits[i];
    }
    return text;
}

/// `bytes` in mebibytes for a person to read: one decimal below 10 MiB ("4.5 MiB"), whole above
/// ("773 MiB"), where a tenth of a mebibyte no longer tells a reader anything.
inline std::string FormatMebibytes(unsigned long long bytes)
{
    const double mebibytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char text[32];
    std::snprintf(text, sizeof(text), mebibytes < 10.0 ? "%.1f MiB" : "%.0f MiB", mebibytes);
    return text;
}

inline std::string ToLowerAscii(std::string_view sv)
{
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Case-insensitive substring check. Avoids allocating lowered copies.
inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    auto toLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [&](char a, char b) { return toLower(a) == toLower(b); });
    return it != haystack.end();
}

// Case-insensitive prefix check.
inline bool StartsWithIgnoreCase(std::string_view haystack, std::string_view prefix)
{
    if (prefix.size() > haystack.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(haystack[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

// Case-insensitive suffix check.
inline bool EndsWithIgnoreCase(std::string_view haystack, std::string_view suffix)
{
    if (suffix.size() > haystack.size())
        return false;
    const size_t off = haystack.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(haystack[off + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return true;
}

// Case-insensitive equality.
inline bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && StartsWithIgnoreCase(a, b);
}

/// True when `text` holds an ASCII control character (below 0x20, or 0x7F), a byte no
/// name or path a user authors contains.
inline bool ContainsControlCharacter(std::string_view text)
{
    constexpr unsigned char kFirstPrintable = 0x20;
    constexpr unsigned char kDelete = 0x7F;
    return std::any_of(text.begin(), text.end(),
                       [](char character)
                       {
                           const auto byte = static_cast<unsigned char>(character);
                           return byte < kFirstPrintable || byte == kDelete;
                       });
}

inline std::string TrimWhitespace(std::string s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    if (start > 0)
        s.erase(0, start);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

inline std::string_view TrimWhitespaceView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

inline std::string ToUpperSnakeCase(const std::string& name)
{
    std::string result;
    result.reserve(name.size() + 4);
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c)) && i > 0
            && std::islower(static_cast<unsigned char>(name[i - 1])))
        {
            result += '_';
        }
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return result;
}

/// A code identifier as words for a label: "castsShadows" and "CastsShadows" read "Casts Shadows",
/// "initial_linear_velocity" reads "Initial Linear Velocity" and "m_SheetColumns" reads "Sheet
/// Columns". Acronym and digit runs stay together ("GPUSkin" reads "GPU Skin", "World3D" reads
/// "World 3D"), an acronym keeps the number that follows it ("MSM4" reads "MSM4") and a dimension
/// stays whole ("Grid5x5" reads "Grid 5x5"). Each word starts with a capital and keeps the case of the
/// rest. Text that already holds a space is returned as it is.
inline std::string IdentifierToWords(std::string_view identifier)
{
    if (identifier.find(' ') != std::string_view::npos)
        return std::string(identifier);
    if (identifier.substr(0, 2) == "m_")
        identifier.remove_prefix(2);
    while (!identifier.empty() && identifier.front() == '_')
        identifier.remove_prefix(1);
    while (!identifier.empty() && identifier.back() == '_')
        identifier.remove_suffix(1);

    std::string words;
    words.reserve(identifier.size() + 8);
    bool wordLedByDigit = false;
    for (size_t i = 0; i < identifier.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(identifier[i]);
        if (c == '_')
        {
            if (!words.empty() && words.back() != ' ')
                words += ' ';
            continue;
        }
        if (!words.empty() && words.back() != ' ' && i > 0)
        {
            const unsigned char previous = static_cast<unsigned char>(identifier[i - 1]);
            const bool nextIsLower =
                i + 1 < identifier.size() && std::islower(static_cast<unsigned char>(identifier[i + 1]));
            const bool afterLower = std::islower(previous) != 0;
            const bool endsAcronym = (std::isupper(previous) || std::isdigit(previous)) && nextIsLower;
            // A number starts a word after a lowercase word, not after an acronym ("MSM4") and not
            // inside a word that began with a number ("5x5").
            const bool startsNumber =
                std::isdigit(c) && !std::isdigit(previous) && !std::isupper(previous) && !wordLedByDigit;
            const bool wordStart = (std::isupper(c) && (afterLower || endsAcronym)) || startsNumber;
            if (wordStart)
                words += ' ';
        }
        const bool firstOfWord = words.empty() || words.back() == ' ';
        if (firstOfWord)
            wordLedByDigit = std::isdigit(c) != 0;
        words += firstOfWord ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
    }
    return words;
}

} // namespace GameEngine
