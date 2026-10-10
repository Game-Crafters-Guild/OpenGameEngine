#pragma once

#include "UI/Controls/DockPanel.h"
#include "UI/Controls/TreeView.h"
#include "UI/EditorSearchBars.h"
#include "ColorPicker/ColorPickerScope.h"
#include "InspectorRegistry.h"
#include "UI/Interaction/Selection.h"
#include <memory>
#include <filesystem>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <functional>

namespace GameEngine {

struct EditorContext;
struct Stylesheet;
using StylesheetHandle = std::shared_ptr<const Stylesheet>;
class Slider;
class Label;
class FloatField;
class UIManager;
class TreeView;
class TextField;
class Toggle;
class Checkbox;
class UIElement;
class ScrollView;

namespace Editor
{
struct SettingsCategoryDescriptor;
}

namespace Rendering
{
class IDevice;
}

// Settings category identifiers (start from 1, 0 is reserved as invalid TreeId)
enum class SettingsCategory : uint64_t {
    Root = 1000,  // Not used directly since we show roots without parent
    ProjectSettings = 1,
    UserSettings = 2,
    // Project Settings subcategories
    AudioSettings = 100,
    Camera = 101,
    Animation = 102,
    Input = 103,
    VersionControl = 104,  // Parent category for VCS settings
    Rendering = 108,       // Project rendering settings (pipeline, quality, etc.)
    Tags = 109,            // Project tags (name + color for asset labeling)
    Scene = 117,           // Scene auto-save backup (Project Settings)
    Physics = 118,         // Physics world settings (gravity, stepping, etc.)
    HDROutput = 120,       // Project HDR output settings and display diagnostics
    AssetImport = 121,      // Project asset import defaults
    // 110 (Build, with its per-platform children) migrated to
    // EditorSettingsRegistry — see Editor/Settings/BuildSettingsPage.h.
    // User Settings subcategories
    Script = 200,
    UI = 201,
    Shortcuts = 202,
    Gizmo = 203,
    // 204 (Tooltips) and 205 (Online Assets) migrated to EditorSettingsRegistry.
    GridAndSnapping = 206,
    Performance = 207,
    // UI sub-categories
    UIColors = 300,
    UIAppearance = 301,
    UIFontSizes = 302,
    UITrees = 303,
    UIAssets = 304,
    UIInspector = 305,
    UIHiDpi = 306,
    };

// Per-provider VCS tabs are dynamic children of VersionControl: TreeId =
// kVcsProviderCategoryBase + index into the EditorVcsProviderRegistry snapshot
// (detection order). They flow through SettingsCategory casts but never match
// a named enum case.
inline constexpr uint64_t kVcsProviderCategoryBase = 5000;
inline constexpr uint64_t kVcsProviderCategoryMaxCount = 64;

inline bool IsVcsProviderCategory(SettingsCategory category)
{
    const auto value = static_cast<uint64_t>(category);
    return value >= kVcsProviderCategoryBase &&
           value < kVcsProviderCategoryBase + kVcsProviderCategoryMaxCount;
}

// Categories registered through Editor::EditorSettingsRegistry are dynamic
// children of a settings root: TreeId = kRegistrySettingsCategoryBase + index
// into the registry snapshot (registration order; replace-forward keeps
// indices stable). They flow through SettingsCategory casts but never match a
// named enum case.
inline constexpr uint64_t kRegistrySettingsCategoryBase = 10000;
inline constexpr uint64_t kRegistrySettingsCategoryMaxCount = 1024;

inline bool IsRegistrySettingsCategory(SettingsCategory category)
{
    const auto value = static_cast<uint64_t>(category);
    return value >= kRegistrySettingsCategoryBase &&
           value < kRegistrySettingsCategoryBase + kRegistrySettingsCategoryMaxCount;
}

// Data provider for the settings tree
class SettingsTreeDataProvider : public TreeChangeTrackingProvider {
public:
    SettingsTreeDataProvider();
    
    int GetRootCount() const override;
    TreeId GetRootId(int index) const override;
    int GetChildCount(TreeId parent) const override;
    TreeId GetChildId(TreeId parent, int index) const override;
    const char* GetLabel(TreeId id) const override;
    bool IsExpandable(TreeId id) const override;

    /** Returns set of tree ids that should be expanded so every node whose label contains \p query (case-insensitive) is visible. */
    std::unordered_set<TreeId> GetAncestorIdsToExpandForSearch(const std::string& query) const;

    /** Returns set of tree ids that should be expanded so every node in \p nodeIds is visible (ancestors of each node). */
    std::unordered_set<TreeId> GetAncestorIdsToExpandForNodes(const std::unordered_set<TreeId>& nodeIds) const;

    /**
     * Appends categories for tree nodes whose titles match \p lowerQuery (already lowercased),
     * in DFS order from roots. Skips ids already present in \p seen (typically curated search hits).
     */
    void AppendCategoriesMatchingTreeLabels(const std::string& lowerQuery,
                                            std::vector<SettingsCategory>& outCategories,
                                            std::unordered_set<SettingsCategory>& seen) const;

    /**
     * Row class of each registered category, indexed by its id minus
     * kRegistrySettingsCategoryBase; empty where the category declares none.
     * Filled by RefreshRegistryNodes, which runs before a registry id can
     * enter the tree's flat list and on each registration, so a bound
     * registry row always has its class here. Takes no registry snapshot.
     */
    const std::vector<std::string>& GetRegistryRowClasses() const { return m_RegistryRowClasses; }

private:
    struct TreeNode {
        std::string Label;
        std::vector<TreeId> Children;
        bool Expandable = false;
    };

    // Rebuilds the dynamic per-provider VCS tab nodes from the
    // EditorVcsProviderRegistry so tabs registered after construction (package
    // modules load at project open) appear on the next tree query.
    void RefreshVcsProviderNodes() const;

    // Same for categories registered through Editor::EditorSettingsRegistry:
    // upserts one node per snapshot entry and re-merges each root's children.
    void RefreshRegistryNodes() const;

    mutable std::unordered_map<TreeId, TreeNode> m_Nodes;
    mutable std::vector<std::string> m_RegistryRowClasses;
    std::vector<TreeId> m_Roots;
    // Hand-built children per root, captured at construction; registered
    // categories merge into these on every refresh.
    mutable std::unordered_map<TreeId, std::vector<TreeId>> m_LegacyRootChildren;
};

enum class LayoutMode {
    Vertical,    // Tree on top, content below
    Horizontal   // Tree on left, content on right
};

// Searchable setting item
struct SearchableSettingItem {
    std::string Label;           // Display name for search matching
    std::string Keywords;        // Additional keywords for search
    SettingsCategory Category;   // Which category this belongs to
    std::function<void(UIElement*)> CreateUI; // Function to create the UI element
};

class SettingsPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "settings-icon"; }

    SettingsPanel();
    ~SettingsPanel() override;

    void SetContext(EditorContext* context);
    void SetOpenColorPickerWindow(OpenColorPickerWindowFn fn) { m_OpenColorPickerWindow = std::move(fn); }
    void SetOnHdrOutputSettingsChanged(std::function<void()> cb) { m_OnHdrOutputSettingsChanged = std::move(cb); }
    // Invoked when a preview-thumbnail setting (e.g. IBL on/off, resolution) changes
    // and already-rendered thumbnails were invalidated, so panels that display model
    // thumbnails (Hierarchy icons) can re-fetch them.
    void SetOnPreviewThumbnailsChanged(std::function<void()> cb) { m_OnPreviewThumbnailsChanged = std::move(cb); }

    // Device used to read live HDR display capabilities; gates the HDR Mode dropdown to the
    // colorspaces the active display actually supports. Null = no device, fall back to all modes.
    // Invariant: this is the main window's device, which outlives the SettingsPanel, so the raw
    // pointer never dangles. If that ever changes, clear it (SetDevice(nullptr)) on device teardown.
    void SetDevice(Rendering::IDevice* device) { m_HdrDevice = device; }

    // Set the UIManager to configure (required for AA strength slider)
    void SetUIManager(UIManager* uiManager);

    void SetOnHierarchyTreeRowHeightChanged(std::function<void(float)> cb)
    {
        m_OnHierarchyTreeRowHeightChanged = std::move(cb);
    }
    void SetOnAssetsTreeRowHeightChanged(std::function<void(float)> cb) { m_OnAssetsTreeRowHeightChanged = std::move(cb); }
    void SetOnGetHierarchyTreeRowHeight(std::function<float()> cb) { m_OnGetHierarchyTreeRowHeight = std::move(cb); }
    void SetOnGetAssetsTreeRowHeight(std::function<float()> cb) { m_OnGetAssetsTreeRowHeight = std::move(cb); }
    void SetOnHierarchyTreeChildIndentChanged(std::function<void(float)> cb)
    {
        m_OnHierarchyTreeChildIndentChanged = std::move(cb);
    }
    void SetOnAssetsTreeChildIndentChanged(std::function<void(float)> cb)
    {
        m_OnAssetsTreeChildIndentChanged = std::move(cb);
    }
    void SetOnHierarchyTreeIconSizeChanged(std::function<void(float)> cb)
    {
        m_OnHierarchyTreeIconSizeChanged = std::move(cb);
    }
    void SetOnAssetsTreeIconSizeChanged(std::function<void(float)> cb) { m_OnAssetsTreeIconSizeChanged = std::move(cb); }
    // Notify other panels when the base UI font size changes (UI Text Scale slider).
    void SetOnBaseFontSizeChanged(std::function<void(float)> cb) { m_OnBaseFontSizeChanged = std::move(cb); }
    /// Fan out runtime font preference CSS to every editor window (main + floating).
    void SetOnEditorFontPreferencesChanged(std::function<void()> cb) { m_OnEditorFontPreferencesChanged = std::move(cb); }
    /// Re-apply the shared Toggle presentation to every editor window.
    void SetOnToggleStyleChanged(std::function<void(bool)> cb) { m_OnToggleStyleChanged = std::move(cb); }
    /// Fan out the text contrast (colour-keyed coverage boost) to every editor window's UIManager.
    void SetOnTextContrastChanged(std::function<void(float)> cb) { m_OnTextContrastChanged = std::move(cb); }
    /// Fan out the subpixel text AA opt-in to every editor window's UIManager.
    void SetOnTextSubpixelAAChanged(std::function<void(bool)> cb) { m_OnTextSubpixelAAChanged = std::move(cb); }
    /// Fan out the text smoothing gamma to every editor window's UIManager.
    void SetOnTextSmoothingGammaChanged(std::function<void(float)> cb) { m_OnTextSmoothingGammaChanged = std::move(cb); }
    /// Re-apply HiDPI / content-scale preferences to every editor window platform adapter.
    void SetOnHiDpiPlatformSettingsChanged(std::function<void()> cb) { m_OnHiDpiPlatformSettingsChanged = std::move(cb); }
    void SetOnGridIconSizeChanged(std::function<void(float)> cb) { m_OnGridIconSizeChanged = std::move(cb); }
    void SetOnGridIconSizeFinalized(std::function<void(float)> cb) { m_OnGridIconSizeFinalized = std::move(cb); }
    void SetOnSmartFoldersAtTopChanged(std::function<void(bool)> cb) { m_OnSmartFoldersAtTopChanged = std::move(cb); }
    void SetOnSmartFoldersExpandedOnStartupChanged(std::function<void(bool)> cb) { m_OnSmartFoldersExpandedOnStartupChanged = std::move(cb); }
    void SetOnAssetsFoldersFirstChanged(std::function<void(bool)> cb) { m_OnAssetsFoldersFirstChanged = std::move(cb); }
    void SetOnAssetsExpandFoldersOnLoadChanged(std::function<void(bool)> cb) { m_OnAssetsExpandFoldersOnLoadChanged = std::move(cb); }
    void SetOnAssetsExtraBottomViewToolbarChanged(std::function<void(bool)> cb) { m_OnAssetsExtraBottomViewToolbarChanged = std::move(cb); }
    void SetOnAssetsSingleViewToggleIconChanged(std::function<void(bool)> cb) { m_OnAssetsSingleViewToggleIconChanged = std::move(cb); }
    void SetOnAssetsBottomToolbarZoomSliderVisibleChanged(std::function<void(bool)> cb)
    {
        m_OnAssetsBottomToolbarZoomSliderVisibleChanged = std::move(cb);
    }
    void SetOnVsyncChanged(std::function<void(bool)> cb) { m_OnVsyncChanged = std::move(cb); }
    // Fired after a project render setting that needs the shared apply path is
    // persisted. Carries no value: the re-apply reads one SettingsStore
    // snapshot and fans it out to every window.
    void SetOnProjectRenderSettingsChanged(std::function<void()> cb)
    {
        m_OnProjectRenderSettingsChanged = std::move(cb);
    }
    void SetOnInspectorCollapseArrowVisibilityChanged(std::function<void(bool)> cb) { m_OnInspectorCollapseArrowVisibilityChanged = std::move(cb); }
    void SetOnInspectorComponentIconsVisibilityChanged(std::function<void(bool)> cb) { m_OnInspectorComponentIconsVisibilityChanged = std::move(cb); }
    void SetOnInspectorFilledSectionsChanged(std::function<void(bool)> cb) { m_OnInspectorFilledSectionsChanged = std::move(cb); }
    void SetOnInspectorInfoCardsChanged(std::function<void(bool)> cb) { m_OnInspectorInfoCardsChanged = std::move(cb); }
    void SetOnInspectorBigNumberSpacingChanged(std::function<void(bool)> cb) { m_OnInspectorBigNumberSpacingChanged = std::move(cb); }
    void SetOnInspectorToggleAlignChanged(std::function<void(const std::string&)> cb);
    void SetOnInspectorSoloSectionsChanged(std::function<void(bool)> cb) { m_OnInspectorSoloSectionsChanged = std::move(cb); }
    void SetOnInspectorSoloKeepTransformChanged(std::function<void(bool)> cb) { m_OnInspectorSoloKeepTransformChanged = std::move(cb); }
    void SetOnTruncationThresholdChanged(std::function<void(float)> cb) { m_OnTruncationThresholdChanged = std::move(cb); }
    void SetOnTruncationEnabledChanged(std::function<void(bool)> cb) { m_OnTruncationEnabledChanged = std::move(cb); }
    void SetOnAssetsListColumnsChanged(std::function<void()> cb) { m_OnAssetsListColumnsChanged = std::move(cb); }
    void SetPingAsset(std::function<void(const std::filesystem::path&)> cb) { m_PingAsset = std::move(cb); }
    
    // Gizmo thickness callbacks
    void SetOnTranslateGizmoThicknessChanged(std::function<void(float)> cb) { m_OnTranslateGizmoThicknessChanged = std::move(cb); }
    void SetOnRotateGizmoThicknessChanged(std::function<void(float)> cb) { m_OnRotateGizmoThicknessChanged = std::move(cb); }
    void SetOnScaleGizmoThicknessChanged(std::function<void(float)> cb) { m_OnScaleGizmoThicknessChanged = std::move(cb); }
    void SetOnGizmoConstantSizeChanged(std::function<void(bool)> cb) { m_OnGizmoConstantSizeChanged = std::move(cb); }
    void SetOnRotateGizmoEnhancedChanged(std::function<void(bool)> cb) { m_OnRotateGizmoEnhancedChanged = std::move(cb); }
    
    // Gizmo scale callbacks (for constant screen size mode)
    void SetOnTranslateGizmoScaleChanged(std::function<void(float)> cb) { m_OnTranslateGizmoScaleChanged = std::move(cb); }
    void SetOnRotateGizmoScaleChanged(std::function<void(float)> cb) { m_OnRotateGizmoScaleChanged = std::move(cb); }
    void SetOnScaleGizmoScaleChanged(std::function<void(float)> cb) { m_OnScaleGizmoScaleChanged = std::move(cb); }
    
    // Constant size thickness callbacks (separate from normal thickness)
    void SetOnTranslateConstantThicknessChanged(std::function<void(float)> cb) { m_OnTranslateConstantThicknessChanged = std::move(cb); }
    void SetOnRotateConstantThicknessChanged(std::function<void(float)> cb) { m_OnRotateConstantThicknessChanged = std::move(cb); }
    void SetOnScaleConstantThicknessChanged(std::function<void(float)> cb) { m_OnScaleConstantThicknessChanged = std::move(cb); }

    // Grid settings callbacks
    void SetOnGridOpacityChanged(std::function<void(float)> cb) { m_OnGridOpacityChanged = std::move(cb); }
    void SetOnGridSnapSizeChanged(std::function<void(float)> cb) { m_OnGridSnapSizeChanged = std::move(cb); }
    void SetHierarchyTreeIconSizeValue(float px);
    void SetAssetsTreeIconSizeValue(float px);
    void SetHierarchyTreeIconSizeValueAndPersist(float px);
    void SetAssetsTreeIconSizeValueAndPersist(float px);
    void SetGridIconSizeValue(float px);
    void SetSmartFoldersAtTopValue(bool atTop);
    void SetSmartFoldersExpandedOnStartupValue(bool expanded);
    void SetAssetsFoldersFirstValue(bool foldersFirst);
    void SetAssetsExpandFoldersOnLoadValue(bool expand);
    void SetAssetsExtraBottomViewToolbarValue(bool enabled);
    void SetAssetsSingleViewToggleIconValue(bool enabled);
    void SetAssetsBottomToolbarZoomSliderVisibleValue(bool visible);
    void SetInspectorCollapseArrowVisibleValue(bool visible);
    void SetInspectorComponentIconsVisibleValue(bool visible);
    void SetInspectorFilledSectionsValue(bool enabled);
    void SetInspectorInfoCardsVisibleValue(bool visible);
    void SetInspectorBigNumberSpacingValue(bool enabled);
    void SetInspectorToggleAlignValue(const std::string& value);
    void SetInspectorSoloSectionsValue(bool enabled);
    void SetInspectorSoloKeepTransformValue(bool enabled);

    // Get global search bar visibility state
    static bool GetSearchBarsVisible() { return EditorSearchBars::GetVisible(); }
    static bool GetSearchBarsAtTop() { return EditorSearchBars::GetAtTop(); }

    /** Apply saved inspector component header height (compact 24px vs default 28px). */
    static void ApplySavedCompactComponentHeadersStyle(UIManager* ui);

    /** Apply saved "Colored Hierarchy Icons" toggle (adds/removes root class). */
    static void ApplySavedHierarchyIconsColoredStyle(UIManager* ui);

    /** Apply saved "3D Model Thumbnails Always Colored" toggle (adds/removes `hierarchy-model-thumbs-colored` root class). */
    static void ApplySavedHierarchyModelThumbsAlwaysColoredStyle(UIManager* ui);

    /** Apply saved "Gray Sliders" toggle (adds/removes `gray-sliders` root class). */
    static void ApplySavedGraySlidersStyle(UIManager* ui);

    /** Apply saved Toggle presentation (switches or square checkmarks) and the inner-shadow root class. */
    static void ApplySavedToggleStyle(UIManager* ui);

    /** Apply a Toggle presentation without another preference-file read. */
    static void ApplyToggleStyle(UIManager* ui, bool useCheckmarks);
    /// Apply the saved subpixel text AA opt-in to a UIManager. Startup + new
    /// windows: the Font Rendering settings content builds lazily, so the
    /// saved preference must engage without it.
    static void ApplySavedTextSubpixelAA(UIManager* ui);

    /** Apply saved "Big Number Spacing" to every FloatField (process-wide; the Settings page builds lazily). */
    static void ApplySavedInspectorBigNumberSpacing();

    /// Apply the saved text contrast / smoothing prefs (same lazy-build
    /// constraint as the subpixel opt-in, #714).
    static void ApplySavedTextContrast(UIManager* ui);
    static void ApplySavedTextSmoothingGamma(UIManager* ui);

    /** Apply saved "Panel Tab Icons" toggle (adds/removes `no-tab-icons` root class). */
    static void ApplySavedTabIconsStyle(UIManager* ui);

    /** Apply saved popup drop-shadow toggles (adds/removes the `no-dropdown-shadow`,
        `no-picker-shadow`, and `no-completion-shadow` root classes). */
    static void ApplySavedPopupShadowStyle(UIManager* ui);

    /** Apply saved Inspector row spacing before the lazy Settings panel is opened. */
    static void ApplySavedRowGapStyles(UIManager* ui);

    /** Apply the saved global value-box height (sets `--value_box_height` on the root). */
    static void ApplySavedValueBoxHeightStyle(UIManager* ui);

    /** Apply saved node graph node-header alignment to any mounted node graph canvas. */
    static void ApplySavedNodeGraphHeaderAlignmentStyle(UIManager* ui);

    /** Apply saved script editor line-height multiplier as a dynamic stylesheet. */
    static void ApplySavedScriptLineHeightStyle(UIManager* ui);

    // Register/unregister search bars for real-time updates
    static void RegisterSearchBar(UIElement* searchBar, UIElement* /*icon*/) { EditorSearchBars::Register(searchBar); }
    static void UnregisterSearchBar(UIElement* searchBar) { EditorSearchBars::Unregister(searchBar); }

    /// Request that the panel show a specific category (e.g. from Add Tag modal "All Tags…").
    void RequestShowCategory(SettingsCategory category);
    /// Request a category registered through EditorSettingsRegistry by stable id.
    void RequestShowRegistryCategory(std::string_view categoryId);

    /// Search metadata shared with the global command palette. The vector is populated
    /// when the settings tree is built and remains owned by this panel.
    const std::vector<SearchableSettingItem>& GetSearchableItems() const { return m_SearchableItems; }

private:
    void BuildUI();
    void RebuildLayout(LayoutMode newMode);
    void CreateTreeView(UIElement* parent);
    void CreateContentPane(UIElement* parent);
    void CreateSearchBar(UIElement* parent);
    void OnPostLayout() override;
    void BindFromAssetsDeferred();
    void OnCategorySelected(TreeId id);
    void ShowCategoryContent(SettingsCategory category);
    void CreateUISettingsContent();
    void CreateUIColorsContent();
    void CreateUIAppearanceContent();
    void CreateUIHiDpiContent();
    void CreateUIFontSizesContent();
    void CreateUITreesContent();
    void CreateUIAssetsContent();
    void CreateUIInspectorContent();
    void CreateUserSettingsContent();
    void CreateAudioSettingsContent();
    void CreateCameraSettingsContent();
    void SyncCameraSettingsControls();
    void CreateSceneSettingsContent();
    void CreateAnimationSettingsContent();
    void CreateInputSettingsContent();
    void CreateAssetImportSettingsContent();
    void CreateRenderingSettingsContent();
    void NotifyProjectRenderSettingsChanged();
    void CreateHDROutputSettingsContent();
public:
    // Called by other systems (e.g. RenderPipelineInspector) when the active
    // render pipeline is changed elsewhere, so any open Settings panel can
    // update its dropdown without requiring a category re-open.
    static void NotifyActivePipelineChanged(const std::string& assetRelativePath);
private:
    void CreateTagsSettingsContent();
    void CreateScriptSettingsContent();
    void CreateShortcutsSettingsContent();
    void CreateGizmoSettingsContent();
    void CreateGridAndSnappingContent();
    void CreatePerformanceSettingsContent();
    void CreateProjectSettingsContent();
    void CreatePhysicsSettingsContent();
    void CreateVersionControlContent();
    // Per-provider VCS tabs are dynamic: content comes from
    // EditorVcsProviderRegistry descriptors (BuildSettingsContent).
    void CreateVcsProviderContent(const std::string& providerTypeId);
    void ClearContentPane();
    void CreateTreeScalingSettingsForTarget(bool forHierarchy);
    void ApplyHierarchyTreeIconSize(float px);
    void ApplyAssetsTreeIconSize(float px);
    void PerformSearch(const std::string& query);
    void RequestApplySearchState();
    void ApplySearchStateNow();
    void ShowSearchResults(const std::string& query);
    void OpenSearchResultInContext(SettingsCategory category,
                                   const std::string& label,
                                   const std::string& query);
    void HighlightSearchResultInContext(const std::string& label,
                                        const std::string& query);
    void ScrollPendingSearchRowIntoView();
    void ClearSearchResults();
    void ClearSearchHighlights();
    void ApplySearchFilterToContent(const std::string& searchText);
    void RegisterSearchableItems();
    void CreateRegistrySettingsContent(const Editor::SettingsCategoryDescriptor& descriptor);
    void OpenColorPicker(uint32_t initialArgb,
                         std::function<void(uint32_t)> onApply,
                         std::function<void()> onCancel,
                         std::function<void(uint32_t)> onValueChanging = {});
    
    bool IsParentCategory(SettingsCategory category) const;
    SettingsCategory GetDefaultChild(SettingsCategory parent) const;
    void UpdateTreePaneHeight();
    void ExportUserSettings();
    void ShowImportUserSettingsModal(const std::filesystem::path& importPath);
    int GetVisibleItemCount() const;

    // Tree and layout components
    TreeView* m_TreeView = nullptr;
    UIElement* m_TreePane = nullptr;
    UIElement* m_ContentPane = nullptr;
    UIElement* m_VerticalSplitter = nullptr;
    UIElement* m_SearchBar = nullptr;
    Label* m_ContentHeader = nullptr;
    ScrollView* m_ContentBody = nullptr;
    TextField* m_SearchField = nullptr;
    std::shared_ptr<bool> m_LifetimeToken = std::make_shared<bool>(true);

    // Vertical-mode tree pane height: 0 = auto-size to content; >0 = user override.
    float m_UserTreePaneHeightPx = 0.0f;
    bool m_TreeSplitterDragging = false;
    float m_TreeSplitterDragStartY = 0.0f;
    float m_TreeSplitterDragStartHeight = 0.0f;

    // Layout mode tracking
    LayoutMode m_CurrentLayoutMode = LayoutMode::Vertical;
    float m_LastAspectRatio = 0.0f;
    
    // Data providers
    std::unique_ptr<SettingsTreeDataProvider> m_TreeDataProvider;
    std::unique_ptr<UI::Interaction::SelectionModel> m_TreeSelectionModel;

    // UI elements for settings
    Slider* m_HierarchyTreeChildIndentSlider = nullptr;
    Label* m_HierarchyTreeChildIndentLabel = nullptr;
    Slider* m_HierarchyTreeRowHeightSlider = nullptr;
    Label* m_HierarchyTreeRowHeightLabel = nullptr;
    Slider* m_HierarchyTreeIconSizeSlider = nullptr;
    Label* m_HierarchyTreeIconSizeLabel = nullptr;
    FloatField* m_HierarchyTreeIconSizeField = nullptr;
    Slider* m_AssetsTreeChildIndentSlider = nullptr;
    Label* m_AssetsTreeChildIndentLabel = nullptr;
    Slider* m_AssetsTreeRowHeightSlider = nullptr;
    Label* m_AssetsTreeRowHeightLabel = nullptr;
    Slider* m_AssetsTreeIconSizeSlider = nullptr;
    Label* m_AssetsTreeIconSizeLabel = nullptr;
    FloatField* m_AssetsTreeIconSizeField = nullptr;
    Slider* m_GridIconSizeSlider = nullptr;
    FloatField* m_GridIconSizeField = nullptr;
    Toggle* m_SearchBarsToggle = nullptr;
    Toggle* m_CheckmarkTogglesToggle = nullptr;
    Toggle* m_TabIconsToggle = nullptr;
    TextField* m_FontSmoothingField = nullptr;
    
    // Font size sliders
    Slider* m_FontSizeBaseSlider = nullptr;
    Slider* m_FontSizeTreeSlider = nullptr;
    Slider* m_FontSizeSmallSlider = nullptr;
    Slider* m_FontSizeHeaderSlider = nullptr;
    float m_HierarchyTreeIconSizeValue = 20.0f;
    bool m_HierarchyTreeIconSizeUpdating = false;
    float m_AssetsTreeIconSizeValue = 20.0f;
    bool m_AssetsTreeIconSizeUpdating = false;
    float m_GridIconSizeValue = 80.0f;
    bool m_GridIconSizeUpdating = false;
    bool m_GridIconSizePending = false;
    
    // Syntax highlighting color fields
    TextField* m_SyntaxKeywordColorField = nullptr;
    TextField* m_SyntaxStringColorField = nullptr;
    TextField* m_SyntaxCommentColorField = nullptr;
    TextField* m_SyntaxNumberColorField = nullptr;
    TextField* m_SyntaxTypeColorField = nullptr;
    TextField* m_SyntaxDefaultColorField = nullptr;
    
    // Script settings
    Toggle* m_ScriptOpenInInspectorToggle = nullptr;
    Toggle* m_ShaderGraphGlslOpenToggle = nullptr;
    Slider* m_ScriptLineHeightSlider = nullptr;
    
    // Closed whenever content-page elements are destroyed: a colour picker the
    // page opened closes with it instead of calling into the destroyed page.
    Editor::ColorPickerScope m_PageColorPickers;
    // UI accent color (Settings > UI): swatch opens the editor color picker
    UIElement* m_UIAccentColorSwatch = nullptr;
    uint32_t m_UIAccentColor; // Initialized to AccentStyleHelper::kDefaultAccentColor.
    // Asset icon tint (folders/scenes/scripts in grid/list/tree)
    UIElement* m_UIAssetIconTintSwatch = nullptr;
    uint32_t m_UIAssetIconTint = 0xFFAAAAAA; // default neutral icon tint (#AAAAAA)
    // Smart Folder icon tint (special blue folder icons in hierarchy)
    UIElement* m_UISmartFolderIconTintSwatch = nullptr;
    uint32_t m_UISmartFolderIconTint = 0xFF6BA4F8; // default blue tint used in smartfolders.css
    // Tooltip arrow color swatch
    // Scene View selection highlight color swatches.
    UIElement* m_SelectionBoxColorSwatch = nullptr;
    uint32_t m_SelectionBoxColor = 0x80FFFFFFu;
    UIElement* m_SelectionOutlineColorSwatch = nullptr;
    uint32_t m_SelectionOutlineColor = 0xFFFFB300u;
    UIElement* m_SceneViewBackgroundColorSwatch = nullptr;
    uint32_t m_SceneViewBackgroundColor = 0xFF1A1A1Au;
    UIElement* m_RulerIndicatorColorSwatch = nullptr;
    UIElement* m_MeasureColorSwatch = nullptr;
    UIElement* m_GridColor3DSwatch = nullptr;
    uint32_t m_GridColor3D = 0xFF000000u;
    UIElement* m_GridColor2DSwatch = nullptr;
    uint32_t m_GridColor2D = 0xFF000000u;
    uint32_t m_RulerIndicatorColor = 0xF2F2664Du;
    uint32_t m_MeasureColor = 0xFFFFC738u;
    StylesheetHandle m_UIAccentStylesheet;
    // Inspector row indent + gap + label width: runtime stylesheet
    Slider* m_InspectorRowIndentSlider = nullptr;
    Slider* m_InspectorRowGapSlider = nullptr;
    Slider* m_InspectorLabelWidthSlider = nullptr;
    void ApplyInspectorRowStyleToUI(float indent, float gap, float labelWidthPercent);
    // UI scrollbar thumb color (Settings > UI): swatch + runtime stylesheet
    UIElement* m_UIScrollbarColorSwatch = nullptr;
    uint32_t m_UIScrollbarColor = 0xFF383838;
    StylesheetHandle m_UIScrollbarStylesheet;
    StylesheetHandle m_UISceneToolStylesheet;
    EditorContext* m_EditorContext = nullptr;
    Rendering::IDevice* m_HdrDevice = nullptr;
    OpenColorPickerWindowFn m_OpenColorPickerWindow;
    std::function<void()> m_OnHdrOutputSettingsChanged;
    std::function<void()> m_OnPreviewThumbnailsChanged;
    void ApplyAccentColorToUI();    // Update CSS variables for accent color
    void ApplyScrollbarColorToUI(); // Update runtime styles for scrollbar thumb color
    void ApplyGraySlidersStyleToUI(); // Apply gray vs accent slider style from preference
    void ApplySceneToolActiveColor(const std::string& mode); // accent or gray
    void UpdateSwatchStyle(UIElement* swatch, uint32_t colorRgb);
    void UpdateAccentSwatchStyle()      { UpdateSwatchStyle(m_UIAccentColorSwatch, m_UIAccentColor); }
    void UpdateAssetIconTintSwatchStyle() { UpdateSwatchStyle(m_UIAssetIconTintSwatch, m_UIAssetIconTint); }
    void UpdateSmartFolderIconTintSwatchStyle() { UpdateSwatchStyle(m_UISmartFolderIconTintSwatch, m_UISmartFolderIconTint); }
    void UpdateSelectionBoxColorSwatchStyle()     { UpdateSwatchStyle(m_SelectionBoxColorSwatch, m_SelectionBoxColor); }
    void UpdateSelectionOutlineColorSwatchStyle() { UpdateSwatchStyle(m_SelectionOutlineColorSwatch, m_SelectionOutlineColor); }
    void UpdateSceneViewBackgroundColorSwatchStyle() { UpdateSwatchStyle(m_SceneViewBackgroundColorSwatch, m_SceneViewBackgroundColor); }
    void UpdateRulerIndicatorColorSwatchStyle()      { UpdateSwatchStyle(m_RulerIndicatorColorSwatch, m_RulerIndicatorColor); }
    void UpdateMeasureColorSwatchStyle()             { UpdateSwatchStyle(m_MeasureColorSwatch, m_MeasureColor); }
    void UpdateGridColor3DSwatchStyle()              { UpdateSwatchStyle(m_GridColor3DSwatch, m_GridColor3D); }
    void UpdateGridColor2DSwatchStyle()              { UpdateSwatchStyle(m_GridColor2DSwatch, m_GridColor2D); }

    UIManager* m_UIManager = nullptr;

    bool m_BindApplied = false;
    bool m_BindScheduled = false;
    bool m_UIBuilt = false;
    bool m_RebuildScheduled = false;
    bool m_InitialTreeFocusApplied = false;
    bool m_InitialTreeScrollApplied = false;
    uint64_t m_SceneViewCameraSettingsListenerId = 0;
    
    SettingsCategory m_CurrentCategory = SettingsCategory::UIAppearance;
    
    // Track last used child for each parent category
    std::unordered_map<SettingsCategory, SettingsCategory> m_LastUsedChild;
    
    // Track last panel height for responsive updates
    float m_LastPanelHeight = 0.0f;

    std::function<void(float)> m_OnHierarchyTreeRowHeightChanged;
    std::function<void(float)> m_OnAssetsTreeRowHeightChanged;
    std::function<float()> m_OnGetHierarchyTreeRowHeight;
    std::function<float()> m_OnGetAssetsTreeRowHeight;
    std::function<void(float)> m_OnHierarchyTreeChildIndentChanged;
    std::function<void(float)> m_OnAssetsTreeChildIndentChanged;
    std::function<void(float)> m_OnHierarchyTreeIconSizeChanged;
    std::function<void(float)> m_OnAssetsTreeIconSizeChanged;
    std::function<void(float)> m_OnBaseFontSizeChanged;
    std::function<void()> m_OnEditorFontPreferencesChanged;
    std::function<void(bool)> m_OnToggleStyleChanged;
    std::function<void(float)> m_OnTextContrastChanged;
    std::function<void(bool)> m_OnTextSubpixelAAChanged;
    std::function<void(float)> m_OnTextSmoothingGammaChanged;
    std::function<void()> m_OnHiDpiPlatformSettingsChanged;
    std::function<void(float)> m_OnGridIconSizeChanged;
    std::function<void(float)> m_OnGridIconSizeFinalized;
    std::function<void(bool)> m_OnSmartFoldersAtTopChanged;
    std::function<void(bool)> m_OnSmartFoldersExpandedOnStartupChanged;
    Toggle* m_SmartFoldersAtTopToggle = nullptr;
    Toggle* m_SmartFoldersExpandedOnStartupToggle = nullptr;
    std::function<void(bool)> m_OnAssetsFoldersFirstChanged;
    std::function<void(bool)> m_OnAssetsExpandFoldersOnLoadChanged;
    std::function<void(bool)> m_OnAssetsExtraBottomViewToolbarChanged;
    std::function<void(bool)> m_OnAssetsSingleViewToggleIconChanged;
    std::function<void(bool)> m_OnAssetsBottomToolbarZoomSliderVisibleChanged;
    std::function<void(bool)> m_OnVsyncChanged;
    std::function<void()> m_OnProjectRenderSettingsChanged;
    Toggle* m_AssetsFoldersFirstToggle = nullptr;
    Toggle* m_AssetsExpandFoldersOnLoadToggle = nullptr;
    Toggle* m_AssetsExtraBottomViewToolbarToggle = nullptr;
    Toggle* m_AssetsSingleViewToggleIconToggle = nullptr;
    Toggle* m_AssetsBottomToolbarZoomSliderVisibleToggle = nullptr;
    std::function<void(bool)> m_OnInspectorCollapseArrowVisibilityChanged;
    Toggle* m_InspectorCollapseArrowVisibleToggle = nullptr;
    std::function<void(bool)> m_OnInspectorComponentIconsVisibilityChanged;
    Toggle* m_InspectorComponentIconsVisibleToggle = nullptr;
    std::function<void(bool)> m_OnInspectorFilledSectionsChanged;
    Toggle* m_InspectorFilledSectionsToggle = nullptr;
    std::function<void(bool)> m_OnInspectorInfoCardsChanged;
    Toggle* m_InspectorInfoCardsToggle = nullptr;
    std::function<void(bool)> m_OnInspectorBigNumberSpacingChanged;
    Toggle* m_InspectorBigNumberSpacingToggle = nullptr;
    std::function<void(bool)> m_OnInspectorSoloSectionsChanged;
    Toggle* m_InspectorSoloSectionsToggle = nullptr;
    std::function<void(bool)> m_OnInspectorSoloKeepTransformChanged;
    Toggle* m_InspectorSoloKeepTransformToggle = nullptr;
    Toggle* m_GraySlidersToggle = nullptr;
    Toggle* m_CompactComponentHeadersToggle = nullptr;
    std::function<void(const std::string&)> m_OnInspectorToggleAlignChanged;
    Checkbox* m_InspectorToggleAlignLeft = nullptr;
    Checkbox* m_InspectorToggleAlignMiddle = nullptr;
    Checkbox* m_InspectorToggleAlignRight = nullptr;
    std::function<void(float)> m_OnTruncationThresholdChanged;
    std::function<void(bool)> m_OnTruncationEnabledChanged;
    std::function<void()> m_OnAssetsListColumnsChanged;
    std::function<void(const std::filesystem::path&)> m_PingAsset;
    
    // Search functionality (Inspector-style: highlight in tree and content, expand tree to show matches)
    std::vector<SearchableSettingItem> m_SearchableItems;
    std::string m_CurrentSearchQuery;
    bool m_ShowingSearchResults = false;
    bool m_SearchApplyPosted = false;
    std::vector<UIElement*> m_SearchHighlightedElements;
    std::vector<UIElement*> m_SearchFilteredElements;
    /** Categories that contain at least one search match, in tree order. */
    std::vector<SettingsCategory> m_SearchMatchCategories;
    struct PendingSearchContextReveal {
        SettingsCategory Category;
        std::string Label;
        std::string Query;
    };
    std::optional<PendingSearchContextReveal> m_PendingSearchContextReveal;
    // The row a search reveal still has to scroll into view. Centering it needs
    // real layout geometry, which only exists once layout has run, so the scroll
    // waits for OnPostLayout rather than for a guessed number of action drains.
    std::optional<PendingSearchContextReveal> m_PendingSearchScroll;
    struct SearchSegmentReplacement {
        UIElement* parent = nullptr;
        std::unique_ptr<UIElement> OriginalLabel;
        UIElement* segmentContainer = nullptr;
    };
    std::vector<SearchSegmentReplacement> m_SearchSegmentReplacements;

    // Registered button rows with a visibility condition, polled after layout
    // so a row can hide while its page stays open. Cleared with the content.
    std::vector<std::pair<UIElement*, std::function<bool()>>> m_RegistryButtonRows;

    // Gizmo thickness callbacks
    std::function<void(float)> m_OnTranslateGizmoThicknessChanged;
    std::function<void(float)> m_OnRotateGizmoThicknessChanged;
    std::function<void(float)> m_OnScaleGizmoThicknessChanged;
    std::function<void(bool)> m_OnGizmoConstantSizeChanged;
    std::function<void(bool)> m_OnRotateGizmoEnhancedChanged;

    // Gizmo scale callbacks (for constant screen size mode)
    std::function<void(float)> m_OnTranslateGizmoScaleChanged;
    std::function<void(float)> m_OnRotateGizmoScaleChanged;
    std::function<void(float)> m_OnScaleGizmoScaleChanged;
    
    // Constant size thickness callbacks
    std::function<void(float)> m_OnTranslateConstantThicknessChanged;
    std::function<void(float)> m_OnRotateConstantThicknessChanged;
    std::function<void(float)> m_OnScaleConstantThicknessChanged;

    // Grid settings callbacks
    std::function<void(float)> m_OnGridOpacityChanged;
    std::function<void(float)> m_OnGridSnapSizeChanged;
    
    // Gizmo thickness sliders
    Slider* m_TranslateGizmoThicknessSlider = nullptr;
    Slider* m_RotateGizmoThicknessSlider = nullptr;
    Slider* m_ScaleGizmoThicknessSlider = nullptr;
    
    // Gizmo scale sliders (constant screen size)
    Slider* m_TranslateGizmoScaleSlider = nullptr;
    Slider* m_RotateGizmoScaleSlider = nullptr;
    Slider* m_ScaleGizmoScaleSlider = nullptr;
    
    // Constant size thickness sliders
    Slider* m_TranslateConstantThicknessSlider = nullptr;
    Slider* m_RotateConstantThicknessSlider = nullptr;
    Slider* m_ScaleConstantThicknessSlider = nullptr;
    
    // Gizmo settings section containers (for show/hide)
    UIElement* m_GizmoThicknessSection = nullptr;
    UIElement* m_GizmoConstantSizeSection = nullptr;
};

} // namespace GameEngine
