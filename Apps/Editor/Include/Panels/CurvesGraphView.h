#pragma once

#include "Panels/Animation/CurveDisplayScope.h"

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include <array>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine { namespace Animation { class AnimationClip; } }

namespace GameEngine
{

// Draws time vs value curves for animation channels (linear/step segments; optional spline).
class CurvesGraphView : public UIElement
{
public:
    struct SelectedKey
    {
        int Channel = -1;
        float KeyTime = -1.0f;
    };

    enum class EditGesture
    {
        Keyframe,
        Tangent,
        Retime,
        Lattice
    };

    using KeyframeEditedFn = std::function<void(int channel, int component, float currentTime, float newTime, float newValue)>;
    using KeyframeSelectedFn = std::function<void(int channel, int component, int keyIndex, float keyTime)>;
    using KeyframeContextMenuFn = std::function<void(int channel, float keyTime, float screenX, float screenY)>;
    using TangentEditedFn = std::function<void(int channel, int component, float keyTime, bool incoming, float tangent, float weight)>;
    using EditGestureFn = std::function<void(EditGesture)>;
    /// One key's time change committed during a retime gesture.
    /// `originalTime` is the snapshot at gesture start (unchanged for the
    /// whole gesture); `newTime` is the proposed new time for this commit.
    /// During live-update commits the consumer must look up the key by its
    /// *current* live time and move it to `newTime`; CurvesGraphView keeps
    /// that live time internally so callers receive a clean delta each call.
    struct RetimeChange { int channel; float originalTime; float newTime; };
    /// Batch fired both per MouseMove (live updates) and on MouseUp (final).
    /// Entries are pre-sorted into a safe order (rightward shifts → descending
    /// originalTime; leftward → ascending) so time-based key lookups don't
    /// trample siblings mid-batch.
    using RetimeAppliedFn = std::function<void(const std::vector<RetimeChange>&)>;
    using IsChannelLockedFn = std::function<bool(int channel)>;
    using LatticeEditedFn = std::function<void(int channel, float keyTime, float newValue)>;
    using InsertKeyFn = std::function<void(int channel, float time)>;
    /// One sample from a freehand DrawCurve stroke.
    struct DrawCurveSample { float time; float value; };
    /// Fired once at the end of a DrawCurve gesture with all sampled
    /// points, sorted by time. Consumer is expected to wipe existing keys
    /// in [first.time, last.time] on the target channel/component and
    /// replace them with one keyframe per sample.
    using DrawCurveCommittedFn = std::function<void(int channel, uint32 component,
                                                    const std::vector<DrawCurveSample>&)>;

    CurvesGraphView();
    void SetClip(const Animation::AnimationClip* clip);
    const Animation::AnimationClip* GetClip() const { return m_Clip; }
    void SetTimeRange(float rangeStart, float rangeEnd);
    void SetValueRange(float valueMin, float valueMax);
    void GetValueRange(float& outMin, float& outMax) const { outMin = m_ValueMin; outMax = m_ValueMax; }
    void SetCurrentTime(float t);
    void SetSnapTime(bool snap) { m_SnapTime = snap; }
    void SetSnapValue(bool snap) { m_SnapValue = snap; }
    void SetSnapValueStep(float step) { m_SnapValueStep = std::max(0.0f, step); }
    float GetSnapValueStep() const { return m_SnapValueStep; }
    void SetSnapTimeStep(float step) { m_SnapTimeStep = std::max(0.0f, step); }
    float GetSnapTimeStep() const { return m_SnapTimeStep; }
    void SetVisibleChannels(const std::vector<int>& channels);
    void SetPinnedCurves(std::vector<PinnedCurve> curves);
    void FitVisibleCurves() { RecomputeValueRange(); }
    void SetSelectedCurve(int channel, uint32 component);
    void SetChannelColors(const std::unordered_map<uint32, uint32>& colors);
    void SetShowGrid(bool show);
    void SetGridColor(uint32 argb);
    void SetGridHLineColor(uint32 argb);
    void SetGridVLineColor(uint32 argb);
    void SetGridLineThickness(float px);
    void SetBaselineColor(uint32 argb);
    void SetBaselineThickness(float px);
    void SetCurveLineWidth(float px);
    float GetCurveLineWidth() const { return m_CurveLineWidth; }
    void SetFps(float fps) { m_Fps = fps; MarkDirty(VisualDirty); }
    float GetFps() const { return m_Fps; }
    void SetKeyframeColor(uint32 argb)         { m_KeyframeColor = argb;         MarkDirty(VisualDirty); }
    void SetKeyframeSelectedColor(uint32 argb) { m_KeyframeSelectedColor = argb; MarkDirty(VisualDirty); }
    void ClearSelection();
    void SelectAllVisibleKeys();
    void SelectKeyAtTime(int channel, float keyTime);
    float GetSelectedKeyframeTime() const;
    const std::vector<SelectedKey>& GetSelectedKeyframes() const { return m_SelectedKeys; }
    bool IsDraggingKey() const { return m_DraggingKey && m_DragThresholdMet; }
    bool IsBoxSelecting() const { return m_BoxSelecting; }
    void SetShowAllComponents(bool show);
    bool GetShowAllComponents() const { return m_ShowAllComponents; }
    void RestoreSelection(std::vector<SelectedKey> keys, int selectedChannel, uint32 selectedComponent);
    void SetOnSeekToTime(std::function<void(float)> fn) { m_OnSeekToTime = std::move(fn); }
    void SetOnPan(std::function<void(float deltaTimeSeconds)> fn) { m_OnPan = std::move(fn); }
    void SetOnKeyframeEdited(KeyframeEditedFn fn) { m_OnKeyframeEdited = std::move(fn); }
    void SetOnKeyframeSelected(KeyframeSelectedFn fn) { m_OnKeyframeSelected = std::move(fn); }
    void SetOnKeyframeContextMenu(KeyframeContextMenuFn fn) { m_OnKeyframeContextMenu = std::move(fn); }
    void SetOnTangentEdited(TangentEditedFn fn) { m_OnTangentEdited = std::move(fn); }
    void SetOnEditStarted(EditGestureFn fn) { m_OnEditStarted = std::move(fn); }
    void SetOnEditFinished(EditGestureFn fn) { m_OnEditFinished = std::move(fn); }
    void SetOnRetimeApplied(RetimeAppliedFn fn) { m_OnRetimeApplied = std::move(fn); }
    void SetIsChannelLocked(IsChannelLockedFn fn) { m_IsChannelLocked = std::move(fn); }

    /// Public snapshot of the retime region for undo plumbing. The panel
    /// captures these around a retime gesture and re-applies them via
    /// SetRetimeRegion when the gesture's undo command runs.
    struct RetimeRegionState { bool active = false; float left = 0.0f; float right = 0.0f; };
    RetimeRegionState GetRetimeRegion() const
    {
        return { m_RetimeRegionActive, m_RetimeRegionLeft, m_RetimeRegionRight };
    }
    void SetRetimeRegion(const RetimeRegionState& s)
    {
        m_RetimeRegionActive = s.active;
        m_RetimeRegionLeft   = s.left;
        m_RetimeRegionRight  = s.right;
        m_RetimePendingIn    = false;
        m_RetimeHover        = RetimeHover::None;
        MarkDirty(VisualDirty);
    }
    void SetOnLatticeEdited(LatticeEditedFn fn) { m_OnLatticeEdited = std::move(fn); }
    void SetOnInsertKeyAt(InsertKeyFn fn) { m_OnInsertKeyAt = std::move(fn); }
    void SetOnDrawCurveCommitted(DrawCurveCommittedFn fn) { m_OnDrawCurveCommitted = std::move(fn); }
    void SetOnSelectionChanged(std::function<void()> fn) { m_OnSelectionChanged = std::move(fn); }

    enum class CurveViewMode { Absolute, Stacked, Normalized };
    void SetCurveViewMode(CurveViewMode mode);
    CurveViewMode GetCurveViewMode() const { return m_CurveViewMode; }

    void TakeBufferSnapshot();
    void SwapBufferCurve();
    bool HasBufferSnapshot() const { return m_BufferSnapshotValid; }
    const std::vector<std::vector<float>>& GetBufferValues() const { return m_BufferValues; }
    void SetBufferValues(std::vector<std::vector<float>> values) { m_BufferValues = std::move(values); m_BufferSnapshotValid = !m_BufferValues.empty(); MarkDirty(VisualDirty); }

    enum class CurveTool { Select, Retime, Lattice, DrawCurve };
    void SetActiveTool(CurveTool tool);
    CurveTool GetActiveTool() const { return m_ActiveTool; }

    /// Lattice tool: change the number of control points. Clamped to [2..16].
    /// Calling this while the tool is active rebuilds the lattice using the
    /// current selection; calling it from anywhere else just stores N for the
    /// next activation. Ignored mid-drag so a transient keypress can't
    /// invalidate the captured drag entries.
    void BumpLatticePointCount(int delta);
    void SetLatticePointCount(int count);
    int  GetLatticePointCount() const { return m_LatticePointCount; }

    /// Lattice deformation basis. Bezier (Bernstein, default) treats interior
    /// CPs as control handles — dragging any CP bows the deformation curve at
    /// every key. CatmullRom interpolates every CP, so the curve passes
    /// through each handle exactly but keys whose times match a non-dragged
    /// CP stay put. ToggleLatticeBasis cycles between the two.
    enum class LatticeBasis { Bezier, CatmullRom };
    void ToggleLatticeBasis();
    void SetLatticeBasis(LatticeBasis basis);
    LatticeBasis GetLatticeBasis() const { return m_LatticeBasis; }
    void SetKeyInsertionEnabled(bool enabled) { m_KeyInsertionEnabled = enabled; }
    bool IsKeyInsertionEnabled() const { return m_KeyInsertionEnabled; }
    void ApplyValueZoom(float scrollY);
    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;
private:
    struct KeyDragSnapshot
    {
        int Channel = -1;
        float CurrentTime = -1.0f;
        float CurrentValue = 0.0f;
        float InitialTime = -1.0f;
        float InitialValue = 0.0f;
    };

public:
    // Returns the live time/value of the primary dragged key; valid only while IsDraggingKey().
    bool GetActiveDragTimeValue(int& outChannel, float& outTime, float& outValue) const
    {
        if (!m_DraggingKey || !m_DragThresholdMet || m_DragKeys.empty()) return false;
        outChannel = m_DragKeys[0].Channel;
        outTime    = m_DragKeys[0].CurrentTime;
        outValue   = m_DragKeys[0].CurrentValue;
        return true;
    }

private:

    struct HandleDragSnapshot
    {
        bool Valid = false;
        bool Incoming = false;
        float StartTime = 0.0f;
        float StartValue = 0.0f;
    };

    void RecomputeValueRange();
    float ComputeValueSnapStep(float valueRange) const;
    float ComputeTimeSnap(float t) const;
    /// Build (or rebuild) the lattice from the current selection using the
    /// configured m_LatticePointCount. Returns true if the lattice is now
    /// active. Skipped mid-drag so a captured gesture isn't invalidated.
    bool RebuildLatticeFromSelection();

    std::unordered_map<uint32, uint32> m_ChannelColors;
    const Animation::AnimationClip* m_Clip = nullptr;
    float m_RangeStart = 0.0f;
    float m_RangeEnd = 10.0f;
    float m_ValueMin = -2.0f;
    float m_ValueMax = 2.0f;
    bool m_SnapTime = false;
    bool m_SnapValue = false;
    float m_SnapValueStep = 0.0f; // 0.0 = auto (match visual grid), >0 = custom step
    float m_SnapTimeStep  = 0.0f; // 0.0 = snap to frame grid (1/fps), >0 = custom time step
    float m_CurrentTime = 0.0f;
    std::vector<int> m_VisibleChannels;
    std::vector<PinnedCurve> m_PinnedCurves;
    std::vector<int> m_DrawChannels; // Reuse render scratch capacity between frames.
    int m_SelectedChannel = -1;
    int m_SelectedKeyIndex = -1;
    float m_SelectedKeyTime = -1.0f;
    uint32 m_SelectedComponent = 0u;
    std::vector<SelectedKey> m_SelectedKeys;
    bool m_DraggingKey = false;
    bool m_DraggingInHandle = false;
    bool m_DraggingOutHandle = false;
    bool m_DraggingLinkedHandles = false;
    bool m_ScalingSelectionTime = false;
    bool m_ScalingSelectionValue = false;
    bool m_ScaleTimeFromRight = false;
    bool m_ScaleValueFromBottom = false;
    bool m_BoxSelecting = false;
    bool m_PendingEmptyAction = false;
    bool m_BoxSelectModifierDown = false;
    float m_DragCurrentTime = 0.0f;
    float m_DragCurrentValue = 0.0f;
    float m_DragAnchorPointerTime = 0.0f;
    float m_DragAnchorPointerValue = 0.0f;
    float m_EmptyActionStartX = 0.0f;
    float m_EmptyActionStartY = 0.0f;
    float m_BoxStartX = 0.0f;
    float m_BoxStartY = 0.0f;
    float m_BoxEndX = 0.0f;
    float m_BoxEndY = 0.0f;
    float m_ScaleAnchorTime = 0.0f;
    float m_ScaleInitialDuration = 0.0f;
    float m_ScaleAnchorValue = 0.0f;
    float m_ScaleInitialValueExtent = 0.0f;
    std::vector<SelectedKey> m_BoxSelectionSeed;
    std::vector<KeyDragSnapshot> m_DragKeys;
    HandleDragSnapshot m_PrimaryHandleDrag;
    HandleDragSnapshot m_SecondaryHandleDrag;
    std::function<void(float)> m_OnSeekToTime;
    std::function<void(float)> m_OnPan;
    bool m_Panning = false;
    bool m_PanHorizontalOnly = false;
    float m_PanLastGlobalX = 0.0f;
    float m_PanLastGlobalY = 0.0f;
    KeyframeEditedFn m_OnKeyframeEdited;
    KeyframeSelectedFn m_OnKeyframeSelected;
    KeyframeContextMenuFn m_OnKeyframeContextMenu;
    TangentEditedFn m_OnTangentEdited;
    EditGestureFn m_OnEditStarted;
    EditGestureFn m_OnEditFinished;
    RetimeAppliedFn m_OnRetimeApplied;
    IsChannelLockedFn m_IsChannelLocked;
    LatticeEditedFn m_OnLatticeEdited;
    InsertKeyFn m_OnInsertKeyAt;
    DrawCurveCommittedFn m_OnDrawCurveCommitted;
    /// Live state for the freehand DrawCurve gesture.
    bool m_DrawingCurve = false;
    int m_DrawCurveChannel = -1;
    uint32 m_DrawCurveComponent = 0;
    std::vector<DrawCurveSample> m_DrawCurveSamples;
    std::function<void()> m_OnSelectionChanged;
    bool m_ShowGrid = true;
    uint32 m_GridColor = 0xFF595959u;
    uint32 m_GridHLineColor = 0xFF808080u;
    uint32 m_GridVLineColor = 0xFFA4A4A4u;
    float m_GridLineThickness = 1.0f;
    uint32 m_BaselineColor  = 0xFF787878u;
    float m_BaselineThickness = 2.0f;
    float m_CurveLineWidth = 2.0f;
    float m_Fps = 60.0f;
    uint32 m_KeyframeColor         = 0xFF33CC80u;
    uint32 m_KeyframeSelectedColor = 0xFFFF9933u;
    float m_ContentX = 0.0f;
    float m_ContentY = 0.0f;
    float m_ContentW = 0.0f;
    float m_ContentH = 0.0f;
    float m_ContentScale = 1.0f; // cached from ctx.contentScale each frame
    bool m_Seeking = false;
    bool m_RightClickScrubbing = false;
    bool m_DragThresholdMet = false;
    float m_DragStartLocalX = 0.0f;
    float m_DragStartLocalY = 0.0f;
    bool m_DragConstrained = false;
    bool m_DragConstrainHorizontal = true;
    bool m_FreeTangentDrag = false;
    bool m_HandleDragThresholdMet = false;
    float m_HandleDragStartX = 0.0f;
    float m_HandleDragStartY = 0.0f;
    int m_HoverKeyChannel = -1;
    float m_HoverKeyTime = -1.0f;
    bool m_HoverInHandle = false;
    bool m_HoverOutHandle = false;
    enum class HoveredSelHandle { None, Right, Left, Top, Bottom };
    HoveredSelHandle m_HoveredSelHandle = HoveredSelHandle::None;
    CurveViewMode m_CurveViewMode = CurveViewMode::Absolute;
    CurveTool m_ActiveTool = CurveTool::Select;
    bool m_KeyInsertionEnabled = false;
    bool m_BufferSnapshotValid = false;
    std::vector<std::vector<float>> m_BufferValues;
    bool m_ShowAllComponents = false;
    /// Modes the retime tool can be in during a drag. The first two are the
    /// "single-pivot" gestures from the original implementation; the rest are
    /// region-aware (Maya-style two-handle).
    enum class RetimeMode : uint8_t
    {
        Scale,             // single pivot, scale keys past pivot
        Translate,         // single pivot, translate keys past pivot (Shift)
        DefineRegion,      // Alt+drag — drawing a region rubber-band, no keys move
        RegionScaleRight,  // drag region's right handle, anchor at regionLeft
        RegionScaleLeft,   // drag region's left handle, anchor at regionRight
        RegionTranslate    // drag region's middle, translate region + keys
    };
    bool m_RetimeDragging = false;
    /// Pivot stays fixed for the duration of the gesture; keys past this time
    /// are scaled or translated relative to it.
    float m_RetimePivotTime = 0.0f;
    /// Pixel x where the drag started; used to compute a stable drag delta.
    float m_RetimeAnchorX = 0.0f;
    /// Reference span pivot→rightmost original key, for proportional scale.
    /// Zero (or near-zero) means there is nothing to scale and we fall back
    /// to translate semantics.
    float m_RetimeReferenceDist = 0.0f;
    RetimeMode m_RetimeMode = RetimeMode::Scale;
    /// Snapshot + live-tracking for the affected keys. originalTime is the
    /// time at gesture start (immutable). liveTime tracks the key's current
    /// position so per-move commits can find it by its present time. newTime
    /// is the proposed time for the next commit.
    struct RetimePreviewEntry { int channel; float originalTime; float liveTime; float newTime; };
    std::vector<RetimePreviewEntry> m_RetimePreview;
    /// HUD text shown during drag; updated each MouseMove.
    float m_RetimeDeltaSeconds = 0.0f;
    float m_RetimeScaleFactor = 1.0f;
    /// True once the cursor has moved past the click-vs-drag pixel threshold
    /// during a region-translate gesture. Mouse-down inside the region body
    /// is ambiguous (translate vs. dismiss); we don't commit any keyframe
    /// motion until this flips, and a release that never flipped it acts as
    /// a click-to-clear instead.
    bool m_RetimeDragConfirmed = false;

    // ---- Region (two-handle Maya-style) state. Persists across gestures. ----
    bool m_RetimeRegionActive = false;
    /// Sorted region bounds in time. While in DefineRegion mode these track
    /// the live rubber-band; on mouse-up they're finalized.
    float m_RetimeRegionLeft = 0.0f;
    float m_RetimeRegionRight = 0.0f;
    /// Original region bounds at the start of a region drag, so Escape can
    /// revert. For DefineRegion this also holds the prior region (or invalid).
    float m_RetimeRegionLeftOrig = 0.0f;
    float m_RetimeRegionRightOrig = 0.0f;
    bool m_RetimeRegionWasActive = false;
    /// Pending in-point set by a click-without-drag. The next click on the
    /// timeline pairs with this to form the region (so users can either
    /// click→click to set in/out, or click-and-drag in one gesture).
    bool m_RetimePendingIn = false;
    float m_RetimePendingInTime = 0.0f;
    /// Which part of an active region the cursor is hovering, used purely
    /// for visual feedback (the matching handle/body brightens).
    enum class RetimeHover : uint8_t { None, LeftHandle, RightHandle, Body };
    RetimeHover m_RetimeHover = RetimeHover::None;
    struct LatticeCP { float Time = 0.0f; float Value = 0.0f; float BaselineValue = 0.0f; };
    // Per-entry snapshot captured at drag start: channel + key time + baseline value.
    struct LatticeDragEntry { int Channel = -1; float Time = 0.0f; float BaselineValue = 0.0f; };
    /// Variable-N control points. Default N=5; user can change via the [ and ]
    /// keys while the lattice tool is active. Layout is N CPs evenly spaced in
    /// time across the selection's bounds; each CP's Value is initialized by
    /// sampling the curve at its time so the rest lattice rides on the curve
    /// (zero-displacement = no visible change). Deformation uses Catmull-Rom
    /// basis with mirror-extended endpoints, so the curve passes through every
    /// CP and dragging any CP visibly anchors the deformation at that point.
    std::vector<LatticeCP> m_LatticePoints;
    int  m_LatticePointCount = 5;
    LatticeBasis m_LatticeBasis = LatticeBasis::CatmullRom;
    bool m_LatticeActive = false;
    int m_LatticeDragIndex = -1;
    int m_LatticeHoverIndex = -1; // CP under cursor, -1 = none. Drives the hover highlight.
    float m_LatticeAnchorY = 0.0f;
    std::vector<LatticeDragEntry> m_LatticeDragKeys;
    /// Per-key snapshot taken when the lattice activates: channel, key time,
    /// baseline value at that time. The lattice keeps operating on this set
    /// even after the user deselects (so the deformer outlives the selection
    /// instead of becoming a non-interactive overlay), and rebuilds when the
    /// selection changes to a different non-empty set.
    std::vector<LatticeDragEntry> m_LatticeBaselineKeyValues;
};

} // namespace GameEngine
