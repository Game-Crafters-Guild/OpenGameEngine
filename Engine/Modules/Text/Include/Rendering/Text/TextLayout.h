#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "Rendering/Text/FontAtlas.h"

namespace GameEngine { namespace Rendering { namespace Text {

struct StyledRun {
    const FontAtlas* Font = nullptr;   // font/face provider (per-run)
    std::string Text;                     // UTF-8
    // Per-run device size (font size x content scale), deliberately fractional
    // — the line box rounds at this size. See FontAtlas::GetLineMetrics.
    float PixelSize = 0.0f;
    float Color[4] = {1,1,1,1};           // optional color (renderer uses it)
    // CSS letter-spacing in device px, applied after every cluster including
    // the run's last (see FontAtlas::MeasureUtf8) — the trailing gap is what
    // makes consecutive runs compose into the whole line, matching Chrome.
    float LetterSpacingPx = 0.0f;
    // Opt-in perf path for digit-heavy, simple ASCII overlays (FPS/perf counters).
    // When true, text shaping may bypass HarfBuzz in favor of a simple per-codepoint advance path.
    bool PreferFastAscii = false;
};

class TextLayout {
public:
    enum class HorizontalAlign { Left, Center, Right };
    enum class VerticalAlign { Baseline, Top, Middle, Bottom, InkMiddle };
    enum class WordBreak { Normal, BreakAll, KeepAll, BreakWord };

    struct LineBreakInfo {
        size_t ByteStart;
        size_t ByteEnd;
        // Advance width of the line's content, excluding whitespace that hangs
        // past a soft wrap. Per-line alignment offsets are derived from this,
        // so it must not count the space the breaker consumed.
        float Width = 0.0f;
        // Half-open range into MultilineShapeResult::Glyphs for this line.
        size_t GlyphStart = 0;
        size_t GlyphEnd = 0;
        // Baseline of this line in the block's Y-down frame — the space
        // Glyphs[i].y lives in. Emission snaps against it to put the baseline
        // on a whole device pixel; the line's Y is not otherwise recoverable,
        // since each glyph carries its own quad top rather than the baseline.
        float BaselineY = 0.0f;
    };

    // Multiline glyph placement result for the UI renderer.
    struct MultilineShapeResult
    {
        std::vector<FontAtlas::GlyphPlacement> Glyphs;
        TextMetrics Metrics;       // overall metrics (max width, total height)
        bool AtlasChanged = false;
        size_t LineCount = 0;
        std::vector<LineBreakInfo> LineBreaks;

        void Clear()
        {
            Glyphs.clear();
            Metrics = {};
            AtlasChanged = false;
            LineCount = 0;
            LineBreaks.clear();
        }
    };

    // Resolve a CSS line-height to a physical-pixel line box height.
    // cssLineHeight uses the parser's sign encoding: < 0 is a unitless
    // multiplier (resolves against the font SIZE, per CSS — not the font's
    // metric height), > 0 is a length in logical px, 0 is "normal"/unset.
    // Returns 0 for "normal" — ShapeMultiline then uses the font's exact
    // metric height per line.
    static float ResolveLineBoxPx(float cssLineHeight, float fontSizeLogical, float contentScale)
    {
        if (cssLineHeight < 0.0f)
            return -cssLineHeight * fontSizeLogical * contentScale;
        if (cssLineHeight > 0.0f)
            return cssLineHeight * contentScale;
        return 0.0f;
    }

    // Half-leading: CSS centres the font's ascent/descent box inside the line
    // box, so each edge takes half the difference. Negative when the declared
    // line-height is tighter than the font's own height — CSS lifts the ink
    // rather than pinning it to the line-box top. lineBoxPx <= 0 is "normal",
    // where the box IS the content height and the leading is zero. Physical px.
    //
    // Floored, because a line box one odd pixel taller than the font leaves
    // half a pixel of leading and CSS does not put a baseline there: Blink
    // takes floor(ascent + halfLeading) (FontHeight::AddLeading) and gives the
    // odd pixel to the descent. Flooring the leading is the same placement
    // because this engine's ascent is already whole — ExactLineMetricsWithFace
    // rounds ascent, descent and gap to whole physical px before summing — and
    // it keeps the leading itself integral, which is what lets two items on a
    // baseline row round to the SAME device pixel instead of straddling one.
    static float HalfLeadingPx(float contentHeightPx, float lineBoxPx)
    {
        const float box = (lineBoxPx > 0.0f) ? lineBoxPx : contentHeightPx;
        return std::floor((box - contentHeightPx) * 0.5f);
    }

    // Distance from the top of a text element's CONTENT box to the alphabetic
    // baseline of its FIRST line — the value `align-items: baseline` aligns on
    // (`last baseline` is a separate keyword this engine does not support).
    // Physical px, as are contentHeightPx and lineBoxPx.
    //
    // A single line is centred in the content box and a block of them is
    // top-aligned, matching where UIManager_PrimitiveGen puts the glyphs. Both
    // branches then place the baseline inside that line box the same way, so
    // the leading term is shared rather than folded into the centring: with a
    // floored leading the two no longer cancel, and a text leaf whose content
    // box IS its line box must still land on floor(halfLeading) + ascent.
    static float FirstBaselineInContentBoxPx(float contentHeightPx,
                                             const LineMetrics& fontMetrics,
                                             float lineBoxPx,
                                             int lineCount)
    {
        const float fontHeightPx = std::max(1.0f, fontMetrics.height);
        const float boxPx = (lineBoxPx > 0.0f) ? lineBoxPx : fontHeightPx;
        const float lineBoxTop = (lineCount <= 1) ? (contentHeightPx - boxPx) * 0.5f : 0.0f;
        return lineBoxTop + HalfLeadingPx(fontHeightPx, lineBoxPx) + fontMetrics.ascender;
    }

    // Shape multiline text into glyph placements (Y-down, top-left origin).
    // Handles explicit '\n' line breaks and optional width-based word wrapping.
    // Each GlyphPlacement.color is stamped from the corresponding StyledRun.
    // lineBoxPx is the CSS line box height in physical px (see
    // ResolveLineBoxPx); <= 0 means "normal": each line's box is the font's
    // metric height. Half-leading centres the ascent/descent box inside the
    // line box and may be negative when the box is tighter than the font.
    static MultilineShapeResult ShapeMultiline(std::span<const StyledRun> runs,
                                               float wrapWidth = 0.0f,
                                               float lineBoxPx = 0.0f,
                                               WordBreak wordBreak = WordBreak::Normal);
};

}}} // namespace

