#pragma once

#include "UI/UIElement.h"

#include "AssetCore/GUID.h"
#include "AssetCore/NineSlice.h"

#include <cstdint>
#include <functional>

namespace GameEngine
{

/// Interactive 9-slice (nine-patch) border editor for the texture inspector.
/// Shows the source texture with four draggable cut lines (left/right/top/bottom)
/// and a live preview of the sliced result below. Owns no asset: the host supplies
/// the texture (GUID + source dimensions) and the current NineSlice, and edits come
/// back through the changing/changed callbacks so the host keeps ownership of the
/// metadata write + undo. M1 edits the canonical "shared lines" model (one handle
/// per side); independent-edge splitting is a later milestone.
class NineSliceField : public UIElement
{
  public:
    NineSliceField();

    /// The texture to display and slice. `width`/`height` are the source dimensions
    /// in texels (cut positions are expressed in this space).
    void SetTexture(const GUID& guid, uint32_t width, uint32_t height);

    /// Push slice state onto the field without firing callbacks (host -> field sync,
    /// e.g. from the numeric inputs or on inspector rebuild).
    void SetSlice(const NineSlice& slice);
    const NineSlice& GetSlice() const { return m_Slice; }

    /// Shared mode (default): one draggable line per side (the coincident cut pair).
    /// Independent mode: two lines per side (corner + center boundary) so a gap can
    /// open between them. The renderer drops gap bands either way.
    void SetIndependent(bool independent);
    bool IsIndependent() const { return m_Independent; }

    /// Live during a drag gesture (every move). Keep cheap.
    void SetOnChanging(std::function<void(const NineSlice&)> cb) { m_OnChanging = std::move(cb); }
    /// Once when a gesture commits (drag end) — the host persists + pushes undo here.
    void SetOnChanged(std::function<void(const NineSlice&)> cb) { m_OnChanged = std::move(cb); }

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

  private:
    // A draggable cut line: which axis (0 = X / vertical lines, 1 = Y / horizontal)
    // and which of the four cut positions [0..3]. Shared mode shows only cuts 0 and
    // 3 (each moving its coincident pair); independent mode shows all four.
    struct LineId
    {
        int axis = -1; // 0 = X, 1 = Y
        int cut = -1;  // 0..3
        bool Valid() const { return axis >= 0 && cut >= 0; }
        bool operator==(const LineId& o) const { return axis == o.axis && cut == o.cut; }
        bool operator!=(const LineId& o) const { return !(*this == o); }
    };

    struct Rect
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    };

    Rect ElementRect() const;
    static Rect CanvasArea(const Rect& el);  // where the source canvas lives
    static Rect PreviewArea(const Rect& el); // region the resizable preview lives in
    Rect PreviewBox(const Rect& el) const;   // the (resizable) preview box within that region
    Rect FitTexture(const Rect& area) const; // letterboxed source-texture rect inside `area`

    // Cut positions (texels) actually shown as lines for an axis: {0,3} shared, {0,1,2,3} independent.
    int VisibleCuts(int outCuts[4]) const;
    float CutScreenX(const Rect& canvas, int cut) const;
    float CutScreenY(const Rect& canvas, int cut) const;
    void GripCenter(const Rect& canvas, const LineId& line, float& outX, float& outY) const;
    LineId HitTestLine(float globalX, float globalY) const; // 2D grip pick (disambiguates overlaps)
    void ApplyLineDrag(LineId line, float globalX, float globalY, bool commit);
    bool HitPreviewGrip(float globalX, float globalY) const;

    GUID m_Guid;
    uint32_t m_Sw = 0;
    uint32_t m_Sh = 0;
    NineSlice m_Slice;
    bool m_Independent = false;
    LineId m_Drag;
    LineId m_Hover;

    // Resizable preview box (editor-only view state; fractions of the preview region).
    float m_PreviewWFrac = 1.0f;
    float m_PreviewHFrac = 0.85f;
    bool m_ResizingPreview = false;
    bool m_HoverPreviewGrip = false;
    float m_ResizeStartX = 0.0f, m_ResizeStartY = 0.0f;
    float m_ResizeStartWFrac = 0.0f, m_ResizeStartHFrac = 0.0f;

    std::function<void(const NineSlice&)> m_OnChanging;
    std::function<void(const NineSlice&)> m_OnChanged;
};

} // namespace GameEngine
