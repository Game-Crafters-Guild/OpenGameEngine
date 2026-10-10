#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Local constant to avoid non-portable M_PI
static constexpr float kPi = 3.14159265358979323846f;

namespace GameEngine
{
namespace Rendering
{
namespace Geometry
{

// Compact 2D UI vertex: position (px), uv, color (float4)
struct ShapeVertex2D
{
    float x, y;       // pixel-space position (top-left origin)
    float u, v;       // 0..1 UV by default (caller can override)
    float r, g, b, a; // color
};

struct RectF
{
    float x, y, w, h;
};
struct Vec2
{
    float x, y;
};
struct ColorF
{
    float r, g, b, a;
};
struct CornerRadii
{
    float tl = 0, tr = 0, br = 0, bl = 0;
};
struct BorderWidths
{
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct BorderColorsLTRB
{
    ColorF left{0, 0, 0, 1};
    ColorF top{0, 0, 0, 1};
    ColorF right{0, 0, 0, 1};
    ColorF bottom{0, 0, 0, 1};
};

// Simple gradient modes for fills
enum class GradientMode
{
    None = 0,
    Vertical,
    Horizontal,
    FourCorner, // bilinear interpolation of 4 corner colors
    PolarHSV,   // hue from angle around center, saturation from radius
    HueVertical // hue from top->bottom, full S/V (analytic in instanced shader)
};

// Per-corner colors for FourCorner gradient mode.
struct FourCornerColors
{
    ColorF tl{1, 1, 1, 1}; // top-left
    ColorF tr{1, 1, 1, 1}; // top-right
    ColorF bl{1, 1, 1, 1}; // bottom-left
    ColorF br{1, 1, 1, 1}; // bottom-right
};

inline ColorF Lerp(const ColorF& a, const ColorF& b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

struct UVRect
{
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

struct ShapeOptions
{
    // Geometry transform
    Vec2 pivot{0.0f, 0.0f};    // relative to shape local space (px)
    float rotationRadians = 0; // applied around pivot

    // Appearance
    ColorF fillColor{1, 1, 1, 1};
    ColorF borderColor{0, 0, 0, 1};
    BorderColorsLTRB borderColorsLTRB{}; // optional per-edge border colors (l,t,r,b)
    bool useBorderColorsLTRB = false;
    float borderThickness = 0.0f;        // legacy uniform thickness (used if per-edge are zero)
    BorderWidths borderLTRB{0, 0, 0, 0}; // per-edge border thickness (l,t,r,b)
    enum class StrokeAlign
    {
        Inside = 0,
        Center = 1,
        Outside = 2
    };
    StrokeAlign strokeAlign = StrokeAlign::Inside; // default to Inside for compatibility
    float borderRadius = 0.0f;                     // px; used by rect/rounded rect stroke

    // Fill gradient (optional)
    GradientMode gradient = GradientMode::None; // None by default
    ColorF gradientColor0{1, 1, 1, 1};          // start color (top or left)
    ColorF gradientColor1{1, 1, 1, 1};          // end color (bottom or right)
    FourCornerColors fourCornerColors{};         // per-corner colors for FourCorner mode

    // UV mapping
    UVRect uv{0, 0, 1, 1};
    enum class UVMappingMode
    {
        Global = 0,
        Local = 1
    };
    UVMappingMode uvMapping = UVMappingMode::Local; // Local = rotates/scales with shape

    // Tessellation (circles/ellipses/rounded corners/splines)
    uint32_t segments = 32; // default circle resolution

    // Optional geometry-based edge AA (cheap, improves rounded-corner quality).
    bool antialias = false;
    float antialiasWidthPx = 1.0f;

    // ---------------------------------------------------------------------
    // UI instancing extension:
    // When UI geometry is emitted as GPU instances, these
    // values are forwarded into the instance params. They are ignored by
    // the CPU mesh builder path.
    float uiParams[3] = {0.0f, 0.0f, 0.0f};

    // Line caps and joins (future extension)
    // Stroke joins
    enum class StrokeJoin
    {
        Miter = 0,
        Bevel = 1,
        Round = 2
    };
    StrokeJoin strokeJoin = StrokeJoin::Miter;
    float miterLimit = 4.0f; // ratio of miter length to half-thickness; > limit -> bevel
};

// Utility: rotate point p about pivot by angle (radians)
inline Vec2 RotateAround(const Vec2& p, const Vec2& pivot, float angle)
{
    if (angle == 0.0f)
        return p;
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    const float tx = p.x - pivot.x;

    const float ty = p.y - pivot.y;
    return {pivot.x + tx * c - ty * s, pivot.y + tx * s + ty * c};
}

// Append helpers return starting index for the appended vertices
// Caller can draw submeshes by tracking returned base index and index count

// Forward declaration for per-corner rounded-rect
inline void AppendRoundedRectRadii(std::vector<ShapeVertex2D>& outV,
                                   std::vector<uint32_t>& outI,
                                   const RectF& rc,
                                   const CornerRadii& cr,
                                   const ShapeOptions& opt);

// Forward declaration for polyline stroke (used for triangle borders)
inline void AppendPolyline(std::vector<ShapeVertex2D>& outV,
                           std::vector<uint32_t>& outI,
                           const std::vector<Vec2>& pts,
                           float thickness,
                           const ShapeOptions& opt);

// Rectangle (axis-aligned)
inline void AppendRectangle(std::vector<ShapeVertex2D>& outV,
                            std::vector<uint32_t>& outI,
                            const RectF& rc,
                            const ShapeOptions& opt)
{
    const float x0 = rc.x, y0 = rc.y, x1 = rc.x + rc.w, y1 = rc.y + rc.h;
    const Vec2 p0 = RotateAround({x0, y0}, opt.pivot, opt.rotationRadians);
    const Vec2 p1 = RotateAround({x1, y0}, opt.pivot, opt.rotationRadians);
    const Vec2 p2 = RotateAround({x1, y1}, opt.pivot, opt.rotationRadians);
    const Vec2 p3 = RotateAround({x0, y1}, opt.pivot, opt.rotationRadians);

    const uint32_t base = static_cast<uint32_t>(outV.size());
    auto push = [&](const Vec2& p, float u, float v, const ColorF& c)
    {
        outV.push_back({p.x, p.y, u, v, c.r, c.g, c.b, c.a});
    };
    // Determine per-vertex colors based on gradient mode
    ColorF cTL = opt.fillColor, cTR = opt.fillColor, cBR = opt.fillColor, cBL = opt.fillColor;
    if (opt.gradient == GradientMode::Vertical)
    {
        cTL = cTR = opt.gradientColor0;
        cBL = cBR = opt.gradientColor1;
    }
    else if (opt.gradient == GradientMode::Horizontal)
    {
        cTL = cBL = opt.gradientColor0;
        cTR = cBR = opt.gradientColor1;
    }
    else if (opt.gradient == GradientMode::FourCorner)
    {
        cTL = opt.fourCornerColors.tl;
        cTR = opt.fourCornerColors.tr;
        cBL = opt.fourCornerColors.bl;
        cBR = opt.fourCornerColors.br;
    }
    push(p0, opt.uv.u0, opt.uv.v0, cTL);
    push(p1, opt.uv.u1, opt.uv.v0, cTR);
    push(p2, opt.uv.u1, opt.uv.v1, cBR);
    push(p3, opt.uv.u0, opt.uv.v1, cBL);

    // two triangles (0,1,2) (0,2,3)
    outI.insert(outI.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});

    // Border as ring with per-edge widths and alignment
    {
        BorderWidths bw = opt.borderLTRB;
        if (bw.left == 0 && bw.top == 0 && bw.right == 0 && bw.bottom == 0)
        {
            bw = {opt.borderThickness, opt.borderThickness, opt.borderThickness, opt.borderThickness};
        }
        if (bw.left > 0 || bw.top > 0 || bw.right > 0 || bw.bottom > 0)
        {
            float l = bw.left, t = bw.top, r = bw.right, btm = bw.bottom;
            // Clamp to half extents to avoid inversion
            l = std::min(l, rc.w * 0.5f);
            r = std::min(r, rc.w * 0.5f);
            t = std::min(t, rc.h * 0.5f);
            btm = std::min(btm, rc.h * 0.5f);
            float xo0, xo1, yo0, yo1, xi0, xi1, yi0, yi1;
            switch (opt.strokeAlign)
            {
            case ShapeOptions::StrokeAlign::Inside:
                xo0 = x0;
                yo0 = y0;
                xo1 = x1;
                yo1 = y1;
                xi0 = x0 + l;
                yi0 = y0 + t;
                xi1 = x1 - r;
                yi1 = y1 - btm;
                break;
            case ShapeOptions::StrokeAlign::Center:
            default:
                xo0 = x0 - l * 0.5f;
                yo0 = y0 - t * 0.5f;
                xo1 = x1 + r * 0.5f;
                yo1 = y1 + btm * 0.5f;
                xi0 = x0 + l * 0.5f;
                yi0 = y0 + t * 0.5f;
                xi1 = x1 - r * 0.5f;
                yi1 = y1 - btm * 0.5f;
                break;
            case ShapeOptions::StrokeAlign::Outside:
                xo0 = x0 - l;
                yo0 = y0 - t;
                xo1 = x1 + r;
                yo1 = y1 + btm;
                xi0 = x0;
                yi0 = y0;
                xi1 = x1;
                yi1 = y1;

                break;
            }
            const Vec2 o0 = RotateAround({xo0, yo0}, opt.pivot, opt.rotationRadians);
            const Vec2 o1 = RotateAround({xo1, yo0}, opt.pivot, opt.rotationRadians);
            const Vec2 o2 = RotateAround({xo1, yo1}, opt.pivot, opt.rotationRadians);
            const Vec2 o3 = RotateAround({xo0, yo1}, opt.pivot, opt.rotationRadians);
            const Vec2 i0 = RotateAround({xi0, yi0}, opt.pivot, opt.rotationRadians);
            const Vec2 i1 = RotateAround({xi1, yi0}, opt.pivot, opt.rotationRadians);
            const Vec2 i2 = RotateAround({xi1, yi1}, opt.pivot, opt.rotationRadians);
            const Vec2 i3 = RotateAround({xi0, yi1}, opt.pivot, opt.rotationRadians);
            if (!opt.useBorderColorsLTRB)
            {
                const uint32_t bidx = (uint32_t)outV.size();
                auto bc = opt.borderColor;
                auto pushB = [&](const Vec2& p)
                { outV.push_back({p.x, p.y, 0, 0, bc.r, bc.g, bc.b, bc.a}); };
                pushB(o0);
                pushB(o1);
                pushB(o2);
                pushB(o3);
                pushB(i0);
                pushB(i1);
                pushB(i2);
                pushB(i3);
                auto addQuad = [&](uint32_t a, uint32_t bb, uint32_t c, uint32_t d)
                { outI.insert(outI.end(), {bidx + a, bidx + bb, bidx + c, bidx + a, bidx + c, bidx + d}); };
                addQuad(0, 1, 5, 4); // top
                addQuad(1, 2, 6, 5); // right
                addQuad(2, 3, 7, 6); // bottom
                addQuad(3, 0, 4, 7); // left
            }
            else
            {
                const BorderColorsLTRB ec = opt.borderColorsLTRB;
                auto pushEdgeQuad = [&](const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, const ColorF& col)
                {
                    const uint32_t bidx = (uint32_t)outV.size();
                    outV.push_back({a.x, a.y, 0, 0, col.r, col.g, col.b, col.a});
                    outV.push_back({b.x, b.y, 0, 0, col.r, col.g, col.b, col.a});
                    outV.push_back({c.x, c.y, 0, 0, col.r, col.g, col.b, col.a});
                    outV.push_back({d.x, d.y, 0, 0, col.r, col.g, col.b, col.a});
                    outI.insert(outI.end(), {bidx + 0, bidx + 1, bidx + 2, bidx + 0, bidx + 2, bidx + 3});
                };
                // Split corners diagonally between edges by using separate quads per edge.
                pushEdgeQuad(o0, o1, i1, i0, ec.top);
                pushEdgeQuad(o1, o2, i2, i1, ec.right);
                pushEdgeQuad(o2, o3, i3, i2, ec.bottom);
                pushEdgeQuad(o3, o0, i0, i3, ec.left);
            }
        }
    }
}

// Rounded rectangle via quarter-arc tessellation per corner
inline void AppendRoundedRect(std::vector<ShapeVertex2D>& outV,
                              std::vector<uint32_t>& outI,
                              const RectF& rc,
                              float radius,
                              const ShapeOptions& opt)
{
    // radius is clamped below for uniform per-corner path
    // Wrapper: delegate to per-corner implementation (uniform radii)
    {
        float rClamped = std::max(0.0f, std::min(radius, std::min(rc.w, rc.h) * 0.5f));
        CornerRadii rad{rClamped, rClamped, rClamped, rClamped};
        AppendRoundedRectRadii(outV, outI, rc, rad, opt);
        return;
    }
}

#if 0
    // Build as fill: center fan + 4 edge strips + 4 quarter arcs (triangle list)
    // Simpler robust approach: approximate polygon boundary (clockwise), then ear-clip into a fan around centroid.
    const uint32_t seg = std::max(4u, opt.segments);

    // Collect boundary points clockwise starting at top-left corner arc
    std::vector<Vec2> poly;
    poly.reserve(4*seg);
    const float x0=rc.x, y0=rc.y, x1=rc.x+rc.w, y1=rc.y+rc.h;
    auto emitArc = [&](float cx, float cy, float a0, float a1){
        const int steps = (int)seg;
        for (int i=0;i<=steps;++i){
            float t = (float)i/(float)steps;
            float a = a0 + (a1-a0)*t;
            poly.push_back({cx + r*std::cos(a), cy + r*std::sin(a)});
        }
    };
    // Top-left (from 180 to 270 deg)
    emitArc(x0 + r, y0 + r, kPi, 1.5f*kPi);
    // Top-right (270 to 360)
    emitArc(x1 - r, y0 + r, 1.5f*kPi, 2.0f*kPi);
    // Bottom-right (0 to 90)
    emitArc(x1 - r, y1 - r, 0.0f, 0.5f*kPi);
    // Bottom-left (90 to 180)
    emitArc(x0 + r, y1 - r, 0.5f*kPi, kPi);

    // De-duplicate consecutive points (arc seams) to avoid degenerate triangles
    if (!poly.empty()) {
        std::vector<Vec2> cleaned; cleaned.reserve(poly.size());
        auto eq = [](const Vec2& a, const Vec2& b){ float dx=a.x-b.x, dy=a.y-b.y; return (dx*dx + dy*dy) <= 1e-8f; };
        cleaned.push_back(poly[0]);
        for (size_t i=1;i<poly.size();++i) { if (!eq(poly[i], cleaned.back())) cleaned.push_back(poly[i]); }
        // Also ensure last != first
        if (cleaned.size()>1 && eq(cleaned.front(), cleaned.back())) cleaned.pop_back();
        poly.swap(cleaned);
    }
    // Transform + UV + color
    Vec2 centroid{0,0}; for (auto& p: poly){ centroid.x+=p.x; centroid.y+=p.y; }
    centroid.x /= (float)poly.size(); centroid.y /= (float)poly.size();
    centroid = RotateAround(centroid, opt.pivot, opt.rotationRadians);

    // Best-quality fill: 9‑patch with arc rings in corners (no large centroid fan)
    auto LerpF = [](float a, float b, float t){ return a + (b-a)*t; };
    auto UVForRect = [&](const RectF& rsub){
        float tx0 = (rsub.x - rc.x) / std::max(1e-6f, rc.w);
        float ty0 = (rsub.y - rc.y) / std::max(1e-6f, rc.h);
        float tx1 = (rsub.x + rsub.w - rc.x) / std::max(1e-6f, rc.w);
        float ty1 = (rsub.y + rsub.h - rc.y) / std::max(1e-6f, rc.h);
        UVRect uv{};
        uv.u0 = LerpF(opt.uv.u0, opt.uv.u1, tx0);
        uv.v0 = LerpF(opt.uv.v0, opt.uv.v1, ty0);
        uv.u1 = LerpF(opt.uv.u0, opt.uv.u1, tx1);
        uv.v1 = LerpF(opt.uv.v0, opt.uv.v1, ty1);
        return uv;
    };
    auto WithUVNoBorder = [&](UVRect uv){ ShapeOptions o=opt; o.uv=uv; o.borderThickness=0.0f; return o; };

    // Use outer rect coordinates already defined above (x0,y0,x1,y1)
    const RectF rcInner{ rc.x + r, rc.y + r, rc.w - 2*r, rc.h - 2*r };

    // Precompute local UV frame if needed (rotated axes)
    Vec2 p0r = RotateAround({rc.x,        rc.y       }, opt.pivot, opt.rotationRadians);
    Vec2 p1r = RotateAround({rc.x+rc.w,   rc.y       }, opt.pivot, opt.rotationRadians);
    Vec2 p3r = RotateAround({rc.x,        rc.y+rc.h  }, opt.pivot, opt.rotationRadians);
    auto norm = [](const Vec2& v){ float L=std::sqrt(v.x*v.x+v.y*v.y); if (L<=1e-6f) return Vec2{0,0}; return Vec2{v.x/L, v.y/L}; };
    Vec2 ex = norm({p1r.x-p0r.x, p1r.y-p0r.y});
    Vec2 ey = norm({p3r.x-p0r.x, p3r.y-p0r.y});

    auto PushVert = [&](float x, float y){
        Vec2 p{x,y}; Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
        float u01=0.0f, v01=0.0f;
        if (opt.uvMapping == ShapeOptions::UVMappingMode::Local) {
            Vec2 d{ pr.x - p0r.x, pr.y - p0r.y };
            float du = d.x*ex.x + d.y*ex.y;
            float dv = d.x*ey.x + d.y*ey.y;
            u01 = du / std::max(1e-6f, rc.w);
            v01 = dv / std::max(1e-6f, rc.h);
        } else { // Global: screen-aligned
            u01 = (pr.x - rc.x) / std::max(1e-6f, rc.w);
            v01 = (pr.y - rc.y) / std::max(1e-6f, rc.h);
        }
        float uu = LerpF(opt.uv.u0, opt.uv.u1, u01);
        float vv = LerpF(opt.uv.v0, opt.uv.v1, v01);
        ColorF col = opt.fillColor;
        if (opt.gradient == GradientMode::Vertical)      col = Lerp(opt.gradientColor0, opt.gradientColor1, v01);
        else if (opt.gradient == GradientMode::Horizontal) col = Lerp(opt.gradientColor0, opt.gradientColor1, u01);
        outV.push_back({pr.x, pr.y, uu, vv, col.r,col.g,col.b,col.a});
    };
    auto AppendQuad = [&](const RectF& r){
        uint32_t b = (uint32_t)outV.size();
        PushVert(r.x,         r.y);
        PushVert(r.x + r.w,   r.y);
        PushVert(r.x + r.w,   r.y + r.h);
        PushVert(r.x,         r.y + r.h);
        outI.insert(outI.end(), { b+0, b+1, b+2, b+0, b+2, b+3 });
    };
    auto AppendQuadWithBias = [&](const RectF& r, float uBiasL, float uBiasR, float vBiasT, float vBiasB){
        auto PushVertBias = [&](float x, float y, float uBias, float vBias){
            Vec2 p{x,y}; Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
            float u01=0.0f, v01=0.0f;
            if (opt.uvMapping == ShapeOptions::UVMappingMode::Local) {
                Vec2 d{ pr.x - p0r.x, pr.y - p0r.y };
                float du = d.x*ex.x + d.y*ex.y;
                float dv = d.x*ey.x + d.y*ey.y;
                u01 = du / std::max(1e-6f, rc.w);
                v01 = dv / std::max(1e-6f, rc.h);
            } else {
                u01 = (pr.x - rc.x) / std::max(1e-6f, rc.w);
                v01 = (pr.y - rc.y) / std::max(1e-6f, rc.h);
            }
            float uu = LerpF(opt.uv.u0, opt.uv.u1, u01) + uBias;
            float vv = LerpF(opt.uv.v0, opt.uv.v1, v01) + vBias;
            ColorF col = opt.fillColor;
            if (opt.gradient == GradientMode::Vertical)      col = Lerp(opt.gradientColor0, opt.gradientColor1, v01);
            else if (opt.gradient == GradientMode::Horizontal) col = Lerp(opt.gradientColor0, opt.gradientColor1, u01);
            outV.push_back({pr.x, pr.y, uu, vv, col.r,col.g,col.b,col.a});
        };
        uint32_t b = (uint32_t)outV.size();
        PushVertBias(r.x,         r.y,        uBiasL, vBiasT);
        PushVertBias(r.x + r.w,   r.y,        uBiasR, vBiasT);
        PushVertBias(r.x + r.w,   r.y + r.h,  uBiasR, vBiasB);
        PushVertBias(r.x,         r.y + r.h,  uBiasL, vBiasB);
        outI.insert(outI.end(), { b+0, b+1, b+2, b+0, b+2, b+3 });
    };
    auto AppendQuadSeamAware = [&](const RectF& r){
        // Split r along any integer UV seam lines (u or v) implied by global mapping
        std::vector<float> xCuts{ r.x, r.x + r.w };
        std::vector<float> yCuts{ r.y, r.y + r.h };
        auto addXCuts = [&](float uL, float uR){
            float uMin = std::min(uL,uR), uMax = std::max(uL,uR);
            int k0 = (int)std::ceil(uMin - 1e-6f);
            int k1 = (int)std::floor(uMax + 1e-6f);
            for (int k=k0; k<=k1; ++k){
                if (k<=uMin || k>=uMax) continue;
                float xSplit = rc.x + ( (float)k - opt.uv.u0 ) * rc.w / std::max(1e-6f, (opt.uv.u1 - opt.uv.u0) );
                if (xSplit > r.x + 1e-5f && xSplit < r.x + r.w - 1e-5f) xCuts.push_back(xSplit);
            }
        };
        auto addYCuts = [&](float vT, float vB){
            float vMin = std::min(vT,vB), vMax = std::max(vT,vB);
            int k0 = (int)std::ceil(vMin - 1e-6f);
            int k1 = (int)std::floor(vMax + 1e-6f);
            for (int k=k0; k<=k1; ++k){
                if (k<=vMin || k>=vMax) continue;
                float ySplit = rc.y + ( (float)k - opt.uv.v0 ) * rc.h / std::max(1e-6f, (opt.uv.v1 - opt.uv.v0) );
                if (ySplit > r.y + 1e-5f && ySplit < r.y + r.h - 1e-5f) yCuts.push_back(ySplit);
            }
        };
        // UVs at r edges using global mapping
        auto UOfX = [&](float x){ return LerpF(opt.uv.u0, opt.uv.u1, (x - rc.x)/std::max(1e-6f, rc.w)); };
#endif // end legacy RoundedRect block; new per-corner function follows

// Rounded rectangle with per-corner radii
inline void AppendRoundedRectRadii(std::vector<ShapeVertex2D>& outV,
                                   std::vector<uint32_t>& outI,
                                   const RectF& rc,
                                   const CornerRadii& cr,
                                   const ShapeOptions& opt)
{
    // Clamp radii to half extents
    CornerRadii r = cr;
    float hw = rc.w * 0.5f, hh = rc.h * 0.5f;
    r.tl = std::max(0.0f, std::min(r.tl, std::min(hw, hh)));
    r.tr = std::max(0.0f, std::min(r.tr, std::min(hw, hh)));
    r.br = std::max(0.0f, std::min(r.br, std::min(hw, hh)));
    r.bl = std::max(0.0f, std::min(r.bl, std::min(hw, hh)));

    const uint32_t seg = std::max(8u, opt.segments);
    const float x0 = rc.x, y0 = rc.y, x1 = rc.x + rc.w, y1 = rc.y + rc.h;

    // UV helpers (Global vs Local) and seam-aware quads as in uniform version
    auto LerpF = [](float a, float b, float t)
    { return a + (b - a) * t; };
    // Precompute local UV frame (for Local mapping)
    Vec2 p0r = RotateAround({rc.x, rc.y}, opt.pivot, opt.rotationRadians);
    Vec2 p1r = RotateAround({rc.x + rc.w, rc.y}, opt.pivot, opt.rotationRadians);
    Vec2 p3r = RotateAround({rc.x, rc.y + rc.h}, opt.pivot, opt.rotationRadians);
    auto norm = [](const Vec2& v)
    { float L=std::sqrt(v.x*v.x+v.y*v.y); if (L<=1e-6f) return Vec2{0,0}; return Vec2{v.x/L, v.y/L}; };
    Vec2 ex = norm({p1r.x - p0r.x, p1r.y - p0r.y});
    Vec2 ey = norm({p3r.x - p0r.x, p3r.y - p0r.y});

    // Helper: compute gradient color for a given normalized position within the rect.
    auto GradColorAt = [&](float u01, float v01) -> ColorF
    {
        if (opt.gradient == GradientMode::Vertical)
            return Lerp(opt.gradientColor0, opt.gradientColor1, v01);
        if (opt.gradient == GradientMode::Horizontal)
            return Lerp(opt.gradientColor0, opt.gradientColor1, u01);
        if (opt.gradient == GradientMode::FourCorner)
        {
            ColorF top = Lerp(opt.fourCornerColors.tl, opt.fourCornerColors.tr, u01);
            ColorF bot = Lerp(opt.fourCornerColors.bl, opt.fourCornerColors.br, u01);
            return Lerp(top, bot, v01);
        }
        return opt.fillColor;
    };
    auto PushVert = [&](float x, float y)
    {
        Vec2 p{x, y};
        Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
        float u01 = 0.0f, v01 = 0.0f;
        if (opt.uvMapping == ShapeOptions::UVMappingMode::Local)
        {
            Vec2 d{pr.x - p0r.x, pr.y - p0r.y};
            float du = d.x * ex.x + d.y * ex.y;
            float dv = d.x * ey.x + d.y * ey.y;
            u01 = du / std::max(1e-6f, rc.w);
            v01 = dv / std::max(1e-6f, rc.h);
        }
        else
        {
            u01 = (pr.x - rc.x) / std::max(1e-6f, rc.w);
            v01 = (pr.y - rc.y) / std::max(1e-6f, rc.h);
        }
        float uu = LerpF(opt.uv.u0, opt.uv.u1, u01);
        float vv = LerpF(opt.uv.v0, opt.uv.v1, v01);
        ColorF col = GradColorAt(u01, v01);
        outV.push_back({pr.x, pr.y, uu, vv, col.r, col.g, col.b, col.a});
    };
    auto AppendQuad = [&](const RectF& rct)
    { uint32_t b=(uint32_t)outV.size(); PushVert(rct.x,rct.y); PushVert(rct.x+rct.w,rct.y); PushVert(rct.x+rct.w,rct.y+rct.h); PushVert(rct.x,rct.y+rct.h); outI.insert(outI.end(), { b, b+1, b+2, b, b+2, b+3 }); };
    auto AppendQuadSeamAware = [&](const RectF& rct)
    {
        std::vector<float> xCuts{ rct.x, rct.x + rct.w }; std::vector<float> yCuts{ rct.y, rct.y + rct.h };
        auto UOfX = [&](float x){ return LerpF(opt.uv.u0, opt.uv.u1, (x - rc.x)/std::max(1e-6f, rc.w)); };
        auto VOfY = [&](float y){ return LerpF(opt.uv.v0, opt.uv.v1, (y - rc.y)/std::max(1e-6f, rc.h)); };
        auto addXCuts = [&](float uL, float uR){ float uMin=std::min(uL,uR), uMax=std::max(uL,uR); int k0=(int)std::ceil(uMin-1e-6f), k1=(int)std::floor(uMax+1e-6f); for(int k=k0;k<=k1;++k){ if(k<=uMin||k>=uMax) continue; float xs = rc.x + ((float)k - opt.uv.u0) * rc.w / std::max(1e-6f, (opt.uv.u1 - opt.uv.u0)); if (xs>rct.x+1e-5f && xs<rct.x+rct.w-1e-5f) xCuts.push_back(xs);} };
        auto addYCuts = [&](float vT, float vB){ float vMin=std::min(vT,vB), vMax=std::max(vT,vB); int k0=(int)std::ceil(vMin-1e-6f), k1=(int)std::floor(vMax+1e-6f); for(int k=k0;k<=k1;++k){ if(k<=vMin||k>=vMax) continue; float ys = rc.y + ((float)k - opt.uv.v0) * rc.h / std::max(1e-6f, (opt.uv.v1 - opt.uv.v0)); if (ys>rct.y+1e-5f && ys<rct.y+rct.h-1e-5f) yCuts.push_back(ys);} };
        float uL=UOfX(rct.x), uR=UOfX(rct.x+rct.w), vT=VOfY(rct.y), vB=VOfY(rct.y+rct.h);
        addXCuts(uL,uR); addYCuts(vT,vB);
        std::sort(xCuts.begin(), xCuts.end()); xCuts.erase(std::unique(xCuts.begin(), xCuts.end()), xCuts.end());
        std::sort(yCuts.begin(), yCuts.end()); yCuts.erase(std::unique(yCuts.begin(), yCuts.end()), yCuts.end());
        for(size_t yi=0; yi+1<yCuts.size(); ++yi){ for(size_t xi=0; xi+1<xCuts.size(); ++xi){ RectF sub{ xCuts[xi], yCuts[yi], xCuts[xi+1]-xCuts[xi], yCuts[yi+1]-yCuts[yi] }; if (sub.w>1e-6f && sub.h>1e-6f) AppendQuad(sub); }} };

    // 9-patch fill (center + edge strips + 4 corner sectors)
    float rL = std::max(r.tl, r.bl), rR = std::max(r.tr, r.br), rT = std::max(r.tl, r.tr), rB = std::max(r.bl, r.br);
    const RectF rcInner{x0 + rL, y0 + rT, rc.w - (rL + rR), rc.h - (rT + rB)};
    if (rcInner.w > 0 && rcInner.h > 0)
        AppendQuadSeamAware(rcInner);
    RectF top{x0 + r.tl, y0, rc.w - (r.tl + r.tr), rT};
    RectF bottom{x0 + r.bl, y1 - rB, rc.w - (r.bl + r.br), rB};
    RectF left{x0, y0 + r.tl, rL, rc.h - (r.tl + r.bl)};
    RectF right{x1 - rR, y0 + r.tr, rR, rc.h - (r.tr + r.br)};
    if (top.w > 0 && top.h > 0)
        AppendQuadSeamAware(top);
    if (bottom.w > 0 && bottom.h > 0)
        AppendQuadSeamAware(bottom);
    if (left.w > 0 && left.h > 0)
        AppendQuadSeamAware(left);
    if (right.w > 0 && right.h > 0)
        AppendQuadSeamAware(right);

    auto PushPosUV = [&](const Vec2& p)
    { PushVert(p.x, p.y); };
    auto ArcSectorRad = [&](float cx, float cy, float rad, float a0, float a1)
    {
        Vec2 c{cx, cy};
        Vec2 crp = RotateAround(c, opt.pivot, opt.rotationRadians);
        float u01c = 0.0f, v01c = 0.0f;
        if (opt.uvMapping == ShapeOptions::UVMappingMode::Local)
        {
            Vec2 d{crp.x - p0r.x, crp.y - p0r.y};
            float du = d.x * ex.x + d.y * ex.y;
            float dv = d.x * ey.x + d.y * ey.y;
            u01c = du / std::max(1e-6f, rc.w);
            v01c = dv / std::max(1e-6f, rc.h);
        }
        else
        {
            u01c = (crp.x - rc.x) / std::max(1e-6f, rc.w);
            v01c = (crp.y - rc.y) / std::max(1e-6f, rc.h);
        }
        float cu = LerpF(opt.uv.u0, opt.uv.u1, u01c);
        float cv = LerpF(opt.uv.v0, opt.uv.v1, v01c);
        ColorF cc = GradColorAt(u01c, v01c);
        uint32_t cIdx = (uint32_t)outV.size();
        outV.push_back({crp.x, crp.y, cu, cv, cc.r, cc.g, cc.b, cc.a});
        uint32_t start = (uint32_t)outV.size();
        for (uint32_t i = 0; i <= seg; ++i)
        {
            float t = (float)i / (float)seg;
            float a = a0 + (a1 - a0) * t;
            Vec2 pOut{cx + rad * std::cos(a), cy + rad * std::sin(a)};
            PushPosUV(pOut);
        }
        for (uint32_t i = 0; i < seg; ++i)
        {
            uint32_t i0 = start + i, i1 = start + i + 1;
            outI.insert(outI.end(), {cIdx, i0, i1});
        }
    };
    if (r.tl > 0)
        ArcSectorRad(x0 + r.tl, y0 + r.tl, r.tl, kPi, 1.5f * kPi);
    if (r.tr > 0)
        ArcSectorRad(x1 - r.tr, y0 + r.tr, r.tr, 1.5f * kPi, 2.0f * kPi);
    if (r.br > 0)
        ArcSectorRad(x1 - r.br, y1 - r.br, r.br, 0.0f, 0.5f * kPi);
    if (r.bl > 0)
        ArcSectorRad(x0 + r.bl, y1 - r.bl, r.bl, 0.5f * kPi, kPi);

    // Border ring with per-edge widths and alignment
    BorderWidths bw = opt.borderLTRB;
    if (bw.left == 0 && bw.top == 0 && bw.right == 0 && bw.bottom == 0)
        bw = {opt.borderThickness, opt.borderThickness, opt.borderThickness, opt.borderThickness};
    const bool hasStroke = (bw.left > 0 || bw.top > 0 || bw.right > 0 || bw.bottom > 0);

    if (hasStroke)
    {
        auto clamp2 = [&](float v, float lo)
        { return std::max(0.0f, v - lo); };
        RectF rcInnerB = rc, rcOuterB = rc;
        CornerRadii rInner = r, rOuter = r;
        auto incR = [&](float a, float b)
        { return std::max(a, b); };
        switch (opt.strokeAlign)
        {
        case ShapeOptions::StrokeAlign::Inside:
            rcInnerB = {rc.x + bw.left, rc.y + bw.top, rc.w - (bw.left + bw.right), rc.h - (bw.top + bw.bottom)};
            rInner.tl = clamp2(r.tl, incR(bw.left, bw.top));
            rInner.tr = clamp2(r.tr, incR(bw.right, bw.top));
            rInner.br = clamp2(r.br, incR(bw.right, bw.bottom));
            rInner.bl = clamp2(r.bl, incR(bw.left, bw.bottom));
            rcOuterB = rc;
            rOuter = r;
            break;
        case ShapeOptions::StrokeAlign::Center:
        default:
            rcInnerB = {rc.x + bw.left * 0.5f, rc.y + bw.top * 0.5f, rc.w - (bw.left * 0.5f + bw.right * 0.5f), rc.h - (bw.top * 0.5f + bw.bottom * 0.5f)};
            rInner.tl = clamp2(r.tl, incR(bw.left * 0.5f, bw.top * 0.5f));
            rInner.tr = clamp2(r.tr, incR(bw.right * 0.5f, bw.top * 0.5f));
            rInner.br = clamp2(r.br, incR(bw.right * 0.5f, bw.bottom * 0.5f));
            rInner.bl = clamp2(r.bl, incR(bw.left * 0.5f, bw.bottom * 0.5f));
            rcOuterB = {rc.x - bw.left * 0.5f, rc.y - bw.top * 0.5f, rc.w + (bw.left * 0.5f + bw.right * 0.5f), rc.h + (bw.top * 0.5f + bw.bottom * 0.5f)};
            rOuter.tl = r.tl + incR(bw.left * 0.5f, bw.top * 0.5f);
            rOuter.tr = r.tr + incR(bw.right * 0.5f, bw.top * 0.5f);
            rOuter.br = r.br + incR(bw.right * 0.5f, bw.bottom * 0.5f);
            rOuter.bl = r.bl + incR(bw.left * 0.5f, bw.bottom * 0.5f);
            break;
        case ShapeOptions::StrokeAlign::Outside:
            rcOuterB = {rc.x - bw.left, rc.y - bw.top, rc.w + (bw.left + bw.right), rc.h + (bw.top + bw.bottom)};
            rOuter.tl = r.tl + incR(bw.left, bw.top);
            rOuter.tr = r.tr + incR(bw.right, bw.top);
            rOuter.br = r.br + incR(bw.right, bw.bottom);
            rOuter.bl = r.bl + incR(bw.left, bw.bottom);
            rcInnerB = rc;
            rInner = r;
            break;
        }
        auto emitRRBorder = [&](const RectF& rr, const CornerRadii& rad, std::vector<Vec2>& out)
        {
            out.clear();
            out.reserve(4 * (seg + 1));
            auto add = [&](float cx, float cy, float radv, float a0, float a1)
            {
                for(uint32_t i=0;i<=seg;++i){ float t=(float)i/(float)seg; float a=a0+(a1-a0)*t; out.push_back({cx + radv*std::cos(a), cy + radv*std::sin(a)});} };
            add(rr.x + rad.tl, rr.y + rad.tl, rad.tl, kPi, 1.5f * kPi);
            add(rr.x + rr.w - rad.tr, rr.y + rad.tr, rad.tr, 1.5f * kPi, 2.0f * kPi);
            add(rr.x + rr.w - rad.br, rr.y + rr.h - rad.br, rad.br, 0.0f, 0.5f * kPi);
            add(rr.x + rad.bl, rr.y + rr.h - rad.bl, rad.bl, 0.5f * kPi, kPi);
        };
        std::vector<Vec2> outer, inner;
        emitRRBorder(rcOuterB, rOuter, outer);
        emitRRBorder(rcInnerB, rInner, inner);
        const uint32_t base = (uint32_t)outV.size();
        const uint32_t n = (uint32_t)std::min(outer.size(), inner.size());
        std::vector<ColorF> cols;
        cols.reserve(n);
        if (opt.useBorderColorsLTRB && outer.size() >= 4 * (seg + 1))
        {
            // Arc ordering: TL, TR, BR, BL (each arc contributes seg+1 points).
            const BorderColorsLTRB ec = opt.borderColorsLTRB;
            const uint32_t arcLen = seg + 1;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t arc = (arcLen > 0) ? (i / arcLen) : 0;
                const uint32_t local = (arcLen > 1) ? (i % arcLen) : 0;
                const float t = (arcLen > 1) ? ((float)local / (float)(arcLen - 1)) : 0.0f;
                switch (arc)
                {
                default:
                case 0: // TL: left -> top
                    cols.push_back(Lerp(ec.left, ec.top, t));
                    break;
                case 1: // TR: top -> right
                    cols.push_back(Lerp(ec.top, ec.right, t));
                    break;
                case 2: // BR: right -> bottom
                    cols.push_back(Lerp(ec.right, ec.bottom, t));
                    break;
                case 3: // BL: bottom -> left
                    cols.push_back(Lerp(ec.bottom, ec.left, t));
                    break;
                }
            }
        }
        else
        {
            cols.assign(n, opt.borderColor);
        }

        auto pb = [&](const Vec2& p, const ColorF& c)
        {
            Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
            outV.push_back({pr.x, pr.y, 0, 0, c.r, c.g, c.b, c.a});
        };
        for (uint32_t i = 0; i < n; ++i)
            pb(outer[i], cols[i]);
        for (uint32_t i = 0; i < n; ++i)
            pb(inner[i], cols[i]);
        for (uint32_t i = 0; i < n; ++i)
        {
            uint32_t i0 = i, i1 = (i + 1) % n;
            outI.insert(outI.end(), {base + i0, base + i1, base + n + i1, base + i0, base + n + i1, base + n + i0});
        }

        // Optional geometry-based AA fringes for rounded-rect borders.
        // - Outer fringe: fades border alpha to 0 outside the outer boundary.
        // - Inner fringe (only when fill exists): fades border alpha to 0 inside the inner boundary,
        //   blending smoothly into the fill without relying on MSAA.
        if (opt.antialias && n > 0)
        {
            const float aa = std::max(0.5f, opt.antialiasWidthPx);

            // Outer fringe (outside -> outer boundary)
            const uint32_t aaOuterBase = (uint32_t)outV.size();
            for (uint32_t i = 0; i < n; ++i)
            {
                const Vec2 d = {outer[i].x - inner[i].x, outer[i].y - inner[i].y};
                const Vec2 nd = norm(d);
                const Vec2 p = {outer[i].x + nd.x * aa, outer[i].y + nd.y * aa};
                ColorF c0 = cols[i];
                c0.a = 0.0f;
                pb(p, c0);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t i0 = i, i1 = (i + 1) % n;
                const uint32_t a0 = aaOuterBase + i0, a1 = aaOuterBase + i1;
                const uint32_t o0 = base + i0, o1 = base + i1;
                outI.insert(outI.end(), {a0, a1, o1, a0, o1, o0});
            }

            // Inner fringe (inner boundary -> inside), only when there is a fill behind the border.
            if (opt.fillColor.a > 0.0f)
            {
                const uint32_t aaInnerBase = (uint32_t)outV.size();
                for (uint32_t i = 0; i < n; ++i)
                {
                    const Vec2 d = {inner[i].x - outer[i].x, inner[i].y - outer[i].y};
                    const Vec2 nd = norm(d);
                    const Vec2 p = {inner[i].x + nd.x * aa, inner[i].y + nd.y * aa};
                    ColorF c0 = cols[i];
                    c0.a = 0.0f;
                    pb(p, c0);
                }
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint32_t i0 = i, i1 = (i + 1) % n;
                    const uint32_t in0 = base + n + i0, in1 = base + n + i1;
                    const uint32_t a0 = aaInnerBase + i0, a1 = aaInnerBase + i1;
                    outI.insert(outI.end(), {in0, in1, a1, in0, a1, a0});
                }
            }
        }
    }

    // Optional anti-aliased outer fringe for rounded rect fills (no border).
    // This adds a thin ring that fades alpha to 0 outside the shape boundary, improving corner fidelity
    // at small radii without relying on MSAA.
    //
    // NOTE: When a border is present, the outer edge color should come from the border, not the fill.
    // The border AA fringe above handles that case to avoid a colored halo.
    if (opt.antialias && opt.fillColor.a > 0.0f && !hasStroke)
    {
        const float aa = std::max(0.5f, opt.antialiasWidthPx);
        RectF outerRc{rc.x - aa, rc.y - aa, rc.w + aa * 2.0f, rc.h + aa * 2.0f};
        CornerRadii outerRad{
            r.tl + aa,
            r.tr + aa,
            r.br + aa,
            r.bl + aa,
        };
        // Emit borders for AA fringe as a ring between outer and inner boundaries.
        std::vector<Vec2> outer, inner;
        outer.reserve(4 * (seg + 1));
        inner.reserve(4 * (seg + 1));
        auto emitRRBorder = [&](const RectF& rr, const CornerRadii& rad, std::vector<Vec2>& out)
        {
            out.clear();
            out.reserve(4 * (seg + 1));
            auto add = [&](float cx, float cy, float radv, float a0, float a1)
            {
                for(uint32_t i=0;i<=seg;++i){ float t=(float)i/(float)seg; float a=a0+(a1-a0)*t; out.push_back({cx + radv*std::cos(a), cy + radv*std::sin(a)});} };
            add(rr.x + rad.tl, rr.y + rad.tl, rad.tl, kPi, 1.5f * kPi);
            add(rr.x + rr.w - rad.tr, rr.y + rad.tr, rad.tr, 1.5f * kPi, 2.0f * kPi);
            add(rr.x + rr.w - rad.br, rr.y + rr.h - rad.br, rad.br, 0.0f, 0.5f * kPi);
            add(rr.x + rad.bl, rr.y + rr.h - rad.bl, rad.bl, 0.5f * kPi, kPi);
        };
        emitRRBorder(outerRc, outerRad, outer);
        emitRRBorder(rc, r, inner);
        const uint32_t n = (uint32_t)std::min(outer.size(), inner.size());
        if (n > 0)
        {
            const uint32_t base = (uint32_t)outV.size();
            // Outer vertices (alpha=0)
            ColorF outCol = opt.fillColor;
            ColorF inCol = opt.fillColor;
            outCol.a = 0.0f;
            auto pb = [&](const Vec2& p, const ColorF& c)
            {
                Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
                outV.push_back({pr.x, pr.y, 0, 0, c.r, c.g, c.b, c.a});
            };
            for (uint32_t i = 0; i < n; ++i)
                pb(outer[i], outCol);
            for (uint32_t i = 0; i < n; ++i)
                pb(inner[i], inCol);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t i0 = i, i1 = (i + 1) % n;
                outI.insert(outI.end(), {base + i0, base + i1, base + n + i1, base + i0, base + n + i1, base + n + i0});
            }
        }
    }
}

#if 0  // disable stray legacy code

        auto VOfY = [&](float y){ return LerpF(opt.uv.v0, opt.uv.v1, (y - rc.y)/std::max(1e-6f, rc.h)); };

        float uL = UOfX(r.x), uR = UOfX(r.x + r.w);
        float vT = VOfY(r.y), vB = VOfY(r.y + r.h);
        addXCuts(uL,uR); addYCuts(vT,vB);
        std::sort(xCuts.begin(), xCuts.end()); xCuts.erase(std::unique(xCuts.begin(), xCuts.end()), xCuts.end());
        std::sort(yCuts.begin(), yCuts.end()); yCuts.erase(std::unique(yCuts.begin(), yCuts.end()), yCuts.end());
        auto nearInt = [](float v){ float iv = std::round(v); return std::fabs(v-iv) < 1e-6f; };
        constexpr float kEps = 1e-5f;
        for(size_t yi=0; yi+1<yCuts.size(); ++yi){ for(size_t xi=0; xi+1<xCuts.size(); ++xi){
            RectF sub{ xCuts[xi], yCuts[yi], xCuts[xi+1]-xCuts[xi], yCuts[yi+1]-yCuts[yi] };
            if (sub.w <= 1e-6f || sub.h <= 1e-6f) continue;
            float uLeft = UOfX(sub.x), uRight = UOfX(sub.x + sub.w);
            float vTop  = VOfY(sub.y), vBot   = VOfY(sub.y + sub.h);
            float uBiasL = nearInt(uLeft)  ? +kEps : 0.0f;   // push into its tile
            float uBiasR = nearInt(uRight) ? -kEps : 0.0f;   // pull from seam
            float vBiasT = nearInt(vTop)   ? +kEps : 0.0f;
            float vBiasB = nearInt(vBot)   ? -kEps : 0.0f;
            AppendQuadWithBias(sub, uBiasL, uBiasR, vBiasT, vBiasB);
        }}
    };

    // Center
    AppendQuadSeamAware(rcInner);

    // Edge strips
    RectF top   { x0 + r, y0,        rc.w - 2*r, r };
    RectF bottom{ x0 + r, y1 - r,    rc.w - 2*r, r };
    RectF left  { x0,     y0 + r,    r,          rc.h - 2*r };
    RectF right { x1 - r, y0 + r,    r,          rc.h - 2*r };
    AppendQuadSeamAware(top);
    AppendQuadSeamAware(bottom);
    AppendQuadSeamAware(left);
    AppendQuadSeamAware(right);

    // Corner arc sectors (pie slices) from inner junction to outer arc
    auto PushPosUV = [&](const Vec2& p){
        Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
        float u01=0.0f, v01=0.0f;
        if (opt.uvMapping == ShapeOptions::UVMappingMode::Local) {
            Vec2 d{ pr.x - p0r.x, pr.y - p0r.y };
            float du = d.x*ex.x + d.y*ex.y;
            float dv = d.x*ey.x + d.y*ey.y;
            u01 = du / std::max(1e-6f, rc.w);
            v01 = dv / std::max(1e-6f, rc.h);
        } else {
            u01 = (pr.x - rc.x) / std::max(1e-6f, rc.w);
            v01 = (pr.y - rc.y) / std::max(1e-6f, rc.h);
        }
        float uu = LerpF(opt.uv.u0, opt.uv.u1, u01);
        float vv = LerpF(opt.uv.v0, opt.uv.v1, v01);
        ColorF col = opt.fillColor;
        if (opt.gradient == GradientMode::Vertical)      col = Lerp(opt.gradientColor0, opt.gradientColor1, v01);
        else if (opt.gradient == GradientMode::Horizontal) col = Lerp(opt.gradientColor0, opt.gradientColor1, u01);
        outV.push_back({pr.x, pr.y, uu, vv, col.r,col.g,col.b,col.a});
    };
    auto ArcSector = [&](float cx, float cy, float a0, float a1){
        // Center at the junction of center/edge strips
        Vec2 c{cx, cy};
        Vec2 cr = RotateAround(c, opt.pivot, opt.rotationRadians);
        // Compute UV for center using same mapping
        float u01c=0.0f, v01c=0.0f;
        if (opt.uvMapping == ShapeOptions::UVMappingMode::Local) {
            Vec2 d{ cr.x - p0r.x, cr.y - p0r.y };
            float du = d.x*ex.x + d.y*ex.y;
            float dv = d.x*ey.x + d.y*ey.y;
            u01c = du / std::max(1e-6f, rc.w);
            v01c = dv / std::max(1e-6f, rc.h);
        } else {
            u01c = (cr.x - rc.x) / std::max(1e-6f, rc.w);
            v01c = (cr.y - rc.y) / std::max(1e-6f, rc.h);
        }
        float cu = LerpF(opt.uv.u0, opt.uv.u1, u01c);
        float cv = LerpF(opt.uv.v0, opt.uv.v1, v01c);
        ColorF cc = opt.fillColor;
        if (opt.gradient == GradientMode::Vertical)      cc = Lerp(opt.gradientColor0, opt.gradientColor1, v01c);
        else if (opt.gradient == GradientMode::Horizontal) cc = Lerp(opt.gradientColor0, opt.gradientColor1, u01c);
        uint32_t cIdx = (uint32_t)outV.size();
        outV.push_back({cr.x, cr.y, cu, cv, cc.r,cc.g,cc.b,cc.a});
        // Outer arc points
        uint32_t start = (uint32_t)outV.size();
        for (uint32_t i=0;i<=seg;++i){
            float t = (float)i/(float)seg; float a = a0 + (a1 - a0)*t;
            Vec2 pOut{ cx + r * std::cos(a), cy + r * std::sin(a) };
            PushPosUV(pOut);
        }
        for (uint32_t i=0;i<seg;++i){ uint32_t i0 = start + i, i1 = start + i + 1; outI.insert(outI.end(), { cIdx, i0, i1 }); }
    };
    ArcSector(x0 + r, y0 + r, kPi, 1.5f*kPi);       // top-left
    ArcSector(x1 - r, y0 + r, 1.5f*kPi, 2.0f*kPi);  // top-right
    ArcSector(x1 - r, y1 - r, 0.0f, 0.5f*kPi);      // bottom-right
    ArcSector(x0 + r, y1 - r, 0.5f*kPi, kPi);       // bottom-left

    // Border: build as offset ring between outer poly and inner offset by borderThickness
    if (opt.borderThickness > 0.0f) {
        const float bt = std::min(opt.borderThickness, r); // clamp simplistic
        // Inner rounded rect: same polygon but shrink radii and inset rect by bt
        const RectF irc{ rc.x + bt, rc.y + bt, rc.w - 2*bt, rc.h - 2*bt };
        const float ir = std::max(0.0f, r - bt);
        std::vector<Vec2> inner; inner.reserve(poly.size());
        auto emitArcInner = [&](float cx, float cy, float a0, float a1, float rad){
            const int steps = (int)seg;
            for (int i=0;i<=steps;++i){ float t=(float)i/(float)steps; float a=a0+(a1-a0)*t; inner.push_back({cx + rad*std::cos(a), cy + rad*std::sin(a)}); }
        };
        emitArcInner(irc.x + ir, irc.y + ir, kPi, 1.5f*kPi, ir);
        emitArcInner(irc.x + irc.w - ir, irc.y + ir, 1.5f*kPi, 2.0f*kPi, ir);
        emitArcInner(irc.x + irc.w - ir, irc.y + irc.h - ir, 0.0f, 0.5f*kPi, ir);
        emitArcInner(irc.x + ir, irc.y + irc.h - ir, 0.5f*kPi, kPi, ir);
        // Emit ring verts (outer first, then inner) with border color
        const uint32_t b = static_cast<uint32_t>(outV.size());
        auto pushBorder = [&](const Vec2& p){ Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians); outV.push_back({pr.x, pr.y, 0,0, opt.borderColor.r,opt.borderColor.g,opt.borderColor.b,opt.borderColor.a}); };
        for (auto& p: poly) pushBorder(p);
        for (auto& p: inner) pushBorder(p);
        const uint32_t n = (uint32_t)poly.size();
        for (uint32_t i=0;i<n;++i){
            uint32_t i0 = i, i1 = (i+1)%n; // quad between (outer i,i1) and (inner i,i1)
            outI.insert(outI.end(), { b+i0, b+i1, b+n+i1, b+i0, b+n+i1, b+n+i0 });
        }
    }
}
#endif // legacy AppendRoundedRect body disabled in favor of AppendRoundedRectRadii

inline void AppendEllipse(std::vector<ShapeVertex2D>& outV,
                          std::vector<uint32_t>& outI,
                          const Vec2& center,
                          const Vec2& radii,
                          const ShapeOptions& opt)
{
    const uint32_t seg = std::max(12u, opt.segments);
    // Best-quality: build as a two-ring strip (avoids center-fan pinwheel)
    const float innerScale = 0.5f; // inner ring at 50% of radii
    const uint32_t base = static_cast<uint32_t>(outV.size());

    auto push = [&](const Vec2& p)
    {
        Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
        float u01 = 0.0f, v01 = 0.0f;
        if (opt.uvMapping == ShapeOptions::UVMappingMode::Local)
        {
            // Local: map using ellipse's unrotated bbox (p relative to center/radii)
            u01 = (p.x - (center.x - radii.x)) / std::max(1e-6f, 2.0f * radii.x);
            v01 = (p.y - (center.y - radii.y)) / std::max(1e-6f, 2.0f * radii.y);
        }
        else
        {
            // Global: map using screen-aligned coords of rotated position
            u01 = (pr.x - (center.x - radii.x)) / std::max(1e-6f, 2.0f * radii.x);
            v01 = (pr.y - (center.y - radii.y)) / std::max(1e-6f, 2.0f * radii.y);
        }
        float uu = opt.uv.u0 + (opt.uv.u1 - opt.uv.u0) * u01;
        float vv = opt.uv.v0 + (opt.uv.v1 - opt.uv.v0) * v01;
        ColorF col = opt.fillColor;
        if (opt.gradient == GradientMode::Vertical)
            col = Lerp(opt.gradientColor0, opt.gradientColor1, v01);
        else if (opt.gradient == GradientMode::Horizontal)
            col = Lerp(opt.gradientColor0, opt.gradientColor1, u01);
        outV.push_back({pr.x, pr.y, uu, vv, col.r, col.g, col.b, col.a});
    };

    for (uint32_t i = 0; i <= seg; ++i)
    {
        float a = (2.0f * kPi * (float)i) / (float)seg;
        Vec2 pInner{center.x + innerScale * radii.x * std::cos(a), center.y + innerScale * radii.y * std::sin(a)};
        Vec2 pOuter{center.x + radii.x * std::cos(a), center.y + radii.y * std::sin(a)};
        push(pInner);
        push(pOuter);
    }
    for (uint32_t i = 0; i < seg; ++i)
    {
        uint32_t i0 = base + 2 * i;
        uint32_t i1 = base + 2 * i + 1;
        uint32_t i2 = base + 2 * i + 3;
        uint32_t i3 = base + 2 * i + 2;
        outI.insert(outI.end(), {i0, i1, i2, i0, i2, i3});
    }

    // Fill center area with a small fan so the disk is solid
    {
        float uu = opt.uv.u0 + (opt.uv.u1 - opt.uv.u0) * 0.5f;
        float vv = opt.uv.v0 + (opt.uv.v1 - opt.uv.v0) * 0.5f;
        ColorF col = opt.fillColor;
        if (opt.gradient == GradientMode::Vertical)
            col = Lerp(opt.gradientColor0, opt.gradientColor1, 0.5f);
        else if (opt.gradient == GradientMode::Horizontal)
            col = Lerp(opt.gradientColor0, opt.gradientColor1, 0.5f);
        Vec2 c = RotateAround(center, opt.pivot, opt.rotationRadians);
        uint32_t cIdx = (uint32_t)outV.size();
        outV.push_back({c.x, c.y, uu, vv, col.r, col.g, col.b, col.a});
        for (uint32_t i = 0; i < seg; ++i)
        {
            uint32_t i0 = base + 2 * i;       // inner i
            uint32_t i1 = base + 2 * (i + 1); // inner i+1
            outI.insert(outI.end(), {cIdx, i1, i0});
        }
    }

    if (opt.borderThickness > 0.0f)
    {
        const float bt = std::max(0.0f, opt.borderThickness);
        const uint32_t b = static_cast<uint32_t>(outV.size());
        // Outer ring
        for (uint32_t i = 0; i <= seg; ++i)
        {
            float t = (float)i / (float)seg;
            float a = t * 2.0f * kPi;
            Vec2 p{center.x + radii.x * std::cos(a), center.y + radii.y * std::sin(a)};
            Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
            outV.push_back({pr.x, pr.y, 0, 0, opt.borderColor.r, opt.borderColor.g, opt.borderColor.b, opt.borderColor.a});
        }
        // Inner ring (shrink radii by bt)
        Vec2 ir{std::max(0.0f, radii.x - bt), std::max(0.0f, radii.y - bt)};
        for (uint32_t i = 0; i <= seg; ++i)
        {
            float t = (float)i / (float)seg;
            float a = t * 2.0f * kPi;
            Vec2 p{center.x + ir.x * std::cos(a), center.y + ir.y * std::sin(a)};
            Vec2 pr = RotateAround(p, opt.pivot, opt.rotationRadians);
            outV.push_back({pr.x, pr.y, 0, 0, opt.borderColor.r, opt.borderColor.g, opt.borderColor.b, opt.borderColor.a});
        }
        for (uint32_t i = 0; i < seg; ++i)
        {
            uint32_t o0 = b + i, o1 = b + i + 1, i0 = b + (seg + 1) + i, i1 = b + (seg + 1) + i + 1;
            outI.insert(outI.end(), {o0, o1, i1, o0, i1, i0});
        }
    }
}

inline void AppendCircle(std::vector<ShapeVertex2D>& outV,
                         std::vector<uint32_t>& outI,
                         const Vec2& center,
                         float radius,
                         const ShapeOptions& opt)
{
    AppendEllipse(outV, outI, center, {radius, radius}, opt);
}

inline void AppendTriangle(std::vector<ShapeVertex2D>& outV,
                           std::vector<uint32_t>& outI,
                           const Vec2& a, const Vec2& b, const Vec2& c,
                           const ShapeOptions& opt)
{
    const uint32_t base = static_cast<uint32_t>(outV.size());
    Vec2 ar = RotateAround(a, opt.pivot, opt.rotationRadians);
    Vec2 br = RotateAround(b, opt.pivot, opt.rotationRadians);
    Vec2 cr = RotateAround(c, opt.pivot, opt.rotationRadians);
    // Gradient per-vertex color using triangle's AABB in local (unrotated) space
    float minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
    float minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
    auto gradColFor = [&](const Vec2& p)
    {
        ColorF col = opt.fillColor;
        float tu = (maxX > minX) ? (p.x - minX) / (maxX - minX) : 0.5f;
        float tv = (maxY > minY) ? (p.y - minY) / (maxY - minY) : 0.5f;
        if (opt.gradient == GradientMode::Vertical)
            col = Lerp(opt.gradientColor0, opt.gradientColor1, tv);
        else if (opt.gradient == GradientMode::Horizontal)
            col = Lerp(opt.gradientColor0, opt.gradientColor1, tu);
        return col;
    };
    ColorF colA = gradColFor(a);
    ColorF colB = gradColFor(b);
    ColorF colC = gradColFor(c);
    outV.push_back({ar.x, ar.y, 0, 0, colA.r, colA.g, colA.b, colA.a});
    outV.push_back({br.x, br.y, 1, 0, colB.r, colB.g, colB.b, colB.a});
    outV.push_back({cr.x, cr.y, 0.5f, 1, colC.r, colC.g, colC.b, colC.a});
    outI.insert(outI.end(), {base + 0, base + 1, base + 2});
    if (opt.borderThickness > 0.0f)
    {
        // Use polyline stroke around the triangle perimeter so joins at the apexes are correct
        std::vector<Vec2> loop{a, b, c, a};
        AppendPolyline(outV, outI, loop, opt.borderThickness, opt);
    }
}

inline void AppendPolyline(std::vector<ShapeVertex2D>& outV,
                           std::vector<uint32_t>& outI,
                           const std::vector<Vec2>& pts,
                           float thickness,
                           const ShapeOptions& opt)
{
    if (pts.size() < 2 || thickness <= 0.0f)
        return;
    const float half = thickness * 0.5f;
    // Rotate points into render space
    std::vector<Vec2> P;
    P.reserve(pts.size());
    for (auto& p : pts)
        P.push_back(RotateAround(p, opt.pivot, opt.rotationRadians));
    auto perp = [](const Vec2& v)
    { return Vec2{-v.y, v.x}; };
    auto norm = [](const Vec2& v)
    { float L=std::sqrt(v.x*v.x+v.y*v.y); if (L<=1e-6f) return Vec2{0,0}; return Vec2{v.x/L, v.y/L}; };

    uint32_t prev_aL [[maybe_unused]] = 0, prev_aR [[maybe_unused]] = 0,
                     prev_bL = 0, prev_bR = 0; // indices of previous segment corners
    Vec2 tPrev{0, 0}, nPrev{0, 0};
    bool havePrev = false;

    for (size_t si = 0; si + 1 < P.size(); ++si)
    {
        Vec2 a = P[si];
        Vec2 b = P[si + 1];
        Vec2 t = norm({b.x - a.x, b.y - a.y});
        Vec2 n = perp(t);
        // Segment quad corners (L = outer to the left of direction a->b)
        Vec2 aL{a.x + n.x * half, a.y + n.y * half};
        Vec2 aR{a.x - n.x * half, a.y - n.y * half};
        Vec2 bL{b.x + n.x * half, b.y + n.y * half};
        Vec2 bR{b.x - n.x * half, b.y - n.y * half};
        uint32_t base = (uint32_t)outV.size();
        ColorF col = opt.fillColor;
        outV.push_back({aL.x, aL.y, 0, 0, col.r, col.g, col.b, col.a}); // 0
        outV.push_back({aR.x, aR.y, 1, 0, col.r, col.g, col.b, col.a}); // 1
        outV.push_back({bR.x, bR.y, 1, 1, col.r, col.g, col.b, col.a}); // 2
        outV.push_back({bL.x, bL.y, 0, 1, col.r, col.g, col.b, col.a}); // 3
        outI.insert(outI.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});

        // Join with previous segment on the outer side only
        if (havePrev)
        {
            // Determine turn direction (outer = left if cross>0, else right)
            float cross = tPrev.x * t.y - tPrev.y * t.x;
            bool leftTurn = cross < 0.0f; // UI coords: y+ downward, so invert sign for "left" turns
            // Choose outer corners at the shared point P[si]
            uint32_t prevOuter = leftTurn ? prev_bL : prev_bR;   // from previous segment end
            uint32_t currOuter = leftTurn ? base + 0 : base + 1; // current segment start (aL or aR)
            Vec2 p = P[si];
            Vec2 n0 = nPrev, n1 = n;
            if (!leftTurn)
            {
                n0 = {-nPrev.x, -nPrev.y};
                n1 = {-n.x, -n.y};
            } // map to "outer" normals

            switch (opt.strokeJoin)
            {
            default:
                break;
            case ShapeOptions::StrokeJoin::Miter:
            {
                Vec2 m = norm({n0.x + n1.x, n0.y + n1.y});
                float denom = std::max(1e-6f, m.x * n1.x + m.y * n1.y);
                float miterLen = half / denom;
                float maxLen = half * std::max(1.0f, opt.miterLimit);
                if (miterLen > maxLen)
                {
                    // Fallback to bevel: bridge outer corners to the joint point
                    uint32_t centerIdx = (uint32_t)outV.size();
                    outV.push_back({p.x, p.y, 0, 0, col.r, col.g, col.b, col.a});
                    outI.insert(outI.end(), {prevOuter, currOuter, centerIdx});
                }
                else
                {
                    // Add a miter apex vertex and connect
                    uint32_t apex = (uint32_t)outV.size();
                    Vec2 pm{p.x + m.x * miterLen, p.y + m.y * miterLen};
                    outV.push_back({pm.x, pm.y, 0, 0, col.r, col.g, col.b, col.a});
                    outI.insert(outI.end(), {prevOuter, apex, currOuter});
                }
            }
            break;
            case ShapeOptions::StrokeJoin::Bevel:
            {
                // Simple bevel: bridge directly; approximate wedge with triangle to center p
                uint32_t centerIdx = (uint32_t)outV.size();
                outV.push_back({p.x, p.y, 0, 0, col.r, col.g, col.b, col.a});
                outI.insert(outI.end(), {prevOuter, currOuter, centerIdx});
            }
            break;
            case ShapeOptions::StrokeJoin::Round:
            {
                // Outer round join: fan anchored at prevOuter, marching along arc to currOuter
                uint32_t steps = std::max(16u, opt.segments / 2);
                auto atan2f_ = [](float y, float x)
                { return std::atan2(y, x); };
                float a0 = atan2f_(n0.y, n0.x);
                float a1 = atan2f_(n1.y, n1.x);
                auto wrap = [](float a)
                { const float T=6.28318530718f; while(a<0)a+=T; while(a>=T)a-=T; return a; };
                a0 = wrap(a0);
                a1 = wrap(a1);
                if (leftTurn && a1 < a0)
                    a1 += 6.28318530718f;
                if (!leftTurn && a0 < a1)
                    a0 += 6.28318530718f;
                uint32_t anchor = prevOuter;
                // Build intermediate arc vertices (excluding endpoints)
                uint32_t last = anchor;
                for (uint32_t s = 1; s < steps; ++s)
                {
                    float tt = (float)s / (float)steps;
                    float ang = a0 + (a1 - a0) * tt;
                    Vec2 q{p.x + std::cos(ang) * half, p.y + std::sin(ang) * half};
                    uint32_t vi = (uint32_t)outV.size();
                    outV.push_back({q.x, q.y, 0, 0, col.r, col.g, col.b, col.a});
                    // Triangle from anchor to last to new point
                    outI.insert(outI.end(), {anchor, last, vi});
                    last = vi;
                }
                // Close to currOuter
                outI.insert(outI.end(), {anchor, last, currOuter});
            }
            break;
                // Inner stitch to avoid wedge hole at the joint (cover both possibilities to be safe)
                /* {
                    uint32_t centerIdx = (uint32_t)outV.size();
                    outV.push_back({p.x, p.y, 0, 0, col.r, col.g, col.b, col.a});
                    // Right-side inner (prev_bR → aR)
                    outI.insert(outI.end(), {prev_bR, base + 1, centerIdx});
                    // Left-side inner (prev_bL → aL)
                    outI.insert(outI.end(), {prev_bL, base + 0, centerIdx});
                }*/
            }
        }
        // Stash this segment's start/end outer corner indices for next join
        prev_aL = base + 0;
        prev_aR = base + 1;
        prev_bR = base + 2;
        prev_bL = base + 3;
        tPrev = t;
        nPrev = n;
        havePrev = true;
    }
}

inline void AppendLine(std::vector<ShapeVertex2D>& outV,
                       std::vector<uint32_t>& outI,
                       const Vec2& p0, const Vec2& p1,
                       float thickness,
                       const ShapeOptions& opt)
{
    std::vector<Vec2> pts{p0, p1};
    AppendPolyline(outV, outI, pts, thickness, opt);
}

// Quadratic Bezier spline polyline approximation; thickness used if > 0
inline void AppendQuadraticBezier(std::vector<ShapeVertex2D>& outV,
                                  std::vector<uint32_t>& outI,
                                  const Vec2& p0, const Vec2& p1, const Vec2& p2,
                                  uint32_t segments,
                                  float thickness,
                                  const ShapeOptions& opt)
{
    segments = std::max(8u, segments);
    std::vector<Vec2> pts;
    pts.reserve(segments + 1);
    for (uint32_t i = 0; i <= segments; ++i)
    {
        float t = (float)i / segments;
        float it = 1.0f - t;
        Vec2 p{it * it * p0.x + 2 * it * t * p1.x + t * t * p2.x,
               it * it * p0.y + 2 * it * t * p1.y + t * t * p2.y};
        pts.push_back(p);
    }
    if (thickness <= 0.0f)
    {
        // Triangulate as thin strip (degenerate fill) – instead, draw as connected thick lines
        thickness = 1.0f;
    }
    // Use a single polyline build so joins are handled
    AppendPolyline(outV, outI, pts, thickness, opt);
}

} // namespace Geometry
} // namespace Rendering
} // namespace GameEngine
