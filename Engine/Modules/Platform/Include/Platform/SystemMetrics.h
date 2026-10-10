#pragma once

#include <chrono>
#include <optional>

namespace GameEngine::Platform
{

// Operating-system interaction metrics: the user's configured input and
// accessibility timings, read from the OS instead of guessed.
//
// UNIT CONTRACT
// Every accessor in this header is TIME-valued and returns
// std::chrono::milliseconds. A duration carries no pixel space, so no DPI or
// content-scale conversion is applied to these, and none should be.
// A pixel-valued metric added here later does NOT share that property — the
// obvious candidate is the SM_CXDOUBLECLK / SM_CYDOUBLECLK double-click
// rectangle, which the OS reports in DEVICE pixels at the SYSTEM DPI, while
// UI code compares against CSS-logical pixels further divided by a
// user-controlled content scale. Such an accessor must name the space it
// returns as part of its own contract, and the conversion into UI space must
// happen at exactly one place rather than being repeated per call site.
//
// API FAMILY
// Both metrics below are read with the plain, non-DPI-aware Win32 getters.
// That is a per-metric decision, not a house default: the ...ForDpi family
// exists to rescale metrics that have PIXEL dimensions, so routing a duration
// through it would be the same category error as routing the unitless
// SPI_GETWHEELSCROLLLINES wheel-line count through SystemParametersInfoForDpi.

// Interval between two successive toggles of the text caret: how long it stays
// visible, and then how long it stays hidden. This is a HALF-period — a full
// on-then-off cycle takes twice as long.
//
// std::nullopt means "never blink", i.e. the caret is drawn solid. That is a
// real accessibility setting rather than an error case, so it stays
// representable instead of being folded into some very large duration.
//
// Windows: GetCaretBlinkTime, documented as "the time required to invert the
//          caret's pixels", returning INFINITE when the caret does not blink.
// macOS:   the NSTextInsertionPointBlinkPeriodOn / ...Off user defaults.
// Linux:   the documented GTK default; see the Linux implementation for why it
//          is not queried live.
std::optional<std::chrono::milliseconds> GetCaretBlinkHalfPeriod();

// Longest gap between two clicks for which the second still counts as
// completing a double-click.
//
// Windows: GetDoubleClickTime, documented as returning the current
//          double-click time in milliseconds (capped by the OS at 5000).
// macOS:   NSEvent.doubleClickInterval.
// Linux:   the documented GTK default; see the Linux implementation.
std::chrono::milliseconds GetDoubleClickInterval();

} // namespace GameEngine::Platform
