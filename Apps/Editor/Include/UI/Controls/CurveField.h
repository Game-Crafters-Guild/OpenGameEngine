#pragma once

#include "UI/UIElement.h"

#include "Mathematics/Curve.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

/// An interactive curve graph that edits a set of Math::CurveKey over a time/value range.
/// The user drags a key (value, and optionally time) and right-clicks to add a key at the
/// cursor or remove one. It owns no component and no curve asset: the host supplies the
/// keys + ranges and writes edits back through the changing/changed callbacks, so the host
/// keeps ownership of storage (any Curve container), undo, and multi-entity fan-out.
///
/// Styled to match the value-curve editor. Keys carry per-segment interpolation, and the
/// rendered curve samples the same evaluator the runtime uses. Tangent-handle editing for
/// Smooth keys is an opt-in for the richer consumers. The time labels and the reference line
/// take their font and colours from UI/controls/CurveField/CurveField.css.
class CurveField : public UIElement
{
  public:
    /// How the time axis is labelled.
    enum class TimeLabels : uint8_t
    {
        None,
        /// A 0-24 hour field: the hours under the graph every six hours ("06:00"), and the
        /// playback indicator's time ("17:45") where its line meets them.
        HoursOfDay,
    };

    struct Config
    {
        float TimeMin = 0.0f;
        float TimeMax = 1.0f;
        float ValueMin = 0.0f;
        float ValueMax = 1.0f;
        /// Allow dragging a key horizontally in time. Endpoints stay pinned at the range
        /// ends; interior keys clamp between their neighbours (no reordering).
        bool AllowTimeDrag = true;
        /// Allow right-click add on empty graph space and explicit remove through DeleteKey(),
        /// bounded by MinKeys / MaxKeys.
        bool AllowAddRemove = true;
        /// Draw a closing segment from the last key across to the first at TimeMax, for a
        /// cyclic curve (the sky day cycle).
        bool WrapAround = false;
        /// Enable smooth-key editing: per-key interpolation + tangent handles on the
        /// selected key. Hosts can wire SetOnKeyContextMenu for interpolation/delete commands.
        bool AllowTangentEditing = false;
        /// Draw a read-only vertical playhead at SetScrubberTime() (e.g. the sky's current time of
        /// day). Off by default; purely an indicator -- it never intercepts key dragging.
        bool ShowScrubber = false;
        /// Draw the active playback time/value indicator: a turquoise vertical time line,
        /// a dot on the curve, and a value dot on the left edge.
        bool ShowPlaybackIndicator = false;
        /// Allow left-click/dragging empty graph space to scrub the playback indicator time. Hosts receive
        /// the scrubbed time through SetOnPlaybackScrub().
        bool AllowPlaybackScrub = false;
        /// Draw the playback indicator's value marker on the left edge. A read-only graph whose
        /// value is read out elsewhere turns it off.
        bool ShowPlaybackValueMarker = true;
        TimeLabels TimeAxisLabels = TimeLabels::None;
        /// Interpolate the keys in log2 of their value (Math::EvaluateCurveKeysLogarithmic) and
        /// draw the value axis logarithmically, for a quantity that spans orders of magnitude
        /// (an illuminance from twilight to noon). The axis runs from the larger of ValueMin and
        /// LogarithmicFloor, where keys below it sit on the bottom edge, to ValueMax, widened to
        /// every key above it. LogarithmicFloor is
        /// the evaluator's floor and must be above 0 when LogarithmicValues is on (SetConfig
        /// asserts): give it the floor the runtime evaluates the same keys with, so the
        /// graph draws what the runtime reads. Tangent handles are not offered on this axis: the
        /// evaluator gives every Smooth key but a Flat one its Auto tangent in log2, whatever
        /// tangent the key stores.
        bool LogarithmicValues = false;
        float LogarithmicFloor = 0.0f;
        /// Values marked on the value axis: a faint line across the graph at each, labelled at the
        /// left edge with ValueLabel. Marks outside the axis's range are not drawn.
        std::vector<float> ValueAxisMarks;
        /// Formats a value for the value-axis marks and, with ShowPlaybackValueMarker on, for the
        /// label beside the playback value marker. Unset: neither is labelled.
        std::function<std::string(float)> ValueLabel;
        /// Display the sampled curve without selection handles or editing.
        bool ReadOnly = false;
        uint8_t MinKeys = 2;
        uint8_t MaxKeys = 32;
    };

    CurveField();

    void SetConfig(const Config& config);

    /// Replace the keys (Time-sorted on the way in). Selection is preserved when in range.
    void SetKeys(const std::vector<Math::CurveKey>& keys);
    const std::vector<Math::CurveKey>& GetKeys() const { return m_Keys; }

    int GetSelectedKey() const { return m_Selected; }
    void SetSelectedKey(int index);

    /// Apply a host-readout edit to the selected key WITHOUT firing callbacks (mirrors a
    /// numeric field back onto the graph; no feedback loop). A time edit clamps between
    /// neighbours. Returns the selected index afterwards (-1 if none).
    int SetSelectedKeyValue(float value);
    int SetSelectedKeyTime(float time);

    /// Set a key's interpolation + tangent mode and commit (used by the context menu).
    void SetKeyMode(int index, Math::CurveInterp interp, Math::CurveTangentMode mode);
    /// Remove a key and commit (bounded by MinKeys).
    void DeleteKey(int index);

    /// Position the read-only scrubber playhead (curve time units), shown when Config.ShowScrubber
    /// is on. Clamped to [TimeMin, TimeMax]. Cheap -- safe to call every frame.
    void SetScrubberTime(float time);
    /// Position the playback indicator, shown when Config.ShowPlaybackIndicator is on. `value`
    /// is in curve value units; hosts may hide it by passing visible=false.
    void SetPlaybackIndicator(float time, float value, bool visible = true);

    /// A span of the time axis shaded behind the curve, with a label at its top (for example the
    /// night, where a sun illuminance curve hands the light over to the moon).
    struct ShadedSpan
    {
        float Start = 0.0f;
        float End = 0.0f;
        std::string Label;
    };
    /// The shaded spans, in curve time units; an empty set removes them.
    void SetShadedSpans(const std::vector<ShadedSpan>& spans);

    /// A read-only line drawn behind the curve through these (time, value) samples, on the
    /// field's axes (for example the physical sun behind an authored illuminance curve). It is
    /// never hit-tested or edited. An empty set removes it.
    void SetReferenceLine(const std::vector<Math::CurveKey>& samples);

    /// Live during a drag / add / remove gesture (the whole current key set). Keep cheap.
    void SetOnChanging(std::function<void(const std::vector<Math::CurveKey>&)> callback)
    {
        m_OnChanging = std::move(callback);
    }
    /// Once when a gesture commits (drag end, add, remove) — push undo here.
    void SetOnChanged(std::function<void(const std::vector<Math::CurveKey>&)> callback)
    {
        m_OnChanged = std::move(callback);
    }
    /// When the selected key changes (index, or -1 for none).
    void SetOnSelectionChanged(std::function<void(int)> callback)
    {
        m_OnSelectionChanged = std::move(callback);
    }
    /// Fired on right-click of a key (keyIndex, screen x, y).
    /// The host shows a context menu and calls SetKeyMode / DeleteKey.
    void SetOnKeyContextMenu(std::function<void(int, float, float)> callback)
    {
        m_OnKeyContextMenu = std::move(callback);
    }
    /// Fired continuously while the playback indicator is scrubbed (time in curve time units).
    void SetOnPlaybackScrub(std::function<void(float)> callback)
    {
        m_OnPlaybackScrub = std::move(callback);
    }
    /// Fired when the user starts/stops dragging empty graph space to scrub playback time.
    void SetOnPlaybackScrubActiveChanged(std::function<void(bool)> callback)
    {
        m_OnPlaybackScrubActiveChanged = std::move(callback);
    }
    /// Fired when the curve surface is double-clicked. Hosts decide what "expand" means.
    void SetOnCurveDoubleClick(std::function<void()> callback)
    {
        m_OnCurveDoubleClick = std::move(callback);
    }

    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

  private:
    struct GraphRect
    {
        float X = 0.0f;
        float Y = 0.0f;
        float W = 0.0f;
        float H = 0.0f;
    };

    void ApplyLabelStripHeight();
    GraphRect CurrentGraphRect() const;
    GraphRect ComputeGraphRect(float x, float y, float w, float h, float scale = 1.0f) const;
    static bool PointInGraph(const GraphRect& r, float x, float y);

    float EffectiveValueMin() const;
    float EffectiveValueMax() const;
    bool TangentEditingEnabled() const;
    float ToScreenX(const GraphRect& r, float time) const;
    float ToScreenY(const GraphRect& r, float value) const;
    float FromScreenTime(const GraphRect& r, float px) const;
    float FromScreenValue(const GraphRect& r, float py) const;
    /// Resolved curve value at `time` (handles Smooth/Step + WrapAround/clamped ends).
    float SampleAt(float time) const;

    int HitTestKey(float globalX, float globalY) const;
    int HitTestHandle(float globalX, float globalY) const;
    void TangentHandleScreen(const GraphRect& r, int keyIndex, bool incoming, float scale,
                             float& outX, float& outY) const;
    void UpdateHover(float globalX, float globalY);
    void UpdateCursor();
    void SetSelectedInternal(int index, bool notify);
    bool TryHandleCurveDoubleClick(float globalX, float globalY);
    void ApplyDrag(float globalX, float globalY);
    void ApplyPlaybackScrub(float globalX);
    bool ApplyRightClickAction(float globalX, float globalY, int preferredHit);
    void AddKeyAt(float globalX, float globalY);
    void RemoveKey(int index);

    static void DrawHandle(UI::PrimitiveEmitContext& ctx, float x, float y, uint32_t color, bool active,
                           float maxSizePx = 0.0f);
    void DrawPlaybackIndicator(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r) const;
    void DrawReferenceLine(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r) const;
    void DrawShadedSpans(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r) const;
    void DrawValueAxisMarks(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r) const;
    void DrawTimeLabels(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style, const GraphRect& r, float x,
                        float w) const;

    Config m_Config;
    std::vector<Math::CurveKey> m_Keys;
    std::vector<Math::CurveKey> m_ReferenceLine;
    std::vector<ShadedSpan> m_ShadedSpans;
    // The time labels' font size from the resolved style, in logical px (at least 12); the strip
    // under the plot and the control's height are sized from it, for painting and hit-testing alike.
    float m_TimeLabelFontSizePx = 12.0f;
    // The plot's height above the time-label strip, from --curve-field-plot-height (CurveField.css).
    float m_PlotHeightPx;
    // Whether a --curve-field-plot-height in a unit other than px has been reported, once per graph.
    bool m_ReportedPlotHeightUnit = false;
    int m_Selected = -1;
    int m_Hover = -1;
    int m_Drag = -1;
    bool m_DragMoved = false;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    float m_GrabOffsetX = 0.0f;
    float m_GrabOffsetY = 0.0f;
    // Value axis frozen during a drag so the curve does not chase the moving key.
    float m_DragValueMin = 0.0f;
    float m_DragValueMax = 1.0f;
    float m_HudValue = 0.0f;
    float m_HudTime = 0.0f;
    float m_ScrubberTime = 0.0f;
    float m_PlaybackTime = 0.0f;
    float m_PlaybackValue = 0.0f;
    bool m_PlaybackIndicatorVisible = false;
    bool m_PlaybackScrubActive = false;
    bool m_PlaybackScrubStarted = false;
    int m_PlaybackScrubStartHit = -1;
    float m_PlaybackScrubStartX = 0.0f;
    float m_PlaybackScrubStartY = 0.0f;
    bool m_HasLastCurveClick = false;
    std::chrono::steady_clock::time_point m_LastCurveClickTime{};
    float m_LastCurveClickX = 0.0f;
    float m_LastCurveClickY = 0.0f;

    std::function<void(const std::vector<Math::CurveKey>&)> m_OnChanging;
    std::function<void(const std::vector<Math::CurveKey>&)> m_OnChanged;
    std::function<void(int)> m_OnSelectionChanged;
    std::function<void(int, float, float)> m_OnKeyContextMenu;
    std::function<void(float)> m_OnPlaybackScrub;
    std::function<void(bool)> m_OnPlaybackScrubActiveChanged;
    std::function<void()> m_OnCurveDoubleClick;
};

} // namespace GameEngine
