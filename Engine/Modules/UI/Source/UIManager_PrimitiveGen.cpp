#include "UI/UIManager.h"
#include "UIManager_Internal.h"
#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include "UI/Interaction/TooltipOverlay.h"

#include "UI/Controls/TextField.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "TextShapeCache.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/NineSliceLayout.h"
#include "UI/GlyphRunEmitter.h"
#include "UI/TextSelectionFill.h"
#include "UI/UITextureRegistry.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <string_view>

static constexpr size_t kInitialClipStackCapacity = 8;
static constexpr size_t kInitialSortedChildrenCapacity = 64;
static constexpr size_t kInitialDeferredOverlayCapacity = 8;

namespace
{
std::string MakeBackgroundPathCacheKey(std::string_view path, std::string_view sourceAlias)
{
    if (sourceAlias.empty())
        return std::string(path);

    std::string key;
    key.reserve(sourceAlias.size() + 1u + path.size());
    key.append(sourceAlias);
    key.push_back('|');
    key.append(path);
    return key;
}

// Corner radii of the padding box — the inner border edge, which is both the
// CSS overflow clip contour and the inner edge of the painted border ring.
// The outer radii shrunk by the adjacent border widths, floored at zero
// (CSS inner-edge border-radius rule): the horizontal radius loses the
// left/right border, the vertical one the top/bottom border. Both operands
// are CSS-logical, so is the result — ScaleRadii converts it before it
// reaches PushClip.
//
// ui_sdf.frag derives the painted ring's inner contour with the same
// per-axis subtraction (radInX/radInY). The two MUST agree: a clip derived
// any other way would cut content along a different arc than the ring it is
// supposed to meet, leaving a sliver of background between content and
// border at every rounded corner.
CornerRadiiTLTRBRBL InnerClipRadii(const CornerRadiiTLTRBRBL& outer, const Box4& borderWidth)
{
    const auto inner = [](const CornerRadius& r, float bwX, float bwY) {
        return CornerRadius{std::max(0.0f, r.X - bwX), std::max(0.0f, r.Y - bwY)};
    };
    CornerRadiiTLTRBRBL out{};
    out.TopLeft     = inner(outer.TopLeft,     borderWidth.Left,  borderWidth.Top);
    out.TopRight    = inner(outer.TopRight,    borderWidth.Right, borderWidth.Top);
    out.BottomRight = inner(outer.BottomRight, borderWidth.Right, borderWidth.Bottom);
    out.BottomLeft  = inner(outer.BottomLeft,  borderWidth.Left,  borderWidth.Bottom);
    return out;
}

// CSS-logical radii in physical px (scale = physical px per CSS-logical px).
// A clip slot pairs its radii with its rect and the shader evaluates the two
// together against a physical-px fragment position
// (ui_sdf.frag computeClipAlpha), so an unscaled radius rounds the mask by
// 1/scale of what CSS asked for.
CornerRadiiTLTRBRBL ScaleRadii(const CornerRadiiTLTRBRBL& radii, float scale)
{
    const auto scaled = [scale](const CornerRadius& r) {
        return CornerRadius{r.X * scale, r.Y * scale};
    };
    CornerRadiiTLTRBRBL out{};
    out.TopLeft = scaled(radii.TopLeft);
    out.TopRight = scaled(radii.TopRight);
    out.BottomRight = scaled(radii.BottomRight);
    out.BottomLeft = scaled(radii.BottomLeft);
    return out;
}

// Stamp used corner radii (CSS-logical) onto a primitive's per-axis radius
// fields, scaled to physical px. Shared by every Rect/Textured emission site
// so the two GPU vec4s can never disagree about corner order.
void ApplyCornerRadii(UI::UIPrimitive& p, const CornerRadiiTLTRBRBL& radii, float scale)
{
    p.Radii[0] = radii.TopLeft.X * scale;
    p.Radii[1] = radii.TopRight.X * scale;
    p.Radii[2] = radii.BottomRight.X * scale;
    p.Radii[3] = radii.BottomLeft.X * scale;
    p.RadiiY[0] = radii.TopLeft.Y * scale;
    p.RadiiY[1] = radii.TopRight.Y * scale;
    p.RadiiY[2] = radii.BottomRight.Y * scale;
    p.RadiiY[3] = radii.BottomLeft.Y * scale;
}

// Padding-box mask rect for every overflow clip slot: the element rect
// inset by its border (scale = physical px per CSS-logical px). Shared by
// first emission and the drain's slot rewrite so the mask never changes
// shape between the two.
void InsetToPaddingBox(const Box4& borderWidth, float scale,
                       float& x, float& y, float& w, float& h)
{
    const float bL = borderWidth.Left * scale;
    const float bT = borderWidth.Top * scale;
    x += bL;
    y += bT;
    w = std::max(0.0f, w - bL - borderWidth.Right * scale);
    h = std::max(0.0f, h - bT - borderWidth.Bottom * scale);
}

// Chrome pixel-snaps every painted box: each border-box edge lands on the
// device-pixel gridline nearest its layout position (round, half away from
// zero), and the painted extent is the difference of snapped edges — so a
// box's paint can gain or lose a device pixel, but its edges never carry
// partial coverage. Layout stays on the 1/64 grid; only this element's own
// paint snaps. This is also what keeps opposite borders identical: unsnapped
// fractional edges give each border its own sub-pixel phase and therefore its
// own intensity profile. Measured against real Chrome at device scale 1 and
// 1.5; BorderEdgeSnapTests pins the rules against the fixtures in
// Engine/Modules/UI/Tests/ChromeReference/border-edge.
void SnapPaintRect(float& x, float& y, float& w, float& h)
{
    const float x1 = std::round(x + w);
    const float y1 = std::round(y + h);
    x = std::round(x);
    y = std::round(y);
    w = std::max(0.0f, x1 - x);
    h = std::max(0.0f, y1 - y);
}

// A border's painted thickness is a whole number of device pixels:
// max(1, floor(width)) for any positive width — Chrome floors (1.5 -> 1,
// 22.5 -> 22) and keeps hairlines visible (0.75 -> 1). The band hangs off the
// snapped outer edge, so snapping the width keeps the inner edge on the grid
// too. Paint-only: Blink floors the COMPUTED border width, so its layout
// moves with the floored value; here Yoga and InsetToPaddingBox keep the
// unsnapped width, so at fractional scales content can sit up to half a
// device pixel inside the painted interior.
float SnapBorderWidth(float widthPhysical)
{
    return widthPhysical > 0.0f ? std::max(1.0f, std::floor(widthPhysical)) : 0.0f;
}

// Every clip slot masks at the PADDING box with inner-edge radii
// (InsetToPaddingBox + InnerClipRadii): css-overflow-3 clips overflowing
// content — children and own text alike — to the padding box, never the
// border box, so a child overlapping the border ring is cut at the inner
// border edge instead of painting over the ring. What differs is WHEN the
// slot is pushed: elements with children (or a mount target) push eagerly
// for child clipping; leaves acquire the slot lazily, only once their own
// text actually spills or carries effects (EmitTextPrimitives). The one
// departure from the padding box: a leaf whose text fits grows it by its text
// effects' reach (OwnTextClipOutsetPx).
bool ClipSlotPushedForChildren(UIElement* el)
{
    return !el->GetChildren().empty() || el->GetMountTarget() != nullptr;
}

// How far, device px, a leaf's own-text clip reaches past its padding box: the
// reach of its text effects (TextEffectReachPx), so an effect is cut by the
// ancestors' clips and never by the element's own box. Zero when the text is
// cut by the box (`textCut`: its effects are cut with it, and the glyph fills
// keep the CSS overflow edge) and for an element whose slot also clips its
// children (they keep the padding box). Both writers of the slot, the
// own-text push and the drain's collect-phase rewrite, size it through here.
float OwnTextClipOutsetPx(UIElement* el, const VisualStyle& vs, float contentScale, bool textCut)
{
    if (textCut || ClipSlotPushedForChildren(el) || !vs.TextEffects.HasVisibleEffect())
        return 0.0f;
    return UI::TextEffectReachPx(UI::ResolveTextEffects(vs.TextEffects, contentScale));
}

// Grows a clip rect by `outset` on every side; rounded corners grow with it,
// as an outset shape's corners do, and square corners stay square.
void OutsetClipShape(float outset, float& x, float& y, float& w, float& h, CornerRadiiTLTRBRBL& radii)
{
    if (outset <= 0.0f)
        return;
    x -= outset;
    y -= outset;
    w += 2.0f * outset;
    h += 2.0f * outset;
    for (CornerRadius* corner : {&radii.TopLeft, &radii.TopRight, &radii.BottomRight, &radii.BottomLeft})
    {
        if (corner->X > 0.0f && corner->Y > 0.0f)
        {
            corner->X += outset;
            corner->Y += outset;
        }
    }
}

// Drain re-emission runs with an empty clip stack, so ambient primitives
// come out wearing kNoClip and take the item's preserved ambient here.
// Primitives already wearing a real index are text glyphs self-clipped to
// the element's own slot; those keep it.
void PatchAmbientClip(UI::UIPrimitive* prims, size_t count, uint16_t ambientIdx)
{
    for (size_t p = 0; p < count; ++p)
    {
        UI::UIPrimitive& prim = prims[p];
        if (UI::GetClipIndex(prim.ModeAndFlags) != UI::kNoClip)
            continue;
        prim.ModeAndFlags = (prim.ModeAndFlags & ~UI::kPrimClipIndexMask) |
                            (uint32_t(ambientIdx) << UI::kPrimClipIndexShift);
    }
}

// Upper bound on the dots/dashes one element's decorative border may emit.
// A dashed/dotted stroke scales its segment count with the element perimeter,
// so a large element could otherwise flood the primitive buffer. Realistic
// full-window borders stay well under this; past it the stroke is decorative
// enough that a truncated tail is imperceptible.
constexpr int kMaxBorderDecorationPrimitives = 4096;

// The one colour every border path has to settle on: a UIPrimitive carries a
// single BorderColor covering all four edges. AddBorderLTRB varies the per-edge
// WIDTHS but takes that same single colour, so no path carries per-edge colours
// to the GPU.
//
// It must come from a side that is actually DRAWN. Taking "the first non-zero
// colour" reads Top first, and Top's initial value is 0x000000FF — non-zero,
// and alpha 0 — so a box bordered on any other side alone drew a transparent
// band. Callers gate on hasBorder, so at least one width is non-zero.
uint32_t DrawnBorderColor(const ResolvedStyle& style)
{
    const Box4& bw = style.Layout.BorderWidth;
    const BorderColorsTRBL& c = style.Visual.BorderColor;
    if (bw.Top > 0)    return c.Top;
    if (bw.Right > 0)  return c.Right;
    if (bw.Bottom > 0) return c.Bottom;
    if (bw.Left > 0)   return c.Left;
    return 0;
}

// Emit `border-style: dotted` as evenly spaced square dots along each present
// edge. Dots run the straight edges only; a per-corner inset keeps them clear
// of rounded corners so the pattern never bunches at a curve. The stroke uses
// the max of the four edge widths (dotted borders are drawn uniformly).
// x/y/w/h are the physical-px element rect; cs converts CSS-logical widths and
// radii to physical px.
void EmitDottedBorder(std::vector<UI::UIPrimitive>& outPrims, const ResolvedStyle& style,
                      const CornerRadiiTLTRBRBL& usedRadii,
                      float x, float y, float w, float h, float cs,
                      uint16_t clipIdx, float opacity)
{
    const Box4& bw = style.Layout.BorderWidth;
    const uint32_t packedColor = UI::PackFromARGB(DrawnBorderColor(style));
    const float borderWidth = std::max({bw.Top, bw.Right, bw.Bottom, bw.Left}) * cs;
    const float dotSize = std::max(2.0f * cs, borderWidth * 2.0f);
    const float pitch = dotSize * 3.0f;
    // A non-positive pitch (degenerate content scale) would spin the edge
    // loops forever without ever advancing.
    if (pitch <= 0.0f)
        return;
    const float radius = std::max({usedRadii.TopLeft.X, usedRadii.TopLeft.Y,
                                   usedRadii.TopRight.X, usedRadii.TopRight.Y,
                                   usedRadii.BottomRight.X, usedRadii.BottomRight.Y,
                                   usedRadii.BottomLeft.X, usedRadii.BottomLeft.Y}) * cs;
    const float cornerInset = std::max(dotSize * 0.5f, radius * 0.55f);

    int emitted = 0;
    auto emitDot = [&](float dotX, float dotY) {
        if (emitted >= kMaxBorderDecorationPrimitives)
            return;
        ++emitted;
        UI::UIPrimitive dot = UI::MakeRect(dotX, dotY, dotSize, dotSize, packedColor,
                                           dotSize * 0.5f, dotSize * 0.5f,
                                           dotSize * 0.5f, dotSize * 0.5f);
        UI::SetClip(dot, clipIdx);
        UI::SetOpacity(dot, opacity);
        outPrims.push_back(dot);
    };

    const float left = x + borderWidth * 0.5f - dotSize * 0.5f;
    const float right = x + w - borderWidth * 0.5f - dotSize * 0.5f;
    const float top = y + borderWidth * 0.5f - dotSize * 0.5f;
    const float bottom = y + h - borderWidth * 0.5f - dotSize * 0.5f;
    for (float dotX = x + cornerInset; dotX <= x + w - cornerInset - dotSize; dotX += pitch)
    {
        if (emitted >= kMaxBorderDecorationPrimitives)
            break;
        if (bw.Top > 0.0f) emitDot(dotX, top);
        if (bw.Bottom > 0.0f) emitDot(dotX, bottom);
    }
    for (float dotY = y + cornerInset; dotY <= y + h - cornerInset - dotSize; dotY += pitch)
    {
        if (emitted >= kMaxBorderDecorationPrimitives)
            break;
        if (bw.Left > 0.0f) emitDot(left, dotY);
        if (bw.Right > 0.0f) emitDot(right, dotY);
    }
}

// Emit `border-style: dashed` by walking the element's rounded-rect perimeter
// and stamping dash line primitives with gaps between them. Walking the whole
// perimeter (rather than dashing each edge independently) keeps the dash/gap
// pattern continuous through the corners. The stroke uses the max of the four
// edge widths. x/y/w/h are the physical-px element rect; cs converts
// CSS-logical widths and radii to physical px.
void EmitDashedBorder(std::vector<UI::UIPrimitive>& outPrims, const ResolvedStyle& style,
                      const CornerRadiiTLTRBRBL& usedRadii,
                      float x, float y, float w, float h, float cs,
                      uint16_t clipIdx, float opacity)
{
    const Box4& bw = style.Layout.BorderWidth;
    const uint32_t packedColor = UI::PackFromARGB(DrawnBorderColor(style));
    const float borderWidth = std::max({bw.Top, bw.Right, bw.Bottom, bw.Left}) * cs;
    struct BorderPathPoint
    {
        float X;
        float Y;
    };

    const float dashLength = std::max(6.0f * cs, borderWidth * 4.0f);
    const float gapLength = std::max(4.0f * cs, borderWidth * 3.0f);
    // Without a positive dash+gap (degenerate content scale) the pattern walk
    // can't advance and would loop forever.
    if (dashLength <= 0.0f || gapLength <= 0.0f)
        return;
    const float halfBorder = borderWidth * 0.5f;
    const float left = x + halfBorder;
    const float right = x + w - halfBorder;
    const float top = y + halfBorder;
    const float bottom = y + h - halfBorder;
    // Stroke-centreline corner radii: the used radii pulled in by half the
    // border, per axis, clamped so opposite corners cannot cross.
    const float maxRadiusX = std::max(0.0f, (right - left) * 0.5f);
    const float maxRadiusY = std::max(0.0f, (bottom - top) * 0.5f);
    const auto strokeRadius = [&](const CornerRadius& r) {
        return CornerRadius{std::min(maxRadiusX, std::max(0.0f, r.X * cs - halfBorder)),
                            std::min(maxRadiusY, std::max(0.0f, r.Y * cs - halfBorder))};
    };
    const CornerRadius radiusTL = strokeRadius(usedRadii.TopLeft);
    const CornerRadius radiusTR = strokeRadius(usedRadii.TopRight);
    const CornerRadius radiusBR = strokeRadius(usedRadii.BottomRight);
    const CornerRadius radiusBL = strokeRadius(usedRadii.BottomLeft);
    constexpr float pi = std::numbers::pi_v<float>;

    std::vector<BorderPathPoint> path;
    path.reserve(48);
    auto appendPoint = [&path](float pointX, float pointY) {
        if (path.empty() || std::abs(path.back().X - pointX) > 0.001f ||
            std::abs(path.back().Y - pointY) > 0.001f)
        {
            path.push_back({pointX, pointY});
        }
    };
    // Elliptical corner arc: x rides cos against the horizontal semi-axis,
    // y rides sin against the vertical one. A corner with either axis zero
    // is square (css-backgrounds-3 §5) and contributes only its end point.
    auto appendArc = [&](float centerX, float centerY, const CornerRadius& radius,
                         float startAngle, float endAngle) {
        if (radius.X <= 0.0f || radius.Y <= 0.0f)
        {
            appendPoint(centerX + std::cos(endAngle) * radius.X,
                        centerY + std::sin(endAngle) * radius.Y);
            return;
        }

        const float maxAxis = std::max(radius.X, radius.Y);
        const int steps = std::max(
            4, static_cast<int>(std::ceil(std::abs(endAngle - startAngle) * maxAxis /
                                           std::max(1.0f, dashLength * 0.5f))));
        for (int step = 1; step <= steps; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const float angle = startAngle + (endAngle - startAngle) * t;
            appendPoint(centerX + std::cos(angle) * radius.X,
                        centerY + std::sin(angle) * radius.Y);
        }
    };

    appendPoint(left + radiusTL.X, top);
    appendPoint(right - radiusTR.X, top);
    appendArc(right - radiusTR.X, top + radiusTR.Y, radiusTR, -pi * 0.5f, 0.0f);
    appendPoint(right, bottom - radiusBR.Y);
    appendArc(right - radiusBR.X, bottom - radiusBR.Y, radiusBR, 0.0f, pi * 0.5f);
    appendPoint(left + radiusBL.X, bottom);
    appendArc(left + radiusBL.X, bottom - radiusBL.Y, radiusBL, pi * 0.5f, pi);
    appendPoint(left, top + radiusTL.Y);
    appendArc(left + radiusTL.X, top + radiusTL.Y, radiusTL, pi, pi * 1.5f);

    bool drawing = true;
    float patternRemaining = dashLength;
    int emitted = 0;
    for (size_t pointIndex = 1; pointIndex < path.size(); ++pointIndex)
    {
        const BorderPathPoint& start = path[pointIndex - 1];
        const BorderPathPoint& end = path[pointIndex];
        const float dx = end.X - start.X;
        const float dy = end.Y - start.Y;
        const float segmentLength = std::sqrt(dx * dx + dy * dy);
        if (segmentLength <= 0.001f)
            continue;

        float segmentOffset = 0.0f;
        while (segmentOffset < segmentLength - 0.001f)
        {
            const float chunkLength = std::min(patternRemaining, segmentLength - segmentOffset);
            if (drawing && chunkLength > 0.001f)
            {
                const float startT = segmentOffset / segmentLength;
                const float endT = (segmentOffset + chunkLength) / segmentLength;
                UI::UIPrimitive dash = UI::MakeLine(
                    start.X + dx * startT, start.Y + dy * startT,
                    start.X + dx * endT, start.Y + dy * endT,
                    borderWidth, packedColor, clipIdx);
                UI::SetOpacity(dash, opacity);
                outPrims.push_back(dash);
                if (++emitted >= kMaxBorderDecorationPrimitives)
                    return;
            }

            segmentOffset += chunkLength;
            patternRemaining -= chunkLength;
            if (patternRemaining <= 0.001f)
            {
                drawing = !drawing;
                patternRemaining = drawing ? dashLength : gapLength;
            }
        }
    }
}

// An outline paints as a ring: nothing where the element already is, a stroke
// of `outline-width` immediately outside it. Only style/width/color decide
// whether one exists at all — offset alone never produces a ring.
bool HasOutlineRing(const VisualStyle& vs)
{
    return vs.OutlineStyle != BorderStyle::None && vs.OutlineWidth > 0.0f &&
           ((vs.HasOutlineColor ? vs.OutlineColor : vs.Color) & 0xFF000000u) != 0u;
}

// The ring's outer edge sits (outline-offset + outline-width) beyond the
// border edge; the stroke fills inward from there, so its inner edge lands
// exactly `outline-offset` outside the element. The rect carries a fully
// transparent fill, so the shader's fill/border coverage split paints the
// band and nothing else — the element's own background is not re-covered.
//
// `clipIdx` is the AMBIENT clip (the one the element's own paint uses), never
// a clip this element pushes for its children: an outline is outside the box
// model and an element cannot clip its own ring away with overflow:hidden.
// An ancestor's overflow still clips it, which is what browsers do.
void EmitOutlineRing(std::vector<UI::UIPrimitive>& outPrims, const VisualStyle& vs,
                     const CornerRadiiTLTRBRBL& usedRadii,
                     float x, float y, float w, float h, float scale,
                     uint16_t clipIdx, float opacity)
{
    const float widthPx = SnapBorderWidth(vs.OutlineWidth * scale);
    const float inflate = vs.OutlineOffset * scale + widthPx;

    float ox = x - inflate;
    float oy = y - inflate;
    float ow = w + 2.0f * inflate;
    float oh = h + 2.0f * inflate;
    if (ow <= 0.0f || oh <= 0.0f)
        return; // a negative offset pulled the ring inside out
    // The ring is border paint: its edges snap to the device grid like any
    // painted box edge (SnapPaintRect), so both verticals carry the same ink.
    SnapPaintRect(ox, oy, ow, oh);

    // A square element keeps square outline corners; a rounded one grows its
    // radii by the same amount the box grew, so the ring stays concentric.
    // Per axis: both semi-axes of a rounded corner inflate together. A corner
    // with either axis zero is square (css-backgrounds-3 §5) and stays square.
    auto ringRadius = [inflate, scale](const CornerRadius& logical) -> CornerRadius
    {
        if (logical.X <= 0.0f || logical.Y <= 0.0f)
            return {};
        return {std::max(0.0f, logical.X * scale + inflate),
                std::max(0.0f, logical.Y * scale + inflate)};
    };
    CornerRadiiTLTRBRBL ringRadii{};
    ringRadii.TopLeft = ringRadius(usedRadii.TopLeft);
    ringRadii.TopRight = ringRadius(usedRadii.TopRight);
    ringRadii.BottomRight = ringRadius(usedRadii.BottomRight);
    ringRadii.BottomLeft = ringRadius(usedRadii.BottomLeft);

    const uint32_t color = vs.HasOutlineColor ? vs.OutlineColor : vs.Color;
    UI::UIPrimitive ring = UI::MakeRect(ox, oy, ow, oh, /*fill=*/0u);
    ApplyCornerRadii(ring, ringRadii, 1.0f); // already physical px
    UI::AddBorderLTRB(ring, widthPx, widthPx, widthPx, widthPx, UI::PackFromARGB(color));
    UI::SetClip(ring, clipIdx);
    UI::SetOpacity(ring, opacity);
    outPrims.push_back(ring);
}

// Source space -> the primitive's space flag bits (kPrimHdrTextureBit /
// kPrimEncodedSourceBit, mutually exclusive):
//   neither — SDR-referred content. ui_sdf.frag clamps to [0,1], premultiplies,
//             and takes the UI lift under HDR output; the target's own sample
//             adapter converts it into the blend space.
//   HDR     — paper-white-relative linear, composited straight.
//   ENCODED — content that already carries the blend target's transfer curve, so
//             its sample adapter is the identity.
//
// Exhaustive on purpose, with no default arm, and pinned on the space count: a
// new space fails to compile here rather than silently inheriting some other
// space's encoding.
constexpr uint32_t SpaceFlagBitsForSpace(UI::UITextureSpace space)
{
    static_assert(UI::UITextureSpace::kKindCount == 4,
                  "A new UITextureSpace needs a shader-bit arm here.");
    uint32_t bits = 0;
    switch (space.GetKind())
    {
    case UI::UITextureSpace::Kind::SrgbAuthored:
        // Authored sRGB reaches the sampler DECODED (its view carries the
        // transfer curve), so it is SDR-referred content like any other here.
        break;
    case UI::UITextureSpace::Kind::SdrFinalized:
        bits = UI::kPrimEncodedSourceBit;
        break;
    case UI::UITextureSpace::Kind::DisplayLinearSdr:
        // Display-referred [0,1] content rides the SDR-referred arm: in SDR
        // output that arm differs from straight linear only by clamps that
        // cannot fire on [0,1] content, and under HDR output it takes the UI
        // lift — a thumbnail or snapshot rendered under SDR sits amongst
        // chrome and follows the UI white; it must not read as
        // paper-white-relative scene content.
        break;
    case UI::UITextureSpace::Kind::HdrLinear:
        bits = UI::kPrimHdrTextureBit;
        break;
    case UI::UITextureSpace::Kind::Count:
        break; // size sentinel; no factory produces it
    }
    return bits;
}
}

struct UIManager::ResolvedBgSlot
{
    uint32_t TexSlot = 0;
    uint32_t ImgW = 0;
    uint32_t ImgH = 0;
    uint32_t SpaceBits = 0;
    NineSlice Slice{};
};

// PrimitiveGenContext holds the scratch state for the DFS tree walk.
struct UIManager::PrimitiveGenContext
{
    std::vector<UI::UIPrimitive>& Primitives;
    std::vector<UI::UIClipRect>& ClipRects;
    UI::UITextureRegistry* Textures;
    UIManager* Manager;
    float ViewportW = 0.0f;
    float ViewportH = 0.0f;

    // Multiplier applied to CSS-logical visual values (border widths, corner
    // radii, shadow/glow geometry, caret thickness) so they match the
    // physical-px layout rects produced by ConvergePostLayout.
    float ContentScale = 1.0f;

    // Accumulated opacity from ancestor chain. Multiplied by each element's
    // local opacity during the recursive walk; save/restore uses the call stack.
    float EffectiveOpacity = 1.0f;

    // Clip stack: indices into ClipRects (which is m_PersistentClipRects
    // under Stage 3; slot indices are allocated per-element via
    // UIManager::m_ClipAllocator and persist across frames).
    std::vector<uint16_t> ClipStack;

    uint16_t CurrentClipIndex() const
    {
        return ClipStack.empty() ? UI::kNoClip : ClipStack.back();
    }

    // Stage 3: per-element clip ownership. `el` may be nullptr for the
    // anonymous viewport clip used by deferred-overlay rendering — that
    // case routes to UIManager::m_ViewportClipSlot, allocated lazily and
    // retained for the lifetime of the manager.
    //
    // Rect AND radii are physical px: the shader reads both out of one
    // UIClipRect and evaluates them against a physical fragment position,
    // so a caller holding CSS-logical style values scales them first
    // (ScaleRadii).
    uint16_t PushClip(UIElement* el, float x, float y, float w, float h,
                      const CornerRadiiTLTRBRBL& radii)
    {
        // Clip rects snap to the device grid like paint boxes: a fractional
        // clip edge shaves a snapped painted edge back to partial coverage —
        // the very asymmetry SnapPaintRect exists to remove. Radii stay
        // fractional; only the rect grid-aligns.
        SnapPaintRect(x, y, w, h);

        // Intersect with the current parent clip so nested overflow:hidden
        // elements don't replace the parent's clip region.
        if (!ClipStack.empty())
        {
            const UI::UIClipRect& parent = ClipRects[ClipStack.back()];
            float px1 = parent.Rect[0];
            float py1 = parent.Rect[1];
            float px2 = px1 + parent.Rect[2];
            float py2 = py1 + parent.Rect[3];

            float cx2 = x + w;
            float cy2 = y + h;

            x = std::max(x, px1);
            y = std::max(y, py1);
            w = std::max(0.0f, std::min(cx2, px2) - x);
            h = std::max(0.0f, std::min(cy2, py2) - y);
        }

        // Resolve persistent slot — allocate lazily if this is the first
        // time we're pushing a clip for this element (or for the viewport).
        uint16_t slot;
        if (el)
        {
            if (el->m_ClipSlotIdx == UI::kNoClip)
            {
                const uint32_t newSlot = Manager->m_ClipAllocator.Allocate();
                // Slot index must fit in 16 bits because UIPrimitive's
                // modeAndFlags packs clipIndex in [16:31] and parentIndex
                // is read with kNoClip = 0xFFFF as the sentinel.
                if (newSlot >= UI::kNoClip)
                {
                    // Out of clip-slot space (>65534 live clips). Skip the
                    // clip rather than corrupt the chain. Extremely unlikely.
                    Manager->m_ClipAllocator.Free(newSlot);
                    return UI::kNoClip;
                }
                el->m_ClipSlotIdx = static_cast<uint16_t>(newSlot);
            }
            slot = el->m_ClipSlotIdx;
        }
        else
        {
            if (Manager->m_ViewportClipSlot == UI::SlotAllocator::kInvalidSlot)
            {
                const uint32_t newSlot = Manager->m_ClipAllocator.Allocate();
                if (newSlot >= UI::kNoClip)
                {
                    Manager->m_ClipAllocator.Free(newSlot);
                    return UI::kNoClip;
                }
                Manager->m_ViewportClipSlot = newSlot;
            }
            slot = static_cast<uint16_t>(Manager->m_ViewportClipSlot);
        }

        // Ensure the persistent CPU mirror covers the slot.
        if (ClipRects.size() <= slot)
            ClipRects.resize(static_cast<size_t>(slot) + 1);

        UI::UIClipRect& cr = ClipRects[slot];
        cr.Rect[0] = x;
        cr.Rect[1] = y;
        cr.Rect[2] = w;
        cr.Rect[3] = h;
        cr.Radii[0] = radii.TopLeft.X;
        cr.Radii[1] = radii.TopRight.X;
        cr.Radii[2] = radii.BottomRight.X;
        cr.Radii[3] = radii.BottomLeft.X;
        cr.RadiiY[0] = radii.TopLeft.Y;
        cr.RadiiY[1] = radii.TopRight.Y;
        cr.RadiiY[2] = radii.BottomRight.Y;
        cr.RadiiY[3] = radii.BottomLeft.Y;
        cr.ParentIndex = ClipStack.empty() ? UI::kNoClip : ClipStack.back();
        ClipStack.push_back(slot);
        return slot;
    }

    void PopClip() { if (!ClipStack.empty()) ClipStack.pop_back(); }

    // Block E1: when set, GeneratePrimitivesForElement emits ONLY the
    // element's own primitives (bg/border/shadow/glow/text/control) into
    // the scratch buffer and then returns — skipping FinalizeElementSlots
    // (the persistent-buffer copy + DrawOrder push) AND child recursion.
    // Used by DrainPrimitiveDataDirty to refresh a single element's
    // primitives without re-running the full DFS.
    bool DrainModeOnly = false;

    // MT-3 parallel drain: set while this context emits on a JobSystem
    // worker. Emission paths that would touch shared-mutable state (font
    // resolve miss, text shaping, background/texture resolution, caret
    // measurement) must not run off-thread — they set EscalateToUiThread
    // and return, and the drain's apply phase re-emits the whole item on
    // the UI thread instead.
    bool OffThread = false;
    bool EscalateToUiThread = false;

    // MT-3: background-image slot pre-resolved on the UI thread during the
    // drain's collect phase for the item currently being emitted. Consumed
    // by EmitBackgroundImagePrimitive in place of ResolveBackgroundTextureSlot
    // (meaningful only in drain mode, where exactly one element emits).
    const ResolvedBgSlot* PreResolvedBg = nullptr;

    // Drain-inline emission runs with an empty clip stack; this carries the
    // item's PreservedClipIdx so a text-overflow PushClip during the drain
    // still intersects against (and parents to) the correct ancestor clip.
    // kNoClip outside the drain's apply phase.
    uint16_t DrainAmbientClipIdx = UI::kNoClip;

    // Z-sorted child iteration scratch (stack discipline — see plan Phase 4A).
    std::vector<UIElement*> SortedChildren;

    // Overlay deferral
    struct DeferredOverlay
    {
        UIElement* Element;
        float ParentAbsX, ParentAbsY;
        OverlayLayer Layer;
    };
    std::vector<DeferredOverlay> DeferredOverlays;
};

// MT-3: one drained element's worth of work. Collected on the UI thread,
// emitted (possibly on a JobSystem worker) into a per-task scratch buffer,
// applied strictly in queue order on the UI thread.
struct UIManager::DrainItem
{
    UIElement* El = nullptr;
    // Clip index recorded from the element's previous primitives; drain
    // emission has no clip stack, so it is patched back post-emit.
    uint16_t PreservedClipIdx = 0;
    // Emission must run on the UI thread: custom control emission observed
    // (m_CustomEmitObserved) or TextInput caret/selection overlays (measure
    // on the shared primary face). Decided during collect.
    bool UiThreadOnly = false;
    // A worker hit a shared-state tripwire; re-emit during apply.
    bool Escalated = false;
    bool HasPreResolvedBg = false;
    ResolvedBgSlot Bg{};
    // Where the emitted primitives landed: emit-task index + span within
    // that task's scratch. Unused for UiThreadOnly/Escalated items (those
    // emit inline during apply).
    uint32_t Task = 0;
    uint32_t EmitBegin = 0;
    uint32_t EmitCount = 0;
};

// ContentBox/ComputeContentBox live in UIManager_Internal.h — pointer
// hit-testing (TextField.cpp) shares the same box-model rules.

// Resolve font from VisualStyle fields (mirrors ResolveFontForStyle).
FontAtlas* UIManager::ResolveFontForVisualStyle(const VisualStyle& vs, PrimitiveGenContext& ctx)
{
    const void* famPtr = vs.FontFamily ? static_cast<const void*>(vs.FontFamily.get()) : nullptr;
    const int weight = vs.FontWeight;
    const std::uint8_t style = static_cast<std::uint8_t>(vs.FontStyle);
    const std::uint8_t variant = static_cast<std::uint8_t>(vs.FontVariant);

    for (const auto& entry : m_FrameFontResolveCache)
    {
        if (entry.FamilyPtr == famPtr && entry.Weight == weight
            && entry.Style == style && entry.Variant == variant)
            return entry.Atlas;
    }

    // Off-thread misses may not mutate the cache (plain vector, no lock) or
    // request font loads. The drain's collect phase pre-resolves every work
    // item's own style, so this only fires for a style first seen inside a
    // worker emission — escalate the item to a UI-thread re-emit.
    if (ctx.OffThread)
    {
        ctx.EscalateToUiThread = true;
        return nullptr;
    }

    FontAtlas* result = nullptr;
    if (vs.FontFamily && !vs.FontFamily->empty())
    {
        for (const auto& family : *vs.FontFamily)
        {
            if (auto* fa = GetOrRequestFontFamilyInternal(family, vs.FontWeight, vs.FontStyle, vs.FontVariant))
            {
                result = fa;
                break;
            }
        }
    }
    if (!result && m_FontAtlas)
        result = m_FontAtlas.get();
    if (!result && !m_FontAtlases.empty())
        result = m_FontAtlases.begin()->second.get();

    m_FrameFontResolveCache.push_back({famPtr, weight, style, variant, result});
    return result;
}

// ---------------------------------------------------------------------------
// PrimitiveEmitContext::EmitText — shapes text and emits Slug glyph
// primitives for canvas controls that need data-driven text fragments.
// ---------------------------------------------------------------------------

void UI::PrimitiveEmitContext::EmitText(std::string_view text, float x, float y,
                                        float fontSize, uint32_t color,
                                        Rendering::Text::FontAtlas* font)
{
    if (text.empty() || !font || !Textures)
        return;

    // ShapeText mutates the shared FontAtlas (glyph packing, FT face state)
    // — UI-thread-only. A worker emission escalates the item instead.
    if (OffThread)
    {
        if (EscalateFlag)
            *EscalateFlag = true;
        return;
    }

    const float pixelSize = std::max(1.0f, fontSize * ContentScale);
    const uint32_t packedColor = UI::PackFromARGB(color);

    static thread_local Rendering::Text::FontAtlas::ShapeResult scratchResult;
    font->ShapeText(text, pixelSize, scratchResult, packedColor);
    if (scratchResult.glyphs.empty())
        return;

    const auto& slugPages = Textures->RegisterSlugTextures(*font);

    // ShapeText's glyphs sit against a baseline at metrics.ascender, so the
    // run's baseline on screen is y + ascender.
    UI::EmitGlyphRun(scratchResult.glyphs, x, y, scratchResult.metrics.ascender,
                     UI::MakeGlyphRunTarget(*this, font, slugPages));
}

void UIManager::GenerateAllPrimitives()
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // Block E (E0/E1): timed entry — flush results into the most recent
    // UpdateProfileFrame (back()) so per-scenario captures see PrimitiveGen
    // cost and skip-rate even though this runs in Render rather than Update.
    using ProfClk = std::chrono::high_resolution_clock;
    const auto e0Start = ProfClk::now();
    auto e0Flush = [&](double ms, uint32_t skipped) {
        if (m_UpdateProfilingEnabled && !m_UpdateProfilingHistory.empty())
        {
            auto& back = m_UpdateProfilingHistory.back();
            back.GenAllPrimitivesMs       = ms;
            back.GenAllPrimitivesSkipped  = skipped;
        }
    };

    const bool needFullRegen = m_PrimitivesNeedRegen.load(std::memory_order_relaxed);

    // Block E (E0): no full regen and no visual-only mutations queued? The
    // persistent primitive / clip / draw-order buffers are still valid.
    // Skip the full DFS + memcpy and let the render prep re-upload the
    // existing snapshot.
    if (!needFullRegen && m_PrimitiveDataDirty.empty() &&
        !m_DrawOrder.empty() && m_Root)
    {
        GE_CPU_PROFILE_SCOPE("UIManager.GenerateAllPrimitives.Skipped");
        e0Flush(std::chrono::duration<double, std::milli>(ProfClk::now() - e0Start).count(), 1u);
        return;
    }

    // Block E1: drain-only path. No layout/structural changes since last
    // full DFS — only visual-only mutations on existing elements. Drain
    // those into their existing slot ranges instead of re-walking the tree.
    if (!needFullRegen && !m_PrimitiveDataDirty.empty() &&
        !m_DrawOrder.empty() && m_Root)
    {
        GE_CPU_PROFILE_SCOPE("UIManager.GenerateAllPrimitives.Drain");
        const bool drained = DrainPrimitiveDataDirty();
        if (drained)
        {
            // Drain succeeded; snapshot still valid for next-frame skip.
            // The drain bumped exactly the store versions it touched and
            // range-marked the primitive ring, so the render prep uploads
            // only the affected spans.
            e0Flush(std::chrono::duration<double, std::milli>(ProfClk::now() - e0Start).count(), 0u);
            return;
        }
        // Drain bailed (count mismatch or other invariant break) — fall
        // through to full regen. m_PrimitivesNeedRegen was set inside the
        // drain on failure paths.
    }

    GE_CPU_PROFILE_SCOPE("UIManager.GenerateAllPrimitives");

    // Stage 2: m_SdfPrimitiveBuffer is now a scratch buffer that elements
    // push into; FinalizeElementSlots pops each element's contribution
    // and writes it into m_PersistentPrimitives at slot-allocator-assigned
    // positions. m_DrawOrder accumulates the slot indices in DFS pre-order.
    // After the DFS phases complete, we rebuild m_SdfPrimitiveBuffer in
    // DrawOrder for the existing GPU upload path (Stage 2 part 3 will
    // switch the shader to read DrawOrder + persistent buffer directly).
    m_SdfPrimitiveBuffer.clear();
    m_SdfClipBuffer.clear();
    m_DrawOrder.clear();
    m_FrameFontResolveCache.clear();

    // Bump the PrimitiveGen frame counter, skipping 0 on wrap so default-
    // initialized UIElement::m_LastPrimitiveGenFrame can never collide with
    // the current value.
    ++m_PrimitiveGenFrame;
    if (m_PrimitiveGenFrame == 0)
        ++m_PrimitiveGenFrame;
    // This is a full DFS: elements it processes get stamped with this value
    // (m_LastFullDfsFrame). The drain uses the stamp to reject elements whose
    // slot/DrawOrder bookkeeping predates the current DrawOrder build.
    m_LastFullDfsGenFrame = m_PrimitiveGenFrame;

    // Recycle deferred RG texture slots from the previous frame before reserving new ones.
    if (m_SdfTextureRegistry)
    {
        for (const auto& binding : m_SdfDeferredRGBindings)
        {
            if (binding.Slot != 0)
                m_SdfTextureRegistry->ReleaseSlot(binding.Slot);
        }
    }
    m_SdfDeferredRGBindings.clear();

    if (!m_Root)
        return;

    // Stage 3: ctx.clipRects now references m_PersistentClipRects (slot-
    // indexed, persistent across frames) instead of m_SdfClipBuffer (the
    // legacy per-frame transient list). PushClip writes directly into the
    // element's allocated slot; the slot is preserved across frames via
    // UIElement::m_ClipSlotIdx and freed in ~UIElement / SetOwnerManager.
    PrimitiveGenContext ctx{m_SdfPrimitiveBuffer, m_PersistentClipRects,
                            m_SdfTextureRegistry.get(), this,
                            static_cast<float>(m_LastLayoutWidth), static_cast<float>(m_LastLayoutHeight),
                            std::max(0.01f, m_ContentScale)};
    ctx.ClipStack.reserve(kInitialClipStackCapacity);
    ctx.SortedChildren.reserve(kInitialSortedChildrenCapacity);
    ctx.DeferredOverlays.reserve(kInitialDeferredOverlayCapacity);

    // Phase 1: main tree walk (z-sorted DFS)
    GeneratePrimitivesForElement(m_Root.get(), ctx, 0.0f, 0.0f);

    // Phase 2: overlay layers, lowest first (dropdowns, panel tooltips,
    // modals, drag previews, hover tooltip). Indexed loops with a per-step
    // size re-check: generating an overlay can defer overlays nested in its
    // subtree, growing the vector mid-iteration.
    std::stable_sort(ctx.DeferredOverlays.begin(), ctx.DeferredOverlays.end(),
        [](const PrimitiveGenContext::DeferredOverlay& a,
           const PrimitiveGenContext::DeferredOverlay& b) {
            return static_cast<uint8_t>(a.Layer) < static_cast<uint8_t>(b.Layer);
        });
    size_t overlayIdx = 0;
    for (; overlayIdx < ctx.DeferredOverlays.size() &&
           ctx.DeferredOverlays[overlayIdx].Layer < OverlayLayer::HoverTooltip; ++overlayIdx)
    {
        const auto def = ctx.DeferredOverlays[overlayIdx]; // copy: vector may grow
        GenerateOverlayPrimitives(def.Element, ctx, def.ParentAbsX, def.ParentAbsY);
    }

    // Between the lower layers and the hover tooltip: the arrow draws above
    // everything generated so far but behind the tooltip body that follows.
    EmitTooltipArrowSlot(ctx);

    for (; overlayIdx < ctx.DeferredOverlays.size(); ++overlayIdx)
    {
        const auto def = ctx.DeferredOverlays[overlayIdx];
        GenerateOverlayPrimitives(def.Element, ctx, def.ParentAbsX, def.ParentAbsY);
    }

    // Stage 2 part 3: m_SdfPrimitiveBuffer is now solely a per-element
    // scratch buffer that FinalizeElementSlots pops after copying into
    // m_PersistentPrimitives. The GPU upload reads m_PersistentPrimitives
    // directly via shader-side DrawOrder indirection — no compatibility
    // rebuild needed. The scratch should be empty at this point because
    // every Finalize call resizes back to its scratchStart.
    assert(m_SdfPrimitiveBuffer.empty() && "PrimitiveGen scratch leak");

    // Block E (E0/E1): clear the regen flag and the data-dirty queue —
    // a full regen subsumes any visual-only marks, and subsequent idle
    // frames will skip via the early-return at the top until a MarkDirty
    // / Update mutation re-sets it.
    // The CPU stores were rewritten wholesale: bump every store version and
    // invalidate the primitive ring's dirty-range tracking so each
    // frame-in-flight slice re-uploads in full as it comes around.
    ++m_PrimitiveStoreVersion;
    ++m_ClipStoreVersion;
    ++m_DrawOrderStoreVersion;
    if (m_SdfPrimRing)
        m_SdfPrimRing->MarkAllRangesDirty();
    m_PrimitivesNeedRegen.store(false, std::memory_order_relaxed);
    for (auto* el : m_PrimitiveDataDirty)
        if (el)
            el->m_InQueueFlags &= ~UIElement::InPrimitiveDataDirty;
    m_PrimitiveDataDirty.clear();
    e0Flush(std::chrono::duration<double, std::milli>(ProfClk::now() - e0Start).count(), 0u);
#endif
}

void UIManager::GenerateOverlayPrimitives(UIElement* el, PrimitiveGenContext& ctx, float parentAbsX, float parentAbsY)
{
    const size_t savedClipDepth = ctx.ClipStack.size();
    const float savedOpacity = ctx.EffectiveOpacity;
    ctx.ClipStack.clear();
    ctx.EffectiveOpacity = 1.0f;
    ctx.PushClip(/*el=*/nullptr, 0, 0, ctx.ViewportW, ctx.ViewportH, {});
    GeneratePrimitivesForElement(el, ctx, parentAbsX, parentAbsY);
    ctx.PopClip();
    ctx.ClipStack.resize(savedClipDepth);
    ctx.EffectiveOpacity = savedOpacity;
}

void UIManager::EmitTooltipArrowSlot(PrimitiveGenContext& ctx)
{
    if (!m_TooltipOverlay)
        return;

    const size_t scratchStart = ctx.Primitives.size();
    m_TooltipOverlay->EmitArrowPrimitive(ctx.Primitives, 1.0f, ctx.ContentScale);
    const size_t arrowCount = ctx.Primitives.size() - scratchStart;
    if (arrowCount == 0)
        return; // tooltip hidden this frame — the slot stays out of DrawOrder

    if (m_TooltipArrowSlot == UI::SlotAllocator::kInvalidSlot)
    {
        uint16_t cap = 0;
        m_TooltipArrowSlot = m_PrimitiveAllocator.Allocate(static_cast<uint16_t>(arrowCount), cap);
        // (cap will be the size class for arrowCount; arrow is currently 1 primitive.)
        (void)cap;
    }
    const size_t neededSize = static_cast<size_t>(m_TooltipArrowSlot) + arrowCount;
    if (m_PersistentPrimitives.size() < neededSize)
        m_PersistentPrimitives.resize(neededSize);
    std::memcpy(&m_PersistentPrimitives[m_TooltipArrowSlot],
                ctx.Primitives.data() + scratchStart,
                arrowCount * sizeof(UI::UIPrimitive));
    for (size_t i = 0; i < arrowCount; ++i)
        m_DrawOrder.push_back(m_TooltipArrowSlot + static_cast<uint32_t>(i));
    ctx.Primitives.resize(scratchStart);
}

bool UIManager::ElementRectIntersectsNearestClip(const UIElement* el) const
{
    const float cs = std::max(0.01f, m_ContentScale);
    const float x = el->GetLayoutX() * cs;
    const float y = el->GetLayoutY() * cs;
    const float w = std::max(0.0f, el->GetLayoutWidth() * cs);
    const float h = std::max(0.0f, el->GetLayoutHeight() * cs);
    if (w <= 0.0f || h <= 0.0f)
        return false;
    // Nearest ancestor that owns a persistent clip slot. Ancestors precede
    // descendants in the drain queue (the mark walks are pre-order), so a
    // scrolled clip owner's slot has already been rewritten to its current
    // rect by the time descendants are tested.
    for (const UIElement* p = el->GetParent(); p; p = p->GetParent())
    {
        if (p->m_ClipSlotIdx != UI::kNoClip &&
            static_cast<size_t>(p->m_ClipSlotIdx) < m_PersistentClipRects.size())
        {
            const UI::UIClipRect& cr = m_PersistentClipRects[p->m_ClipSlotIdx];
            return x < cr.Rect[0] + cr.Rect[2] && x + w > cr.Rect[0] &&
                   y < cr.Rect[1] + cr.Rect[3] && y + h > cr.Rect[1];
        }
    }
    // m_LastLayoutWidth/Height are physical px (see the Update snapshot).
    return x < static_cast<float>(m_LastLayoutWidth) &&
           y < static_cast<float>(m_LastLayoutHeight) &&
           x + w > 0.0f && y + h > 0.0f;
}

bool UIManager::OwnTextEmissionPaints(UIElement* el, PrimitiveGenContext& ctx)
{
    const ITextMeasurable* textEl = el->GetTextMeasurable();
    if (!textEl || !textEl->HandlesOwnTextRendering())
        return false;
    // Emitted on the UI thread into the drain scratch tail with the state the
    // apply phase seeds, read for a count and discarded: the element has no
    // slot range to write into, so only a full regen can keep the result.
    const size_t start = ctx.Primitives.size();
    ctx.EscalateToUiThread = false;
    ctx.PreResolvedBg = nullptr;
    ctx.EffectiveOpacity = el->m_PrimitiveAncestorOpacity;
    ctx.DrainAmbientClipIdx = el->m_AmbientClipIdx;
    GeneratePrimitivesForElement(el, ctx, 0.0f, 0.0f);
    ctx.DrainAmbientClipIdx = UI::kNoClip;
    const bool paints = ctx.Primitives.size() > start;
    ctx.Primitives.resize(start);
    // The probe is not this frame's emission; a full regen that follows must
    // not see it as a same-frame revisit.
    el->m_LastPrimitiveGenFrame = 0;
    return paints;
}

bool UIManager::DrainPrimitiveDataDirty()
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    if (m_PrimitiveDataDirty.empty())
        return true;

    // Bump the per-PrimitiveGen frame counter so the re-entry detector in
    // GeneratePrimitivesForElement (compares el->m_LastPrimitiveGenFrame)
    // doesn't trip when an element re-emitted in a previous full DFS gets
    // visited again here. Skip 0 on wrap (matches GenerateAllPrimitives).
    ++m_PrimitiveGenFrame;
    if (m_PrimitiveGenFrame == 0)
        ++m_PrimitiveGenFrame;

    // Reuse m_SdfPrimitiveBuffer as the per-element scratch buffer (same
    // pattern as full GenerateAllPrimitives). It must be empty at entry
    // because the previous full regen pops it back to scratchStart at the
    // end of every FinalizeElementSlots call.
    PrimitiveGenContext ctx{m_SdfPrimitiveBuffer, m_PersistentClipRects,
                            m_SdfTextureRegistry.get(), this,
                            static_cast<float>(m_LastLayoutWidth),
                            static_cast<float>(m_LastLayoutHeight),
                            std::max(0.01f, m_ContentScale)};
    ctx.DrainModeOnly = true;
    ctx.ClipStack.reserve(kInitialClipStackCapacity);

    bool ok = true;
    // Per-store touch tracking: bump exactly the store versions this drain
    // mutated (a text re-emit must not force draw-order/clip re-uploads),
    // and range-mark the primitive ring at every write so the upload can
    // rewrite just the touched slot span.
    bool touchedPrimitives = false;
    bool touchedClips = false;
    bool touchedDrawOrder = false;
    auto bailToFullRegen = [&]() {
        MarkPrimitivesNeedRegen(0x400u);
        ok = false;
    };
    auto hasDrawOrderSlice = [&](uint32_t start, size_t count) {
        return start != 0xFFFFFFFFu &&
               static_cast<size_t>(start) <= m_DrawOrder.size() &&
               count <= (m_DrawOrder.size() - static_cast<size_t>(start));
    };

    // ---------------- Phase 1: collect (UI thread, queue order) -----------
    // Order-dependent shared bookkeeping stays here: RG slot recycling,
    // stale-bookkeeping escalation, clip-slot rewrites (ancestors precede
    // descendants in the queue). Emission-facing caches are pre-warmed so
    // the emit phase can run on JobSystem workers without touching them.
    //
    // Storage is thread_local only to retain capacity across drains; all
    // cross-thread access below goes through explicit references — a lambda
    // body naming a thread_local reads the EXECUTING thread's (empty)
    // instance, never the UI thread's.
    static thread_local std::vector<DrainItem> sItemsStorage;
    std::vector<DrainItem>& items = sItemsStorage;
    items.clear();
    items.reserve(m_PrimitiveDataDirty.size());

    for (UIElement* el : m_PrimitiveDataDirty)
    {
        if (!el) continue;
        el->m_InQueueFlags &= ~UIElement::InPrimitiveDataDirty;
        if (!ok)
            continue;

        // Release this element's deferred RG slot reservation(s) before any
        // early-out below. The re-emit path reserves fresh slots, and unlike
        // GenerateAllPrimitives the drain has no release-all step, so without
        // this an RG-backed element (e.g. the live scene view) leaks one
        // descriptor slot per frame until the pool fills. Doing it ahead of the
        // skip/bail guards also frees the slot of an element that is now hidden
        // or has lost its range (it won't be re-emitted, so it must not keep a
        // reservation). Order-preserving compaction keeps the deferred-binding
        // hash stable for the entries that survive.
        if (m_SdfTextureRegistry && !m_SdfDeferredRGBindings.empty())
        {
            size_t write = 0;
            for (size_t read = 0; read < m_SdfDeferredRGBindings.size(); ++read)
            {
                DeferredRGBinding& b = m_SdfDeferredRGBindings[read];
                if (b.Owner == el)
                {
                    if (b.Slot != 0)
                        m_SdfTextureRegistry->ReleaseSlot(b.Slot);
                    continue;
                }
                if (write != read)
                    m_SdfDeferredRGBindings[write] = b;
                ++write;
            }
            m_SdfDeferredRGBindings.resize(write);
        }

        // Elements the last full DFS never processed (clip-culled subtree,
        // created since, or under a display/opacity/size gate that has since
        // changed) carry stale slot-range / DrawOrder bookkeeping — a stale
        // m_DrawOrderIdx bounds-checks fine but indexes entries now owned by
        // OTHER elements, so slice surgery on it corrupts the draw order. If
        // such an element is now visible inside its nearest recorded clip
        // (it scrolled back into view), only a full DFS can re-establish its
        // slots — escalate. Still-clipped ones stay invisible: skip.
        if (el->m_LastFullDfsFrame != m_LastFullDfsGenFrame)
        {
            // Own-flags gate: a hidden overlay (display:none / opacity:0)
            // marked VisualDirty while dormant must not escalate — its stale
            // rect can intersect the viewport indefinitely, which would turn
            // every such mark into a full regen.
            const ResolvedStyle& rs = el->GetResolvedStyle();
            const bool selfVisible = rs.Layout.DisplayMode != DisplayMode::None &&
                                     rs.Visual.Opacity > 0.0f;
            if (selfVisible && ElementRectIntersectsNearestClip(el))
                bailToFullRegen();
            continue;
        }

        // Drain-mode emit skips PushClip, so an element that owns a clip slot
        // (overflow != visible) would leave its children clipped against the
        // pre-mutation rect. Rewrite the slot in place from the current layout
        // rect, re-intersected with the recorded parent clip (unchanged under
        // the drain invariant). Must run before the range early-outs below —
        // a pure-layout clip container owns a slot but no primitive range.
        if (el->m_ClipSlotIdx != UI::kNoClip &&
            static_cast<size_t>(el->m_ClipSlotIdx) < m_PersistentClipRects.size())
        {
            UI::UIClipRect& cr = m_PersistentClipRects[el->m_ClipSlotIdx];
            const float clipScale = std::max(0.01f, m_ContentScale);
            float cx = el->GetLayoutX() * clipScale;
            float cy = el->GetLayoutY() * clipScale;
            float cw = std::max(0.0f, el->GetLayoutWidth() * clipScale);
            float ch = std::max(0.0f, el->GetLayoutHeight() * clipScale);
            // Every slot masks at the padding box — mirror the inset both
            // push sites apply, or the first drain would re-expand the mask
            // over the element's border.
            const ResolvedStyle& elRs = el->GetResolvedStyle();
            const VisualStyle& elVs = elRs.Visual;
            const Box4& elBw = elRs.Layout.BorderWidth;
            InsetToPaddingBox(elBw, clipScale, cx, cy, cw, ch);
            CornerRadiiTLTRBRBL radii = ScaleRadii(
                InnerClipRadii(UsedBorderRadius(elVs, el->GetLayoutWidth(), el->GetLayoutHeight()),
                               elBw),
                clipScale);
            // The own-text outset, from the cut verdict of the element's last
            // emission: a drain changes neither the layout nor the text, and
            // a worker emitting this element after collect reuses this rect
            // as written. An element without a shape cache always re-emits on
            // the UI thread, whose push writes the shape again. Only a leaf
            // with visible effects can grow, so only it reads the verdict.
            if (!ClipSlotPushedForChildren(el) && elVs.TextEffects.HasVisibleEffect())
            {
                const ITextMeasurable* textEl = el->GetTextMeasurable();
                const bool textCut =
                    !textEl || textEl->GetOrCreateTextShapeCache().LastRunCut.load(std::memory_order_relaxed);
                OutsetClipShape(OwnTextClipOutsetPx(el, elVs, clipScale, textCut), cx, cy, cw, ch, radii);
            }
            // Same snap PushClip applies on push — the drain's rewrite must
            // reproduce the exact stored shape, or a drained frame re-expands
            // the mask by a fraction of a pixel.
            SnapPaintRect(cx, cy, cw, ch);
            if (cr.ParentIndex != UI::kNoClip &&
                static_cast<size_t>(cr.ParentIndex) < m_PersistentClipRects.size())
            {
                const UI::UIClipRect& parent = m_PersistentClipRects[cr.ParentIndex];
                const float px1 = parent.Rect[0];
                const float py1 = parent.Rect[1];
                const float px2 = px1 + parent.Rect[2];
                const float py2 = py1 + parent.Rect[3];
                const float cx2 = cx + cw;
                const float cy2 = cy + ch;
                cx = std::max(cx, px1);
                cy = std::max(cy, py1);
                cw = std::max(0.0f, std::min(cx2, px2) - cx);
                ch = std::max(0.0f, std::min(cy2, py2) - cy);
            }
            cr.Rect[0] = cx;
            cr.Rect[1] = cy;
            cr.Rect[2] = cw;
            cr.Rect[3] = ch;
            cr.Radii[0] = radii.TopLeft.X;
            cr.Radii[1] = radii.TopRight.X;
            cr.Radii[2] = radii.BottomRight.X;
            cr.Radii[3] = radii.BottomLeft.X;
            cr.RadiiY[0] = radii.TopLeft.Y;
            cr.RadiiY[1] = radii.TopRight.Y;
            cr.RadiiY[2] = radii.BottomRight.Y;
            cr.RadiiY[3] = radii.BottomLeft.Y;
            touchedClips = true;
        }

        // Skip elements that don't currently own a slot range — they emitted
        // nothing at their last DFS, so a translation or visual mark has
        // nothing to re-emit. This skip must never escalate: scroll
        // translation enqueues the whole scrolled subtree, spacers and
        // zero-size boxes included, and stays drain-only by contract
        // (ZeroSizeParentsChildFollowsAScrollDrain pins it). The one mark a
        // rangeless element cannot afford to lose — its first real content —
        // never reaches this queue: UIManagerNotifyElementContentDirty
        // escalates it to a full regen at the producer (cause 0x800).
        //
        // Zero-size elements that DO own a range are not skipped: the emit
        // path below produces zero primitives for them, which routes through
        // the DrawOrder-slice shrink and range free — skipping instead would
        // leave the old primitives rendering at the stale rect until an
        // unrelated full regen.
        if (el->m_PrimitiveRangeCap == 0 || el->m_PrimitiveRangeCount == 0)
        {
            // One exception to the never-escalate contract above: a rangeless
            // element whose CURRENT style would paint has first-time content
            // that only a full DFS can allocate slots for — e.g. a menu row
            // that was transparent at its last DFS and just gained its :hover
            // background from an in-place re-bake. Dropping that mark leaves
            // the highlight invisible until an unrelated full regen (the
            // "hover only works while the console spams" bug). The scrolled
            // spacers the contract protects resolve no paint and still skip.
            const VisualStyle& vs = el->GetResolvedStyle().Visual;
            const Box4& bw = el->GetResolvedStyle().Layout.BorderWidth;
            const bool paintsNow =
                (vs.BackgroundColor & 0xFF000000u) != 0u ||
                vs.BackgroundImage.HasImage ||
                (vs.OutlineStyle != ::GameEngine::BorderStyle::None &&
                 (vs.OutlineColor & 0xFF000000u) != 0u) ||
                ((bw.Left > 0.0f || bw.Top > 0.0f || bw.Right > 0.0f || bw.Bottom > 0.0f) &&
                 ((vs.BorderColor.Top | vs.BorderColor.Right |
                   vs.BorderColor.Bottom | vs.BorderColor.Left) & 0xFF000000u) != 0u);
            // The same holds for a control that draws its own text: it culls
            // its lines to the scroll viewport, so a text area below the view
            // at its last DFS emitted nothing, and scrolling it into view is
            // exactly this drain. Only re-emitting it answers whether it now
            // paints, so the probe runs after the cheaper rect test.
            if (ElementRectIntersectsNearestClip(el) &&
                (paintsNow || OwnTextEmissionPaints(el, ctx)))
                bailToFullRegen();
            continue;
        }
        const size_t existingSlotStart = static_cast<size_t>(el->m_PrimitiveRangeStart);
        if (el->m_PrimitiveRangeStart == UI::SlotAllocator::kInvalidSlot ||
            existingSlotStart >= m_PersistentPrimitives.size() ||
            !hasDrawOrderSlice(el->m_DrawOrderIdx, el->m_PrimitiveRangeCount))
        {
            bailToFullRegen();
            continue;
        }

        // The ambient clip recorded at the element's last full-DFS emission.
        // Drain assumes the parent clip chain is unchanged since then
        // (visual-only mutation invariant). Reading the stamp instead of the
        // baked primitives matters because glyphs of overflowing text wear
        // the element's own slot, not the ambient — and overlay/Mount
        // subtrees' ambient doesn't follow tree ancestry.
        const uint16_t preservedClipIdx = el->m_AmbientClipIdx;

        // Build the work item; emission happens in the emit phase below,
        // possibly on a JobSystem worker, so pre-warm every shared cache the
        // emit path reads. UiThreadOnly items (observed custom emission,
        // TextInput caret overlays) skip the warm — they emit inline during
        // apply exactly like the pre-MT-3 drain did.
        DrainItem item;
        item.El = el;
        item.PreservedClipIdx = preservedClipIdx;
        // UI-thread pins: observed custom emission, caret/selection overlays
        // (TextInput), and controls that render their own text (TextArea
        // family — their emission shapes on the shared primary face and is
        // not covered by the manager's shape-cache pre-warm).
        auto* textMeasurable = el->GetTextMeasurable();
        item.UiThreadOnly = el->m_CustomEmitObserved ||
                            el->GetAsTextInput() != nullptr ||
                            (textMeasurable && textMeasurable->HandlesOwnTextRendering());
        if (!item.UiThreadOnly)
        {
            // Warm only what the emit path will actually paint. The ancestor
            // term is the drain's alone — the DFS accumulates opacity down the
            // stack, this walk has no stack — so it rides alongside the shared
            // gate. A mismatch is harmless (the worker escalates on the
            // resulting cache miss).
            const ResolvedStyle& rs = el->GetResolvedStyle();
            const float pcs = ctx.ContentScale;
            const float ew = el->GetLayoutWidth() * pcs;
            const float eh = el->GetLayoutHeight() * pcs;
            const bool selfEmits =
                EmitWalkPaintsElement(*el) &&
                el->m_PrimitiveAncestorOpacity * rs.Visual.Opacity > 0.0f;
            if (selfEmits)
            {
                // Fill m_FrameFontResolveCache so worker lookups are hits.
                (void)ResolveFontForVisualStyle(rs.Visual, ctx);
                const std::string& elText = el->GetTextContent();
                if (!elText.empty())
                {
                    auto* textEl = el->GetTextMeasurable();
                    if (!textEl || !textEl->HandlesOwnTextRendering())
                        EmitTextPrimitives(el, ctx, rs,
                                           el->GetLayoutX() * pcs, el->GetLayoutY() * pcs,
                                           ew, eh, elText, /*warmOnly=*/true);
                }
                if (rs.Visual.BackgroundImage.HasImage ||
                    el->HasBackgroundImageTextureOverride())
                {
                    item.Bg = ResolveBackgroundTextureSlot(el, ctx, rs.Visual.BackgroundImage, ew, eh);
                    item.HasPreResolvedBg = true;
                }
            }
        }
        items.push_back(item);
    }

    if (!ok)
    {
        // Collect bailed — the fall-through full regen rewrites everything
        // this same frame, so skip emit/apply for the collected items. Bump
        // the clip store if collect already rewrote clip slots (harmless —
        // the regen bumps every store again anyway).
        if (touchedClips)
            ++m_ClipStoreVersion;
        m_PrimitiveDataDirty.clear();
        return false;
    }

    // ---------------- Phase 2: emit ---------------------------------------
    // Each work item re-emits its own primitives from warm caches into a
    // per-task scratch buffer — disjoint outputs, read-only shared state
    // (enforced by the OffThread tripwires). Forked across JobSystem workers
    // when the drain is large enough for fork-join to win; below that the
    // same loop runs inline. Apply order is by item index either way, so the
    // frame output is identical to the sequential path.
    static const bool sParallelDrainEnabled = [] {
        const char* e = std::getenv("GE_UI_PARALLEL_DRAIN");
        return !(e && e[0] == '0'); // default ON; GE_UI_PARALLEL_DRAIN=0 disables
    }();
    // Below this many items per task, fork-join overhead beats the win.
    // Env-overridable so soaks can force the fork path on small drains
    // (GE_UI_PARALLEL_DRAIN_MIN_ITEMS=1 forks everything with >= 2 items).
    static const size_t sMinItemsPerTask = [] {
        const char* e = std::getenv("GE_UI_PARALLEL_DRAIN_MIN_ITEMS");
        const long v = e ? std::atol(e) : 0;
        return v > 0 ? static_cast<size_t>(v) : static_cast<size_t>(8);
    }();

    size_t numTasks = 1;
    if (sParallelDrainEnabled && m_JobSystem)
    {
        // +1 lane: DispatchAndWait runs the first task on this thread.
        const size_t lanes = m_JobSystem->GetWorkerCount() + 1;
        numTasks = std::max<size_t>(
            1, std::min(lanes, (items.size() + sMinItemsPerTask - 1) / sMinItemsPerTask));
    }

    static thread_local std::vector<std::vector<UI::UIPrimitive>> sTaskScratchStorage;
    std::vector<std::vector<UI::UIPrimitive>>& taskScratch = sTaskScratchStorage;
    if (taskScratch.size() < numTasks)
        taskScratch.resize(numTasks);

    // Runs on JobSystem workers: everything it touches arrives via explicit
    // references/parameters (see the thread_local note above) or via `this`
    // members that are read-only during the emit phase.
    const auto emitSlice = [this](std::vector<DrainItem>& sliceItems,
                                  std::vector<UI::UIPrimitive>& scratch,
                                  uint32_t taskIdx, size_t sliceBegin, size_t sliceEnd,
                                  bool offThread)
    {
        scratch.clear();
        PrimitiveGenContext ectx{scratch, m_PersistentClipRects,
                                 m_SdfTextureRegistry.get(), this,
                                 static_cast<float>(m_LastLayoutWidth),
                                 static_cast<float>(m_LastLayoutHeight),
                                 std::max(0.01f, m_ContentScale)};
        ectx.DrainModeOnly = true;
        ectx.OffThread = offThread;
        for (size_t i = sliceBegin; i < sliceEnd; ++i)
        {
            DrainItem& it = sliceItems[i];
            if (it.UiThreadOnly)
                continue; // emitted inline during apply
            ectx.EscalateToUiThread = false;
            ectx.PreResolvedBg = it.HasPreResolvedBg ? &it.Bg : nullptr;
            // Seed emission from the stored ancestor-chain product, NOT the
            // baked primitive opacity: the baked value already includes this
            // element's own opacity, so reusing it would multiply self in a
            // second time on every drain.
            ectx.EffectiveOpacity = it.El->m_PrimitiveAncestorOpacity;
            // Ambient clip for a text-overflow PushClip. Only the inline
            // (offThread=false) slice can reach that push — workers escalate
            // first — but seeding is per-item state either way.
            ectx.DrainAmbientClipIdx = it.PreservedClipIdx;
            const size_t start = scratch.size();
            GeneratePrimitivesForElement(it.El, ectx, 0.0f, 0.0f);
            if (ectx.EscalateToUiThread)
            {
                scratch.resize(start);
                // Reset the re-entry stamp so the apply-phase re-emit isn't
                // rejected as a same-frame duplicate visit.
                it.El->m_LastPrimitiveGenFrame = 0;
                it.Escalated = true;
                continue;
            }
            it.Task = taskIdx;
            it.EmitBegin = static_cast<uint32_t>(start);
            it.EmitCount = static_cast<uint32_t>(scratch.size() - start);
            PatchAmbientClip(scratch.data() + it.EmitBegin, it.EmitCount, it.PreservedClipIdx);
        }
    };

    uint32_t forkedTasks = 1;
    if (numTasks >= 2)
    {
        const size_t perTask = (items.size() + numTasks - 1) / numTasks;
        std::vector<std::function<void()>> tasks;
        tasks.reserve(numTasks);
        for (size_t t = 0, b = 0; t < numTasks && b < items.size(); ++t, b += perTask)
        {
            const size_t e = std::min(b + perTask, items.size());
            // Explicit pointers: the worker must operate on the UI thread's
            // items/scratch objects, never its own thread_local instances.
            std::vector<DrainItem>* itemsPtr = &items;
            std::vector<UI::UIPrimitive>* scratchPtr = &taskScratch[t];
            tasks.push_back([&emitSlice, itemsPtr, scratchPtr, t, b, e]
                            { emitSlice(*itemsPtr, *scratchPtr, static_cast<uint32_t>(t), b, e,
                                        /*offThread=*/true); });
        }
        forkedTasks = static_cast<uint32_t>(tasks.size());
        JobSystem::DispatchAndWait(m_JobSystem, tasks.data(), forkedTasks);
    }
    else
    {
        emitSlice(items, taskScratch[0], 0, 0, items.size(), /*offThread=*/false);
    }

    // ---------------- Phase 3: apply (UI thread, queue order) -------------
    // All order-sensitive mutation lives here: allocator ops, memcpy into
    // persistent slot ranges, DrawOrder splices, ring range-marks — applied
    // in item order so output matches the sequential path byte-for-byte.
    // Escalated and UiThreadOnly items re-emit inline first (same code path,
    // on-thread, exactly like the pre-MT-3 drain).
    uint32_t escalatedCount = 0;
    for (DrainItem& item : items)
    {
        if (!ok)
            break; // full regen follows; it rewrites everything

        UIElement* el = item.El;
        const UI::UIPrimitive* emitted = nullptr;
        size_t emittedCount = 0;
        size_t inlineStart = 0;
        const bool inlineEmit = item.UiThreadOnly || item.Escalated;
        if (inlineEmit)
        {
            if (item.Escalated)
                ++escalatedCount;
            ctx.EscalateToUiThread = false;
            ctx.PreResolvedBg = item.HasPreResolvedBg ? &item.Bg : nullptr;
            ctx.EffectiveOpacity = el->m_PrimitiveAncestorOpacity;
            ctx.DrainAmbientClipIdx = item.PreservedClipIdx;
            inlineStart = ctx.Primitives.size();
            GeneratePrimitivesForElement(el, ctx, 0.0f, 0.0f);
            ctx.DrainAmbientClipIdx = UI::kNoClip;
            emittedCount = ctx.Primitives.size() - inlineStart;
            PatchAmbientClip(ctx.Primitives.data() + inlineStart, emittedCount,
                             item.PreservedClipIdx);
            emitted = ctx.Primitives.data() + inlineStart;
        }
        else
        {
            emitted = taskScratch[item.Task].data() + item.EmitBegin;
            emittedCount = item.EmitCount;
        }

        // Block E1 step 4/5: handle any primitive-count change inline.
        //   * fits-in-cap shrink/grow: keep slot range, edit DrawOrder
        //   * grow past cap: try TryGrowInPlace, else relocate (free + alloc new)
        //   * shrink to 0 with cap > 0: free slot range
        //
        // Per-element counts are 16-bit throughout the slot bookkeeping; an
        // element emitting past that (pathological — ~65k glyphs) would
        // truncate the allocator request below while memcpying the full
        // span. Bail to the full regen instead of corrupting the heap.
        if (emittedCount > std::numeric_limits<uint16_t>::max())
        {
            bailToFullRegen();
            continue;
        }
        const uint16_t oldCount = el->m_PrimitiveRangeCount;
        const uint16_t emittedCount16 = static_cast<uint16_t>(emittedCount);
        constexpr uint32_t kInvalid = 0xFFFFFFFFu;
        bool didRelocate = false;
        uint32_t writeStart = el->m_PrimitiveRangeStart;

        if (emittedCount > el->m_PrimitiveRangeCap)
        {
            // Try in-place grow first (cheap: extends the existing range
            // when the next slots in m_PersistentPrimitives are free).
            const uint16_t grown = m_PrimitiveAllocator.TryGrowInPlace(
                el->m_PrimitiveRangeStart, el->m_PrimitiveRangeCap,
                emittedCount16);
            if (grown != 0)
            {
                el->m_PrimitiveRangeCap = grown;
                // Same start address, larger cap. The fits-in-cap path
                // below handles the DrawOrder shift.
            }
            else
            {
                // Relocate: free old, allocate new range. Slot start
                // address changes — m_DrawOrder slice and downstream
                // m_DrawOrderIdx need full-replace + shift.
                const uint32_t oldStart = el->m_PrimitiveRangeStart;
                const uint16_t oldCap = el->m_PrimitiveRangeCap;
                m_PrimitiveAllocator.Free(oldStart, oldCap);
                uint16_t newCap = 0;
                const uint32_t newStart = m_PrimitiveAllocator.Allocate(
                    emittedCount16, newCap);
                el->m_PrimitiveRangeStart = newStart;
                el->m_PrimitiveRangeCap   = newCap;
                writeStart = newStart;
                didRelocate = true;
            }
        }

        // Both grow branches may advance the allocator's high-water mark
        // past the CPU mirror — TryGrowInPlace extends m_TotalSlots, and
        // Allocate may too when no free range fits. Resize the mirror to
        // cover the full granted cap before the memcpy below.
        const size_t neededSize =
            static_cast<size_t>(writeStart) + el->m_PrimitiveRangeCap;
        if (m_PersistentPrimitives.size() < neededSize)
            m_PersistentPrimitives.resize(neededSize);

        // Copy into the (possibly new) slot range. The recorded clip index
        // was already patched onto the emitted primitives in the emit phase.
        if (emittedCount > 0)
        {
            std::memcpy(&m_PersistentPrimitives[writeStart], emitted,
                        emittedCount * sizeof(UI::UIPrimitive));
            touchedPrimitives = true;
            if (m_SdfPrimRing)
                m_SdfPrimRing->MarkRangeDirty(writeStart, emittedCount);
        }
        if (inlineEmit)
            ctx.Primitives.resize(inlineStart);

        // Relocate path: replace the entire DrawOrder slice with new
        // slot indices and shift downstream. Then we're done with this
        // element; skip the count-change path below.
        if (didRelocate)
        {
            const uint32_t doIdx = el->m_DrawOrderIdx;
            if (doIdx != kInvalid)
            {
                if (!hasDrawOrderSlice(doIdx, oldCount))
                {
                    bailToFullRegen();
                    continue;
                }
                // Erase old slice then reserve emittedCount placeholders
                // and patch sequential slot indices in place — avoids a
                // temp vector allocation each relocate.
                m_DrawOrder.erase(m_DrawOrder.begin() + doIdx,
                                  m_DrawOrder.begin() + doIdx + oldCount);
                m_DrawOrder.insert(m_DrawOrder.begin() + doIdx, emittedCount, 0u);
                for (size_t i = 0; i < emittedCount; ++i)
                    m_DrawOrder[doIdx + i] = writeStart + static_cast<uint32_t>(i);
                const int32_t delta = static_cast<int32_t>(emittedCount) - static_cast<int32_t>(oldCount);
                if (delta != 0)
                    ShiftDrawOrderIndicesAfter(doIdx + oldCount, delta);
                touchedDrawOrder = true;
            }
            el->m_PrimitiveRangeCount = static_cast<uint16_t>(emittedCount);
            continue;
        }

        // m_DrawOrder slice maintenance for count changes. The element's
        // slice currently lives at [doIdx, doIdx + oldCount). After this
        // drain it should be [doIdx, doIdx + emittedCount) with slot
        // indices [start, start + emittedCount).
        if (emittedCount != oldCount)
        {
            const uint32_t doIdx = el->m_DrawOrderIdx;
            const uint32_t slotStart = el->m_PrimitiveRangeStart;
            if (doIdx != kInvalid)
            {
                if (!hasDrawOrderSlice(doIdx, oldCount))
                {
                    bailToFullRegen();
                    continue;
                }
                const uint32_t emittedCount32 = static_cast<uint32_t>(emittedCount);
                if (emittedCount < oldCount)
                {
                    // Shrink: erase tail entries. Downstream entries
                    // shift left by (oldCount - emittedCount).
                    const uint16_t shrink = static_cast<uint16_t>(oldCount - emittedCount);
                    m_DrawOrder.erase(m_DrawOrder.begin() + doIdx + emittedCount,
                                      m_DrawOrder.begin() + doIdx + oldCount);
                    ShiftDrawOrderIndicesAfter(doIdx + emittedCount32, -static_cast<int32_t>(shrink));
                }
                else
                {
                    // Grow (still fits in cap): insert placeholders after
                    // the existing slice and patch sequential slot
                    // indices in place. Downstream shifts right.
                    const uint16_t growBy = static_cast<uint16_t>(emittedCount - oldCount);
                    m_DrawOrder.insert(m_DrawOrder.begin() + doIdx + oldCount, growBy, 0u);
                    for (size_t i = 0; i < growBy; ++i)
                        m_DrawOrder[doIdx + oldCount + i] = slotStart + static_cast<uint32_t>(oldCount + i);
                    ShiftDrawOrderIndicesAfter(doIdx + oldCount, static_cast<int32_t>(growBy));
                }
                touchedDrawOrder = true;
            }
            el->m_PrimitiveRangeCount = static_cast<uint16_t>(emittedCount);
        }

        // Block E1 step 5: free slot range when an element shrinks to 0
        // primitives (e.g. background-color removed, displayMode flipped
        // to None). Otherwise the slot range sits dormant — allocator
        // still considers cap slots owned, wasting CPU mirror space.
        if (emittedCount == 0 && el->m_PrimitiveRangeCap > 0)
        {
            m_PrimitiveAllocator.Free(el->m_PrimitiveRangeStart, el->m_PrimitiveRangeCap);
            el->m_PrimitiveRangeStart = kInvalid;
            el->m_PrimitiveRangeCap   = 0;
            el->m_DrawOrderIdx        = kInvalid;
        }

        // Note: we do NOT call el->ClearDirty(VisualDirty) here. ClearDirty
        // also calls Overrides::ClearDirty() which wipes m_NeedsCascadeRerun
        // — and the cascade still needs that signal. Since no production
        // code reads UIElement::VisualDirty as a state, leaving it set is
        // harmless; the bit is effectively just "was queued for drain".
    }

    m_PrimitiveDataDirty.clear();

    // Bump exactly what changed. On a bail (ok=false) the fall-through full
    // regen bumps everything anyway; these extra bumps are harmless.
    if (touchedPrimitives)
        ++m_PrimitiveStoreVersion;
    if (touchedClips)
        ++m_ClipStoreVersion;
    if (touchedDrawOrder)
        ++m_DrawOrderStoreVersion;

    // Drain-shape counters for soak verification (read via profile history).
    if (m_UpdateProfilingEnabled && !m_UpdateProfilingHistory.empty())
    {
        auto& back = m_UpdateProfilingHistory.back();
        back.DrainItems         = static_cast<uint32_t>(items.size());
        back.DrainParallelTasks = (numTasks >= 2) ? forkedTasks : 1;
        back.DrainEscalated     = escalatedCount;
    }

    return ok;
#else
    return true;
#endif
}

const UI::UIPrimitive* UIManager::PeekPrimitiveForTesting(const UIElement& el, uint16_t index) const
{
    if (el.m_PrimitiveRangeStart == UI::SlotAllocator::kInvalidSlot ||
        index >= el.m_PrimitiveRangeCount)
        return nullptr;
    const size_t slot = static_cast<size_t>(el.m_PrimitiveRangeStart) + index;
    return slot < m_PersistentPrimitives.size() ? &m_PersistentPrimitives[slot] : nullptr;
}

const UI::UIClipRect* UIManager::PeekClipRectForTesting(uint16_t slot) const
{
    return slot < m_PersistentClipRects.size() ? &m_PersistentClipRects[slot] : nullptr;
}

int UIManager::FindDrawOrderPosForTesting(const UIElement& el) const
{
    if (el.m_PrimitiveRangeStart == UI::SlotAllocator::kInvalidSlot ||
        el.m_PrimitiveRangeCount == 0)
        return -1;
    for (size_t i = 0; i < m_DrawOrder.size(); ++i)
        if (m_DrawOrder[i] == el.m_PrimitiveRangeStart)
            return static_cast<int>(i);
    return -1;
}

void UIManager::FinalizeElementSlots(UIElement* el, PrimitiveGenContext& ctx, size_t scratchStart)
{
    const size_t scratchEnd = ctx.Primitives.size();
    size_t count = scratchEnd - scratchStart;

    // Per-element counts are 16-bit throughout the slot bookkeeping; an
    // element emitting past that (pathological — ~65k glyphs) would truncate
    // the allocator request below while memcpying the full span. The drain
    // path bails to this full pass on overflow; here there is nothing left
    // to bail to, so drop the excess primitives instead of corrupting the
    // heap.
    if (count > std::numeric_limits<uint16_t>::max())
    {
        Logger::Log::Warning("UIManager: element emitted {} primitives; clamping to {} (excess dropped)",
                             count, std::numeric_limits<uint16_t>::max());
        count = std::numeric_limits<uint16_t>::max();
    }

    if (count == 0)
    {
        // Element emits no primitives this frame. If it had a slot range
        // from a previous frame (e.g. background-color was just removed),
        // free it — there is no benefit to keeping a dormant range that
        // produces no rendering.
        if (el->m_PrimitiveRangeCap > 0)
        {
            m_PrimitiveAllocator.Free(el->m_PrimitiveRangeStart, el->m_PrimitiveRangeCap);
            el->m_PrimitiveRangeStart = UI::SlotAllocator::kInvalidSlot;
            el->m_PrimitiveRangeCount = 0;
            el->m_PrimitiveRangeCap = 0;
        }
        // Block E1: no slice in m_DrawOrder this frame.
        el->m_DrawOrderIdx = 0xFFFFFFFFu;
        return;
    }

    // Element wants `count` slots. Reuse the existing range if it fits;
    // otherwise grow in place at the tail or relocate.
    if (count > el->m_PrimitiveRangeCap)
    {
        if (el->m_PrimitiveRangeCap > 0)
        {
            const uint16_t grown = m_PrimitiveAllocator.TryGrowInPlace(
                el->m_PrimitiveRangeStart, el->m_PrimitiveRangeCap, static_cast<uint16_t>(count));
            if (grown != 0)
            {
                el->m_PrimitiveRangeCap = grown;
            }
            else
            {
                m_PrimitiveAllocator.Free(el->m_PrimitiveRangeStart, el->m_PrimitiveRangeCap);
                uint16_t newCap = 0;
                el->m_PrimitiveRangeStart = m_PrimitiveAllocator.Allocate(
                    static_cast<uint16_t>(count), newCap);
                el->m_PrimitiveRangeCap = newCap;
            }
        }
        else
        {
            uint16_t newCap = 0;
            el->m_PrimitiveRangeStart = m_PrimitiveAllocator.Allocate(
                static_cast<uint16_t>(count), newCap);
            el->m_PrimitiveRangeCap = newCap;
        }
    }
    el->m_PrimitiveRangeCount = static_cast<uint16_t>(count);

    // Ensure the persistent CPU mirror is sized to cover the full granted cap.
    const size_t neededSize = static_cast<size_t>(el->m_PrimitiveRangeStart) +
                              el->m_PrimitiveRangeCap;
    if (m_PersistentPrimitives.size() < neededSize)
        m_PersistentPrimitives.resize(neededSize);

    // Copy this element's primitives from the scratch tail into its slot range.
    std::memcpy(&m_PersistentPrimitives[el->m_PrimitiveRangeStart],
                ctx.Primitives.data() + scratchStart,
                count * sizeof(UI::UIPrimitive));

    // Push slot indices into DrawOrder in DFS pre-order. Stage 2 part 3 will
    // bind m_DrawOrder as a shader-side indirection; until then this is also
    // the order in which the compatibility copy in GenerateAllPrimitives
    // populates m_SdfPrimitiveBuffer for upload.
    //
    // Block E1 (sparse drain prep): record the slice start index on the
    // element so the future drain code can locate this element's entries
    // in m_DrawOrder in O(1) without re-walking the tree.
    el->m_DrawOrderIdx = static_cast<uint32_t>(m_DrawOrder.size());
    for (size_t i = 0; i < count; ++i)
        m_DrawOrder.push_back(el->m_PrimitiveRangeStart + static_cast<uint32_t>(i));

    // Pop the scratch — primitives now live in m_PersistentPrimitives.
    ctx.Primitives.resize(scratchStart);
}

// --- Emit-path gates -------------------------------------------------------
// The DFS below is the definition; these are the parts of it that other walks
// need to reproduce. Keeping them here means a change to the DFS's gates is a
// change to what every walk believes, instead of a change one of them misses.

bool UIManager::EmitClipsChildrenToOwnRect(const UIElement& el, const ResolvedStyle& style)
{
    // Virtualized controls opt their pooled rows/cells out (SetDisableClipCulling)
    // so offscreen overscan items keep baked geometry and scrolling reveals them
    // without a full regen. The opt-out suppresses exactly the culls this
    // predicate authorises, so it reads here as "does not take its subtree".
    return style.Layout.Overflow != Overflow::Visible && !el.IsClipCullingDisabled();
}

bool UIManager::EmitContributionBoundedByOwnRect(const UIElement& el, const ResolvedStyle& style)
{
    const VisualStyle& vs = style.Visual;
    // A shadow, glow or outline paints outside the rect, so the rect no longer
    // bounds what the element contributes and a rect that misses the ambient
    // clip is no longer evidence that nothing is visible. The outline belongs
    // here and not in the box model: its ring sits entirely beyond the border
    // edge, so it survives a cull the rect alone would authorise.
    const bool hasShadow = vs.ShadowSoftness > 0 || vs.ShadowOffsetX != 0 || vs.ShadowOffsetY != 0;
    const bool hasOuterShadow = hasShadow && !vs.ShadowInset;
    const bool hasGlow = vs.GlowRadius > 0 && (vs.GlowColor & 0xFF000000u) != 0;
    return !hasOuterShadow && !hasGlow && !HasOutlineRing(vs) && EmitClipsChildrenToOwnRect(el, style);
}

bool UIManager::EmitWalkEntersSubtree(const UIElement& el)
{
    const ResolvedStyle& style = el.GetResolvedStyle();
    if (style.Layout.DisplayMode == DisplayMode::None || style.Visual.Opacity <= 0.0f)
        return false;
    if (el.GetLayoutWidth() > 0.0f && el.GetLayoutHeight() > 0.0f)
        return true;
    // Zero area. The box paints nothing itself, but an overflowing or
    // absolutely-positioned child keeps its own geometry and still renders —
    // unless this box also clips its children to that empty rect. Only the
    // clipping question applies: a box with no area paints no shadow or glow
    // either, so whether its own paint would have spilled decides nothing.
    return !EmitClipsChildrenToOwnRect(el, style);
}

bool UIManager::EmitWalkPaintsElement(const UIElement& el)
{
    if (!EmitWalkEntersSubtree(el))
        return false;
    // visibility:hidden suppresses self-emission only, and a zero-area box has
    // nowhere to put a background, border or its own text.
    return el.GetResolvedStyle().Visual.Visible && el.GetLayoutWidth() > 0.0f &&
           el.GetLayoutHeight() > 0.0f;
}

void UIManager::GeneratePrimitivesForElement(UIElement* el, PrimitiveGenContext& ctx,
                                             float /*parentAbsX*/, float /*parentAbsY*/)
{
    if (!el)
        return;

    const ResolvedStyle& style = el->GetResolvedStyle();
    // CSS spec: display:none culls the entire subtree; visibility:hidden only
    // suppresses self-emission — descendants may override with visibility:visible
    // and still render. Visibility-gating is applied per-element below; here we
    // only hard-return on display:none.
    if (style.Layout.DisplayMode == DisplayMode::None)
        return;

    {
        static const bool sThumbTrace = [] {
            const char* v = std::getenv("GE_UI_THUMB_TRACE");
            return v && v[0] == '1';
        }();
        if (sThumbTrace && el->HasClass("slider-thumb"))
            Logger::Log::Info("[ThumbTrace] emit x={:.1f} drain={}", el->GetLayoutX(),
                              ctx.DrainModeOnly);
    }

    const float savedOpacity = ctx.EffectiveOpacity;
    el->m_PrimitiveAncestorOpacity = savedOpacity;
    ctx.EffectiveOpacity *= style.Visual.Opacity;

    if (ctx.EffectiveOpacity <= 0.0f)
    {
        ctx.EffectiveOpacity = savedOpacity;
        return;
    }

    // Yoga-computed layout rect is in CSS logical px (single space for
    // layout + hit-testing). Convert to physical px once here; every
    // primitive emitted below is in physical.
    const float cs = ctx.ContentScale;
    const float xL = el->GetLayoutX();
    const float yL = el->GetLayoutY();
    const float wL = el->GetLayoutWidth();
    const float hL = el->GetLayoutHeight();
    const float x = xL * cs;
    const float y = yL * cs;
    const float w = wL * cs;
    const float h = hL * cs;

    // A zero-area box has nowhere to put a background, border or its own text,
    // so it paints nothing of its own — but its descendants keep their own
    // geometry and still render (an overflowing or absolutely-positioned child
    // of a 0x0 parent is visible in every browser). Self-emission is gated on
    // this alongside visibility:hidden; the descent below runs regardless.
    const bool hasPaintArea = (w > 0.0f && h > 0.0f);

    // Percentage radii refer to this element's own border box, so they resolve
    // here — in the CSS-logical space the layout rect is in — and every radius
    // consumer below reads the resolved value. vs.BorderRadius on its own is the
    // style input and holds a bare percentage number on a percentage corner.
    const CornerRadiiTLTRBRBL usedRadii = UsedBorderRadius(style.Visual, wL, hL);

    // Stage 2: re-entrancy detector. An element being entered with the
    // current frame counter already stored means a synchronous event handler
    // or control extension caused a recursive call back into PrimitiveGen
    // for this element on the same frame. The first visit's slot data has
    // already been finalized into m_PersistentPrimitives; a second visit
    // would silently overwrite it with possibly different primitive data
    // and DOUBLE-push the slot indices into m_DrawOrder. Assert in Debug;
    // in Release, skip the duplicate to keep the slot data consistent.
    if (el->m_LastPrimitiveGenFrame == m_PrimitiveGenFrame)
    {
        assert(false && "PrimitiveGen re-entry on same frame for the same element");
        ctx.EffectiveOpacity = savedOpacity;
        return;
    }
    el->m_LastPrimitiveGenFrame = m_PrimitiveGenFrame;

    // Stage 2: snapshot the scratch buffer position. Self-emission below
    // pushes into ctx.primitives; FinalizeElementSlots after self-emission
    // pops [scratchStart, ctx.primitives.size()) into m_PersistentPrimitives.
    const size_t scratchStart = ctx.Primitives.size();

    const VisualStyle& vs = style.Visual;
    const uint16_t clipIdx = ctx.CurrentClipIndex();

    // Background rect
    const uint32_t bgColor = vs.BackgroundColor;
    const bool hasBg = (bgColor & 0xFF000000u) != 0;
    const bool hasBorder = vs.BorderStyle != BorderStyle::None &&
                           (style.Layout.BorderWidth.Top > 0 || style.Layout.BorderWidth.Right > 0 ||
                            style.Layout.BorderWidth.Bottom > 0 || style.Layout.BorderWidth.Left > 0);
    const bool hasShadow = vs.ShadowSoftness > 0 || vs.ShadowOffsetX != 0 || vs.ShadowOffsetY != 0;
    const bool hasGlow = vs.GlowRadius > 0 && (vs.GlowColor & 0xFF000000u) != 0;
    const bool hasOutline = HasOutlineRing(vs);

    // Subtree cull, in two questions that are not the same question.
    //
    // A zero-area box has nowhere to paint, so nothing it could contribute
    // survives its own rect and the descent turns purely on whether it clips
    // its children to that empty rect. Its shadow, glow and outline are not
    // asked about: they are not painted either (self-emission below gates on
    // the same hasPaintArea), so they cannot make an invisible subtree visible.
    //
    // A box WITH area is culled instead when its rect misses the ambient clip,
    // and that needs the stronger question. Here a shadow, glow or outline
    // really is painted, and it spills outside the very rect the miss was
    // measured against — so an element carrying one is not cullable from that
    // rect. Text content stays inside the rect, so it needs no term of its own.
    if (!hasPaintArea)
    {
        if (EmitClipsChildrenToOwnRect(*el, style))
        {
            ctx.EffectiveOpacity = savedOpacity;
            return;
        }
    }
    else if (EmitContributionBoundedByOwnRect(*el, style))
    {
        float clipL = 0.0f;
        float clipT = 0.0f;
        float clipR = ctx.ViewportW;
        float clipB = ctx.ViewportH;
        if (!ctx.ClipStack.empty())
        {
            const UI::UIClipRect& cur = ctx.ClipRects[ctx.ClipStack.back()];
            clipL = cur.Rect[0];
            clipT = cur.Rect[1];
            clipR = clipL + cur.Rect[2];
            clipB = clipT + cur.Rect[3];
        }
        if (x >= clipR || y >= clipB || x + w <= clipL || y + h <= clipT)
        {
            ctx.EffectiveOpacity = savedOpacity;
            return;
        }
    }

    // Full-DFS visit stamp — after the cull so culled subtrees stay unstamped
    // and the drain can detect their stale slot/DrawOrder bookkeeping. The
    // ambient clip is recorded alongside it (drain emission runs with an
    // empty clip stack, so its CurrentClipIndex is not the ambient).
    if (!ctx.DrainModeOnly)
    {
        el->m_LastFullDfsFrame = m_PrimitiveGenFrame;
        el->m_AmbientClipIdx = clipIdx;
    }

    // CSS visibility:hidden — skip self-emission but still recurse into
    // descendants (a child with visibility:visible should render). A zero-area
    // box takes the same route for the same reason. This gate must wrap every
    // self-emit path; the clip push, slot finalize, and child recursion below
    // run regardless so descendants see the correct clip stack and emit
    // normally.
    if (vs.Visible && hasPaintArea)
    {
        // The element's own paint box: the layout rect snapped to the device
        // grid (SnapPaintRect). Every layer this element paints into its border
        // box shares it — background colour, background image, dotted/dashed
        // bands — so they cannot drift half a device pixel apart. x/y/w/h stay
        // the layout rect for children, clips and hit-testing.
        float px = x, py = y, pw = w, ph = h;
        SnapPaintRect(px, py, pw, ph);

        if (hasBg || hasBorder || hasShadow || hasGlow)
        {
            // vs.* values are CSS logical px; scale to match the physical-px rect.
            UI::UIPrimitive prim = UI::MakeRect(px, py, pw, ph, UI::PackFromARGB(bgColor));
            ApplyCornerRadii(prim, usedRadii, cs);
            UI::SetClip(prim, clipIdx);
            UI::SetOpacity(prim, ctx.EffectiveOpacity);

            if (hasBorder && vs.BorderStyle == BorderStyle::Solid)
            {
                const uint32_t bc = DrawnBorderColor(style);
                UI::AddBorderLTRB(prim,
                    SnapBorderWidth(style.Layout.BorderWidth.Left * cs),
                    SnapBorderWidth(style.Layout.BorderWidth.Top * cs),
                    SnapBorderWidth(style.Layout.BorderWidth.Right * cs),
                    SnapBorderWidth(style.Layout.BorderWidth.Bottom * cs),
                    UI::PackFromARGB(bc));
            }

            if (hasShadow)
            {
                UI::AddShadow(prim,
                    vs.ShadowOffsetX * cs, vs.ShadowOffsetY * cs,
                    vs.ShadowSoftness * cs,
                    UI::PackFromARGB(vs.ShadowColor));
                if (vs.ShadowInset)
                    prim.ModeAndFlags |= UI::kPrimInsetShadowBit;
            }

            if (hasGlow)
            {
                UI::AddGlow(prim, vs.GlowRadius * cs, UI::PackFromARGB(vs.GlowColor));
            }

            if (hasShadow || hasGlow)
                UI::ExpandForEffects(prim);

            ctx.Primitives.push_back(prim);

            if (hasBorder && vs.BorderStyle == BorderStyle::Dotted)
                EmitDottedBorder(ctx.Primitives, style, usedRadii, px, py, pw, ph, cs, clipIdx, ctx.EffectiveOpacity);
            else if (hasBorder && vs.BorderStyle == BorderStyle::Dashed)
                EmitDashedBorder(ctx.Primitives, style, usedRadii, px, py, pw, ph, cs, clipIdx, ctx.EffectiveOpacity);
        }

        // Background image (CSS background-image or per-element live texture override)
        if (vs.BackgroundImage.HasImage || el->HasBackgroundImageTextureOverride())
        {
            EmitBackgroundImagePrimitive(el, ctx, vs, style.Layout.BorderWidth, px, py, pw, ph, clipIdx);
        }

        // Text and its input overlays. Emission order is composite order and
        // CSS fixes the stack: a selection highlight is a background and goes
        // UNDER the glyphs, the caret goes over them. All three place against
        // the input's internal scroll offset, so that is settled first and
        // nothing below writes it.
        auto* input = el->GetAsTextInput();
        TextInputOverlays overlays{};
        if (input)
        {
            const bool isFocused = el->IsFocusTargetForId(ctx.Manager->GetFocusedElementId());
            RefreshTextInputScroll(input, ctx, style, x, y, w, h, isFocused);
            overlays = BuildTextInputOverlays(input, ctx, style, x, y, w, h, isFocused);
        }

        if (overlays.HasSelection)
            ctx.Primitives.push_back(overlays.Selection);

        // Text content (skipped for controls that handle their own text rendering)
        const std::string& text = el->GetTextContent();
        if (!text.empty())
        {
            auto* textEl = el->GetTextMeasurable();
            if (!textEl || !textEl->HandlesOwnTextRendering())
                EmitTextPrimitives(el, ctx, style, x, y, w, h, text);
        }

        if (overlays.HasCaret)
            ctx.Primitives.push_back(overlays.Caret);

        // Control-specific primitives (slider tracks, dock overlays, etc.)
        {
            FontAtlas* resolvedFont = ctx.Manager->ResolveFontForVisualStyle(style.Visual, ctx);
            UI::PrimitiveEmitContext emitCtx{ctx.Primitives, clipIdx, ctx.EffectiveOpacity, ctx.Textures,
                                             ctx.Manager, resolvedFont, ctx.Manager->GetContentScale()};
            emitCtx.Effects = vs.TextEffects;
            emitCtx.OffThread = ctx.OffThread;
            emitCtx.EscalateFlag = &ctx.EscalateToUiThread;
            const size_t beforeCustom = ctx.Primitives.size();
            el->OnGeneratePrimitives(emitCtx, style, x, y, w, h);
            // Sticky custom-emission marker: the parallel drain re-emits
            // elements with observed custom emission on the UI thread
            // (custom emitters may touch shared text/measure state that has
            // no off-thread tripwire).
            if (ctx.Primitives.size() != beforeCustom)
            {
                if (!el->m_CustomEmitObserved && ctx.OffThread)
                {
                    // First-ever custom emission landed on a worker: the
                    // override lacks an OffThread tripwire (every shipped
                    // one has an escalate-and-return preamble). Escalate so
                    // the apply phase re-emits on the UI thread, and fail
                    // loudly in Debug so the missing tripwire gets added.
                    assert(false &&
                           "OnGeneratePrimitives emitted on a JobSystem worker before being "
                           "observed — add the OffThread escalation preamble to this override");
                    ctx.EscalateToUiThread = true;
                }
                el->m_CustomEmitObserved = true;
            }
        }

        // Outline last, so the ring paints over this element's own background,
        // border and content — CSS paints the outline after everything the
        // element itself draws. It deliberately carries `clipIdx`, the clip
        // this element paints under, and not the child clip pushed further
        // below: overflow on the element itself must not eat its own ring.
        if (hasOutline)
            EmitOutlineRing(ctx.Primitives, vs, usedRadii, x, y, w, h, cs, clipIdx, ctx.EffectiveOpacity);
    }

    // Block E1: in drain mode, skip FinalizeElementSlots (caller copies the
    // scratch into the existing slot range itself) and skip child recursion
    // entirely — only the dirty element is being refreshed.
    if (ctx.DrainModeOnly)
    {
        ctx.EffectiveOpacity = savedOpacity;
        return;
    }

    // Stage 2: finalize this element's primitives into a persistent slot
    // range, push DrawOrder entries in DFS pre-order, and pop the scratch.
    // After this call ctx.primitives.size() == scratchStart again.
    FinalizeElementSlots(el, ctx, scratchStart);

    // Push clip for overflow:hidden. Childless elements skip the push —
    // nothing would consume it, and the lazily-allocated slot would be
    // wasted (own-text overflow acquires the same slot inside
    // EmitTextPrimitives only when the text actually spills).
    bool pushedClip = false;
    if (style.Layout.Overflow != Overflow::Visible && ClipSlotPushedForChildren(el))
    {
        // CSS overflow clip region: the padding box, corners rounded by the
        // inner border edge. Clipping at the element rect with the outer
        // radii instead lets children paint over the border ring.
        float clipX = x, clipY = y, clipW = w, clipH = h;
        InsetToPaddingBox(style.Layout.BorderWidth, cs, clipX, clipY, clipW, clipH);
        ctx.PushClip(el, clipX, clipY, clipW, clipH,
                     ScaleRadii(InnerClipRadii(usedRadii, style.Layout.BorderWidth), cs));
        pushedClip = true;
    }

    // Recurse children in z-sorted order
    const auto& children = el->GetChildren();
    if (!children.empty())
    {
        const size_t base = ctx.SortedChildren.size();
        bool needsSort = false;
        for (auto& child : children)
        {
            ctx.SortedChildren.push_back(child.get());
            if (child->GetResolvedStyle().Layout.ZIndex != 0)
                needsSort = true;
        }
        if (needsSort)
        {
            std::stable_sort(
                ctx.SortedChildren.begin() + base,
                ctx.SortedChildren.end(),
                [](const UIElement* a, const UIElement* b) {
                    return a->GetResolvedStyle().Layout.ZIndex
                         < b->GetResolvedStyle().Layout.ZIndex;
                });
        }

        for (size_t i = base; i < ctx.SortedChildren.size(); ++i)
        {
            UIElement* child = ctx.SortedChildren[i];
            if (child->GetOverlayLayer() != OverlayLayer::None)
            {
                ctx.DeferredOverlays.push_back({child, x, y, child->GetOverlayLayer()});
                continue;
            }
            GeneratePrimitivesForElement(child, ctx, x, y);
        }

        ctx.SortedChildren.resize(base); // pop this level
    }

    // Recurse into Mount portal target (not part of normal m_Children).
    if (UIElement* tgt = el->GetMountTarget())
        GeneratePrimitivesForElement(tgt, ctx, x, y);

    if (pushedClip)
        ctx.PopClip();

    ctx.EffectiveOpacity = savedOpacity;
}

UIManager::ResolvedBgSlot UIManager::ResolveBackgroundTextureSlot(
    UIElement* el, PrimitiveGenContext& ctx,
    const BackgroundImageStyle& bgImg, float /*elW*/, float /*elH*/)
{
    // Resolution mutates shared caches (m_BgTextureCache, m_BgPathToGuid,
    // deferred RG bindings) and can trigger GPU uploads — UI-thread-only.
    // The drain pre-resolves work items during collect (ctx.PreResolvedBg),
    // so this only fires when that pre-resolution was skipped or missed.
    if (ctx.OffThread)
    {
        ctx.EscalateToUiThread = true;
        return {};
    }

    uint32_t texSlot = 0;
    uint32_t imgW = 0;
    uint32_t imgH = 0;
    uint32_t spaceBits = 0;
    NineSlice slice;

    if (texSlot == 0 && bgImg.Source.HasImage())
    {
        if (bgImg.Source.Kind == BackgroundImageSource::SourceKind::ResourceName &&
            !bgImg.Source.Value.empty())
        {
            const auto resolved = ctx.Manager->ResolveExternalTexture(bgImg.Source.Value);
            if (!resolved)
            {
                // Record the miss so the producer (e.g. the thumbnail service)
                // can render this name on demand — the prefetch only covers the
                // asset browser's own requests, not entity/inspector icons.
                ctx.Manager->NoteUnresolvedExternalTextureRequest(bgImg.Source.Value);
                return {0, 0, 0};
            }

            imgW = resolved->Width;
            imgH = resolved->Height;
            spaceBits = SpaceFlagBitsForSpace(resolved->Space);

            if (resolved->DirectTex.IsValid())
            {
                texSlot = ctx.Textures->Register(resolved->DirectTex, resolved->DirectSamp);
            }
            else if (resolved->Rg2Registered)
            {
                // RenderGraph-published name: the binding carries the NAME; RenderRG
                // resolves it against the per-frame publish map at declare
                // time (frame-local ids are never stored on the binding).
                texSlot = ctx.Textures->ReserveSlot();
                if (texSlot != 0)
                    ctx.Manager->m_SdfDeferredRGBindings.push_back(
                        {texSlot, el, bgImg.Source.Value});
            }
        }
        else if (bgImg.Source.Kind == BackgroundImageSource::SourceKind::Guid ||
                 bgImg.Source.Kind == BackgroundImageSource::SourceKind::Path)
        {
            GUID guid = bgImg.Source.Guid;
            if (bgImg.Source.Kind == BackgroundImageSource::SourceKind::Path && guid.IsNull())
            {
                auto itGuid = ctx.Manager->m_BgPathToGuid.find(
                    MakeBackgroundPathCacheKey(bgImg.Source.Value, bgImg.Source.SourceAlias));
                if (itGuid != ctx.Manager->m_BgPathToGuid.end())
                    guid = itGuid->second;
                else
                    guid = ctx.Manager->ResolveBackgroundImagePath(bgImg.Source.Value, bgImg.Source.SourceAlias);
            }

            if (!guid.IsNull())
            {
                auto itTex = ctx.Manager->m_BgTextureCache.find(guid);
                const bool cached = (itTex != ctx.Manager->m_BgTextureCache.end() && itTex->second.Handle.IsValid());
                if (cached)
                {
                    itTex->second.LastUsedFrame = ctx.Manager->m_BgCacheFrame;
                    const bool pointFilter = el->HasClass("ui-bg-point-filter");
                    texSlot = ctx.Textures->Register(
                        itTex->second.Handle,
                        pointFilter ? ctx.Manager->m_SamplerNearestClamp : Rendering::SamplerHandle{});
                    imgW = itTex->second.Width;
                    imgH = itTex->second.Height;
                    slice = itTex->second.Slice;
                }
                else
                {
                    ctx.Manager->EnsureBackgroundTextureUploaded(guid);

                    // EnsureBackgroundTextureUploaded can complete synchronously
                    // when the asset was already loaded by startup preloading.
                    // Re-check immediately so first paint does not need a later
                    // global style refresh (for example, closing the project picker)
                    // before CSS icon backgrounds become visible.
                    itTex = ctx.Manager->m_BgTextureCache.find(guid);
                    if (itTex != ctx.Manager->m_BgTextureCache.end() && itTex->second.Handle.IsValid())
                    {
                        const bool pointFilter = el->HasClass("ui-bg-point-filter");
                        texSlot = ctx.Textures->Register(
                            itTex->second.Handle,
                            pointFilter ? ctx.Manager->m_SamplerNearestClamp : Rendering::SamplerHandle{});
                        imgW = itTex->second.Width;
                        imgH = itTex->second.Height;
                        slice = itTex->second.Slice;
                    }
                }
            }
        }
    }

    return {texSlot, imgW, imgH, spaceBits, slice};
}

void UIManager::EmitBackgroundImagePrimitive(UIElement* el, PrimitiveGenContext& ctx,
                                              const VisualStyle& vs, const Box4& borderWidth,
                                              float x, float y, float w, float h,
                                              uint16_t clipIdx)
{
    if (!ctx.Textures || !ctx.Manager)
        return;

    const auto& bgImg = vs.BackgroundImage;
    const ResolvedBgSlot bgSlot = ctx.PreResolvedBg
        ? *ctx.PreResolvedBg
        : ResolveBackgroundTextureSlot(el, ctx, bgImg, w, h);
    const uint32_t texSlot = bgSlot.TexSlot;
    const uint32_t imgW = bgSlot.ImgW;
    const uint32_t imgH = bgSlot.ImgH;
    if (texSlot == 0)
        return;

    // Two boxes, and they are not the same box.
    //
    // The POSITIONING area is the padding box: background-origin's initial value
    // is padding-box, so background-size and background-position measure against
    // the element rect inset by its borders. The PAINT area stays the border box
    // (background-clip's initial value), so an image whose own box reaches that
    // far still extends under the border and is clipped at the outer contour.
    // Using the border box for both is what let a contain-fit thumbnail grow
    // over the border ring it is supposed to sit inside.
    float px = x;
    float py = y;
    float pw = w;
    float ph = h;
    InsetToPaddingBox(borderWidth, ctx.ContentScale, px, py, pw, ph);

    // border-radius clips background imagery to the element's rounded border
    // box, matching the rect background above (CSS background painting). The
    // paint box rides the primitive because the image quad may not equal the
    // element rect (cover/contain/position offsets, nine-slice regions).
    const CornerRadiiTLTRBRBL bgUsedRadii =
        UsedBorderRadius(vs, el->GetLayoutWidth(), el->GetLayoutHeight());
    const CornerRadiiTLTRBRBL bgRadii = ScaleRadii(bgUsedRadii, ctx.ContentScale);
    const bool roundedClip =
        (std::max({bgRadii.TopLeft.X, bgRadii.TopLeft.Y, bgRadii.TopRight.X, bgRadii.TopRight.Y,
                   bgRadii.BottomRight.X, bgRadii.BottomRight.Y, bgRadii.BottomLeft.X,
                   bgRadii.BottomLeft.Y}) > 0.0f);

    // 9-slice: subdivide the background into up to nine quads (corners 1:1, edges
    // and center stretched/tiled) instead of a single stretched quad. The texture's
    // intrinsic slice metadata drives this by default; a per-element CSS
    // border-image override replaces it when present. Fills the element rect,
    // ignoring background-size/position/repeat AND the positioning area above —
    // a border image's area is the border box in CSS too, and the intrinsic
    // slice path is a texture feature, not a background-positioning one.
    NineSlice slice = bgSlot.Slice;
    if (bgImg.HasBorderImage)
    {
        const float fsw = (imgW > 0) ? static_cast<float>(imgW) : 1.0f;
        const float fsh = (imgH > 0) ? static_cast<float>(imgH) : 1.0f;
        const auto toTexel = [](float v, float hi) {
            return static_cast<uint16_t>(std::clamp(v, 0.0f, hi));
        };
        const float L = bgImg.BiSlice[3], R = bgImg.BiSlice[1];
        const float T = bgImg.BiSlice[0], B = bgImg.BiSlice[2];
        NineSlice ov;
        ov.Enabled = (L > 0.0f || R > 0.0f || T > 0.0f || B > 0.0f); // all-zero = un-sliced
        ov.FillCenter = bgImg.BiSliceFill;
        ov.X[0] = ov.X[1] = toTexel(L, fsw);
        ov.X[2] = ov.X[3] = toTexel(fsw - R, fsw);
        ov.Y[0] = ov.Y[1] = toTexel(T, fsh);
        ov.Y[2] = ov.Y[3] = toTexel(fsh - B, fsh);
        ov.FillX = bgImg.BiRepeatX;
        ov.FillY = bgImg.BiRepeatY;
        // CSS border-image-repeat has no separate center control, so the center
        // tracks the horizontal repeat by convention; biRepeatY is not applied to
        // the center, and CenterFill::Scale is unreachable from CSS (texture-only).
        ov.CenterFill = bgImg.BiRepeatX;
        slice = ov;
    }

    if (slice.IsSliced())
    {
        const uint32_t tint = bgImg.HasTint ? UI::PackFromARGB(bgImg.Tint) : 0xFFFFFFFFu;
        const float sat = std::clamp(bgImg.Saturation, 0.0f, 1.0f);
        UI::BuildNineSliceRegions(
            slice, static_cast<float>(imgW), static_cast<float>(imgH),
            x, y, w, h, ctx.ContentScale,
            [&](const UI::NineSliceRegion& r) {
                UI::UIPrimitive prim = UI::MakeTexturedQuad(
                    r.X, r.Y, r.W, r.H, texSlot, tint, r.U0, r.V0, r.U1, r.V1);
                if (roundedClip)
                    UI::SetTextureRoundedClip(prim, x, y, w, h,
                                              bgRadii.TopLeft.X, bgRadii.TopLeft.Y,
                                              bgRadii.TopRight.X, bgRadii.TopRight.Y,
                                              bgRadii.BottomRight.X, bgRadii.BottomRight.Y,
                                              bgRadii.BottomLeft.X, bgRadii.BottomLeft.Y);
                UI::SetTextureSourceSpaceBits(prim, bgSlot.SpaceBits);
                prim.Saturation = sat;
                prim.PointFilter = el->HasClass("ui-bg-point-filter") ? 1.0f : 0.0f;
                UI::SetClip(prim, clipIdx);
                UI::SetOpacity(prim, ctx.EffectiveOpacity);
                ctx.Primitives.push_back(prim);
            });
        return;
    }

    // Compute display rect based on background-size mode. Every term measures
    // against the positioning area (pw/ph), never the element rect.
    float displayW = pw;
    float displayH = ph;
    float naturalW = (imgW > 0) ? static_cast<float>(imgW) : pw;
    float naturalH = (imgH > 0) ? static_cast<float>(imgH) : ph;

    switch (bgImg.SizeMode)
    {
    case BackgroundSizeMode::Cover:
    {
        float s = std::max(pw / naturalW, ph / naturalH);
        displayW = naturalW * s;
        displayH = naturalH * s;
        break;
    }
    case BackgroundSizeMode::Contain:
    {
        float s = std::min(pw / naturalW, ph / naturalH);
        displayW = naturalW * s;
        displayH = naturalH * s;
        break;
    }
    case BackgroundSizeMode::Explicit:
    {
        // CSS explicit sizes are logical px; scale to match physical rect.
        if (bgImg.SizeX > 0)
            displayW = bgImg.SizeXIsPercent ? (bgImg.SizeX * 0.01f * pw) : (bgImg.SizeX * ctx.ContentScale);
        else
            displayW = naturalW;
        if (bgImg.SizeY > 0)
            displayH = bgImg.SizeYIsPercent ? (bgImg.SizeY * 0.01f * ph) : (bgImg.SizeY * ctx.ContentScale);
        else
            displayH = naturalH;
        break;
    }
    default: // Auto: natural size, clamped to the positioning area
    {
        float s = std::min(pw / naturalW, ph / naturalH);
        if (s < 1.0f)
        {
            displayW = naturalW * s;
            displayH = naturalH * s;
        }
        else
        {
            displayW = naturalW;
            displayH = naturalH;
        }
        break;
    }
    }

    displayW = std::max(1.0f, displayW);
    displayH = std::max(1.0f, displayH);

    // Compute position offset, also within the positioning area.
    float posOffX = 0.0f;
    float posOffY = 0.0f;
    if (bgImg.Repeat == BackgroundRepeat::NoRepeat)
    {
        float fx = bgImg.PosXIsPercent ? (bgImg.PosX * 0.01f) : 0.0f;
        float fy = bgImg.PosYIsPercent ? (bgImg.PosY * 0.01f) : 0.0f;
        // Non-percent offsets are logical px; scale to match physical rect.
        posOffX = bgImg.PosXIsPercent ? (pw - displayW) * fx : (bgImg.PosX * ctx.ContentScale);
        posOffY = bgImg.PosYIsPercent ? (ph - displayH) * fy : (bgImg.PosY * ctx.ContentScale);
    }

    // Compute UV coordinates based on repeat mode.
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float drawX = px + posOffX;
    float drawY = py + posOffY;
    float drawW = displayW;
    float drawH = displayH;

    if (bgImg.Repeat == BackgroundRepeat::Repeat ||
        bgImg.Repeat == BackgroundRepeat::RepeatX ||
        bgImg.Repeat == BackgroundRepeat::RepeatY)
    {
        // Tiling covers the whole paint area (the element rect), so tiles run on
        // under the border — but the grid's PHASE comes from the positioning
        // area, which is where the first tile's origin sits. The quad therefore
        // starts at the element rect and its UV span opens NEGATIVE by the
        // border inset expressed in tiles.
        const float tileU = (px - x) / displayW;
        const float tileV = (py - y) / displayH;
        drawX = x;
        drawY = y;
        drawW = w;
        drawH = h;
        const float uRepeat = std::max(1.0f, w / displayW);
        const float vRepeat = std::max(1.0f, h / displayH);
        const bool tilesX = bgImg.Repeat != BackgroundRepeat::RepeatY;
        const bool tilesY = bgImg.Repeat != BackgroundRepeat::RepeatX;
        u0 = tilesX ? -tileU : 0.0f;
        v0 = tilesY ? -tileV : 0.0f;
        u1 = tilesX ? (u0 + uRepeat) : 1.0f;
        v1 = tilesY ? (v0 + vRepeat) : 1.0f;
    }

    // Build tint color.
    uint32_t tint = bgImg.HasTint ? UI::PackFromARGB(bgImg.Tint) : 0xFFFFFFFFu;

    UI::UIPrimitive prim = UI::MakeTexturedQuad(
        drawX, drawY, drawW, drawH,
        texSlot, tint,
        u0, v0, u1, v1);
    if (roundedClip)
        UI::SetTextureRoundedClip(prim, x, y, w, h,
                                              bgRadii.TopLeft.X, bgRadii.TopLeft.Y,
                                              bgRadii.TopRight.X, bgRadii.TopRight.Y,
                                              bgRadii.BottomRight.X, bgRadii.BottomRight.Y,
                                              bgRadii.BottomLeft.X, bgRadii.BottomLeft.Y);
    UI::SetTextureSourceSpaceBits(prim, bgSlot.SpaceBits);
    prim.PointFilter = el->HasClass("ui-bg-point-filter") ? 1.0f : 0.0f;
    prim.Saturation = std::clamp(bgImg.Saturation, 0.0f, 1.0f);
    UI::SetClip(prim, clipIdx);
    UI::SetOpacity(prim, ctx.EffectiveOpacity);

    ctx.Primitives.push_back(prim);
}

static uint64_t ComputeTextShapeCacheKey(std::string_view text, const FontAtlas* font,
                                          float pixelSize, float wrapWidth,
                                          float lineBoxPx, TextLayout::WordBreak wb,
                                          uint32_t packedTextColor, float letterSpacingPx,
                                          bool wantEllipsis)
{
    constexpr uint64_t kFnv1aPrime = 0x100000001B3ull;
    auto mix = [](uint64_t h, uint64_t v) -> uint64_t {
        h ^= v;
        h *= kFnv1aPrime;
        return h;
    };
    uint64_t h = std::hash<std::string_view>{}(text);
    h = mix(h, reinterpret_cast<uintptr_t>(font));
    uint32_t bits;
    std::memcpy(&bits, &pixelSize, sizeof(float));
    h = mix(h, static_cast<uint64_t>(bits));
    std::memcpy(&bits, &wrapWidth, sizeof(float));
    h = mix(h, static_cast<uint64_t>(bits));
    std::memcpy(&bits, &lineBoxPx, sizeof(float));
    h = mix(h, static_cast<uint64_t>(bits));
    std::memcpy(&bits, &letterSpacingPx, sizeof(float));
    h = mix(h, static_cast<uint64_t>(bits));
    h = mix(h, static_cast<uint64_t>(wb));
    // GlyphPlacement stores per-glyph colors from shaping; without this, cache hits
    // replay stale colors after CSS-only changes (e.g. list row selection).
    h = mix(h, static_cast<uint64_t>(packedTextColor));
    // The ellipsis run is shaped into the cache only when the style asks for
    // it, so a text-overflow flip must rebuild rather than hit a cache that
    // lacks (or wastes) the "…" glyphs.
    h = mix(h, wantEllipsis ? 0x9E3779B97F4A7C15ull : 0ull);
    return h;
}

// Tolerance on "does this text overflow its content box" — the question that
// decides whether a text input is left-anchored and scrolled. The three sites
// in this file that ask it share this value: the glyph run below, the scroll
// writer (RefreshTextInputScroll) and the overlay geometry
// (BuildTextInputOverlays). The glyph run measures from the shape cache and the
// other two from the measure cache; ShapedWidthMatchesMeasuredWidth pins those
// widths to 0.01px, well inside this slop, so one constant is one answer.
//
// TextField::BuildPointerGeometry asks the same question with its own 0.5f
// literal. Sharing one definition across the two files needs a header the
// value does not have yet — until it does, changing this one means changing
// that one.
static constexpr float kTextOverflowSlopPx = 0.5f;

void UIManager::EmitTextPrimitives(UIElement* el, PrimitiveGenContext& ctx,
                                   const ResolvedStyle& style,
                                   float x, float y, float w, float h,
                                   std::string_view text, bool warmOnly)
{
    if (text.empty())
        return;

    ITextMeasurable* textEl = el ? el->GetTextMeasurable() : nullptr;
    const VisualStyle& vs = style.Visual;
    FontAtlas* font = ResolveFontForVisualStyle(vs, ctx);
    if (!font)
        return;

    const float pixelSize = std::max(1.0f, vs.FontSize * m_ContentScale);
    // Device px, matching pixelSize — the same value MeasureTextWithAtlas used,
    // so the rendered run fills exactly the box Yoga sized for it.
    const float letterSpacingPx = vs.LetterSpacing * m_ContentScale;
    const uint32_t textColor = UI::PackFromARGB(vs.Color);
    const uint16_t clipIdx = ctx.CurrentClipIndex();

    const auto [contentX, contentY, contentW, contentH] = ComputeContentBox(
        x, y, w, h, el->GetLayoutPadding(), style.Layout.BorderWidth, ctx.ContentScale);

    // Controls that declare themselves single-line (TextInput) are laid out
    // as one line by Yoga — MeasureTextFn reads the same AllowWrapForMeasure
    // flag — and their caret map, selection overlay and pointer hit-testing
    // all index one shaped run. Wrapping them here would stack glyphs on rows
    // that model cannot address and would defeat the horizontal scroll they
    // use for overflow instead.
    const bool singleLineOnly = textEl && !textEl->AllowWrapForMeasure();
    const bool noWrap = singleLineOnly || vs.WhiteSpace == WhiteSpace::NoWrap ||
                        vs.WhiteSpace == WhiteSpace::Pre;
    const float wrapWidth = (!noWrap && contentW > 0) ? contentW : 0.0f;

    const bool hasNewlines = !singleLineOnly && text.find('\n') != std::string::npos;
    const bool needsMultiline = hasNewlines || (wrapWidth > 0.0f);

    TextLayout::WordBreak wb = TextLayout::WordBreak::Normal;
    switch (ResolveTextBreakPolicy(vs.WordBreak, vs.OverflowWrap))
    {
    case WordBreak::BreakAll: wb = TextLayout::WordBreak::BreakAll; break;
    case WordBreak::KeepAll: wb = TextLayout::WordBreak::KeepAll; break;
    case WordBreak::BreakWord: wb = TextLayout::WordBreak::BreakWord; break;
    default: break;
    }
    // CSS line box in physical px; 0 = "normal" (font metric height). Must
    // resolve identically to MeasureTextWithAtlas or the rendered block
    // disagrees with the box Yoga sized for it.
    const float lineBoxPx =
        TextLayout::ResolveLineBoxPx(vs.LineHeight, vs.FontSize, m_ContentScale);

    // text-overflow: ellipsis applies to a single-line run on an element that
    // contains its overflow. Single-line controls (TextInput) scroll instead,
    // and a wrapped/multi-line block always clips. The classic CSS recipe —
    // white-space: nowrap; overflow: hidden; text-overflow: ellipsis — lands
    // here as the !needsMultiline single-line shape.
    const bool wantEllipsis = vs.TextOverflow == TextOverflowMode::Ellipsis &&
                              !singleLineOnly && !needsMultiline &&
                              style.Layout.Overflow != Overflow::Visible;

    const uint64_t cacheKey =
        ComputeTextShapeCacheKey(text, font, pixelSize, wrapWidth, lineBoxPx, wb, textColor,
                                 letterSpacingPx, wantEllipsis);

    // --- Ensure the shape cache is current (skip HarfBuzz when text + layout
    // are unchanged). Elements without an ITextMeasurable have no cache home
    // and re-shape every emit into a thread-local, exactly as before. ---
    ITextMeasurable::TextShapeCache* cache =
        textEl ? &textEl->GetOrCreateTextShapeCache() : nullptr;
    static thread_local ITextMeasurable::TextShapeCache uncachedShape;

    if (!cache || cache->Key != cacheKey || cache->TextLength != text.size() ||
        cache->Glyphs.empty())
    {
        // Shaping mutates the shared FontAtlas (glyph packing, FT face
        // state) — UI-thread-only. The drain's collect phase pre-warms via
        // warmOnly so worker emissions land on the cache-hit path; anything
        // that still misses here escalates to a UI-thread re-emit.
        if (ctx.OffThread)
        {
            ctx.EscalateToUiThread = true;
            return;
        }

        static thread_local FontAtlas::ShapeResult singleLineResult;
        static thread_local TextLayout::MultilineShapeResult multilineResult;

        const std::vector<FontAtlas::GlyphPlacement>* placements = nullptr;

        if (needsMultiline)
        {
            StyledRun run{};
            run.Font = font;
            run.Text = std::string(text);
            run.PixelSize = pixelSize;
            run.LetterSpacingPx = letterSpacingPx;
            run.Color[0] = ((vs.Color >> 16) & 0xFF) / 255.0f;
            run.Color[1] = ((vs.Color >> 8) & 0xFF) / 255.0f;
            run.Color[2] = ((vs.Color) & 0xFF) / 255.0f;
            run.Color[3] = ((vs.Color >> 24) & 0xFF) / 255.0f;

            multilineResult = TextLayout::ShapeMultiline(std::span<const StyledRun>(&run, 1), wrapWidth, lineBoxPx, wb);
            placements = &multilineResult.Glyphs;
        }
        else
        {
            font->ShapeText(text, pixelSize, singleLineResult, textColor, letterSpacingPx);
            placements = &singleLineResult.glyphs;
        }

        if (!placements || placements->empty())
            return;

        // Line metrics are captured alongside the glyphs so cache-hit quad
        // building reads them without consulting the font at all. (Reading them
        // is now cheap and const — the metrics come from the design-unit tables
        // and no longer size the FT face — but the cache still carries them so a
        // cache hit stays a pure lookup.)
        auto lm = font->GetFontLineMetrics(pixelSize);

        if (!cache)
            cache = &uncachedShape;
        cache->Key = cacheKey;
        cache->TextLength = text.size();
        cache->Glyphs.assign(placements->begin(), placements->end());
        cache->Lines.clear();
        if (needsMultiline)
        {
            cache->Lines.reserve(multilineResult.LineBreaks.size());
            for (const auto& lb : multilineResult.LineBreaks)
                cache->Lines.push_back({lb.Width, lb.GlyphStart, lb.GlyphEnd, lb.BaselineY});
        }
        else
        {
            // ShapeText puts its baseline at the shaped ascender, so for a
            // single line that ascender IS the block-frame baseline.
            cache->Lines.push_back({singleLineResult.metrics.width, 0, cache->Glyphs.size(),
                                    singleLineResult.metrics.ascender});
        }
        cache->TotalWidth = needsMultiline ? multilineResult.Metrics.width : singleLineResult.metrics.width;
        cache->LineH = std::max(1.0f, lm.height);
        cache->NeedsMultiline = needsMultiline;
        cache->HasColorGlyphs = false;
        for (const auto& gp : cache->Glyphs)
        {
            if (gp.isColor)
            {
                cache->HasColorGlyphs = true;
                break;
            }
        }
        cache->ActualLineCount = needsMultiline ? static_cast<int>(multilineResult.LineCount) : 1;

        cache->EllipsisGlyphs.clear();
        cache->EllipsisWidth = 0.0f;
        if (wantEllipsis)
        {
            // Shaped here, on the UI thread, because shaping can pack new
            // glyphs into the shared atlas; emission then treats the run as
            // pure cached data. U+2026 with a three-period fallback for fonts
            // that lack it.
            const std::string_view ellipsisText =
                font->HasGlyph(0x2026u) ? std::string_view("\xE2\x80\xA6") : std::string_view("...");
            static thread_local FontAtlas::ShapeResult ellipsisResult;
            font->ShapeText(ellipsisText, pixelSize, ellipsisResult, textColor, letterSpacingPx);
            cache->EllipsisGlyphs.assign(ellipsisResult.glyphs.begin(), ellipsisResult.glyphs.end());
            cache->EllipsisWidth = ellipsisResult.metrics.width;
        }
    }

    if (warmOnly)
    {
        // Collect-phase pre-warm: also pre-register the font's Slug pages so
        // worker-side RegisterSlugTextures calls are pure locked lookups.
        if (ctx.Textures)
        {
            ctx.Textures->RegisterSlugTextures(*font);
            // Color (emoji) pages register lazily during quad building —
            // pre-register any this cache references so worker-side
            // EmitGlyphRun never creates GPU textures off-thread.
            for (const auto& gp : cache->Glyphs)
                if (gp.isColor)
                    ctx.Textures->RegisterColorAtlasPage(*font, static_cast<int>(gp.atlasPageIndex));
        }
        return;
    }

    // Color (emoji) quads call RegisterColorAtlasPage, whose lazily-packed
    // pages validate against an atlas-GLOBAL content generation — another
    // item's emoji pack during collect can leave this cache's pages "stale"
    // and route a GPU re-upload through a worker. Escalate instead.
    if (ctx.OffThread && cache->HasColorGlyphs)
    {
        ctx.EscalateToUiThread = true;
        return;
    }

    // --- Build glyph quads from the cache (single path for hit and fresh
    // shape). Center single-line text vertically within the content box.
    // Multiline text is top-aligned so caret/selection overlays stay in sync.
    // Either way it is the CSS line box that gets centred, and the half-leading
    // then places the baseline inside it: ShapeMultiline already baked that
    // leading into its glyph Y positions, while ShapeText sits on the bare
    // ascender and needs it added here. Keeping the two terms apart is what
    // holds a `white-space: nowrap` label on the same device pixel as its
    // wrapping twin — they only cancel while the leading is unrounded. ---
    const float lineH = cache->LineH;
    const float lineBoxOrNormal = (lineBoxPx > 0.0f) ? lineBoxPx : lineH;
    float vOffset;
    if (cache->ActualLineCount <= 1)
    {
        vOffset = (contentH - lineBoxOrNormal) * 0.5f;
        if (!cache->NeedsMultiline)
            vOffset += TextLayout::HalfLeadingPx(lineH, lineBoxPx);
    }
    else
    {
        vOffset = 0.0f;
    }

    float alignOffsetX = 0.0f;
    if (vs.TextAlign != TextAlign::Left)
    {
        if (vs.TextAlign == TextAlign::Center)
            alignOffsetX = (contentW - cache->TotalWidth) * 0.5f;
        else if (vs.TextAlign == TextAlign::Right)
            alignOffsetX = contentW - cache->TotalWidth;
    }

    // TextInput internal horizontal scroll: overflowing content is
    // left-anchored (the convention pointer hit-testing already uses) and
    // shifted so the caret stays visible while editing. This path only READS
    // the offset — RefreshTextInputScroll settled it before any of this
    // element's primitives were emitted, so the glyphs, the selection
    // highlight and the caret all place against the same value. Reading a float
    // and an already-built shape cache needs no UI-thread tripwire of its own;
    // the writer carries one.
    TextInput* input = el ? el->GetAsTextInput() : nullptr;
    float scrollPx = 0.0f;
    if (input)
    {
        if (cache->TotalWidth > contentW + kTextOverflowSlopPx)
        {
            alignOffsetX = 0.0f;
            scrollPx = input->GetTextScrollX() * ctx.ContentScale;
        }
    }

    // text-overflow: ellipsis — the run is truncated at emission and "…"
    // emitted after the last fully visible glyph. Left-anchored whatever
    // text-align says: the visible prefix is the start of the string, which
    // is the only stable anchor once the run is wider than the box.
    const bool applyEllipsis = wantEllipsis && !cache->EllipsisGlyphs.empty() &&
                               cache->TotalWidth > contentW + kTextOverflowSlopPx;
    if (applyEllipsis)
        alignOffsetX = 0.0f;
    // Tooltip overlay reads this to offer a truncated label's full text on
    // hover; relaxed — a one-frame-stale answer only delays the tooltip.
    cache->LastRunEllipsized.store(applyEllipsis, std::memory_order_relaxed);

    const float ox = contentX + alignOffsetX - scrollPx;
    const float oy = contentY + vOffset;

    // Own-text overflow clip. The children-overflow clip is pushed after
    // self-emission, so an element's own glyphs only ever carry the ancestor
    // clip — without this, text wider or taller than the box paints past its
    // edges. TextInput always contains its text (it scrolls internally; the
    // caret/selection overlays already hard-clamp); other elements opt in
    // via overflow != visible. The slot is only allocated when glyphs
    // actually spill the box, so fitting text — the common case — costs
    // nothing.
    //
    // The mask is the padding box (CSS overflow semantics): the border is
    // not part of the mask, so scrolled-out glyph fragments never paint
    // over it. A leaf whose text fits grows it by its text effects' reach,
    // so the effects are cut by the ancestors' clips and not by the box
    // (OwnTextClipOutsetPx). The children-overflow push and the drain's
    // collect-phase slot rewrite produce the same shape for the shared
    // el->m_ClipSlotIdx.
    uint16_t selfClipIdx = UI::kNoClip;
    const bool wantsContainment =
        el && (input != nullptr || style.Layout.Overflow != Overflow::Visible);
    if (wantsContainment)
    {
        float clipX = x, clipY = y, clipW = w, clipH = h;
        InsetToPaddingBox(style.Layout.BorderWidth, ctx.ContentScale, clipX, clipY, clipW, clipH);
        CornerRadiiTLTRBRBL clipRadii = ScaleRadii(
            InnerClipRadii(UsedBorderRadius(vs, el->GetLayoutWidth(), el->GetLayoutHeight()),
                           style.Layout.BorderWidth),
            ctx.ContentScale);
        // A different question from kTextOverflowSlopPx above — that one asks
        // whether an input's text overflows its content box, this one whether
        // the laid-out run spills past the clip rect and needs a slot. Same
        // tolerance, deliberately not the same constant.
        constexpr float kSpillSlopPx = 0.5f;
        // Vertical containment applies only to genuinely multi-line content
        // (wrapping or embedded newlines, e.g. pasted into a single-line
        // field). Single-line text may legitimately center a line box a few
        // px taller than the field; clipping that would shave ascenders.
        bool spillsY = false;
        if (cache->ActualLineCount > 1)
        {
            const float textH =
                static_cast<float>(cache->ActualLineCount) * lineBoxOrNormal;
            spillsY = oy < clipY - kSpillSlopPx ||
                      oy + textH > clipY + clipH + kSpillSlopPx;
        }
        const bool runSpillsX = ox < clipX - kSpillSlopPx ||
                                ox + cache->TotalWidth > clipX + clipW + kSpillSlopPx;
        // The visible glyphs pass the box: an ellipsis ends the run inside it,
        // so an ellipsized run is not cut.
        const bool textCut = spillsY || (runSpillsX && !applyEllipsis);
        cache->LastRunCut.store(textCut, std::memory_order_relaxed);
        // Effects paint past the glyph ink even when the text fits, so text
        // with effects takes the slot too; its clip grows by their reach
        // unless the text itself is cut (OwnTextClipOutsetPx).
        const bool spills = vs.TextEffects.HasVisibleEffect() || spillsY || runSpillsX;
        if (spills)
        {
            OutsetClipShape(OwnTextClipOutsetPx(el, vs, ctx.ContentScale, textCut),
                            clipX, clipY, clipW, clipH, clipRadii);
            if (ctx.OffThread)
            {
                // Slot allocation and rect writes are UI-thread-only. Reuse
                // an existing slot (its rect was refreshed during the
                // drain's collect phase); first-time overflow escalates to
                // an inline re-emit that can allocate.
                if (el->m_ClipSlotIdx != UI::kNoClip)
                {
                    selfClipIdx = el->m_ClipSlotIdx;
                }
                else
                {
                    ctx.EscalateToUiThread = true;
                    return;
                }
            }
            else
            {
                // Drain emission runs with an empty clip stack; seed the
                // item's preserved ambient so PushClip intersects with (and
                // parents to) the true ancestor clip instead of the
                // viewport.
                const bool seedAmbient =
                    ctx.ClipStack.empty() && ctx.DrainAmbientClipIdx != UI::kNoClip;
                if (seedAmbient)
                    ctx.ClipStack.push_back(ctx.DrainAmbientClipIdx);
                selfClipIdx = ctx.PushClip(el, clipX, clipY, clipW, clipH, clipRadii);
                // PushClip pushes only on success (allocator exhaustion
                // returns kNoClip without pushing).
                if (selfClipIdx != UI::kNoClip)
                    ctx.PopClip();
                if (seedAmbient)
                    ctx.ClipStack.pop_back();
                // The renderer uploads the clip SSBO only when the store
                // version moved. Collect bumps it for slot rewrites, but a
                // push during drain emission (first-time overflow) writes
                // rect data collect never saw.
                if (selfClipIdx != UI::kNoClip && ctx.DrainModeOnly)
                    ++m_ClipStoreVersion;
            }
        }
    }
    const uint16_t glyphClipIdx = (selfClipIdx != UI::kNoClip) ? selfClipIdx : clipIdx;

    static const std::vector<UI::UITextureRegistry::SlugTextureIndices> kEmptySlugPages;
    const auto& slugPages = (ctx.Textures && font)
        ? ctx.Textures->RegisterSlugTextures(*font)
        : kEmptySlugPages;

    // text-align resolves per line box, not per block: a centred paragraph
    // centres its short final line on its own width instead of inheriting the
    // widest line's offset. alignOffsetX above stays block-level because the
    // overflow/clip decision is about the block.
    const bool perLineAlign =
        vs.TextAlign != TextAlign::Left && input == nullptr && cache->Lines.size() > 1;

    UI::GlyphRunTarget runTarget{};
    runTarget.Primitives = &ctx.Primitives;
    runTarget.Textures = ctx.Textures;
    runTarget.Font = font;
    runTarget.SlugPages = &slugPages;
    runTarget.ClipIndex = glyphClipIdx;
    runTarget.Opacity = ctx.EffectiveOpacity;
    runTarget.Effects = UI::ResolveTextEffects(vs.TextEffects, ctx.ContentScale);

    for (const auto& line : cache->Lines)
    {
        float lineOx = ox;
        if (perLineAlign)
        {
            const float slack = contentW - line.Width;
            lineOx = contentX + (vs.TextAlign == TextAlign::Center ? slack * 0.5f : slack);
        }
        const size_t begin = std::min(line.GlyphStart, cache->Glyphs.size());
        size_t end = std::min(line.GlyphEnd, cache->Glyphs.size());
        float ellipsisPenX = -1.0f;
        if (applyEllipsis)
        {
            // Keep glyphs whose quads end left of where the "…" must start
            // for it to finish inside the box. Glyph x/width are pen-space
            // quad extents in visual order, so x is monotonic. Emission does
            // not honour `direction`: the block is always LTR, and the line's
            // end edge — where CSS puts the "…" — is its right edge.
            const float limit = contentW - cache->EllipsisWidth;
            size_t cut = begin;
            while (cut < end)
            {
                const auto& gp = cache->Glyphs[cut];
                if (gp.x + gp.width > limit)
                    break;
                ++cut;
            }
            // The "…" pen: where the first dropped glyph began, clamped back
            // inside the box when a wide gap (or a box narrower than the
            // ellipsis itself) would push it out.
            ellipsisPenX = std::max(0.0f, std::min(cut < end ? cache->Glyphs[cut].x : limit, limit));
            end = cut;
        }
        // The vertical twin of lineOx: EmitGlyphRun turns `oy` into a per-line
        // rigid shift that puts this line's baseline on a whole device pixel.
        if (begin < end)
            UI::EmitGlyphRun({cache->Glyphs.data() + begin, end - begin},
                             lineOx, oy, line.BaselineY, runTarget);
        if (ellipsisPenX >= 0.0f)
            UI::EmitGlyphRun({cache->EllipsisGlyphs.data(), cache->EllipsisGlyphs.size()},
                             lineOx + ellipsisPenX, oy, line.BaselineY, runTarget);
    }
}

// Width of the caret bar in CSS-logical px; scaled to physical at emission.
static constexpr float kCaretThicknessLogicalPx = 1.5f;

void UIManager::RefreshTextInputScroll(TextInput* input, PrimitiveGenContext& ctx,
                                       const ResolvedStyle& style,
                                       float x, float y, float w, float h,
                                       bool isFocused)
{
    // Measure caches are built on the shared primary face — UI-thread-only.
    // TextInput items are classified UiThreadOnly during drain collect; this
    // tripwire is the backstop.
    if (ctx.OffThread)
    {
        ctx.EscalateToUiThread = true;
        return;
    }

    const VisualStyle& vs = style.Visual;
    FontAtlas* font = ResolveFontForVisualStyle(vs, ctx);
    if (!font)
        return;

    const float px = std::max(1.0f, vs.FontSize * m_ContentScale);
    const float lsPx = vs.LetterSpacing * m_ContentScale;
    const auto cb = ComputeContentBox(x, y, w, h, input->GetLayoutPadding(),
                                      style.Layout.BorderWidth, ctx.ContentScale);

    // Only the rare focused-and-overflowing case pays for a caret-map measure;
    // everything else snaps the offset back to the value start.
    const float textWidth = input->EnsureMeasureCache(font, px, lsPx).metrics.width;
    if (isFocused && textWidth > cb.W + kTextOverflowSlopPx)
        input->UpdateScrollToCaret(font, px, lsPx, cb.W, ctx.ContentScale);
    else
        input->ResetTextScroll();
}

UIManager::TextInputOverlays UIManager::BuildTextInputOverlays(
    TextInput* input, PrimitiveGenContext& ctx, const ResolvedStyle& style,
    float x, float y, float w, float h, bool isFocused)
{
    TextInputOverlays out{};

    // Caret maps and measure caches are built on the shared primary face —
    // UI-thread-only. TextInput items are classified UiThreadOnly during
    // drain collect; this tripwire is the backstop.
    if (ctx.OffThread)
    {
        ctx.EscalateToUiThread = true;
        return out;
    }

    const VisualStyle& vs = style.Visual;
    FontAtlas* font = ResolveFontForVisualStyle(vs, ctx);
    if (!font)
        return out;

    const std::string& value = input->GetValue();
    const float px = std::max(1.0f, vs.FontSize * m_ContentScale);
    const float lsPx = vs.LetterSpacing * m_ContentScale;

    // Ensure we have a caret map
    const FontAtlas::MeasureResult& measure = input->EnsureMeasureCache(font, px, lsPx);
    const std::vector<float>& caretMap = measure.caretXByByte;
    const TextMetrics& total = measure.metrics;

    const auto cb = ComputeContentBox(x, y, w, h, input->GetLayoutPadding(),
                                      style.Layout.BorderWidth, ctx.ContentScale);
    const float iw = cb.W;
    const float ih = cb.H;

    float xBase = cb.X;
    if (vs.TextAlign == TextAlign::Center)
        xBase = cb.X + (iw - total.width) * 0.5f;
    else if (vs.TextAlign == TextAlign::Right)
        xBase = cb.X + (iw - total.width);

    // Internal scroll offset (stored CSS-logical; this math is physical px).
    // Read-only here: RefreshTextInputScroll settled it before this element
    // emitted anything, so the glyph run and both overlays agree.
    float scrollX = input->GetTextScrollX() * ctx.ContentScale;
    if (total.width > iw + kTextOverflowSlopPx)
    {
        xBase = cb.X;
    }
    else
    {
        scrollX = 0.0f;
    }

    // Vertical band over the font's own ascent/descent box, placed exactly
    // where the glyph run puts it: the CSS line box is centred in the content
    // box and the half-leading seats the font box inside that. A band that
    // re-centres on the font box instead drifts off the text by the leading's
    // rounding as soon as line-height is set. TextInput is always single-line,
    // so this is the ActualLineCount <= 1 branch of that placement.
    auto lm = font->GetFontLineMetrics(px);
    const float lineH = std::max(1.0f, lm.height);
    const float lineBoxPx = TextLayout::ResolveLineBoxPx(vs.LineHeight, vs.FontSize, m_ContentScale);
    const float lineBoxOrNormal = (lineBoxPx > 0.0f) ? lineBoxPx : lineH;
    const float bandTop =
        cb.Y + (ih - lineBoxOrNormal) * 0.5f + TextLayout::HalfLeadingPx(lineH, lineBoxPx);
    const float bandBottom = bandTop + lineH;

    // Clip to content box
    const float clipLeft = cb.X;
    const float clipRight = cb.X + iw;
    const float clipTop = cb.Y;
    const float clipBottom = cb.Y + ih;

    const float selTopY = std::max(bandTop, clipTop);
    const float selBottomY = std::min(bandBottom, clipBottom);
    const float selHeight = std::max(0.0f, selBottomY - selTopY);
    if (selHeight <= 0.0f)
        return out;

    const uint16_t clipIdx = ctx.CurrentClipIndex();
    const int textLen = static_cast<int>(value.size());

    // Selection highlight
    const int selStart = input->GetSelectionStart();
    const int selEnd = input->GetSelectionEnd();
    const bool hasSelection = isFocused && selStart >= 0 && selEnd >= 0 && selStart != selEnd;

    float selScreenL = 0.0f;
    float selScreenR = 0.0f;

    if (hasSelection)
    {
        int a = std::clamp(std::min(selStart, selEnd), 0, textLen);
        int b = std::clamp(std::max(selStart, selEnd), 0, textLen);
        float startLocal = (a < (int)caretMap.size()) ? caretMap[(size_t)a] : 0.0f;
        float endLocal = (b < (int)caretMap.size()) ? caretMap[(size_t)b] : (caretMap.empty() ? 0.0f : caretMap.back());

        float selX = (xBase - scrollX) + startLocal;
        float selW = std::max(0.0f, endLocal - startLocal);

        float sL = std::max(selX, clipLeft);
        float sR = std::min(selX + selW, clipRight);
        selW = std::max(0.0f, sR - sL);

        if (selW > 0.0f)
        {
            UI::UIPrimitive prim = UI::MakeRect(sL, selTopY, selW, selHeight,
                UI::PackedTextSelectionFill(vs, isFocused));
            UI::SetClip(prim, clipIdx);
            UI::SetOpacity(prim, ctx.EffectiveOpacity);
            out.Selection = prim;
            out.HasSelection = true;

            selScreenL = sL;
            selScreenR = sR;
        }
    }

    // Caret (visible only when focused)
    {
        int clamped = std::clamp(input->GetCaretIndex(), 0, textLen);
        float caretLocal = 0.0f;
        if (!caretMap.empty())
        {
            caretLocal = (clamped < (int)caretMap.size()) ? caretMap[(size_t)clamped] : caretMap.back();
        }
        float caretX = (xBase - scrollX) + caretLocal;

        // Snap to selection edge when there's a selection
        if (hasSelection)
        {
            int a = std::clamp(std::min(selStart, selEnd), 0, textLen);
            int b = std::clamp(std::max(selStart, selEnd), 0, textLen);
            if (clamped == b)
                caretX = selScreenR;
            else if (clamped == a)
                caretX = selScreenL;
        }

        // Keep the whole bar inside the content box, not just its left edge —
        // clamping to clipRight would leave the caret hanging over the border
        // when it sits at the end of scrolled-out text.
        const float caretWidth = kCaretThicknessLogicalPx * ctx.ContentScale;
        caretX = std::clamp(caretX, clipLeft, std::max(clipLeft, clipRight - caretWidth));

        const uint32_t caretColor = isFocused ? UI::PackFromARGB(vs.Color) : 0x00000000u;
        const float caretAlpha = isFocused ? ctx.EffectiveOpacity : 0.0f;

        UI::UIPrimitive prim{};
        prim.X = caretX;
        prim.Y = selTopY;
        prim.W = caretWidth;
        prim.H = selHeight;
        prim.FillColor = caretColor;
        prim.Opacity = caretAlpha;
        prim.CaretTime = isFocused ? input->GetCaretForceVisibleUntil() : 0.0f;
        prim.ModeAndFlags = UI::MakeFlags(UI::PrimitiveMode::Rect, UI::GradientMode::None,
                                           clipIdx, false, isFocused);
        out.Caret = prim;
        out.HasCaret = true;
    }

    return out;
}
