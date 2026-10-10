#pragma once

#include "Panels/Animation/TimelineTransport.h"

#include "Assets/AnimationClip.h"
#include "Events/Event.h"
#include "InspectorRegistry.h"
#include "Panels/CurvesGraphView.h"
#include "Panels/TimeCompositeModel.h"
#include "Panels/LaneClipModel.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/TreeView.h"
#include "UI/Interaction/Selection.h"
#include "UI/UIEvents.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace Editor
{
class UndoRedoService;
class ToolbarDragDrop;
}
}

namespace GameEngine
{
struct EditorContext;
struct AssetLoadHandle;
class TimelineAsset;
class ClipSetAsset;
class Button;
class Dropdown;
class FloatField;
class AssetField;
class IntField;
class Label;
class TextField;
class UIElement;
class UIManager;
class TimelineBarElement;
class TimelineMarkersElement;
class TimelineTimeLabelsElement;
class TimelineFrameLabelsElement;
class DopeSheetView;
class TimeRangeSliderElement;
class TimeCompositeView;
class LaneClipEditorView;
class INativeContextMenu;
class SaveSceneChangesModal;
class ConfirmActionModal;
class RenameLayoutModal;
namespace Platform
{
class Window;
}

// Animation Window panel: timeline bar, dope sheet, curves, time composite, lane-based clip editor.
class AnimationWindowPanel : public DockPanel
{
public:
    enum class PanelKind { Animation, Timeline, ClipEditor };

    AnimationWindowPanel();
    explicit AnimationWindowPanel(PanelKind kind);
    ~AnimationWindowPanel() override;

    PanelKind GetPanelKind() const { return m_PanelKind; }

    // Depends on the instance's kind, not on the most-derived type: this class is
    // directly constructible as any kind, and the derived panels only fix the kind.
    std::string_view DeclaredTabIconClass() const override;

    UIElement* GetToolbarElement() const { return m_Toolbar; }
    UIElement* GetContentArea() const { return m_ContentArea; }
    UIElement* GetTimelineBar() const { return m_TimelineBar; }

    const TimelineState& GetTimelineState() const { return m_TimelineState; }
    TimelineState& GetTimelineState() { return m_TimelineState; }
    TimelineBarElement* GetTimelineBarElement() const { return m_TimelineBarElement; }
    DopeSheetView* GetDopeSheetView() const { return m_DopeSheetView; }
    CurvesGraphView* GetCurvesGraphView() const { return m_CurvesGraphView; }
    TimeCompositeView* GetTimeCompositeView() const { return m_TimeCompositeView; }
    LaneClipEditorView* GetLaneClipEditorView() const { return m_LaneClipEditorView; }
    TreeView* GetPropertiesTree() const { return m_PropertiesTree; }

    void OnPostLayout() override;
    void OnEvent(UIEvent& e) override;
    void UpdateTimelineViewHeaderOffsets();

    // Advance timeline when playing (call from editor tick).
    void TickTimeline(float deltaTimeSeconds);

    // Set current clip for Dope Sheet and Curves views (e.g. from asset selection).
    void SetCurrentClip(const AnimationClip* clip);
    bool OpenAnimation(const std::filesystem::path& path, uint32 selectedAnimationIndex = 0u);
    bool SaveCurrentClip();
    /// Save the currently-loaded timeline/clipset. Each routes through the
    /// .timeline / .clipset JSON serializer; if no path is bound yet the
    /// caller should prompt for one (the existing Save flow stays a no-op).
    bool SaveCurrentTimeline();
    bool SaveCurrentClipSet();
    /// Open / load helpers — JSON parsed into the editor's live model and
    /// the asset reference is held so subsequent saves overwrite the file.
    bool OpenTimeline(const std::filesystem::path& path);
    bool OpenClipSet(const std::filesystem::path& path);
    bool OpenAnimationAssetPath(const std::filesystem::path& path);
    bool HasUnsavedChanges() const { return m_Dirty || m_TimelineDirty || m_ClipSetDirty; }
    const std::filesystem::path& GetCurrentAnimationPath() const { return m_CurrentClipPath; }
    const std::filesystem::path& GetPreviewModelPath() const { return m_PreviewModelPath; }
    bool IsPreviewEnabled() const { return m_PreviewEnabled; }
    void SetPreviewEnabled(bool enabled);
    void SetOnPreviewToggled(std::function<void(bool /*enabled*/)> cb) { m_OnPreviewToggled = std::move(cb); }
    void SetOnClipSourcePreview(std::function<void(const std::filesystem::path&)> cb) { m_OnClipSourcePreview = std::move(cb); }
    void SetOnOpenSettings(std::function<void()> cb) { m_OnOpenSettings = std::move(cb); }
    void SetOnOpenAnimationPanel(std::function<void()> cb) { m_OnOpenAnimationPanel = std::move(cb); }
    void SetOnOpenTimelinePanel(std::function<void()> cb) { m_OnOpenTimelinePanel = std::move(cb); }
    void SetOnOpenClipEditorPanel(std::function<void()> cb) { m_OnOpenClipEditorPanel = std::move(cb); }
    void SetOnRevealInAssetsPanel(std::function<void(const std::filesystem::path&)> cb) { m_OnRevealInAssetsPanel = std::move(cb); }
    void SetOnTimelineInspectorRequested(std::function<void(const std::string&, std::function<void(UIElement*)>)> cb)
    {
        m_OnTimelineInspectorRequested = std::move(cb);
    }
    void SetUndoRedoService(Editor::UndoRedoService* undoRedo) { m_UndoRedo = undoRedo; }
    void SetOpenColorPickerWindow(OpenColorPickerWindowFn fn) { m_OpenColorPickerWindow = std::move(fn); }
    void SetContext(const EditorContext* ctx);
    struct PanelEditSnapshot
    {
        std::vector<float> MarkerTimes;
        TimeCompositeModel CompositeModel;
        LaneClipModel LaneModel;
        size_t SelectedCompositeTrack = static_cast<size_t>(-1);
        size_t SelectedCompositeClip = static_cast<size_t>(-1);
        CompositeTrackType SelectedTimelineKeyType = CompositeTrackType::Animation;
        size_t SelectedTimelineKeyTrack = static_cast<size_t>(-1);
        size_t SelectedTimelineKey = static_cast<size_t>(-1);
        size_t SelectedLane = static_cast<size_t>(-1);
        size_t SelectedLaneClip = static_cast<size_t>(-1);
    };
    void ApplyPanelEditSnapshot(const PanelEditSnapshot& snapshot);

private:
    void BindFromAssetsDeferred();
    void LoadBindAttachLayoutAndStyle(UIManager* ui);
    void MarkBindFailed(std::string_view reason);
    void RefreshElementPointers();
    void ApplyAppearanceSettings();
    void WireMarkersElementCallbacks();
    void WireAnimationControls();
    void UpdateTitle();
    void MarkDirty();
    void ClearDirty();
    bool EnsureEditableClip();
    bool SaveClipToPath(const std::filesystem::path& path);
    /// Active-view dirty getter — used by SetActiveView's save prompt to
    /// decide whether the outgoing view actually has unsaved changes.
    bool IsActiveViewDirty() const;
    void ClearActiveViewDirty();
    void RefreshPropertyTree();
    void RefreshChannelSelection(bool syncTreeSelection = true);
    void RefreshSequencingModels();
    void RefreshLeftPane();
    void RefreshSequencerSidebar();
    void RefreshTimelineInspector();
    void BuildTimelineInspector(UIElement* parent);
    std::pair<std::function<void(float)>, std::function<void(float)>> MakeTimelineFloatEditHandlers(
        const char* undoLabel, std::function<void(float)> applyToModel);
    void RefreshClipViewEmptyState();
    void SelectCompositeClip(size_t trackIdx, size_t clipIdx);
    void SelectLaneClip(size_t laneIdx, size_t clipIdx);
    size_t InsertTimelineTrackKeyAt(size_t trackIdx, float time);
    bool InsertTimelineTrackMarkerAt(size_t trackIdx, float time);
    bool MoveTimelineTrackInTime(size_t trackIdx, float deltaSeconds, bool refreshSidebar = true);
    void OpenSequencerColorPicker(uint32_t initialArgb, std::function<void(uint32_t)> onApply);
    void UpdatePlaybackRangeFromCurrentClip();
    void UpdatePreviewBinding();
    void InsertKeyAtCurrentTime();
    void DuplicateSelectedKey();
    void DeleteSelectedKey();
    void DeleteSelectedTimelineKeys();
    void DuplicateSelectedStrip();
    void DeleteSelectedStrip();
    void RestoreTimelineKeyboardFocus();
    void ShowContextMenuKeepingFocus(INativeContextMenu* menu, int x, int y);
    std::filesystem::path GetSuggestedEditablePath() const;
    void CreateNewAnimationAtPath(const std::filesystem::path& path);
    void CreateNewTimelineAtPath(const std::filesystem::path& path);
    void CreateNewClipSetAtPath(const std::filesystem::path& path);

    enum class ActiveView { DopeSheet = 0, Curves = 1, TimeComposite = 2, ClipEditor = 3 };
    void SetActiveView(ActiveView view);
    ActiveView GetDefaultActiveView() const;
    ActiveView NormalizeViewForPanel(ActiveView view) const;
    int GetViewDropdownIndex() const;
    std::string GetPanelBaseTitle() const;
    void ApplyTimelineZoom(float scrollY, float mouseX);
    void ApplyTimelineZoomSliderValue(float value, bool commit);
    float ComputeTimelineZoomSliderValue() const;
    void SyncTimelineZoomSlider();
    float ComputeTimelineContentEnd() const;
    float ComputeTimelineFullRangeEnd() const;
    void FrameSelected();
    void FrameAll();
    bool FrameSelectionBounds();
    void UpdateCurrentFrameLabel();
    void UpdatePlayButtonState();
    void UpdatePlayBackwardButtonState();
    void UpdateLoopButtonState();
    void UpdateSyncPlayheadButtonState();
    void UpdateRecordButtonState();
    void UpdateInterpolationButtonState();
    void UpdateTimelineRulerAndLabels();
    void BeginClipUndoGesture(const char* actionName);
    void CommitClipUndoGesture();
    void ClearPendingClipUndoGesture();
    void BeginPanelUndoGesture(const char* actionName);
    void CommitPanelUndoGesture();
    void ClearPendingPanelUndoGesture();
    bool ExecuteClipEditWithUndo(const char* actionName, const std::function<bool()>& applyEdit);
    PanelEditSnapshot CapturePanelEditSnapshot() const;
    bool ExecutePanelEditWithUndo(const char* actionName, const std::function<bool()>& applyEdit, bool mergeable);
    void RefreshDiscreteClipEditState();
    void RefreshEditedClipState();
    /// When auto-fit-height is on, recompute the curves view's value range
    /// from currently visible channels. No-op otherwise.
    void ApplyAutoFitHeightIfEnabled();
    void EnsurePropertyTreeRowActions(UIElement* row);
    void UpdatePropertyTreeRowActionState(TreeId id, UIElement* row);
    void GoToAdjacentKeyOnTree(const std::vector<TreeId>& ids, bool forward);
    void AddKeyOnTree(TreeId id);
    void AddKeysOnTree(TreeId clickedId, const std::vector<TreeId>& ids);
    std::vector<TreeId> GetEffectiveSelection(TreeId clickedId) const;
    bool DoOpenAnimation(const std::filesystem::path& path, uint32 selectedAnimationIndex);
    std::filesystem::path GetCurrentMenuAnimationAssetPath() const;
    bool SaveCurrentMenuAnimationAssetIfDirty();
    void DuplicateCurrentAnimationAsset();
    void RenameCurrentAnimationAsset();
    void RemoveCurrentAnimationAsset();
    void ShowAnimationManagerInspector();
    void ShowAnimationTransitionsInspector();
    void RefreshClipDropdown();
    bool CanResampleBakedChannelAtTreeId(TreeId treeId) const;
    void RecomputeAutoTangents(size_t channelIndex);
    void SetTangentTypeOnSelectedKeys(AnimTangentType type);
    void BreakTangents();
    void UnifyTangents();
    void ToggleTangentMode();
    void GoToPreviousKey();
    void GoToNextKey();
    void CenterViewOnCurrentTime();
    void FramePlaybackRange();
    void UpdateStatsBar();
    void BuildDopeSheetTrackRows();

    // Curve-edit options bar (Add Noise / Simplify / Smooth).
    // Forward-declare the mode enum so the EnterCurveOptionsMode declaration
    // below can refer to it without reordering the header.
    enum class CurveOptionsMode : uint8_t;
    void WireCurveOptions();
    void EnterCurveOptionsMode(CurveOptionsMode mode);
    void ExitCurveOptionsMode(bool commit);
    void RestoreOptionsBaseline();
    void RefreshOptionsPreview();
    void UpdateCurveOptionsToolbarButtonStates();
    void ApplyCurveOptionsOperation(); // applies current mode + values to the live clip
    /// Show / hide the Lattice options panel based on the curves view's
    /// active tool. Called from the Lattice toolbar toggle.
    void UpdateLatticeOptionsPanel();

private:
    // Deferred layout bind, three terminal-or-pending states. m_BindPending covers the
    // whole attempt — the queued action AND the async asset load it starts — so a bind
    // in flight is not re-armed by every layout pass while the .uxml resolves. Every
    // path that can end the attempt releases it: applied, failed, "panel detached, try
    // again", and the deferred action being dropped outright (PostAction returning
    // false, which is why OnPostLayout checks it).
    bool m_BindApplied = false;
    bool m_BindPending = false;
    bool m_BindFailed = false;
    bool m_SuppressDSRowRebuild = false;
    PanelKind m_PanelKind = PanelKind::Animation;

    class Dropdown* m_ClipDropdown = nullptr;
    class Dropdown* m_ViewDropdown = nullptr;
    UIElement* m_ViewButtons = nullptr;
    class Button* m_DopeSheetViewButton = nullptr;
    class Button* m_CurvesViewButton = nullptr;
    class Button* m_PreviewToggleBtn = nullptr;
    class Label* m_UnsavedIndicator = nullptr;
    bool m_PreviewEnabled = true;
    std::function<void(bool)> m_OnPreviewToggled;
    std::function<void(const std::filesystem::path&)> m_OnClipSourcePreview;
    std::function<void()> m_OnOpenSettings;
    std::function<void()> m_OnOpenAnimationPanel;
    std::function<void()> m_OnOpenTimelinePanel;
    std::function<void()> m_OnOpenClipEditorPanel;
    std::function<void(const std::filesystem::path&)> m_OnRevealInAssetsPanel;
    std::function<void(const std::string&, std::function<void(UIElement*)>)> m_OnTimelineInspectorRequested;
    const EditorContext* m_Context = nullptr;
    UIElement* m_Toolbar = nullptr;
    UIElement* m_ContentArea = nullptr;
    UIElement* m_PropertiesPane = nullptr;
    UIElement* m_PropertiesHeader = nullptr;
    UIElement* m_PropertiesFilterRow = nullptr;
    TextField* m_PropertiesSearchField = nullptr;
    Button* m_FilterAnimatedButton = nullptr;
    Button* m_FilterHierarchyButton = nullptr;
    Button* m_FilterChannelsOnlyButton = nullptr;
    Button* m_FilterFlatButton = nullptr;
    UIElement* m_SequencerSidebar = nullptr;
    UIElement* m_StatsBar = nullptr;
    TextField* m_StatsTimeField = nullptr;
    TextField* m_StatsValueField = nullptr;
    Label* m_StatsTimeLabel = nullptr;
    Label* m_StatsValueLabel = nullptr;

    // Curve-edit options bar (Add Noise / Simplify / Smooth).
    enum class CurveOptionsMode : uint8_t { None, Noise, Simplify, SmoothLowpass, SmoothPeak };
    UIElement* m_OptionsBar = nullptr;
    UIElement* m_OptionsNoisePanel = nullptr;
    UIElement* m_OptionsSimplifyPanel = nullptr;
    UIElement* m_OptionsSmoothPanel = nullptr;
    UIElement* m_OptionsLatticePanel = nullptr;
    Label*      m_LatticePointCountLabel = nullptr;
    IntField*   m_LatticePointCountField = nullptr;
    Dropdown*   m_LatticeBasisDropdown = nullptr;
    IntField* m_NoiseFreqMinField = nullptr;
    IntField* m_NoiseFreqMaxField = nullptr;
    FloatField* m_NoiseMagnitudeField = nullptr;
    Dropdown* m_SimplifyMethodDropdown = nullptr;
    FloatField* m_SimplifyTimeTolField = nullptr;
    FloatField* m_SimplifyValueTolField = nullptr;
    IntField* m_SmoothFilterWidthField = nullptr;
    IntField* m_SmoothSampleCountField = nullptr;
    Label* m_SmoothTitleLabel = nullptr;
    Label* m_NoiseFreqMinLabel = nullptr;
    Label* m_NoiseFreqMaxLabel = nullptr;
    Label* m_NoiseMagnitudeLabel = nullptr;
    Label* m_SimplifyTimeTolLabel = nullptr;
    Label* m_SimplifyValueTolLabel = nullptr;
    Label* m_SmoothFilterWidthLabel = nullptr;
    Label* m_SmoothSampleCountLabel = nullptr;
    Button* m_NoiseConfirmBtn = nullptr;
    Button* m_SimplifyConfirmBtn = nullptr;
    Button* m_SmoothConfirmBtn = nullptr;
    CurveOptionsMode m_CurveOptionsMode = CurveOptionsMode::None;
    std::vector<std::uint8_t> m_CurveOptionsBaseline; // clip bytes captured at toggle-on
    int   m_NoiseFreqMin = 5;
    int   m_NoiseFreqMax = 12;
    float m_NoiseMagnitude = 0.05f;
    float m_SimplifyTimeTol = 0.1f;
    float m_SimplifyValueTol = 0.001f;
    int   m_SmoothFilterWidth = 5;
    int   m_SmoothSampleCount = 5;
    bool m_StatsTimeDragging = false;
    bool m_StatsValueDragging = false;
    float m_StatsDragStartValue = 0.0f;
    float m_StatsDragLastX = 0.0f;
    UIElement* m_TimelineBar = nullptr;
    UIElement* m_TimeLabelsBar = nullptr;
    UIElement* m_TimelineLeftBar = nullptr;
    TreeView* m_PropertiesTree = nullptr;
    TimelineBarElement* m_TimelineBarElement = nullptr;
    TimeRangeSliderElement* m_TimeRangeSlider = nullptr;
    IntField* m_RangeStartField = nullptr;
    IntField* m_RangeEndField = nullptr;
    TimelineMarkersElement* m_MarkersElement = nullptr;
    TimelineTimeLabelsElement* m_TimeLabelsElement = nullptr;
    TimelineFrameLabelsElement* m_FrameLabelsElement = nullptr;
    Label* m_CurrentFrameLabel = nullptr;
    DopeSheetView* m_DopeSheetView = nullptr;
    CurvesGraphView* m_CurvesGraphView = nullptr;
    TimeCompositeView* m_TimeCompositeView = nullptr;
    LaneClipEditorView* m_LaneClipEditorView = nullptr;

    TimelineState m_TimelineState;
    bool m_ZoomSliderDragActive = false;
    float m_ZoomSliderAnchorTime = 0.0f;
    std::vector<float> m_MarkerTimes;
    bool m_ScrollWithPlayhead = false; // S toggles: when on, timeline scrolls from current playhead position
    bool m_Recording = false;
    std::string m_PropertiesSearchQuery;
    enum class PropertyFilter { Hierarchy, Animated, ChannelsOnly, Flat };
    PropertyFilter m_PropertyFilter = PropertyFilter::Hierarchy;
    bool m_ShowGrid = true;
    bool m_SnapTime = false;
    bool m_SnapValue = false;
    float m_SnapTimeStep  = 0.0f; // 0.0 = snap to frame grid, >0 = custom time step
    float m_SnapValueStep = 0.0f; // 0.0 = auto (match grid), >0 = custom value step
    bool m_KeytoolEnabled = false;
    struct ClipboardKeyframe
    {
        int Channel = -1;
        AnimKeyframe Keyframe;
    };
    std::vector<ClipboardKeyframe> m_KeyClipboard;
    bool m_Dirty = false;
    bool m_NeedsInitialFrame = false;
    /// When true, the curves view auto-fits the value range to whatever is
    /// currently visible (kept in sync via TickTimeline so playback edits
    /// keep the curve framed). Toggled by AnimationWindowFitHeight.
    bool m_AutoFitHeight = false;
    ActiveView m_ActiveView = ActiveView::Curves;
    int m_SelectedChannel = -1;
    uint32 m_SelectedComponent = 0u;
    size_t m_SelectedCompositeTrack = static_cast<size_t>(-1);
    size_t m_SelectedCompositeClip = static_cast<size_t>(-1);
    CompositeTrackType m_SelectedTimelineKeyType = CompositeTrackType::Animation;
    size_t m_SelectedTimelineKeyTrack = static_cast<size_t>(-1);
    size_t m_SelectedTimelineKey = static_cast<size_t>(-1);
    size_t m_SelectedLane = static_cast<size_t>(-1);
    size_t m_SelectedLaneClip = static_cast<size_t>(-1);
    TreeId m_SelectedTreeId = 0;
    std::vector<int> m_VisibleChannels;
    std::unordered_map<TreeId, std::pair<int, uint32>> m_ChannelBindings;
    std::unordered_map<TreeId, std::vector<int>> m_TreeNodeChannels;
    std::unordered_map<TreeId, uint32> m_ComponentLeafIndex; // X=0, Y=1, Z=2, W=3
    std::unordered_set<TreeId> m_LockedTreeIds;
    std::unordered_set<TreeId> m_PinnedTreeIds;
    std::unordered_set<TreeId> m_BoneTreeIds;
    std::unordered_map<TreeId, uint32> m_TreeColorTags; // 0xAARRGGBB; absent = no tag
    std::unordered_map<UIElement*, TreeId> m_RowToTreeId;

    // Owned "new" animation created via + button (unsaved, in-memory).
    std::shared_ptr<AnimationClip> m_NewClip;
    std::shared_ptr<AnimationClip> m_CurrentClipAsset;
    const AnimationClip* m_CurrentClip = nullptr;
    std::filesystem::path m_CurrentClipPath;

    // Timeline (.timeline) + ClipSet (.clipset) edited alongside the clip.
    // Each tracks its own dirty flag so a view-switch can prompt only when
    // the outgoing view's data has actually changed.
    std::shared_ptr<TimelineAsset> m_CurrentTimelineAsset;
    std::filesystem::path          m_CurrentTimelinePath;
    bool                           m_TimelineDirty = false;
    std::shared_ptr<ClipSetAsset>  m_CurrentClipSetAsset;
    std::filesystem::path          m_CurrentClipSetPath;
    bool                           m_ClipSetDirty = false;
    std::filesystem::path m_PreviewModelPath;
    Platform::Window* m_Window = nullptr; // not owned; host for native context menu
    std::unique_ptr<INativeContextMenu> m_PropertiesTreeContextMenu;
    std::unique_ptr<INativeContextMenu> m_KeyframeContextMenu;
    std::unique_ptr<INativeContextMenu> m_AnimationMenuContextMenu;
    std::unique_ptr<INativeContextMenu> m_AddTrackContextMenu;
    std::unique_ptr<INativeContextMenu> m_SequencerTrackContextMenu;
    std::unique_ptr<INativeContextMenu> m_SequencerLaneContextMenu;
    std::unique_ptr<INativeContextMenu> m_TimelineTrackContextMenu;
    int m_ContextMenuKeyChannel = -1;
    float m_ContextMenuKeyTime = -1.0f;
    TreeId m_ContextMenuTreeId = 0;
    size_t m_ContextMenuSequencerIdx = static_cast<size_t>(-1);
    size_t m_ContextMenuTimelineTrackIdx = static_cast<size_t>(-1);
    float m_ContextMenuTimelineTime = 0.0f;
    Editor::UndoRedoService* m_UndoRedo = nullptr; // not owned

    float m_SeekBeforeTime = 0.0f;
    bool m_IsSeekActive = false;

    // Selection undo
    struct SelectionSnapshot
    {
        std::vector<UI::Interaction::ItemId> TreeIds;
        UI::Interaction::ItemId TreeAnchor = 0;
        TreeId SelectedTreeId = 0;
        int SelectedChannel = -1;
        uint32 SelectedComponent = 0u;
        std::vector<int> VisibleChannels;
        std::vector<CurvesGraphView::SelectedKey> GraphKeys;
    };
    SelectionSnapshot m_LastSelectionSnapshot;
    bool m_SuppressSelectionUndo = false;
    void PushSelectionUndo(const SelectionSnapshot& before, const SelectionSnapshot& after);
    SelectionSnapshot CaptureSelectionSnapshot() const;
    void ApplySelectionSnapshot(const SelectionSnapshot& snapshot, bool syncTree);
    std::shared_ptr<AnimationClip> m_PendingCurveUndoClip;
    std::vector<std::uint8_t> m_PendingCurveUndoBefore;
    std::string m_PendingCurveUndoName;
    /// Retime region snapshot captured at gesture start so undo/redo can
    /// restore the in/out alongside the keyframe times.
    bool m_PendingRetimeRegionCaptured = false;
    CurvesGraphView::RetimeRegionState m_PendingRetimeRegionBefore;
    bool m_PendingPanelUndoActive = false;
    PanelEditSnapshot m_PendingPanelUndoBefore;
    std::string m_PendingPanelUndoName;

    // Curve color overrides: keys 0-3 = per-component; key ((ch+1)<<8)|comp = per-channel.
    std::unordered_map<uint32, uint32> m_CurveColorOverrides;
    OpenColorPickerWindowFn m_OpenColorPickerWindow;
    class SaveSceneChangesModal* m_UnsavedChangesModal = nullptr; // not owned; child of this panel
    class ConfirmActionModal* m_GltfInterpWarningModal = nullptr; // not owned; child of this panel
    class ConfirmActionModal* m_OverwriteConfirmModal = nullptr; // not owned; child of this panel
    class ConfirmActionModal* m_RemoveAnimationConfirmModal = nullptr; // not owned; child of this panel
    class RenameLayoutModal* m_RenameAnimationModal = nullptr; // not owned; child of this panel
    std::filesystem::path m_PendingOpenPath;
    std::filesystem::path m_PendingNewAnimationPath;
    uint32 m_PendingOpenIndex = 0u;

    // Composite timeline model (tracks + clips); used by TimeCompositeView. Dragging clips updates this.
    std::unique_ptr<TimeCompositeModel> m_CompositeModel;
    std::unique_ptr<LaneClipModel> m_LaneClipModel;

    std::unique_ptr<ITreeDataProvider> m_PropertiesTreeProvider;
    std::unique_ptr<UI::Interaction::ISelectionModel> m_PropertiesTreeSelection;

    std::unique_ptr<AssetLoadHandle> m_LayoutLoadHandle;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;

    void SetupToolbarDragDrop();
    void WireToolbarRightClickToggles();
    void RestoreToolbarGapsFromPrefs();
    void CreateToolbarGap(UIElement* container, int insertIndex);
    void RemoveToolbarGap(class Button* gap);
    std::unique_ptr<Editor::ToolbarDragDrop> m_ToolbarDragDrop;
    bool m_ToolbarDragDropOrderLoaded = false;
    bool m_ToolbarRightClickWired = false;
    int m_NextToolbarGapSerial = 1;

    // Live updates from AnimationWindowSettings. Declared last so it unsubscribes
    // before any member its callback touches is destroyed.
    EventSubscription m_SettingsChangedSubscription;
};

class AnimationTimelinePanel final : public AnimationWindowPanel
{
public:
    AnimationTimelinePanel();
};

class AnimationClipEditorPanel final : public AnimationWindowPanel
{
public:
    AnimationClipEditorPanel();
};

} // namespace GameEngine
