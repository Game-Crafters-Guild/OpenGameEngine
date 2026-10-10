// Word segmentation against a measured Chrome oracle.
//
// Every expectation below is a value read out of Chrome — measured 2026-07-26
// with real page.mouse.click() / page.mouse.dblclick() calls on a real <input>
// in Chrome/150 on Windows at devicePixelRatio 1, then read back from
// selectionStart/selectionEnd. Nothing here is derived from
// GameEngine::TextSegmentation; deriving the expected value from the code under
// test would only prove the code equals itself.
//
// Where the measurement and UAX #29 disagree, the measurement is the contract:
// the goal is parity with the browser the editor is judged against. The two
// places they disagree are called out at the Mid* joiner tests below.
//
// The measurement separates two questions that the engine also answers
// separately:
//
//   click X   -> caret boundary   (TextField's hit-testing; TextInputGeometryTests)
//   boundary  -> selected range   (this file)
//
// Chrome resolves a click to the *nearest caret boundary*, so every glyph was
// sampled at 25% and 75% of its advance rather than at its midpoint — a
// midpoint sample sits on the tipping point of that rule and cannot falsify it.
// The two columns compose: the same boundary reached from two different glyphs
// always produced the same selection, which is why this file can pin
// boundary -> selection on its own.
//
// One conversion is applied to the raw oracle: Chrome reports offsets in UTF-16
// code units and this engine indexes UTF-8 bytes, so the non-ASCII row has its
// offsets restated in bytes. The conversion is spelled out on that row.
//
// The measured contract, for a boundary B in [0, N]:
//   1. seg(B) is the word segment [s,e) with s <= B < e; B == N uses the last
//      segment. At a junction this picks the segment to the right.
//   2. A whitespace segment is selected unchanged.
//   3. Anything else is selected together with the whitespace run that
//      immediately follows it — punctuation included ("hello, world" boundary 5
//      selects ", ", not ",").

#include "UI/TextSegmentation.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace GameEngine;

namespace
{

// One row of a measured boundary -> selection map. The oracle prints contiguous
// boundaries that share a selection as a range, and this mirrors that layout so
// a row can be diffed against the document line by line. FirstBoundary and
// LastBoundary are both inclusive.
struct BoundaryRow
{
    size_t FirstBoundary;
    size_t LastBoundary;
    size_t Begin;
    size_t End;
    const char* SelectedText;
};

std::string Selected(std::string_view text, TextSegmentation::ByteRange range)
{
    return std::string(text.substr(range.Begin, range.End - range.Begin));
}

template <size_t N>
void ExpectOracleMap(std::string_view value, const char* what, const BoundaryRow (&rows)[N])
{
    for (const BoundaryRow& row : rows)
    {
        for (size_t boundary = row.FirstBoundary; boundary <= row.LastBoundary; ++boundary)
        {
            SCOPED_TRACE(::testing::Message()
                         << "value='" << value << "' (" << what << ") boundary=" << boundary);

            const TextSegmentation::ByteRange selection =
                TextSegmentation::DoubleClickSelectionAt(value, boundary);

            EXPECT_EQ(selection.Begin, row.Begin);
            EXPECT_EQ(selection.End, row.End);
            EXPECT_EQ(Selected(value, selection), row.SelectedText);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Part B of the oracle: the complete boundary -> selection map for each string.
// Every boundary in [0, N] is covered, so a row that moved in either direction
// fails here rather than only at the one offset someone thought to test.
// ---------------------------------------------------------------------------

TEST(TextSegmentationTests, WordsAndTheWhitespaceTrailingThemMatchTheOracle)
{
    constexpr BoundaryRow kPlayerCharacter[] = {
        {0, 5, 0, 7, "Player "},
        {6, 6, 6, 7, " "},
        {7, 16, 7, 16, "Character"},
    };
    ExpectOracleMap("Player Character", "word, space, word", kPlayerCharacter);

    constexpr BoundaryRow kCamelCase[] = {
        {0, 8, 0, 10, "camelCase "},
        {9, 9, 9, 10, " "},
        {10, 14, 10, 14, "word"},
    };
    ExpectOracleMap("camelCase word", "a case change does not split a word", kCamelCase);

    constexpr BoundaryRow kAlphanumeric[] = {
        {0, 3, 0, 5, "a1b2 "},
        {4, 4, 4, 5, " "},
        {5, 6, 5, 6, "c"},
    };
    ExpectOracleMap("a1b2 c", "letters and digits are one segment", kAlphanumeric);

    constexpr BoundaryRow kUnderscore[] = {
        {0, 6, 0, 8, "foo_bar "},
        {7, 7, 7, 8, " "},
        {8, 11, 8, 11, "baz"},
    };
    ExpectOracleMap("foo_bar baz", "'_' joins", kUnderscore);
}

// Punctuation is where the two plausible models diverge, so these carry the
// most oracle weight: each punctuation character is its own segment, it never
// groups into runs, and it still takes the whitespace that follows it.
TEST(TextSegmentationTests, PunctuationSegmentsMatchTheOracle)
{
    constexpr BoundaryRow kComma[] = {
        {0, 4, 0, 5, "hello"},
        {5, 5, 5, 7, ", "},
        {6, 6, 6, 7, " "},
        {7, 12, 7, 12, "world"},
    };
    ExpectOracleMap("hello, world", "a comma takes the space after it", kComma);

    constexpr BoundaryRow kDoubleComma[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 2, ","},
        {2, 2, 2, 3, ","},
        {3, 4, 3, 4, "b"},
    };
    ExpectOracleMap("a,,b", "punctuation does not group into runs", kDoubleComma);

    constexpr BoundaryRow kHyphen[] = {
        {0, 2, 0, 3, "foo"},
        {3, 3, 3, 4, "-"},
        {4, 6, 4, 8, "bar "},
        {7, 7, 7, 8, " "},
        {8, 11, 8, 11, "baz"},
    };
    ExpectOracleMap("foo-bar baz", "'-' splits", kHyphen);

    constexpr BoundaryRow kSlash[] = {
        {0, 3, 0, 4, "path"},
        {4, 4, 4, 5, "/"},
        {5, 6, 5, 7, "to"},
        {7, 7, 7, 8, "/"},
        {8, 12, 8, 12, "file"},
    };
    ExpectOracleMap("path/to/file", "'/' splits", kSlash);
}

// ---------------------------------------------------------------------------
// The Mid* joiners: which characters join, and on which flanks.
//
// Measured 2026-07-26 by the same method, on 44 further strings. Both columns
// were re-measured on a second, independently built harness; every row here
// appeared twice with identical values.
//
// The first harness seeded a full-value selection before every action so a
// click that missed the input would be visible. That guard is blind on exactly
// the rows below whose answer *is* the whole value, and it recorded one wrong
// row ("1·5" as joined) before the guard was fixed to seed a collapsed caret
// before a double-click and a range before a single click — a missed click then
// fails whatever the true answer is. Every row below passed both guards.
//
// Chrome does not implement UAX #29 here, and the two deviations both matter in
// an editor:
//
//   "." (U+002E) is MidNum, not MidNumLet.  WB6/WB7 would join "a.b"; Chrome
//   splits it, which is why a double-click on "Player.fbx" selects the stem.
//   The other MidNumLet dots (U+2024, U+FE52) *do* join letters, so this is
//   FULL STOP specifically, not dots as a class.
//
//   The colons (U+003A, U+FF1A, U+FE55) are not joiners at all. WB6/WB7 would
//   join "a:b"; Chrome splits it, and splits "1:5" and "12:30" too. The rest of
//   MidLetter (U+00B7, U+0387, U+2027, U+FE13) does join letters, so the class
//   is not empty.
//
// Parity with the browser is the goal, so where the measurement and the spec
// disagree these rows are the contract. Do not "correct" them back to UAX #29.
// ---------------------------------------------------------------------------

// The joiner decision reads the two characters immediately flanking it, not the
// composition of the runs either side: every string here has a letter in the
// run on one or both sides of a digit-flanked ".", and Chrome joins all of them.
TEST(TextSegmentationTests, AFullStopJoinsOnlyBetweenDigitsPerTheOracle)
{
    constexpr BoundaryRow kAssetName[] = {
        {0, 5, 0, 6, "Player"},
        {6, 6, 6, 7, "."},
        {7, 10, 7, 10, "fbx"},
    };
    ExpectOracleMap("Player.fbx", "'.' between letters splits", kAssetName);

    constexpr BoundaryRow kDottedWords[] = {
        {0, 2, 0, 3, "foo"},
        {3, 3, 3, 4, "."},
        {4, 7, 4, 7, "bar"},
    };
    ExpectOracleMap("foo.bar", "'.' between letters splits", kDottedWords);

    constexpr BoundaryRow kMinimalLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 2, "."},
        {2, 3, 2, 3, "b"},
    };
    ExpectOracleMap("a.b", "'.' between letters splits", kMinimalLetters);

    constexpr BoundaryRow kDecimal[] = {
        {0, 3, 0, 3, "1.5"},
    };
    ExpectOracleMap("1.5", "'.' between digits joins", kDecimal);

    constexpr BoundaryRow kVersion[] = {
        {0, 6, 0, 6, "v1.2.3"},
    };
    ExpectOracleMap("v1.2.3", "digit-flanked at both dots, letter in the run", kVersion);

    constexpr BoundaryRow kLettersAroundADecimal[] = {
        {0, 5, 0, 5, "x1.5y"},
    };
    ExpectOracleMap("x1.5y", "digit-flanked, letters at both outer ends", kLettersAroundADecimal);

    constexpr BoundaryRow kLettersOutsideDigits[] = {
        {0, 5, 0, 5, "a1.2b"},
    };
    ExpectOracleMap("a1.2b", "digit-flanked, mixed runs", kLettersOutsideDigits);

    // The same string with the flanks swapped to digit/letter. Nothing about the
    // runs changed; the flanking pair did, and Chrome splits.
    constexpr BoundaryRow kMixedFlanks[] = {
        {0, 1, 0, 2, "a1"},
        {2, 2, 2, 3, "."},
        {3, 5, 3, 5, "b2"},
    };
    ExpectOracleMap("a1.b2", "letter on the right flank splits", kMixedFlanks);

    constexpr BoundaryRow kLetterThenDigit[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 2, "."},
        {2, 3, 2, 3, "1"},
    };
    ExpectOracleMap("a.1", "letter on the left flank splits", kLetterThenDigit);

    constexpr BoundaryRow kDecimalThenLetter[] = {
        {0, 4, 0, 4, "1.5a"},
    };
    ExpectOracleMap("1.5a", "digit-flanked with a trailing letter", kDecimalThenLetter);

    constexpr BoundaryRow kDottedWordsThenWord[] = {
        {0, 1, 0, 2, "ab"},
        {2, 2, 2, 3, "."},
        {3, 4, 3, 6, "cd "},
        {5, 5, 5, 6, " "},
        {6, 8, 6, 8, "ef"},
    };
    ExpectOracleMap("ab.cd ef", "the split word still takes its trailing space",
                    kDottedWordsThenWord);
}

// A colon joins nothing, on any flanks. UAX #29 puts it in MidLetter; a
// file path, a URL scheme and a clock time all read better split, and Chrome
// splits all three.
TEST(TextSegmentationTests, AColonNeverJoinsPerTheOracle)
{
    constexpr BoundaryRow kLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 2, ":"},
        {2, 3, 2, 3, "b"},
    };
    ExpectOracleMap("a:b", "':' between letters splits", kLetters);

    constexpr BoundaryRow kDigits[] = {
        {0, 0, 0, 1, "1"},
        {1, 1, 1, 2, ":"},
        {2, 3, 2, 3, "5"},
    };
    ExpectOracleMap("1:5", "':' between digits splits too", kDigits);

    constexpr BoundaryRow kClock[] = {
        {0, 1, 0, 2, "12"},
        {2, 2, 2, 3, ":"},
        {3, 5, 3, 5, "30"},
    };
    ExpectOracleMap("12:30", "a clock time is two words", kClock);

    constexpr BoundaryRow kUrl[] = {
        {0, 3, 0, 4, "http"},
        {4, 4, 4, 5, ":"},
        {5, 5, 5, 6, "/"},
        {6, 6, 6, 7, "/"},
        {7, 7, 7, 8, "x"},
        {8, 8, 8, 9, "."},
        {9, 10, 9, 10, "y"},
    };
    ExpectOracleMap("http://x.y", "every separator in a URL is its own segment", kUrl);
}

// The apostrophe is the one joiner that takes both flanks, which is what keeps
// "don't" a single word.
TEST(TextSegmentationTests, AnApostropheJoinsBothLettersAndDigitsPerTheOracle)
{
    constexpr BoundaryRow kContraction[] = {
        {0, 5, 0, 5, "don't"},
    };
    ExpectOracleMap("don't", "'\\'' between letters joins", kContraction);

    constexpr BoundaryRow kLetters[] = {
        {0, 3, 0, 3, "a'b"},
    };
    ExpectOracleMap("a'b", "'\\'' between letters joins", kLetters);

    constexpr BoundaryRow kDigits[] = {
        {0, 3, 0, 3, "1'5"},
    };
    ExpectOracleMap("1'5", "'\\'' between digits joins", kDigits);
}

// Comma and semicolon are the two joiners whose measured behaviour matches
// UAX #29 exactly, and they are the counterexample that keeps the full stop's
// deviation from being restated as "no ASCII punctuation joins digits".
TEST(TextSegmentationTests, ACommaOrSemicolonJoinsOnlyBetweenDigitsPerTheOracle)
{
    constexpr BoundaryRow kCommaDigits[] = {
        {0, 3, 0, 3, "1,5"},
    };
    ExpectOracleMap("1,5", "',' between digits joins", kCommaDigits);

    constexpr BoundaryRow kCommaLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 2, ","},
        {2, 3, 2, 3, "b"},
    };
    ExpectOracleMap("a,b", "',' between letters splits", kCommaLetters);

    constexpr BoundaryRow kSemicolonDigits[] = {
        {0, 3, 0, 3, "1;5"},
    };
    ExpectOracleMap("1;5", "';' between digits joins", kSemicolonDigits);

    constexpr BoundaryRow kSemicolonLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 2, ";"},
        {2, 3, 2, 3, "b"},
    };
    ExpectOracleMap("a;b", "';' between letters splits", kSemicolonLetters);
}

// One row per non-ASCII code point the joiner tables classify, so no entry sits
// in a class on the strength of the ASCII character it resembles.
//
// Each value is one ASCII character, the joiner, one ASCII character. Chrome
// reported UTF-16 offsets 0..3; the joiner is 2 or 3 UTF-8 bytes, so the
// boundaries restate as 0, 1, 1+n, 2+n and the interior bytes are not
// boundaries a click can produce. Rows are written in bytes.
TEST(TextSegmentationTests, NonAsciiJoinersMatchTheOracle)
{
    // U+00B7 MIDDLE DOT (2 bytes) — MidLetter: joins letters, splits digits.
    constexpr BoundaryRow kMiddleDotLetters[] = {
        {0, 1, 0, 4, "a\xC2\xB7" "b"},
        {3, 4, 0, 4, "a\xC2\xB7" "b"},
    };
    ExpectOracleMap("a\xC2\xB7" "b", "U+00B7 between letters joins", kMiddleDotLetters);

    constexpr BoundaryRow kMiddleDotDigits[] = {
        {0, 0, 0, 1, "1"},
        {1, 1, 1, 3, "\xC2\xB7"},
        {3, 4, 3, 4, "5"},
    };
    ExpectOracleMap("1\xC2\xB7" "5", "U+00B7 between digits splits", kMiddleDotDigits);

    // U+0387 GREEK ANO TELEIA (2 bytes) — MidLetter.
    constexpr BoundaryRow kAnoTeleiaLetters[] = {
        {0, 1, 0, 4, "a\xCE\x87" "b"},
        {3, 4, 0, 4, "a\xCE\x87" "b"},
    };
    ExpectOracleMap("a\xCE\x87" "b", "U+0387 between letters joins", kAnoTeleiaLetters);

    // Not a digit joiner, and as punctuation it takes the whitespace after it.
    constexpr BoundaryRow kAnoTeleiaDigits[] = {
        {0, 0, 0, 1, "1"},
        {1, 1, 1, 4, "\xCE\x87" " "},
        {3, 3, 3, 4, " "},
        {4, 5, 4, 5, "5"},
    };
    ExpectOracleMap("1\xCE\x87" " 5", "U+0387 between digits splits", kAnoTeleiaDigits);

    // U+2027 HYPHENATION POINT (3 bytes) — MidLetter.
    constexpr BoundaryRow kHyphenationPoint[] = {
        {0, 1, 0, 5, "a\xE2\x80\xA7" "b"},
        {4, 5, 0, 5, "a\xE2\x80\xA7" "b"},
    };
    ExpectOracleMap("a\xE2\x80\xA7" "b", "U+2027 between letters joins", kHyphenationPoint);

    // U+FE13 PRESENTATION FORM FOR VERTICAL COLON (3 bytes) — MidLetter despite
    // being a colon: it joins letters, where U+003A does not.
    constexpr BoundaryRow kVerticalColonLetters[] = {
        {0, 1, 0, 5, "a\xEF\xB8\x93" "b"},
        {4, 5, 0, 5, "a\xEF\xB8\x93" "b"},
    };
    ExpectOracleMap("a\xEF\xB8\x93" "b", "U+FE13 between letters joins", kVerticalColonLetters);

    constexpr BoundaryRow kVerticalColonDigits[] = {
        {0, 0, 0, 1, "1"},
        {1, 1, 1, 4, "\xEF\xB8\x93"},
        {4, 5, 4, 5, "5"},
    };
    ExpectOracleMap("1\xEF\xB8\x93" "5", "U+FE13 between digits splits", kVerticalColonDigits);

    // U+FF1A FULLWIDTH COLON and U+FE55 SMALL COLON (3 bytes) — like U+003A,
    // joiners of nothing.
    constexpr BoundaryRow kFullwidthColonLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 4, "\xEF\xBC\x9A"},
        {4, 5, 4, 5, "b"},
    };
    ExpectOracleMap("a\xEF\xBC\x9A" "b", "U+FF1A between letters splits", kFullwidthColonLetters);

    constexpr BoundaryRow kSmallColonLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 4, "\xEF\xB9\x95"},
        {4, 5, 4, 5, "b"},
    };
    ExpectOracleMap("a\xEF\xB9\x95" "b", "U+FE55 between letters splits", kSmallColonLetters);

    constexpr BoundaryRow kSmallColonDigits[] = {
        {0, 0, 0, 1, "1"},
        {1, 1, 1, 4, "\xEF\xB9\x95"},
        {4, 5, 4, 5, "5"},
    };
    ExpectOracleMap("1\xEF\xB9\x95" "5", "U+FE55 between digits splits", kSmallColonDigits);

    // U+FF0E FULLWIDTH FULL STOP (3 bytes) — MidNum, exactly like U+002E.
    constexpr BoundaryRow kFullwidthStopLetters[] = {
        {0, 0, 0, 1, "a"},
        {1, 1, 1, 4, "\xEF\xBC\x8E"},
        {4, 5, 4, 5, "b"},
    };
    ExpectOracleMap("a\xEF\xBC\x8E" "b", "U+FF0E between letters splits", kFullwidthStopLetters);

    constexpr BoundaryRow kFullwidthStopDigits[] = {
        {0, 1, 0, 5, "1\xEF\xBC\x8E" "5"},
        {4, 5, 0, 5, "1\xEF\xBC\x8E" "5"},
    };
    ExpectOracleMap("1\xEF\xBC\x8E" "5", "U+FF0E between digits joins", kFullwidthStopDigits);

    // U+2024 ONE DOT LEADER and U+FE52 SMALL FULL STOP (3 bytes) — genuine
    // MidNumLet: unlike U+002E they join letters as well as digits.
    constexpr BoundaryRow kOneDotLeaderLetters[] = {
        {0, 1, 0, 5, "a\xE2\x80\xA4" "b"},
        {4, 5, 0, 5, "a\xE2\x80\xA4" "b"},
    };
    ExpectOracleMap("a\xE2\x80\xA4" "b", "U+2024 between letters joins", kOneDotLeaderLetters);

    constexpr BoundaryRow kOneDotLeaderDigits[] = {
        {0, 1, 0, 5, "1\xE2\x80\xA4" "5"},
        {4, 5, 0, 5, "1\xE2\x80\xA4" "5"},
    };
    ExpectOracleMap("1\xE2\x80\xA4" "5", "U+2024 between digits joins", kOneDotLeaderDigits);

    constexpr BoundaryRow kSmallStopLetters[] = {
        {0, 1, 0, 5, "a\xEF\xB9\x92" "b"},
        {4, 5, 0, 5, "a\xEF\xB9\x92" "b"},
    };
    ExpectOracleMap("a\xEF\xB9\x92" "b", "U+FE52 between letters joins", kSmallStopLetters);

    constexpr BoundaryRow kSmallStopDigits[] = {
        {0, 1, 0, 5, "1\xEF\xB9\x92" "5"},
        {4, 5, 0, 5, "1\xEF\xB9\x92" "5"},
    };
    ExpectOracleMap("1\xEF\xB9\x92" "5", "U+FE52 between digits joins", kSmallStopDigits);

    // U+FF0C FULLWIDTH COMMA (3 bytes) — MidNum, like U+002C.
    constexpr BoundaryRow kFullwidthCommaDigits[] = {
        {0, 1, 0, 5, "1\xEF\xBC\x8C" "5"},
        {4, 5, 0, 5, "1\xEF\xBC\x8C" "5"},
    };
    ExpectOracleMap("1\xEF\xBC\x8C" "5", "U+FF0C between digits joins", kFullwidthCommaDigits);

    // The quotes — U+2018, U+2019 (3 bytes), U+FF07 (3 bytes) — are MidNumLet
    // with U+0027. U+2019 is what a text field holds after autocorrect.
    constexpr BoundaryRow kSmartContraction[] = {
        {0, 3, 0, 7, "don\xE2\x80\x99t"},
        {6, 7, 0, 7, "don\xE2\x80\x99t"},
    };
    ExpectOracleMap("don\xE2\x80\x99t", "U+2019 between letters joins", kSmartContraction);

    constexpr BoundaryRow kLeftQuote[] = {
        {0, 1, 0, 5, "a\xE2\x80\x98" "b"},
        {4, 5, 0, 5, "a\xE2\x80\x98" "b"},
    };
    ExpectOracleMap("a\xE2\x80\x98" "b", "U+2018 between letters joins", kLeftQuote);

    constexpr BoundaryRow kFullwidthApostrophe[] = {
        {0, 1, 0, 5, "a\xEF\xBC\x87" "b"},
        {4, 5, 0, 5, "a\xEF\xBC\x87" "b"},
    };
    ExpectOracleMap("a\xEF\xBC\x87" "b", "U+FF07 between letters joins", kFullwidthApostrophe);
}

// The inspector's float fields live here: "." between digits has to join, and
// a leading sign has to not.
TEST(TextSegmentationTests, NumbersMatchTheOracle)
{
    constexpr BoundaryRow kDecimals[] = {
        {0, 2, 0, 4, "1.5 "},
        {3, 3, 3, 4, " "},
        {4, 7, 4, 7, "2.5"},
    };
    ExpectOracleMap("1.5 2.5", "'.' between digits joins", kDecimals);

    constexpr BoundaryRow kNegative[] = {
        {0, 0, 0, 1, "-"},
        {1, 4, 1, 4, "1.5"},
    };
    ExpectOracleMap("-1.5", "the sign is not part of the number", kNegative);

    constexpr BoundaryRow kNegativeInline[] = {
        {0, 0, 0, 2, "x "},
        {1, 1, 1, 2, " "},
        {2, 2, 2, 3, "-"},
        {3, 5, 3, 7, "1.5 "},
        {6, 6, 6, 7, " "},
        {7, 8, 7, 8, "y"},
    };
    ExpectOracleMap("x -1.5 y", "a sign with no space after it takes nothing", kNegativeInline);
}

// A whitespace run of any length is one segment, and a boundary inside the run
// still yields the whole run.
TEST(TextSegmentationTests, WhitespaceRunsMatchTheOracle)
{
    constexpr BoundaryRow kInteriorRun[] = {
        {0, 0, 0, 3, "a  "},
        {1, 2, 1, 3, "  "},
        {3, 4, 3, 4, "b"},
    };
    ExpectOracleMap("a  b", "a two-space run", kInteriorRun);

    constexpr BoundaryRow kTrailingRun[] = {
        {0, 2, 0, 5, "foo  "},
        {3, 5, 3, 5, "  "},
    };
    ExpectOracleMap("foo  ", "whitespace at the end of the text", kTrailingRun);

    constexpr BoundaryRow kLeadingRun[] = {
        {0, 0, 0, 1, " "},
        {1, 3, 1, 5, "foo "},
        {4, 4, 4, 5, " "},
        {5, 8, 5, 8, "bar"},
    };
    ExpectOracleMap(" foo bar", "whitespace at the start of the text", kLeadingRun);
}

// The typographic spaces, U+2000 EN QUAD through U+200A HAIR SPACE. Measured
// 2026-07-26 by the same method as the joiner rows — a real page.mouse.dblclick()
// on a real <input> in Chrome/150 at devicePixelRatio 1 — at 96px, where even a
// hair space is 8 px wide and both sample points land inside it. Ten of the
// eleven grouped into runs exactly as U+0020 does; U+2007 FIGURE SPACE did not,
// and the note below says what it does instead.
//
// Each value is "a", the space twice, "b". The space is 3 UTF-8 bytes, so
// Chrome's u16 offsets 0..4 restate as bytes 0, 1, 4, 7, 8; the interior bytes
// are not boundaries a click can produce and have no row here.
//
// U+2007 FIGURE SPACE sits inside this range and is the one member that does not
// belong to it: Chrome makes each figure space its own segment. It stays in the
// range, because U+0009 and U+00A0 measure the same way and are whitespace here
// too — the file header records the whole of that divergence.
TEST(TextSegmentationTests, TypographicSpacesGroupAsWhitespacePerTheOracle)
{
    constexpr BoundaryRow kEnQuad[] = {
        {0, 0, 0, 7, "a\xE2\x80\x80\xE2\x80\x80"},
        {1, 1, 1, 7, "\xE2\x80\x80\xE2\x80\x80"},
        {4, 4, 1, 7, "\xE2\x80\x80\xE2\x80\x80"},
        {7, 8, 7, 8, "b"},
    };
    ExpectOracleMap("a\xE2\x80\x80\xE2\x80\x80" "b", "U+2000 groups as whitespace", kEnQuad);

    constexpr BoundaryRow kHairSpace[] = {
        {0, 0, 0, 7, "a\xE2\x80\x8A\xE2\x80\x8A"},
        {1, 1, 1, 7, "\xE2\x80\x8A\xE2\x80\x8A"},
        {4, 4, 1, 7, "\xE2\x80\x8A\xE2\x80\x8A"},
        {7, 8, 7, 8, "b"},
    };
    ExpectOracleMap("a\xE2\x80\x8A\xE2\x80\x8A" "b", "U+200A groups as whitespace", kHairSpace);
}

// Chrome reported this map in UTF-16 code units; the engine indexes UTF-8
// bytes. The i-diaeresis (u16 2) and the e-acute (u16 9) are two bytes each, so
// the u16 -> u8 boundary map the oracle measured with TextEncoder is
//   u16  0 1 2 3 4 5 6 7 8 9 10
//   u8   0 1 2 4 5 6 7 8 9 10 12
// The two selections [0,6] and [6,10] restate as [0,7] and [7,12]. Byte 3 and
// byte 11 are inside a character and are not boundaries a click can produce, so
// the oracle has no row for them and neither does this table.
TEST(TextSegmentationTests, AccentedLettersAreWordCharactersPerTheOracle)
{
    constexpr BoundaryRow kAccented[] = {
        {0, 2, 0, 7, "na" "\xC3\xAF" "ve "},
        {4, 5, 0, 7, "na" "\xC3\xAF" "ve "},
        {6, 6, 6, 7, " "},
        {7, 10, 7, 12, "caf" "\xC3\xA9"},
        {12, 12, 7, 12, "caf" "\xC3\xA9"},
    };
    ExpectOracleMap("na" "\xC3\xAF" "ve caf" "\xC3\xA9", "precomposed accents", kAccented);
}

// The same text written with combining marks instead of precomposed characters:
// n a i U+0308 v e SPACE c a f e U+0301. Chrome measured it as the identical
// selection map, which is the WB4 rule — a combining mark takes the class of the
// character it follows, so the marks are word characters here.
//
//   u16  0 1 2 3 4 5 6 7 8 9 10 11 12
//   u8   0 1 2 3 5 6 7 8 9 10 11 12 14
//
// Chrome's [0,7] and [7,12] restate as [0,8] and [8,14]. Bytes 3-4 and 12-13
// are inside a grapheme cluster; the oracle records that Chrome's caret never
// stops there, so they are not boundaries and this table has no row for them.
// TextInputGeometryTests pins that the engine's hit-testing does not produce
// them either.
TEST(TextSegmentationTests, CombiningMarksAreWordCharactersPerTheOracle)
{
    constexpr BoundaryRow kCombining[] = {
        {0, 2, 0, 8, "nai\xCC\x88ve "},
        {5, 6, 0, 8, "nai\xCC\x88ve "},
        {7, 7, 7, 8, " "},
        {8, 11, 8, 14, "cafe\xCC\x81"},
        {14, 14, 8, 14, "cafe\xCC\x81"},
    };
    ExpectOracleMap("nai\xCC\x88ve cafe\xCC\x81", "decomposed accents", kCombining);
}

// ---------------------------------------------------------------------------
// Defensive behaviour outside the oracle's domain. A click can only ever
// produce a boundary in [0, N], so these pin that a caller who hands over
// something else gets a sane answer rather than a read past the end.
// ---------------------------------------------------------------------------

TEST(TextSegmentationTests, IndicesOutsideTheTextClampToTheEnds)
{
    const std::string_view value = "hello world";

    const TextSegmentation::ByteRange pastEnd = TextSegmentation::DoubleClickSelectionAt(value, 999);
    EXPECT_EQ(pastEnd.Begin, 6u);
    EXPECT_EQ(pastEnd.End, 11u);

    const TextSegmentation::ByteRange empty = TextSegmentation::DoubleClickSelectionAt("", 0);
    EXPECT_EQ(empty.Begin, 0u);
    EXPECT_EQ(empty.End, 0u);
}

// A byte index inside a multi-byte character names that character, so a caller
// that computed one cannot split a character in half.
TEST(TextSegmentationTests, IndexInsideAMultiByteCharacterNamesThatCharacter)
{
    const std::string_view value = "na" "\xC3\xAF" "ve";

    for (size_t byteIndex = 0; byteIndex < value.size(); ++byteIndex)
    {
        SCOPED_TRACE(::testing::Message() << "byteIndex=" << byteIndex);
        const TextSegmentation::ByteRange selection = TextSegmentation::DoubleClickSelectionAt(value, byteIndex);
        EXPECT_EQ(selection.Begin, 0u);
        EXPECT_EQ(selection.End, value.size());
    }
}

// ---------------------------------------------------------------------------
// Ctrl+Left / Ctrl+Right share the word predicate with double-click, so the
// oracle's segment rules have to hold for them too.
// ---------------------------------------------------------------------------

TEST(TextSegmentationTests, WordJumpTreatsADecimalNumberAsOneWord)
{
    const std::string_view value = "1.5 2.5";

    EXPECT_EQ(TextSegmentation::NextBoundary(value, 0), 4u);
    EXPECT_EQ(TextSegmentation::NextBoundary(value, 4), 7u);
    EXPECT_EQ(TextSegmentation::PrevBoundary(value, 7), 4u);
    EXPECT_EQ(TextSegmentation::PrevBoundary(value, 4), 0u);
}

// The byte-wise isalnum predicate this replaced classified every byte of a
// non-ASCII character as a separator, so Ctrl+Left stopped inside words.
TEST(TextSegmentationTests, WordJumpDoesNotStopInsideAnAccentedWord)
{
    const std::string_view value = "na" "\xC3\xAF" "ve caf" "\xC3\xA9";

    EXPECT_EQ(TextSegmentation::NextBoundary(value, 0), 7u);
    EXPECT_EQ(TextSegmentation::PrevBoundary(value, value.size()), 7u);
}
