#pragma once

#include "UI/ITextMeasurable.h"
#include "Rendering/Text/FontAtlas.h"
#include <atomic>
#include <vector>

namespace GameEngine {

struct ITextMeasurable::TextShapeCache {
    // One visual line's glyph range and content width. text-align resolves per
    // line, so emission needs each line's own width rather than the block's.
    struct Line {
        float Width = 0.0f;
        size_t GlyphStart = 0;
        size_t GlyphEnd = 0;
        // Baseline in the block's Y-down frame — the space Glyphs[i].y is in.
        // Emission snaps each line against it so the baseline lands on a whole
        // device pixel; a per-glyph round would shred the run instead, because
        // Glyphs[i].y is that glyph's quad top, not the shared baseline.
        float BaselineY = 0.0f;
    };

    uint64_t Key = 0;
    size_t TextLength = 0;
    std::vector<Rendering::Text::FontAtlas::GlyphPlacement> Glyphs;
    std::vector<Line> Lines;
    float TotalWidth = 0.0f;
    // Font line height captured at shape time, so emission lays out against
    // exactly the metrics the shaping used and the parallel drain can build
    // glyph quads from a cache hit without reaching back into the atlas.
    float LineH = 0.0f;
    bool NeedsMultiline = false;
    // Any color (emoji) glyphs present. Worker-side quad building escalates
    // when set: color pages register lazily against an atlas-global content
    // generation, so a cross-item emoji pack during collect can route a full
    // GPU re-upload through a JobSystem worker via EmitGlyphRun.
    bool HasColorGlyphs = false;
    int ActualLineCount = 1;
    // "…" shaped with this cache's font/size/color, for text-overflow:
    // ellipsis on a single-line run. Shaped at cache build (UI thread — the
    // atlas mutates) so a clipped emission on any thread is a pure lookup.
    // Empty when the style did not ask for ellipsis; the ellipsis flag is
    // folded into Key, so a style flip rebuilds the cache.
    std::vector<Rendering::Text::FontAtlas::GlyphPlacement> EllipsisGlyphs;
    float EllipsisWidth = 0.0f;
    // Whether the most recent emission actually truncated the run. Stamped by
    // EmitTextPrimitives — which may run on a JobSystem worker during the
    // drain, hence atomic — and read on the UI thread by the tooltip overlay
    // to offer a truncated label's full text on hover.
    std::atomic<bool> LastRunEllipsized{false};
    // Whether the most recent emission's visible glyphs passed the element's
    // clip box: wrapped or multi-line text taller than it, or a run wider than
    // it that no ellipsis ended inside it. Cut text keeps its own-text clip at
    // the padding box; text that fits lets the clip grow by the run's effect
    // reach. Stamped by EmitTextPrimitives (possibly on a drain worker) and read
    // by the next drain's collect phase on the UI thread, which rewrites the
    // clip slot before any worker emits and must reproduce the same shape.
    std::atomic<bool> LastRunCut{false};
};

} // namespace GameEngine
