// Word segmentation for the text controls, as a documented subset of
// UAX #29 (Unicode Text Segmentation) word boundaries, corrected against
// measured Chrome behaviour where the two disagree.
//
// Implemented:
//   * Word characters are letters, ASCII digits and ExtendNumLet connectors
//     ("_" and friends). A word is a maximal run of them.
//   * WB6/WB7  — MidLetter and MidNumLetQ join two letters  ("a·b", "don't").
//   * WB11/12  — MidNum and MidNumLetQ join two digits      ("1,5", "1.5").
//                This is what keeps a float field's "1.5" one word.
//   * WB3d     — a run of whitespace is one segment.
//   * WB4      — combining marks take the class of the character they follow.
//   * WB999    — everything else (punctuation, symbols) breaks on both sides
//                and is therefore a single-character segment.
//
// Two joiners are deliberately NOT classified as UAX #29 classifies them. Both
// were measured in Chrome/150 with real double-clicks on a real <input> (the
// oracle rows are in TextSegmentationTests). Parity with the browser is the
// goal, so the measurement wins over the spec here — do not "correct" these
// back to the UAX #29 tables:
//
//   * FULL STOP (U+002E) and its fullwidth form (U+FF0E) are MidNum, not
//     MidNumLet. UAX #29 would join "a.b"; Chrome splits it, and that is the
//     case that makes a double-click on "Player.fbx" select the stem. The other
//     MidNumLet dots — U+2024, U+FE52 — do join letters, so this is FULL STOP
//     specifically and not dots as a class.
//   * The colons (U+003A, U+FF1A, U+FE55) join nothing at all. UAX #29 has them
//     in MidLetter; Chrome splits "a:b", and splits "1:5" and "12:30" too. The
//     rest of MidLetter (U+00B7, U+0387, U+2027, U+FE13) does join letters.
//
// The other two MidLetter entries, U+055F and U+05F4, keep the UAX #29
// classification unmeasured: both are RTL-context characters, and the LTR
// harness the rows above came from cannot place a pointer over one reliably.
//
// Not implemented, and worth knowing before relying on this for anything
// beyond editing UI text:
//   * Letters are recognised by the block ranges in kLetterRanges rather than
//     by the Unicode general category. Scripts outside those blocks classify as
//     Other, so a double-click there selects one character instead of a word.
//   * Only ASCII 0-9 count as digits; Arabic-Indic and other decimal digits do
//     not join across MidNum.
//   * No dictionary segmentation for scripts that do not space their words
//     (Chinese, Japanese, Thai, Khmer): a run of ideographs is one word, where
//     ICU would find several.
//   * No WB3c (ZWJ x Extended_Pictographic) or WB15/16 (regional indicators),
//     so emoji sequences and flags segment per code point.
//   * Whitespace is one class doing two jobs — a run of it is one segment, and
//     a word takes the run that follows it — where Chrome uses two different
//     sets. Measured 2026-07-26 by the method the joiner rows came from:
//     U+0009, U+00A0 and U+2007 are *not* run-joiners in Chrome (each is its own
//     segment) though a word does still take them as trailing space, and U+202F
//     is not a separator at all but a word character, so Chrome selects
//     "x<U+202F>y" whole. All four are plain whitespace here, so a run of them
//     is one segment and a double-click over one selects too much.

#include "UI/TextSegmentation.h"

#include "UI/Utf8Helpers.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine
{
namespace TextSegmentation
{
namespace
{

enum class Category : uint8_t
{
    Other,        // WB999: breaks on both sides
    Whitespace,   // WB3d: runs together
    ALetter,
    Numeric,
    ExtendNumLet, // a word character that also glues the runs on either side
    MidLetter,    // joins ALetter x ALetter
    MidNum,       // joins Numeric x Numeric
    MidNumLet,    // joins either pair
    Extend,       // WB4: takes the class of the character it follows
};

struct CodepointRange
{
    uint32_t First;
    uint32_t Last;
};

// Approximation of Unicode general category L*. Each entry is a block (or the
// letter part of one) treated as all-letter. A code point missing from this
// table classifies as Other, which breaks a word: the visible symptom is a
// double-click selecting too little, never selecting across a real boundary.
constexpr CodepointRange kLetterRanges[] = {
    {0x00AA, 0x00AA}, {0x00B5, 0x00B5}, {0x00BA, 0x00BA}, // ordinals, micro sign
    {0x00C0, 0x00D6}, {0x00D8, 0x00F6},                   // Latin-1 letters
    {0x00F8, 0x02FF},                                     // Latin Extended-A/B, IPA, modifiers
    {0x0370, 0x0373}, {0x0376, 0x0377}, {0x037A, 0x037D},
    {0x037F, 0x037F}, {0x0386, 0x0386}, {0x0388, 0x03FF},  // Greek and Coptic
    {0x0400, 0x0481}, {0x048A, 0x052F},                   // Cyrillic
    {0x0531, 0x0556}, {0x0560, 0x0588},                   // Armenian
    {0x05D0, 0x05EA}, {0x05EF, 0x05F2},                   // Hebrew
    {0x0620, 0x064A}, {0x066E, 0x06D3},                   // Arabic
    {0x0900, 0x097F},                                     // Devanagari
    {0x0E01, 0x0E3A}, {0x0E40, 0x0E4E},                   // Thai
    {0x1E00, 0x1FFF},                                     // Latin Extended Additional, Greek Extended
    {0x2C60, 0x2C7F},                                     // Latin Extended-C
    {0x3041, 0x3096}, {0x30A1, 0x30FA}, {0x30FC, 0x30FF}, // Hiragana, Katakana
    {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFAFF}, // CJK ideographs
    {0xAC00, 0xD7A3},                                     // Hangul syllables
    {0xFB00, 0xFB06},                                     // Latin ligatures
    {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A},                   // Fullwidth Latin
};

// Combining marks and joiners that continue the preceding character (WB4).
constexpr CodepointRange kExtendRanges[] = {
    {0x0300, 0x036F}, // combining diacritical marks
    {0x200C, 0x200D}, // ZWNJ, ZWJ
    {0x1DC0, 0x1DFF}, // combining diacritical marks supplement
    {0x20D0, 0x20F0}, // combining marks for symbols
    {0xFE00, 0xFE0F}, // variation selectors
    {0xFE20, 0xFE2F}, // combining half marks
};

// The four joiner tables below are consulted only for code points at or above
// U+0080; CategoryOf answers ASCII from its own branch. Listing an ASCII code
// point here would be inert, so none appears.

// UAX #29 ExtendNumLet: connector punctuation.
constexpr uint32_t kExtendNumLetCodepoints[] = {
    0x203F, 0x2040, 0x2054, 0xFE33, 0xFE34, 0xFE4D, 0xFE4E, 0xFE4F, 0xFF3F,
};

// UAX #29 MidLetter, less the colons — measured to join nothing.
constexpr uint32_t kMidLetterCodepoints[] = {
    0x00B7, 0x0387, 0x055F, 0x05F4, 0x2027, 0xFE13,
};

// UAX #29 MidNum, plus the full stops measured to behave as MidNum rather than
// as the MidNumLet the spec assigns them.
constexpr uint32_t kMidNumCodepoints[] = {
    0x037E, 0x0589, 0x060C, 0x060D, 0x066C, 0x07F8, 0x2044,
    0xFE10, 0xFE14, 0xFE50, 0xFE54, 0xFF0C, 0xFF0E, 0xFF1B,
};

// UAX #29 MidNumLetQ (MidNumLet plus Single_Quote), less the full stops above.
// WB6/7 and WB11/12 treat these identically, which is what joins "don't" and
// "1'5" alike.
constexpr uint32_t kMidNumLetCodepoints[] = {
    0x2018, 0x2019, 0x2024, 0xFE52, 0xFF07,
};

constexpr CodepointRange kWhitespaceRanges[] = {
    {0x2000, 0x200A}, // en quad .. hair space
};

constexpr uint32_t kWhitespaceCodepoints[] = {
    0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x0020, 0x0085, 0x00A0,
    0x1680, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000,
};

bool InRanges(uint32_t cp, const CodepointRange* ranges, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (cp >= ranges[i].First && cp <= ranges[i].Last)
            return true;
    }
    return false;
}

bool IsOneOf(uint32_t cp, const uint32_t* set, size_t count)
{
    return std::find(set, set + count, cp) != set + count;
}

template <size_t N>
bool InRanges(uint32_t cp, const CodepointRange (&ranges)[N])
{
    return InRanges(cp, ranges, N);
}

template <size_t N>
bool IsOneOf(uint32_t cp, const uint32_t (&set)[N])
{
    return IsOneOf(cp, set, N);
}

// Decode the character starting at `start`, where the character's extent is
// whatever Utf8::Next calls it: one byte plus the whole run of continuation
// bytes after it.
//
// That makes the decode total rather than validating. A run of 2-4 bytes goes
// through the ordinary mask arithmetic whether or not it is well-formed, so a
// malformed sequence yields some code point rather than an error; a longer run
// yields its lead byte. A text field can hold whatever the clipboard gave it,
// and the property segmentation needs is only that the index always advances
// and never leaves the string, which Utf8::Next guarantees. The resulting
// category on malformed input is unspecified, not Other: a stray 0xC3 decodes
// as U+00C3 and classifies as a letter.
uint32_t DecodeCodepoint(std::string_view text, size_t start)
{
    const size_t end = Utf8::Next(text, start);
    const size_t length = end - start;
    const auto byte = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(text[i])); };

    const uint32_t lead = byte(start);
    if (length == 1)
        return lead;
    if (length == 2)
        return ((lead & 0x1Fu) << 6) | (byte(start + 1) & 0x3Fu);
    if (length == 3)
        return ((lead & 0x0Fu) << 12) | ((byte(start + 1) & 0x3Fu) << 6) | (byte(start + 2) & 0x3Fu);
    if (length == 4)
    {
        return ((lead & 0x07u) << 18) | ((byte(start + 1) & 0x3Fu) << 12) |
               ((byte(start + 2) & 0x3Fu) << 6) | (byte(start + 3) & 0x3Fu);
    }
    return lead;
}

Category CategoryOf(uint32_t cp)
{
    if (cp < 0x80)
    {
        if (cp >= '0' && cp <= '9')
            return Category::Numeric;
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))
            return Category::ALetter;
        if (cp == '_')
            return Category::ExtendNumLet;
        if (cp == '\'')
            return Category::MidNumLet;
        if (cp == '.' || cp == ',' || cp == ';')
            return Category::MidNum;
        // ':' is Other: it joins neither "a:b" nor "1:5".
        if (IsOneOf(cp, kWhitespaceCodepoints))
            return Category::Whitespace;
        return Category::Other;
    }

    if (IsOneOf(cp, kWhitespaceCodepoints) || InRanges(cp, kWhitespaceRanges))
        return Category::Whitespace;
    if (InRanges(cp, kExtendRanges))
        return Category::Extend;
    if (IsOneOf(cp, kExtendNumLetCodepoints))
        return Category::ExtendNumLet;
    if (IsOneOf(cp, kMidNumLetCodepoints))
        return Category::MidNumLet;
    if (IsOneOf(cp, kMidNumCodepoints))
        return Category::MidNum;
    if (IsOneOf(cp, kMidLetterCodepoints))
        return Category::MidLetter;
    if (InRanges(cp, kLetterRanges))
        return Category::ALetter;
    return Category::Other;
}

Category CategoryAt(std::string_view text, size_t start)
{
    return CategoryOf(DecodeCodepoint(text, start));
}

// WB4: a combining mark inherits the class of the character it attaches to. A
// mark with no base (leading Extend) has nothing to attach to and is Other.
Category BaseCategoryAt(std::string_view text, size_t start)
{
    Category category = CategoryAt(text, start);
    while (category == Category::Extend && start > 0)
    {
        start = Utf8::Prev(text, start);
        category = CategoryAt(text, start);
    }
    return category == Category::Extend ? Category::Other : category;
}

Category CategoryBefore(std::string_view text, size_t start)
{
    if (start == 0)
        return Category::Other;
    return BaseCategoryAt(text, Utf8::Prev(text, start));
}

Category CategoryAfter(std::string_view text, size_t start)
{
    size_t next = Utf8::Next(text, start);
    while (next < text.size() && CategoryAt(text, next) == Category::Extend)
        next = Utf8::Next(text, next);
    if (next >= text.size())
        return Category::Other;
    return CategoryAt(text, next);
}

// Whether the character starting at `start` is part of a word. Joiners are
// word characters only while they sit between two characters of the class they
// join, which is what makes "1.5" one word and "hello," two segments.
//
// The test is on the two immediately flanking characters, never on the runs
// they belong to: "x1.5y" is one word because the "." has a digit on each side,
// even though both runs contain letters.
bool IsWordCharAt(std::string_view text, size_t start)
{
    switch (BaseCategoryAt(text, start))
    {
    case Category::ALetter:
    case Category::Numeric:
    case Category::ExtendNumLet:
        return true;
    case Category::MidLetter:
        return CategoryBefore(text, start) == Category::ALetter &&
               CategoryAfter(text, start) == Category::ALetter;
    case Category::MidNum:
        return CategoryBefore(text, start) == Category::Numeric &&
               CategoryAfter(text, start) == Category::Numeric;
    case Category::MidNumLet:
    {
        const Category before = CategoryBefore(text, start);
        const Category after = CategoryAfter(text, start);
        return (before == Category::ALetter && after == Category::ALetter) ||
               (before == Category::Numeric && after == Category::Numeric);
    }
    default:
        return false;
    }
}

bool IsWhitespaceAt(std::string_view text, size_t start)
{
    return CategoryAt(text, start) == Category::Whitespace;
}

// The scans below step by *unit*, where a unit is either one ordinary character
// or a whole run of the combining marks attached to one. WB4 gives every mark in
// a run the class of the character the run follows, so the run tests the same
// either way — but asked at the run's first mark, BaseCategoryAt is a single
// step back to that character instead of a walk over the marks already passed.
// Stepping per code point instead pays that walk once per mark, which is
// quadratic in the length of the run: 16 KB of marks costs 8.8 s per pass that
// way against 4.2 ms this way in a Debug build, and 340 ms against 0.31 ms in
// Release. TextSegmentationPerfTests guards it and carries the full figures.
//
// A unit is not a grapheme cluster. The base and its marks are still tested
// separately, because they do not always answer alike: a joiner is a word
// character only between two characters of the class it joins, and the character
// on a mark's left is the joiner itself, which joins nothing.

bool IsExtendAt(std::string_view text, size_t start)
{
    return CategoryAt(text, start) == Category::Extend;
}

// Start of the unit ending at `end`.
size_t PrevUnit(std::string_view text, size_t end)
{
    size_t start = Utf8::Prev(text, end);
    if (!IsExtendAt(text, start))
        return start;
    while (start > 0 && IsExtendAt(text, Utf8::Prev(text, start)))
        start = Utf8::Prev(text, start);
    return start;
}

// End of the unit starting at `start`.
size_t NextUnit(std::string_view text, size_t start)
{
    size_t end = Utf8::Next(text, start);
    if (!IsExtendAt(text, start))
        return end;
    while (end < text.size() && IsExtendAt(text, end))
        end = Utf8::Next(text, end);
    return end;
}

template <typename Predicate>
ByteRange ExpandRun(std::string_view text, size_t start, Predicate inRun)
{
    ByteRange range{start, Utf8::Next(text, start)};
    while (range.Begin > 0)
    {
        const size_t previous = PrevUnit(text, range.Begin);
        if (!inRun(text, previous))
            break;
        range.Begin = previous;
    }
    while (range.End < text.size() && inRun(text, range.End))
        range.End = NextUnit(text, range.End);
    return range;
}

} // namespace

ByteRange SegmentAt(std::string_view text, size_t boundary)
{
    if (text.empty())
        return {};

    size_t start = boundary >= text.size() ? Utf8::Prev(text, text.size()) : boundary;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
        --start;

    if (IsWordCharAt(text, start))
        return ExpandRun(text, start, IsWordCharAt);
    if (IsWhitespaceAt(text, start))
        return ExpandRun(text, start, IsWhitespaceAt);
    return {start, Utf8::Next(text, start)};
}

ByteRange DoubleClickSelectionAt(std::string_view text, size_t boundary)
{
    ByteRange range = SegmentAt(text, boundary);
    if (range.Begin == range.End || IsWhitespaceAt(text, range.Begin))
        return range;

    // Any non-whitespace segment takes the whitespace run that follows it,
    // punctuation included: Chrome selects ", " from "hello, world".
    while (range.End < text.size() && IsWhitespaceAt(text, range.End))
        range.End = Utf8::Next(text, range.End);
    return range;
}

size_t PrevBoundary(std::string_view text, size_t byteIndex)
{
    if (byteIndex > text.size())
        byteIndex = text.size();
    while (byteIndex > 0)
    {
        const size_t previous = PrevUnit(text, byteIndex);
        if (IsWordCharAt(text, previous))
            break;
        byteIndex = previous;
    }
    while (byteIndex > 0)
    {
        const size_t previous = PrevUnit(text, byteIndex);
        if (!IsWordCharAt(text, previous))
            break;
        byteIndex = previous;
    }
    return byteIndex;
}

size_t NextBoundary(std::string_view text, size_t byteIndex)
{
    if (byteIndex >= text.size())
        return text.size();
    while (byteIndex < text.size() && IsWordCharAt(text, byteIndex))
        byteIndex = NextUnit(text, byteIndex);
    while (byteIndex < text.size() && !IsWordCharAt(text, byteIndex))
        byteIndex = NextUnit(text, byteIndex);
    return byteIndex;
}

} // namespace TextSegmentation
} // namespace GameEngine
