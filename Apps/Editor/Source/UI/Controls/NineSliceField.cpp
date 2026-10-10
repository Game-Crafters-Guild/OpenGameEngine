#include "UI/Controls/NineSliceField.h"

#include "UI/NineSliceLayout.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace GameEngine
{
namespace
{
constexpr float kFieldHeightPx = 480.0f; // logical; tall so the preview can show vertical tiling
constexpr float kPadPx = 6.0f;
constexpr float kCanvasFrac = 0.50f;     // top half of the element is the source canvas
constexpr float kCanvasYOffsetPx = 3.0f; // visually centers the source editor above the preview
constexpr float kHandlePickPx = 12.0f;   // hit-test reach, raw px

const uint32_t kElementBg = UI::PackColor(0.10f, 0.11f, 0.12f, 0.96f);
const uint32_t kElementBorder = UI::PackColor(0.28f, 0.31f, 0.34f, 0.95f);
const uint32_t kCanvasBg = UI::PackColor(0.16f, 0.17f, 0.19f, 1.0f);
const uint32_t kPreviewBg = UI::PackColor(0.06f, 0.06f, 0.07f, 1.0f);
const uint32_t kHandleBorder = UI::PackColor(0.98f, 0.98f, 0.98f, 0.95f);
// Corner cuts (0,3) are blue; inner/center cuts (1,2) are amber, so the two are
// distinguishable when their lines coincide.
const uint32_t kCornerLine = UI::PackColor(0.41f, 0.66f, 1.0f, 0.95f);
const uint32_t kCornerGrip = UI::PackColor(0.41f, 0.66f, 1.0f, 1.0f);
const uint32_t kCornerGripHot = UI::PackColor(0.66f, 0.84f, 1.0f, 1.0f);
const uint32_t kInnerLine = UI::PackColor(1.0f, 0.72f, 0.26f, 0.95f);
const uint32_t kInnerGrip = UI::PackColor(1.0f, 0.72f, 0.26f, 1.0f);
const uint32_t kInnerGripHot = UI::PackColor(1.0f, 0.84f, 0.5f, 1.0f);

// The ink a field that refuses edits paints its frame and handles in. The
// theme owns the value (UI/theme/tokens.css); a canvas control cannot take it
// from the cascade the way a styled box does, so it reads the token by name.
const StringId kDisabledInkVar = HashStringId("--ui_color_canvas_disabled_ink");
// Reached only where the theme is not loaded (an unstyled host, a test).
const uint32_t kUnstyledDisabledInk = UI::PackFromARGB(0xFF666666u);

bool IsCornerCut(int cut) { return cut == 0 || cut == 3; }

uint32_t DisabledInk(const ResolvedStyle& style)
{
    const std::optional<uint32_t> themed = style.GetCustomColor(kDisabledInkVar);
    return themed ? UI::PackFromARGB(*themed) : kUnstyledDisabledInk;
}

// Spread the grips along the line so no two cuts' grips can land on the same point
// when they coincide (corner 0 with inner 1, the two inner cuts 1 & 2, inner 2 with
// corner 3) — each cut gets a distinct fraction so 2D hit-testing can disambiguate.
float GripFrac(bool independent, int cut)
{
    if (!independent)
        return 0.5f;
    switch (cut)
    {
    case 0:  return 0.30f;
    case 1:  return 0.45f;
    case 2:  return 0.55f;
    default: return 0.70f;
    }
}
} // namespace

NineSliceField::NineSliceField()
{
    AddClass("nine-slice-field");
    SetFocusable(false);
    Overrides()
        .Set(Style::Height, StyleLength::Px(kFieldHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kFieldHeightPx));
}

void NineSliceField::SetTexture(const GUID& guid, uint32_t width, uint32_t height)
{
    m_Guid = guid;
    m_Sw = width;
    m_Sh = height;
    MarkDirty(VisualDirty);
}

void NineSliceField::SetSlice(const NineSlice& slice)
{
    m_Slice = slice;
    MarkDirty(VisualDirty);
}

void NineSliceField::SetIndependent(bool independent)
{
    if (m_Independent == independent)
        return;
    m_Independent = independent;
    m_Hover = {};
    m_Drag = {};
    MarkDirty(VisualDirty);
}

NineSliceField::Rect NineSliceField::ElementRect() const
{
    return {GetLayoutX(), GetLayoutY(), GetLayoutWidth(), GetLayoutHeight()};
}

NineSliceField::Rect NineSliceField::CanvasArea(const Rect& el)
{
    return {el.x + kPadPx, el.y + kPadPx + kCanvasYOffsetPx, std::max(1.0f, el.w - 2.0f * kPadPx),
            std::max(1.0f, el.h * kCanvasFrac - kPadPx)};
}

NineSliceField::Rect NineSliceField::PreviewArea(const Rect& el)
{
    const float top = el.y + el.h * (kCanvasFrac + 0.02f);
    return {el.x + kPadPx, top, std::max(1.0f, el.w - 2.0f * kPadPx),
            std::max(1.0f, (el.y + el.h - kPadPx) - top)};
}

NineSliceField::Rect NineSliceField::PreviewBox(const Rect& el) const
{
    const Rect area = PreviewArea(el);
    const float w = std::max(20.0f, area.w * std::clamp(m_PreviewWFrac, 0.2f, 1.0f));
    const float h = std::max(20.0f, area.h * std::clamp(m_PreviewHFrac, 0.2f, 1.0f));
    return {area.x + (area.w - w) * 0.5f, area.y + (area.h - h) * 0.5f, w, h};
}

NineSliceField::Rect NineSliceField::FitTexture(const Rect& area) const
{
    if (m_Sw == 0 || m_Sh == 0)
        return area;
    const float sw = static_cast<float>(m_Sw);
    const float sh = static_cast<float>(m_Sh);
    const float s = std::min(area.w / sw, area.h / sh);
    const float dw = sw * s;
    const float dh = sh * s;
    return {area.x + (area.w - dw) * 0.5f, area.y + (area.h - dh) * 0.5f, dw, dh};
}

int NineSliceField::VisibleCuts(int out[4]) const
{
    if (m_Independent)
    {
        out[0] = 0; out[1] = 1; out[2] = 2; out[3] = 3;
        return 4;
    }
    out[0] = 0; out[1] = 3;
    return 2;
}

float NineSliceField::CutScreenX(const Rect& c, int cut) const
{
    return c.x + (static_cast<float>(m_Slice.X[cut]) / static_cast<float>(std::max<uint32_t>(m_Sw, 1))) * c.w;
}

float NineSliceField::CutScreenY(const Rect& c, int cut) const
{
    return c.y + (static_cast<float>(m_Slice.Y[cut]) / static_cast<float>(std::max<uint32_t>(m_Sh, 1))) * c.h;
}

void NineSliceField::GripCenter(const Rect& c, const LineId& line, float& outX, float& outY) const
{
    const float frac = GripFrac(m_Independent, line.cut);
    if (line.axis == 0)
    {
        outX = CutScreenX(c, line.cut);
        outY = c.y + c.h * frac;
    }
    else
    {
        outX = c.x + c.w * frac;
        outY = CutScreenY(c, line.cut);
    }
}

NineSliceField::LineId NineSliceField::HitTestLine(float gx, float gy) const
{
    if (m_Sw == 0 || m_Sh == 0)
        return {};

    const Rect c = FitTexture(CanvasArea(ElementRect()));
    int cuts[4];
    const int n = VisibleCuts(cuts);

    float best = kHandlePickPx;
    LineId hit;
    for (int axis = 0; axis < 2; ++axis)
    {
        for (int k = 0; k < n; ++k)
        {
            const LineId id{axis, cuts[k]};
            float hxp, hyp;
            GripCenter(c, id, hxp, hyp);
            const float d = std::hypot(gx - hxp, gy - hyp);
            if (d < best) { best = d; hit = id; }
        }
    }
    return hit;
}

bool NineSliceField::HitPreviewGrip(float gx, float gy) const
{
    const Rect b = PreviewBox(ElementRect());
    return std::hypot(gx - (b.x + b.w), gy - (b.y + b.h)) < kHandlePickPx + 2.0f;
}

void NineSliceField::ApplyLineDrag(LineId line, float gx, float gy, bool commit)
{
    if (!line.Valid() || m_Sw == 0 || m_Sh == 0)
        return;

    const Rect c = FitTexture(CanvasArea(ElementRect()));
    const float sw = static_cast<float>(m_Sw);
    const float sh = static_cast<float>(m_Sh);
    uint16* cuts = (line.axis == 0) ? m_Slice.X : m_Slice.Y;
    const int maxv = (line.axis == 0) ? static_cast<int>(m_Sw) : static_cast<int>(m_Sh);
    const float texel = (line.axis == 0) ? std::clamp((gx - c.x) / c.w * sw, 0.0f, sw)
                                         : std::clamp((gy - c.y) / c.h * sh, 0.0f, sh);
    const int t = static_cast<int>(std::lround(texel));

    if (!m_Independent)
    {
        if (line.cut == 0)
        {
            const int v = std::clamp(t, 0, static_cast<int>(cuts[2]));
            cuts[0] = cuts[1] = static_cast<uint16>(v);
        }
        else
        {
            const int v = std::clamp(t, static_cast<int>(cuts[1]), maxv);
            cuts[2] = cuts[3] = static_cast<uint16>(v);
        }
    }
    else
    {
        const int lo = (line.cut > 0) ? static_cast<int>(cuts[line.cut - 1]) : 0;
        const int hi = (line.cut < 3) ? static_cast<int>(cuts[line.cut + 1]) : maxv;
        cuts[line.cut] = static_cast<uint16>(std::clamp(t, lo, hi));
    }

    MarkDirty(VisualDirty);
    if (commit)
    {
        if (m_OnChanged)
            m_OnChanged(m_Slice);
    }
    else if (m_OnChanging)
    {
        m_OnChanging(m_Slice);
    }
}

void NineSliceField::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseLeave)
    {
        if (m_Hover.Valid() || m_HoverPreviewGrip)
        {
            m_Hover = {};
            m_HoverPreviewGrip = false;
            MarkDirty(VisualDirty);
        }
        return;
    }

    if (e.Id == kEventMouseMove && !m_Drag.Valid() && !m_ResizingPreview)
    {
        // A field that refuses edits takes no hover on its cut handles: nothing
        // on the canvas lights up under a cursor that cannot drag it. The
        // preview grip is not a slice edit and stays live, hover included.
        const bool grip = HitPreviewGrip(e.X, e.Y);
        const bool sliceEditable = IsEnabledInHierarchy() && m_Sw > 0 && m_Sh > 0;
        const LineId h = (sliceEditable && !grip) ? HitTestLine(e.X, e.Y) : LineId{};
        if (h != m_Hover || grip != m_HoverPreviewGrip)
        {
            m_Hover = h;
            m_HoverPreviewGrip = grip;
            MarkDirty(VisualDirty);
        }
        return;
    }

    if (e.Button != 0)
        return;

    if (e.Id == kEventMouseDown)
    {
        if (HitPreviewGrip(e.X, e.Y))
        {
            m_ResizingPreview = true;
            m_ResizeStartX = e.X;
            m_ResizeStartY = e.Y;
            m_ResizeStartWFrac = m_PreviewWFrac;
            m_ResizeStartHFrac = m_PreviewHFrac;
            e.Capture(this);
            e.Stop();
            return;
        }
        if (m_Sw == 0 || m_Sh == 0)
            return;
        // A disabled control never activates, and so does everything inside a
        // disabled group. Resizing the preview above stays live either way: it
        // changes how big the image is drawn, not the slice.
        if (!IsEnabledInHierarchy())
            return;
        const LineId hit = HitTestLine(e.X, e.Y);
        if (!hit.Valid())
            return;
        m_Drag = hit;
        ApplyLineDrag(hit, e.X, e.Y, false);
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && m_ResizingPreview)
    {
        const Rect area = PreviewArea(ElementRect());
        if (area.w > 1.0f && area.h > 1.0f)
        {
            m_PreviewWFrac = std::clamp(m_ResizeStartWFrac + 2.0f * (e.X - m_ResizeStartX) / area.w, 0.2f, 1.0f);
            m_PreviewHFrac = std::clamp(m_ResizeStartHFrac + 2.0f * (e.Y - m_ResizeStartY) / area.h, 0.2f, 1.0f);
            MarkDirty(VisualDirty);
        }
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove && m_Drag.Valid())
    {
        ApplyLineDrag(m_Drag, e.X, e.Y, false);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseUp && (m_Drag.Valid() || m_ResizingPreview))
    {
        if (m_Drag.Valid())
            ApplyLineDrag(m_Drag, e.X, e.Y, true);
        m_Drag = {};
        m_ResizingPreview = false;
        MarkDirty(VisualDirty);
        e.Stop();
    }
}

void NineSliceField::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                          float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 1.0f || h <= 1.0f)
        return;

    const float cs = ctx.ContentScale;

    // The frame and the cut handles carry whether this field takes an edit; the
    // canvas, the source image and the preview are what it reports either way,
    // so they keep their colours.
    const bool sliceEditable = IsEnabledInHierarchy();
    const uint32_t disabledInk = sliceEditable ? 0u : DisabledInk(style);

    UI::UIPrimitive bg = UI::MakeRect(x, y, w, h, kElementBg, 6.0f * cs, 6.0f * cs, 6.0f * cs, 6.0f * cs);
    UI::AddBorder(bg, 1.0f * cs, sliceEditable ? kElementBorder : disabledInk);
    ctx.Emit(bg);

    uint32_t slot = 0;
    if (ctx.Manager && ctx.Textures && !m_Guid.IsNull())
    {
        uint32_t tw = 0, th = 0;
        const Rendering::TextureHandle tex = ctx.Manager->TryGetBackgroundTextureHandleByGuid(m_Guid, &tw, &th);
        if (tex.IsValid())
            slot = ctx.Textures->Register(tex);
    }

    const Rect el{x, y, w, h};
    const Rect c = FitTexture(CanvasArea(el));
    const float sw = static_cast<float>(std::max<uint32_t>(m_Sw, 1));
    const float sh = static_cast<float>(std::max<uint32_t>(m_Sh, 1));

    ctx.Emit(UI::MakeRect(c.x, c.y, c.w, c.h, kCanvasBg, 2.0f * cs, 2.0f * cs, 2.0f * cs, 2.0f * cs));
    if (slot != 0)
        ctx.Emit(UI::MakeTexturedQuad(c.x, c.y, c.w, c.h, slot, 0xFFFFFFFFu, 0.0f, 0.0f, 1.0f, 1.0f));

    // Cut lines + draggable grips (corner cuts blue, inner cuts amber).
    if (m_Sw > 0 && m_Sh > 0)
    {
        const float lt = 1.5f * cs;
        int cuts[4];
        const int n = VisibleCuts(cuts);
        for (int axis = 0; axis < 2; ++axis)
        {
            for (int k = 0; k < n; ++k)
            {
                const int cut = cuts[k];
                const bool corner = IsCornerCut(cut);
                const LineId id{axis, cut};
                const bool hot = sliceEditable && (m_Hover == id || m_Drag == id);
                const uint32_t line = corner ? kCornerLine : kInnerLine;
                uint32_t fill = disabledInk;
                if (sliceEditable)
                    fill = hot ? (corner ? kCornerGripHot : kInnerGripHot)
                               : (corner ? kCornerGrip : kInnerGrip);

                if (axis == 0)
                {
                    const float sx = CutScreenX(c, cut);
                    ctx.Emit(UI::MakeLine(sx, c.y, sx, c.y + c.h, lt, line));
                }
                else
                {
                    const float sy = CutScreenY(c, cut);
                    ctx.Emit(UI::MakeLine(c.x, sy, c.x + c.w, sy, lt, line));
                }

                float gxp, gyp;
                GripCenter(c, id, gxp, gyp);
                const float sz = (hot ? 11.0f : 9.0f) * cs;
                UI::UIPrimitive g = UI::MakeRect(gxp - sz * 0.5f, gyp - sz * 0.5f, sz, sz, fill,
                                                 3.0f * cs, 3.0f * cs, 3.0f * cs, 3.0f * cs);
                UI::AddBorder(g, 1.5f * cs, sliceEditable ? kHandleBorder : disabledInk);
                ctx.Emit(g);
            }
        }
    }

    // Resizable live preview: the texture drawn through the same builder the
    // renderer uses, so it tiles / stretches / scales exactly as it will in-game.
    const Rect area = PreviewArea(el);
    ctx.Emit(UI::MakeRect(area.x, area.y, area.w, area.h, kPreviewBg, 3.0f * cs, 3.0f * cs, 3.0f * cs, 3.0f * cs));
    const Rect box = PreviewBox(el);
    if (slot != 0 && m_Sw > 0 && m_Sh > 0)
    {
        const float inset = 4.0f * cs;
        const Rect pb{box.x + inset, box.y + inset, std::max(1.0f, box.w - 2.0f * inset),
                      std::max(1.0f, box.h - 2.0f * inset)};
        UI::BuildNineSliceRegions(m_Slice, sw, sh, pb.x, pb.y, pb.w, pb.h, cs,
            [&](const UI::NineSliceRegion& r) {
                ctx.Emit(UI::MakeTexturedQuad(r.X, r.Y, r.W, r.H, slot, 0xFFFFFFFFu, r.U0, r.V0, r.U1, r.V1));
            });
    }

    // Preview resize grip (bottom-right of the preview box).
    {
        const bool hot = m_HoverPreviewGrip || m_ResizingPreview;
        const float sz = (hot ? 11.0f : 9.0f) * cs;
        const float gx = box.x + box.w;
        const float gy = box.y + box.h;
        UI::UIPrimitive g = UI::MakeRect(gx - sz * 0.5f, gy - sz * 0.5f, sz, sz,
                                         hot ? kCornerGripHot : kCornerGrip,
                                         3.0f * cs, 3.0f * cs, 3.0f * cs, 3.0f * cs);
        UI::AddBorder(g, 1.5f * cs, kHandleBorder);
        ctx.Emit(g);
    }
}

} // namespace GameEngine
