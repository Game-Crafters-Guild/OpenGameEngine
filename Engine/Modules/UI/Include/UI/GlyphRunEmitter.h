#pragma once

#include "Rendering/Text/FontAtlas.h"
#include "UI/UIPrimitive.h"
#include "UI/TextEffects.h"
#include "UI/UITextureRegistry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine {
namespace UI {

// Converts authored ARGB colours and logical lengths to packed RGBA8 colours and
// device pixels at `scale`. Widths, radii and the shadow blur clamp to 64 device
// pixels, shadow offsets to +-256 device pixels, and a non-finite length is zero.
[[nodiscard]] TextEffects ResolveTextEffects(const TextEffects& authored, float scale);

// How far, in device pixels, the visible effects of `resolved` (the output of
// ResolveTextEffects) paint past a vector glyph's box: an outline its width
// plus the one-pixel ramp; a glow the outline width plus twice its radius; a
// shadow its larger offset plus the outline width, 1.5 x its blur and the
// one-pixel ramp. Zero without a visible effect. The quantity, from the same
// constants, that uiTextEffectReachPx (Shaders/UI/ui_sdf_common.glsl) widens
// each glyph quad by; a change to one is a change to both.
[[nodiscard]] float TextEffectReachPx(const TextEffects& resolved);

// Where an emitted glyph run lands: the primitive sink plus the shared state
// every text surface already carries at its emission point.
struct GlyphRunTarget
{
    std::vector<UIPrimitive>* Primitives = nullptr;
    UITextureRegistry* Textures = nullptr;
    Rendering::Text::FontAtlas* Font = nullptr;
    // One entry per Slug page of Font, indexed by GlyphPlacement::pageIndex.
    // Empty is legal (color-only fonts, or no texture registry).
    const std::vector<UITextureRegistry::SlugTextureIndices>* SlugPages = nullptr;
    uint16_t ClipIndex = kNoClip;
    float Opacity = 1.0f;
    // Resolved (device) effects of the run; EmitGlyphRun draws them as effect
    // instances, layer by layer, under the run's fills.
    TextEffects Effects;
};

// Origin that puts a run's baseline on a whole device pixel.
//
// `originY` is the run origin the caller would otherwise have used, and
// `baselineY` is the offset from it to the run's baseline, measured in the same
// frame as the placements' `gp.y`. Only their SUM has to be the true on-screen
// baseline — `baselineY` itself is not required to be integral, and for the
// dominant caller it is not:
//   - callers emitting one shaped run (PrimitiveEmitContext::EmitText, TextArea,
//     DiffPanel, ScriptTextArea) pass the shaped ascender, which
//     ExactLineMetricsWithFace floors to an integer;
//   - UIManager::EmitTextPrimitives — the path every styled text element takes —
//     passes TextShapeCache::Line::BaselineY, which ShapeMultiline builds as
//     `lineTop + halfLeading + maxAscender`. The last two are whole numbers by
//     construction — TextLayout::HalfLeadingPx floors, and
//     ExactLineMetricsWithFace rounds the ascender — but `lineTop` is a multiple
//     of the line box, which is a CSS length scaled by the content scale and
//     routinely fractional: line-height 27px at content scale 1.25 puts line 1's
//     top at 33.75. That one term is enough to need the snap.
//
// Snapping the ORIGIN is the whole point. Never round `originY + gp.y` per
// glyph: `gp.y` is the glyph QUAD TOP (`baseline - EmYMax * emScale`,
// FontAtlas.cpp:456), which differs for every glyph in the run, so rounding it
// per glyph would deform the run's internal geometry instead of translating it.
// The shift this returns is rigid — every glyph moves by the same delta — and
// is bounded by half a pixel.
//
// One caveat on "the run's baseline": HarfBuzz y-offsets are folded into `gp.y`
// (FontAtlas.cpp:430), so a mark positioned off the baseline by GPOS rides the
// same rigid shift but does not itself sit on the snapped row. That is correct —
// the mark's placement relative to its base glyph is what must be preserved.
[[nodiscard]] float SnapRunOriginY(float originY, float baselineY);

// Emit one shaped run at (originX, originY) with its baseline snapped to a
// whole device pixel. Glyph color comes from the placements, which carry the
// color ShapeText was called with.
//
// A run with visible effects emits them in layers over the whole run: one
// shadow-and-glow instance per glyph, then one outline instance per glyph,
// then the plain fills. Every effect of the run paints under every fill of
// it, and every glow under every outline. A run without effects emits the
// plain fills only.
//
// Color glyphs whose atlas page fails to register are skipped: index 0 is
// UITextureRegistry's failure sentinel, not a valid page.
void EmitGlyphRun(std::span<const Rendering::Text::FontAtlas::GlyphPlacement> glyphs,
                  float originX, float originY, float baselineY,
                  const GlyphRunTarget& target);

// Target for a run emitted through a PrimitiveEmitContext — the path every
// control and panel that draws its own text already goes through.
[[nodiscard]] GlyphRunTarget MakeGlyphRunTarget(
    PrimitiveEmitContext& ctx, Rendering::Text::FontAtlas* font,
    const std::vector<UITextureRegistry::SlugTextureIndices>& slugPages);

} // namespace UI
} // namespace GameEngine
