#pragma once

#include "UI/TextEffects.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace Rendering::Text { class FontAtlas; }
class UIManager;

namespace UI
{
class UITextureRegistry;

// Rendering mode for a UIPrimitive. Determines how the fragment shader
// evaluates color for this instance.
enum class PrimitiveMode : uint8_t
{
    Rect     = 0, // Solid/gradient rounded rect with optional border
    Slug     = 1, // Slug text glyph (direct Bezier curve evaluation)
    Textured = 2, // Textured quad (background images, icons)
    Line     = 3, // Anti-aliased line segment
    Bezier   = 4, // Anti-aliased cubic bezier curve
    Triangle = 5  // Filled anti-aliased triangle
};

// Gradient fill modes for Rect primitives.
enum class GradientMode : uint8_t
{
    None         = 0,
    Vertical     = 1, // Top-to-bottom: gradColor0 -> gradColor1
    Horizontal   = 2, // Left-to-right: gradColor0 -> gradColor1
    FourCorner   = 3, // Bilinear: fillColor=TL, gradColor0=TR, gradColor1=BL, borderColor=BR
    PolarHSV     = 4, // Polar hue wheel (hue from angle, sat from radius). Full-sat
                      // color-picker disc: white center -> saturated hue at the rim.
    HueVertical  = 5, // Vertical hue spectrum (0-360 degrees top to bottom)
    PolarHSVGrading = 6 // Grading trackball: a hue ANNULUS with a fully transparent
                        // hollow center — fully-saturated hue only in a thin rim
                        // band (chroma-only; brightness is a separate slider).
                        // Reads the hue push as strength (Unity SMH / Unreal wheels).
};

// Bit layout of UIPrimitive::modeAndFlags:
//   [0:7]   PrimitiveMode
//   [8:10]  GradientMode
//   [11]    encodedSourceContent (Textured mode; see kPrimEncodedSourceBit)
//   [12]    isColorGlyph (Slug mode: 1 = color emoji atlas, sample RGBA directly)
//   [13]    isCaret (blink animation driven by time uniform)
//   [14]    squareLineCap (Line mode: 1 = square caps instead of default round/capsule)
//   [15]    hdrTextureContent (Textured mode: preserve values above SDR white)
//   [16:31] clipIndex (index into ClipRect SSBO; 0xFFFF = no clip)
constexpr uint32_t kPrimModeMask        = 0x000000FFu;
constexpr uint32_t kPrimGradientShift   = 8;
constexpr uint32_t kPrimGradientMask    = 0x00000700u;
// The external source's colour space, as two mutually exclusive bits: both clear
// = SDR-referred content the blend target's adapter converts; kPrimHdrTextureBit
// = paper-white-relative linear, passed through; kPrimEncodedSourceBit = content
// that already carries the blend target's transfer curve, so its sample adapter
// is the IDENTITY. Set only for a finalized world view under an encoded blend
// target (UITextureSpace::SdrFinalized).
constexpr uint32_t kPrimEncodedSourceBit = 0x00000800u;
constexpr uint32_t kPrimColorGlyphBit   = 0x00001000u;
// MODE_RECT: CSS box-shadow `inset`. Slug uses this bit for color glyphs;
// Rect never sets that, so the bits do not collide at emit time.
constexpr uint32_t kPrimInsetShadowBit  = kPrimColorGlyphBit;
constexpr uint32_t kPrimCaretBit        = 0x00002000u;
constexpr uint32_t kPrimSquareCapBit    = 0x00004000u;
constexpr uint32_t kPrimHdrTextureBit   = 0x00008000u;
constexpr uint32_t kPrimClipIndexShift  = 16;
constexpr uint32_t kPrimClipIndexMask   = 0xFFFF0000u;
constexpr uint16_t kNoClip              = 0xFFFFu;

// GradientMode needs three bits, so the fourth bit of the field it used to
// occupy carries the space flag above. An eighth gradient mode must claim a bit
// elsewhere rather than silently overwrite it.
static_assert(static_cast<uint32_t>(GradientMode::PolarHSVGrading) <=
                  (kPrimGradientMask >> kPrimGradientShift),
              "GradientMode no longer fits its 3-bit field — kPrimEncodedSourceBit owns bit 11");

// Unified GPU instance for all UI rendering. One struct, one SSBO, one draw call.
// 144 bytes (9 x vec4), 16-byte aligned for GLSL std430 layout.
//
// All positions are in absolute screen pixels (top-left origin, Y-down).
// Colors are packed RGBA8 (use PackColor/UnpackColor helpers).
struct alignas(16) UIPrimitive
{
    // --- vec4[0]: Bounding rect ---
    float X = 0.0f;
    float Y = 0.0f;
    float W = 0.0f;
    float H = 0.0f;

    // --- vec4[1]: Corner horizontal radii (TL, TR, BR, BL) in pixels ---
    // Rect: element corner radii. Textured: border-radius background clip radii.
    // A corner is an ellipse: this vec4 is its horizontal semi-axis, RadiiY
    // (vec4[8]) its vertical one; circular corners store the radius in both.
    // Other modes overload THIS slot only (Slug band transform, Line/Bezier
    // thickness, Triangle rounding) and leave RadiiY zero.
    float Radii[4] = {0, 0, 0, 0};

    // --- vec4[2]: Packed RGBA8 colors ---
    uint32_t FillColor   = 0x00000000u; // Primary fill / glyph color / tint
    uint32_t BorderColor = 0x00000000u; // Border color (Rect); BR corner in FourCorner gradient; text outline color (Slug)
    uint32_t ShadowColor = 0x00000000u; // Drop shadow color (Rect and Slug)
    uint32_t GlowColor   = 0x00000000u; // Outer glow color (Rect and Slug)

    // --- vec4[3]: Mode-specific data ---
    // Rect: border widths (L, T, R, B) in pixels.
    // Textured: background clip rect (x, y, w, h) in pixels — the element's
    // border box when border-radius is set, zero otherwise.
    float BorderWidths[4] = {0, 0, 0, 0};

    // --- vec4[4]: Mode-specific data ---
    // Rect:     effect padding [padL, padT, padR, padB] (written by ExpandForEffects)
    // Slug:     undilated em-space bounds [emXMin, emYMax, emXMax, emYMin] (vertex shader dilates)
    // Textured: texture UV coordinates [u0, v0, u1, v1]
    // Line:     original endpoints [x0, y0, x1, y1] (rect is the expanded bounding box)
    // Bezier:   endpoints P0,P3 [x0, y0, x1, y1] (control points P1,P2 in borderWidths)
    float UvRect[4] = {0, 0, 1, 1};

    // --- vec4[5]: Effect parameters (Rect and Slug) ---
    // Slug: the text shadow offset, its CSS blur radius and the text glow
    // radius, device pixels (SetTextEffects).
    float ShadowOffsetX  = 0.0f;
    float ShadowOffsetY  = 0.0f;
    float ShadowSoftness = 0.0f;
    float GlowRadius     = 0.0f;

    // --- vec4[6]: Mode parameters ---
    float Opacity        = 1.0f;
    float PointFilter    = 0.0f; // Textured mode: 1.0 = nearest-neighbor, 0.0 = linear (modeParams.y)
    float CaretTime      = 0.0f; // "force visible until" time for caret blink
    float Saturation     = 1.0f; // Textured mode: 1 = full color, 0 = grayscale (luma)

    // --- vec4[7]: Packed integers ---
    uint32_t ModeAndFlags  = 0; // See bit layout above
    uint32_t TextureIndex  = 0; // Bindless descriptor index (0 = no texture)
    uint32_t GradColor0    = 0; // Packed RGBA8: gradient start / TR in FourCorner
    uint32_t GradColor1    = 0; // Packed RGBA8: gradient end / BL in FourCorner

    // --- vec4[8]: Corner vertical radii (TL, TR, BR, BL) in pixels ---
    // The vertical semi-axes of the corners in Radii (Rect and Textured modes;
    // see vec4[1]). Slug stores its text outline width, device pixels, in [0]
    // and leaves the rest zero.
    float RadiiY[4] = {0, 0, 0, 0};
};

static_assert(sizeof(UIPrimitive) == 144, "UIPrimitive must be 144 bytes (9 x vec4)");
static_assert(alignof(UIPrimitive) == 16, "UIPrimitive must be 16-byte aligned");

// Clip rect stored in a separate SSBO. Primitives reference clips by index
// via modeAndFlags[16:31]. Each clip stores an optional parent index so the
// shader can walk the clip chain and evaluate all ancestor clips (rounded
// corners on a parent still clip children even when the child itself has no
// border-radius). parentIndex == kNoClip terminates the chain.
struct alignas(16) UIClipRect
{
    float Rect[4]   = {0, 0, 0, 0};   // x, y, w, h
    float Radii[4]  = {0, 0, 0, 0};   // tl, tr, br, bl horizontal radii (0 = sharp edge)
    float RadiiY[4] = {0, 0, 0, 0};   // tl, tr, br, bl vertical radii
    uint32_t ParentIndex = kNoClip;    // index of enclosing clip, or kNoClip
    uint32_t _pad[3] = {0, 0, 0};
};

static_assert(sizeof(UIClipRect) == 64, "UIClipRect must be 64 bytes (4 x vec4)");

// ---------------------------------------------------------------------------
// Color packing helpers (RGBA8 <-> uint32_t)
// Memory layout: R in low byte, A in high byte (matches GLSL unpackUnorm4x8).
// ---------------------------------------------------------------------------

inline uint32_t PackColor(float r, float g, float b, float a)
{
    auto toByte = [](float v) -> uint32_t {
        return static_cast<uint32_t>(v * 255.0f + 0.5f) & 0xFFu;
    };
    return toByte(r) | (toByte(g) << 8) | (toByte(b) << 16) | (toByte(a) << 24);
}

inline uint32_t PackColorU8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

// Pack from engine ARGB uint32_t (0xAARRGGBB) to our RGBA layout.
inline uint32_t PackFromARGB(uint32_t argb)
{
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8)  & 0xFF;
    uint8_t b = (argb)       & 0xFF;
    return PackColorU8(r, g, b, a);
}

// ---------------------------------------------------------------------------
// ModeAndFlags builder
// ---------------------------------------------------------------------------

inline uint32_t MakeFlags(PrimitiveMode mode,
                          GradientMode gradient = GradientMode::None,
                          uint16_t clipIndex = kNoClip,
                          bool isColorGlyph = false,
                          bool isCaret = false)
{
    uint32_t f = static_cast<uint32_t>(mode);
    f |= (static_cast<uint32_t>(gradient) << kPrimGradientShift);
    if (isColorGlyph) f |= kPrimColorGlyphBit;
    if (isCaret)      f |= kPrimCaretBit;
    f |= (static_cast<uint32_t>(clipIndex) << kPrimClipIndexShift);
    return f;
}

inline PrimitiveMode GetMode(uint32_t flags)
{
    return static_cast<PrimitiveMode>(flags & kPrimModeMask);
}

inline GradientMode GetGradient(uint32_t flags)
{
    return static_cast<GradientMode>((flags & kPrimGradientMask) >> kPrimGradientShift);
}

inline uint16_t GetClipIndex(uint32_t flags)
{
    return static_cast<uint16_t>((flags & kPrimClipIndexMask) >> kPrimClipIndexShift);
}

// ---------------------------------------------------------------------------
// CPU-side primitive construction helpers
// ---------------------------------------------------------------------------

// Create a solid-color (or gradient) rounded rectangle. The scalar radii are
// circular corners; elliptical corners come from CSS and land via the
// per-axis Radii/RadiiY fields directly.
inline UIPrimitive MakeRect(float px, float py, float pw, float ph,
                            uint32_t fill,
                            float rTL = 0, float rTR = 0, float rBR = 0, float rBL = 0,
                            uint16_t clipIndex = kNoClip)
{
    UIPrimitive p{};
    p.X = px; p.Y = py; p.W = pw; p.H = ph;
    p.Radii[0] = rTL; p.Radii[1] = rTR; p.Radii[2] = rBR; p.Radii[3] = rBL;
    p.RadiiY[0] = rTL; p.RadiiY[1] = rTR; p.RadiiY[2] = rBR; p.RadiiY[3] = rBL;
    p.FillColor = fill;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Rect, GradientMode::None, clipIndex);
    return p;
}

// Add uniform border to an existing rect primitive.
inline void AddBorder(UIPrimitive& p, float width, uint32_t color)
{
    p.BorderWidths[0] = width; // L
    p.BorderWidths[1] = width; // T
    p.BorderWidths[2] = width; // R
    p.BorderWidths[3] = width; // B
    p.BorderColor = color;
}

// Add per-edge border widths to an existing rect primitive.
inline void AddBorderLTRB(UIPrimitive& p, float l, float t, float r, float b, uint32_t color)
{
    p.BorderWidths[0] = l;
    p.BorderWidths[1] = t;
    p.BorderWidths[2] = r;
    p.BorderWidths[3] = b;
    p.BorderColor = color;
}

// Add vertical or horizontal gradient.
inline void AddGradient(UIPrimitive& p, GradientMode mode, uint32_t color0, uint32_t color1)
{
    p.GradColor0 = color0;
    p.GradColor1 = color1;
    // Update gradient bits in flags (preserve other bits).
    p.ModeAndFlags = (p.ModeAndFlags & ~kPrimGradientMask) |
                     (static_cast<uint32_t>(mode) << kPrimGradientShift);
}

// Add four-corner bilinear gradient. fillColor = TL, gradColor0 = TR, gradColor1 = BL, borderColor = BR.
inline void AddFourCornerGradient(UIPrimitive& p,
                                  uint32_t colorTL, uint32_t colorTR,
                                  uint32_t colorBL, uint32_t colorBR)
{
    p.FillColor  = colorTL;
    p.GradColor0 = colorTR;
    p.GradColor1 = colorBL;
    p.BorderColor = colorBR;
    p.ModeAndFlags = (p.ModeAndFlags & ~kPrimGradientMask) |
                     (static_cast<uint32_t>(GradientMode::FourCorner) << kPrimGradientShift);
}

// Add drop shadow effect.
inline void AddShadow(UIPrimitive& p, float offsetX, float offsetY, float softness, uint32_t color)
{
    p.ShadowOffsetX = offsetX;
    p.ShadowOffsetY = offsetY;
    p.ShadowSoftness = softness;
    p.ShadowColor = color;
}

// Add outer glow effect.
inline void AddGlow(UIPrimitive& p, float radius, uint32_t color)
{
    p.GlowRadius = radius;
    p.GlowColor = color;
}

// Expand the quad to cover shadow/glow pixels that extend beyond the element
// rect. Stores the per-edge padding [L, T, R, B] in uvRect so the fragment
// shader can reconstruct the original element bounds from the expanded quad.
inline void ExpandForEffects(UIPrimitive& p)
{
    float sx = p.ShadowOffsetX, sy = p.ShadowOffsetY;
    float soft = p.ShadowSoftness;
    float glow = p.GlowRadius;

    const bool insetShadow = (p.ModeAndFlags & kPrimInsetShadowBit) != 0;
    float shadowExtent = insetShadow ? 0.0f : soft * 2.0f;
    const float shadowPadL = insetShadow ? 0.0f : shadowExtent + std::max(0.0f, -sx);
    const float shadowPadT = insetShadow ? 0.0f : shadowExtent + std::max(0.0f, -sy);
    const float shadowPadR = insetShadow ? 0.0f : shadowExtent + std::max(0.0f,  sx);
    const float shadowPadB = insetShadow ? 0.0f : shadowExtent + std::max(0.0f,  sy);
    float padL = std::max(glow, shadowPadL);
    float padT = std::max(glow, shadowPadT);
    float padR = std::max(glow, shadowPadR);
    float padB = std::max(glow, shadowPadB);

    float expand = std::max({padL, padT, padR, padB});
    // Always store the padding, even when it is zero. MODE_RECT treats uvRect
    // as [L,T,R,B] padding whenever a shadow/glow is present; leaving the
    // default {0,0,1,1} UV would shrink an inset-only rect by a pixel.
    p.UvRect[0] = padL;
    p.UvRect[1] = padT;
    p.UvRect[2] = padR;
    p.UvRect[3] = padB;
    if (expand <= 0.0f) return;

    p.X -= padL;
    p.Y -= padT;
    p.W += padL + padR;
    p.H += padT + padB;
}

// Create a textured quad (background image, icon).
inline UIPrimitive MakeTexturedQuad(float px, float py, float pw, float ph,
                                    uint32_t texIndex,
                                    uint32_t tint = 0xFFFFFFFFu,
                                    float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1,
                                    uint16_t clipIndex = kNoClip)
{
    UIPrimitive p{};
    p.X = px; p.Y = py; p.W = pw; p.H = ph;
    p.FillColor = tint;
    p.UvRect[0] = u0; p.UvRect[1] = v0; p.UvRect[2] = u1; p.UvRect[3] = v1;
    p.TextureIndex = texIndex;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Textured, GradientMode::None, clipIndex);
    return p;
}

// Clip a textured quad to the element's rounded border box (CSS border-radius
// clips background imagery). The image quad may be larger (cover) or smaller
// (contain) than this rect, so keep the paint bounds separate from the
// primitive's raster bounds. Radii are per-corner ellipse semi-axes
// (X = horizontal, Y = vertical); pass the same value in both for a circle.
inline void SetTextureRoundedClip(UIPrimitive& p,
                                  float clipX, float clipY, float clipW, float clipH,
                                  float radTLx, float radTLy, float radTRx, float radTRy,
                                  float radBRx, float radBRy, float radBLx, float radBLy)
{
    p.BorderWidths[0] = clipX;
    p.BorderWidths[1] = clipY;
    p.BorderWidths[2] = clipW;
    p.BorderWidths[3] = clipH;
    p.Radii[0] = radTLx;  p.RadiiY[0] = radTLy;
    p.Radii[1] = radTRx;  p.RadiiY[1] = radTRy;
    p.Radii[2] = radBRx;  p.RadiiY[2] = radBRy;
    p.Radii[3] = radBLx;  p.RadiiY[3] = radBLy;
}

// Stamp the external source's colour space onto a Textured primitive. Takes the
// whole two-bit field (see kPrimEncodedSourceBit) rather than one bit at a time,
// so a source can never end up claiming to be both HDR-linear and pre-encoded.
inline void SetTextureSourceSpaceBits(UIPrimitive& p, uint32_t spaceBits)
{
    p.ModeAndFlags &= ~(kPrimHdrTextureBit | kPrimEncodedSourceBit);
    p.ModeAndFlags |= (spaceBits & (kPrimHdrTextureBit | kPrimEncodedSourceBit));
}

// Create a Slug text glyph primitive (direct Bezier curve evaluation).
// For Slug mode, UIPrimitive fields are reinterpreted:
//   radii       = bandTransform (scaleX, scaleY, offsetX, offsetY)
//   borderWidths = glyph data as intBitsToFloat (glyphLocX, glyphLocY, hBandMax, vBandMax)
//   uvRect       = undilated em-space bounds (emXMin, emYMax, emXMax, emYMin)
//   packed.y     = curveTextureIndex, packed.z = bandTextureIndex
// The vertex shader applies half-pixel dilation dynamically to both rect and em bounds.
inline UIPrimitive MakeSlugGlyph(float px, float py, float pw, float ph,
                                 float emXMin, float emYMin, float emXMax, float emYMax,
                                 float bandScaleX, float bandScaleY, float bandOffX, float bandOffY,
                                 int glyphLocX, int glyphLocY, int hBandMax, int vBandMax,
                                 uint32_t curveTexIdx, uint32_t bandTexIdx,
                                 uint32_t glyphColor,
                                 uint16_t clipIndex = kNoClip)
{
    UIPrimitive p{};
    p.X = px; p.Y = py; p.W = pw; p.H = ph;
    p.FillColor = glyphColor;
    // Undilated em-space bounds (vertex shader dilates and interpolates vUV)
    p.UvRect[0] = emXMin; p.UvRect[1] = emYMin; p.UvRect[2] = emXMax; p.UvRect[3] = emYMax;
    // Band transform
    p.Radii[0] = bandScaleX; p.Radii[1] = bandScaleY;
    p.Radii[2] = bandOffX; p.Radii[3] = bandOffY;
    // Glyph data (int → float bit cast, decoded in shader via floatBitsToInt)
    // The shader reads glyphData = borderWidths as ivec4(locX, locY, bandMax.x, bandMax.y).
    // bandMax.x clamps the vertical band index → store vBandMax in .z
    // bandMax.y clamps the horizontal band index (and skip offset) → store hBandMax in .w
    p.BorderWidths[0] = std::bit_cast<float>(glyphLocX);
    p.BorderWidths[1] = std::bit_cast<float>(glyphLocY);
    p.BorderWidths[2] = std::bit_cast<float>(vBandMax);
    p.BorderWidths[3] = std::bit_cast<float>(hBandMax);
    // Texture indices
    p.TextureIndex = curveTexIdx;
    p.GradColor0 = bandTexIdx;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Slug, GradientMode::None, clipIndex);
    return p;
}

// Create a color emoji glyph primitive (RGBA atlas, sampled directly).
inline UIPrimitive MakeColorGlyph(float px, float py, float pw, float ph,
                                  uint32_t texIndex,
                                  float u0, float v0, float u1, float v1,
                                  uint32_t glyphColor,
                                  uint16_t clipIndex = kNoClip)
{
    UIPrimitive p{};
    p.X = px; p.Y = py; p.W = pw; p.H = ph;
    p.FillColor = glyphColor;
    p.UvRect[0] = u0; p.UvRect[1] = v0; p.UvRect[2] = u1; p.UvRect[3] = v1;
    p.TextureIndex = texIndex;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Slug, GradientMode::None, clipIndex, /*isColorGlyph=*/true);
    return p;
}

// Create an anti-aliased line segment. The quad (rectXYWH) is expanded to
// cover the full line extent including thickness; the original endpoints are
// stored in uvRect for the fragment shader's SDF evaluation.
inline UIPrimitive MakeLine(float x0, float y0, float x1, float y1,
                            float thickness, uint32_t color,
                            uint16_t clipIndex = kNoClip)
{
    UIPrimitive p{};
    constexpr float kAApad = 2.0f;
    float halfThick = thickness * 0.5f;
    float pad = halfThick + kAApad;
    float minX = std::min(x0, x1) - pad;
    float minY = std::min(y0, y1) - pad;
    float maxX = std::max(x0, x1) + pad;
    float maxY = std::max(y0, y1) + pad;
    p.X = minX; p.Y = minY;
    p.W = maxX - minX; p.H = maxY - minY;
    p.UvRect[0] = x0; p.UvRect[1] = y0;
    p.UvRect[2] = x1; p.UvRect[3] = y1;
    p.FillColor = color;
    p.Radii[0] = thickness;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Line, GradientMode::None, clipIndex);
    return p;
}

// Create an anti-aliased cubic bezier curve. Control points P1,P2 are stored
// in borderWidths; endpoints P0,P3 are stored in uvRect. The rect is a
// conservative bounding box covering the convex hull of all control points
// plus thickness padding.
inline UIPrimitive MakeBezier(float x0, float y0, float cx0, float cy0,
                              float cx1, float cy1, float x1, float y1,
                              float thickness, uint32_t color,
                              uint16_t clipIndex = kNoClip,
                              bool squareCap = false)
{
    UIPrimitive p{};
    constexpr float kAApad = 2.0f;
    float pad = thickness * 0.5f + kAApad;
    float minX = std::min({x0, cx0, cx1, x1}) - pad;
    float minY = std::min({y0, cy0, cy1, y1}) - pad;
    float maxX = std::max({x0, cx0, cx1, x1}) + pad;
    float maxY = std::max({y0, cy0, cy1, y1}) + pad;
    p.X = minX; p.Y = minY;
    p.W = maxX - minX; p.H = maxY - minY;
    p.UvRect[0] = x0;  p.UvRect[1] = y0;
    p.UvRect[2] = x1;  p.UvRect[3] = y1;
    p.BorderWidths[0] = cx0; p.BorderWidths[1] = cy0;
    p.BorderWidths[2] = cx1; p.BorderWidths[3] = cy1;
    p.FillColor = color;
    p.Radii[0] = thickness;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Bezier, GradientMode::None, clipIndex);
    if (squareCap) p.ModeAndFlags |= kPrimSquareCapBit;
    return p;
}

// Create a filled, anti-aliased triangle. The three vertices are stored in
// uvRect (v0.xy, v1.xy) and borderWidths (v2.xy); the bounding rect covers
// the triangle extent. Fill color goes in fillColor.
// rounding softens all three corners (Minkowski sum with circle). 0 = sharp.
inline UIPrimitive MakeTriangle(float x0, float y0,
                                float x1, float y1,
                                float x2, float y2,
                                uint32_t color,
                                float rounding = 0.0f,
                                uint16_t clipIndex = kNoClip)
{
    UIPrimitive p{};
    constexpr float kAA = 1.0f;
    float pad = kAA + rounding;
    float minX = std::min({x0, x1, x2}) - pad;
    float minY = std::min({y0, y1, y2}) - pad;
    float maxX = std::max({x0, x1, x2}) + pad;
    float maxY = std::max({y0, y1, y2}) + pad;
    p.X = minX; p.Y = minY;
    p.W = maxX - minX; p.H = maxY - minY;
    p.UvRect[0] = x0; p.UvRect[1] = y0;
    p.UvRect[2] = x1; p.UvRect[3] = y1;
    p.BorderWidths[0] = x2; p.BorderWidths[1] = y2;
    p.Radii[0] = rounding; // stored in radii[0], same slot as Line thickness
    p.FillColor = color;
    p.ModeAndFlags = MakeFlags(PrimitiveMode::Triangle, GradientMode::None, clipIndex);
    return p;
}

// Set clip index on an existing primitive.
inline void SetClip(UIPrimitive& p, uint16_t clipIndex)
{
    p.ModeAndFlags = (p.ModeAndFlags & ~kPrimClipIndexMask) |
                     (static_cast<uint32_t>(clipIndex) << kPrimClipIndexShift);
}

// Set opacity on an existing primitive.
inline void SetOpacity(UIPrimitive& p, float opacity)
{
    p.Opacity = opacity;
}

// Turns a glyph primitive into its effect instance: a glyph carrying effect
// fields draws its shadow, glow and outline and not its fill
// (UI/text_effects.glsl), so its fill is a second, plain primitive drawn after
// it (EmitGlyphRun). `effects` holds packed RGBA8 colors and device pixels
// (ResolveTextEffects). Fields: ShadowColor, ShadowOffsetX/Y and ShadowSoftness
// (the CSS blur radius); GlowColor and GlowRadius; BorderColor and RadiiY[0]
// for the outline. An invisible effect leaves its fields zero, and a glyph
// with no visible effect stays a plain fill. RadiiY[0] with a transparent
// BorderColor is the outline width the shadow and glow start from, without
// the outline painted (EmitGlyphRun's shadow-and-glow layer).
inline void SetTextEffects(UIPrimitive& p, const TextEffects& effects)
{
    if (effects.HasVisibleShadow())
    {
        p.ShadowColor = effects.ShadowColor;
        p.ShadowOffsetX = effects.ShadowOffsetX;
        p.ShadowOffsetY = effects.ShadowOffsetY;
        p.ShadowSoftness = effects.ShadowBlur;
    }
    if (effects.HasVisibleGlow())
    {
        p.GlowColor = effects.GlowColor;
        p.GlowRadius = effects.GlowRadius;
    }
    if (effects.HasVisibleOutline())
    {
        p.BorderColor = effects.OutlineColor;
        p.RadiiY[0] = effects.OutlineWidth;
    }
}

// Lightweight context passed to UIElement::OnGeneratePrimitives so controls
// can emit custom shapes without depending on UIManager internals.
//
// Clip contract: Emit() stamps ClipIndex (the ambient clip at this element's
// position in the tree). During a visual-only drain the element re-emits
// with no clip stack, so ClipIndex is kNoClip and the drain patches those
// primitives back to the recorded ambient afterward. A primitive carrying
// any OTHER index (e.g. text glyphs self-clipped to the element's own slot)
// is preserved as-is — kNoClip therefore means "take the ambient", never
// "deliberately unclipped".
struct PrimitiveEmitContext
{
    std::vector<UIPrimitive>& Primitives;
    uint16_t ClipIndex;
    float Opacity;
    UITextureRegistry* Textures = nullptr;
    GameEngine::UIManager* Manager = nullptr;
    // Resolved from the element's visual style during primitive generation (for EmitText).
    Rendering::Text::FontAtlas* FontAtlas = nullptr;
    // OS content scale (physical px per CSS logical px). EmitText scales fontSize
    // by this so canvas controls pass CSS px and get sharp physical-resolution glyphs.
    float ContentScale = 1.0f;

    // Parallel-drain thread contract: set when this context is emitting on a
    // JobSystem worker. EmitText shapes on the shared FontAtlas (UI-thread-
    // only work), so off-thread it sets *EscalateFlag and returns; the drain
    // then re-emits the whole element on the UI thread. Custom emitters that
    // do their own text/measure work must honor the same contract (see the
    // OnGeneratePrimitives thread-contract note in UIElement.h).
    bool OffThread = false;
    bool* EscalateFlag = nullptr;
    TextEffects Effects;

    void Emit(UIPrimitive p)
    {
        SetClip(p, ClipIndex);
        SetOpacity(p, Opacity);
        Primitives.push_back(p);
    }

    // Emit Slug text glyphs at the given position. For canvas controls that
    // need data-driven text (axis labels, frame numbers). For stable text at
    // known positions (node names), prefer child Label elements instead.
    void EmitText(std::string_view text, float x, float y,
                  float fontSize, uint32_t color,
                  Rendering::Text::FontAtlas* font);
};

} // namespace UI
} // namespace GameEngine
