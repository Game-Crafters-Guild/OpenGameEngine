#include "UI/GlyphRunEmitter.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace UI {

using Rendering::Text::FontAtlas;

namespace
{
// Largest outline width, glow radius and shadow blur, device pixels. Each widens
// every glyph quad of the run by its reach, so the bound caps the fragment cost.
constexpr float kMaxTextEffectLengthPx = 64.0f;

// Largest shadow offset along either axis, device pixels. The vertex stage widens
// the glyph quad on every side by the offset plus the other reaches
// (uiTextEffectReachPx), so with the bound above a glyph reaches at most
// 256 + 64 + 1.5 * 64 + 1 = 417 px past its box.
constexpr float kMaxTextShadowOffsetPx = 256.0f;

// A glow's reach in radii past the outline's edge, where its falloff has faded
// out, and a blurred shadow's in blur radii past the offset shape
// (kTextGlowReachRadii and kTextShadowReachBlurs in ui_sdf_common.glsl).
constexpr float kTextGlowReachRadii = 2.0f;
constexpr float kTextShadowReachBlurs = 1.5f;

// The one-pixel coverage ramp an outline edge and a shadow edge add past their shape.
constexpr float kTextEffectEdgeRampPx = 1.0f;

float ScaleEffectLength(float logicalPx, float scale)
{
    const float devicePx = logicalPx * scale;
    return std::isfinite(devicePx) ? devicePx : 0.0f;
}

float ScaleEffectOffset(float logicalPx, float scale)
{
    return std::clamp(ScaleEffectLength(logicalPx, scale), -kMaxTextShadowOffsetPx, kMaxTextShadowOffsetPx);
}

float ScaleEffectExtent(float logicalPx, float scale)
{
    return std::clamp(ScaleEffectLength(logicalPx, scale), 0.0f, kMaxTextEffectLengthPx);
}
} // namespace

TextEffects ResolveTextEffects(const TextEffects& authored, float scale)
{
    TextEffects result = authored;
    scale = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    result.ShadowOffsetX = ScaleEffectOffset(authored.ShadowOffsetX, scale);
    result.ShadowOffsetY = ScaleEffectOffset(authored.ShadowOffsetY, scale);
    result.ShadowBlur = ScaleEffectExtent(authored.ShadowBlur, scale);
    result.GlowRadius = ScaleEffectExtent(authored.GlowRadius, scale);
    result.OutlineWidth = ScaleEffectExtent(authored.OutlineWidth, scale);
    result.ShadowColor = PackFromARGB(authored.ShadowColor);
    result.GlowColor = PackFromARGB(authored.GlowColor);
    result.OutlineColor = PackFromARGB(authored.OutlineColor);
    return result;
}

float TextEffectReachPx(const TextEffects& resolved)
{
    // The shadow and the glow start at the outline's edge; SetTextEffects
    // carries the width only for a visible outline.
    const float width = resolved.HasVisibleOutline() ? resolved.OutlineWidth : 0.0f;
    float reach = resolved.HasVisibleOutline() ? width + kTextEffectEdgeRampPx : 0.0f;
    if (resolved.HasVisibleGlow())
        reach = std::max(reach, width + kTextGlowReachRadii * resolved.GlowRadius);
    if (resolved.HasVisibleShadow())
    {
        const float offset = std::max(std::abs(resolved.ShadowOffsetX), std::abs(resolved.ShadowOffsetY));
        reach = std::max(reach, offset + width + kTextShadowReachBlurs * std::max(resolved.ShadowBlur, 0.0f)
                                    + kTextEffectEdgeRampPx);
    }
    return reach;
}

float SnapRunOriginY(float originY, float baselineY)
{
    const float baseline = originY + baselineY;
    return originY + (std::round(baseline) - baseline);
}

namespace
{
// The fill primitive of one placed glyph, carrying the run's clip and opacity.
// False for a color glyph whose atlas page fails to register.
bool MakeRunGlyph(const FontAtlas::GlyphPlacement& gp, float x, float y,
                  const GlyphRunTarget& target,
                  const std::vector<UITextureRegistry::SlugTextureIndices>& slugPages,
                  UIPrimitive& prim)
{
    if (gp.isColor)
    {
        const uint32_t texIdx = (target.Textures && target.Font)
            ? target.Textures->RegisterColorAtlasPage(*target.Font,
                                                      static_cast<int>(gp.atlasPageIndex))
            : 0;
        if (texIdx == 0)
            return false;
        prim = MakeColorGlyph(x, y, gp.width, gp.height, texIdx,
                              gp.u0, gp.v0, gp.u1, gp.v1, gp.color);
    }
    else
    {
        UITextureRegistry::SlugTextureIndices slugTex{};
        if (gp.pageIndex < slugPages.size())
            slugTex = slugPages[gp.pageIndex];
        prim = MakeSlugGlyph(x, y, gp.width, gp.height,
                             gp.u0, gp.v0, gp.u1, gp.v1,
                             gp.bandScaleX, gp.bandScaleY, gp.bandOffX, gp.bandOffY,
                             gp.glyphLocX, gp.glyphLocY,
                             gp.hBandCount - 1, gp.vBandCount - 1,
                             slugTex.CurveTexIdx, slugTex.BandTexIdx, gp.color);
    }
    SetClip(prim, target.ClipIndex);
    SetOpacity(prim, target.Opacity);
    return true;
}

// The glyph's shadow-and-glow instance. Both are cast by the outlined glyph,
// so the instance keeps the outline width with a transparent outline color:
// it measures from the outline's edge without painting the outline.
UIPrimitive MakeShadowAndGlowInstance(UIPrimitive glyph, const TextEffects& effects)
{
    SetTextEffects(glyph, effects);
    glyph.BorderColor = 0;
    return glyph;
}

// The glyph's outline instance: the outline alone.
UIPrimitive MakeOutlineInstance(UIPrimitive glyph, const TextEffects& effects)
{
    TextEffects outline;
    outline.OutlineColor = effects.OutlineColor;
    outline.OutlineWidth = effects.OutlineWidth;
    SetTextEffects(glyph, outline);
    return glyph;
}
} // namespace

void EmitGlyphRun(std::span<const FontAtlas::GlyphPlacement> glyphs,
                  float originX, float originY, float baselineY,
                  const GlyphRunTarget& target)
{
    if (glyphs.empty() || !target.Primitives)
        return;

    static const std::vector<UITextureRegistry::SlugTextureIndices> kEmptySlugPages;
    const auto& slugPages = target.SlugPages ? *target.SlugPages : kEmptySlugPages;

    const float snappedY = SnapRunOriginY(originY, baselineY);

    // The run paints in layers, each over the whole run: every shadow and
    // glow, then every outline, then every fill. No glyph's effect covers
    // another glyph's fill, and no glyph's glow covers another's outline.
    const TextEffects& effects = target.Effects;
    if (effects.HasVisibleShadow() || effects.HasVisibleGlow())
    {
        for (const auto& gp : glyphs)
        {
            if (gp.isColor && !effects.HasVisibleShadow())
                continue;
            UIPrimitive prim{};
            if (MakeRunGlyph(gp, originX + gp.x, snappedY + gp.y, target, slugPages, prim))
                target.Primitives->push_back(MakeShadowAndGlowInstance(prim, effects));
        }
    }
    if (effects.HasVisibleOutline())
    {
        for (const auto& gp : glyphs)
        {
            UIPrimitive prim{};
            if (!gp.isColor && MakeRunGlyph(gp, originX + gp.x, snappedY + gp.y, target, slugPages, prim))
                target.Primitives->push_back(MakeOutlineInstance(prim, effects));
        }
    }

    for (const auto& gp : glyphs)
    {
        UIPrimitive prim{};
        if (MakeRunGlyph(gp, originX + gp.x, snappedY + gp.y, target, slugPages, prim))
            target.Primitives->push_back(prim);
    }
}

GlyphRunTarget MakeGlyphRunTarget(
    PrimitiveEmitContext& ctx, FontAtlas* font,
    const std::vector<UITextureRegistry::SlugTextureIndices>& slugPages)
{
    GlyphRunTarget target{};
    target.Primitives = &ctx.Primitives;
    target.Textures = ctx.Textures;
    target.Font = font;
    target.SlugPages = &slugPages;
    target.ClipIndex = ctx.ClipIndex;
    target.Opacity = ctx.Opacity;
    target.Effects = ResolveTextEffects(ctx.Effects, ctx.ContentScale);
    return target;
}

} // namespace UI
} // namespace GameEngine
