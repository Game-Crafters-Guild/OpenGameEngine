#pragma once

#include <cstddef>
#include <string_view>

namespace GameEngine
{
namespace TextSegmentation
{

// Half-open byte range [Begin, End) into the UTF-8 text it was computed from.
struct ByteRange
{
    size_t Begin = 0;
    size_t End = 0;
};

// The segment a caret boundary sits in, under the subset of UAX #29 word
// boundaries documented at the top of TextSegmentation.cpp: a run of word
// characters, a run of whitespace, or a single character of anything else.
// `boundary` is a caret position, so at a junction between two segments this
// reports the one to its right. A boundary inside a multi-byte character is
// snapped back to its lead byte, and one at or past the end reports the final
// segment. Empty text gives {0, 0}.
ByteRange SegmentAt(std::string_view text, size_t boundary);

// Double-click selection at a caret boundary: the segment it sits in, plus the
// run of whitespace that follows unless the segment is itself whitespace. That
// trailing run is what a browser selects — double-clicking "Player" in
// "Player Character" selects "Player ", space included, and clicking the comma
// in "hello, world" selects ", ".
ByteRange DoubleClickSelectionAt(std::string_view text, size_t boundary);

// Ctrl+Left destination: back over any separators, then over the word they
// follow, so the caret lands on the start of the previous word.
size_t PrevBoundary(std::string_view text, size_t byteIndex);

// Ctrl+Right destination: forward over the current word, then over the
// separators that follow, so the caret lands on the start of the next word.
size_t NextBoundary(std::string_view text, size_t byteIndex);

} // namespace TextSegmentation
} // namespace GameEngine
