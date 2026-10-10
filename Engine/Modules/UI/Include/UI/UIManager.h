#pragma once

#include <algorithm>
#include <cstdint>
#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

#include "AssetCore/GUID.h"
#include "AssetCore/NineSlice.h"
#include "Mathematics/Vector2.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Geometry/ShapeBuilder.h"
#include "Scheduler/Scheduler.h"
#include "UI/ModifierKeyState.h"
#include "UI/ResolvedStyle.h"
#include "UI/UICompatDrawRuns.h"
#include "UI/SimpleSlotAllocator.h"
#include "UI/SlotAllocator.h"
#include "UI/UIEvents.h"
#include "UI/UIPlatform.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"
#include "UI/UIScaleSettings.h"
#include "UI/UITargetSpace.h"
#include "UI/UITextureSpace.h"
#include "UI/UiDispatcher.h"
#include "UI/VirtualizationCoordinator.h"

namespace JobSystem
{
class WorkStealingThreadPool;
}

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
// Forward declarations to avoid including Yoga headers in this public header.
struct YGNode;
using RetainedYGNodeRef = YGNode*;
struct BuiltNode;
struct UpdateContext;
struct YogaBuildItem; // MT-4.0 cascade seam; defined in UIManager_Internal.h
#endif

namespace GameEngine
{

namespace Rendering
{
namespace Text
{
class FontAtlas;
}
namespace RenderGraph
{
class RGFrame;
struct RGTexture;
enum class RGLoadOp : uint8_t; // defined in RGFrame.h; fwd-declared to keep it out of this header
}
} // namespace Rendering
class AssetManager;
struct ResolvedStyle;   // forward
struct VisualStyle;     // forward
class UIElement;        // forward
class ITextMeasurable;  // forward
class Label;            // forward
class TextInput;        // forward
class DockspaceElement; // forward
class UILayoutAsset;    // forward
class UIStyleAsset;     // forward
class TextureAsset;     // forward
class UIHotReload;      // forward
class TransitionEngine; // forward
class ScrollView;       // forward
class DismissablePopup;    // forward
class IVirtualizedControl; // forward
struct RetainedYogaNode; // forward; defined after class UIManager
namespace UIParsing { struct ElementState; } // forward (MT-4.2 share-key helpers)

namespace UI
{
template <typename T> class FrameBufferRing; // forward
struct UIPrimitive;                          // forward
struct UIClipRect;                           // forward
class UITextureRegistry;                     // forward
}

namespace UI::Interaction
{
class DragDropManager;  // forward
class DragDropOverlay;  // forward
class TooltipOverlay;   // forward
}

class UIManager
{
  public:
    explicit UIManager(Rendering::IDevice* device);
    UIManager(Rendering::IDevice* device, AssetManager* assetManager);
    ~UIManager();

    // Deferred outcome of the declared-transition registry mirror (MT-4.1).
    // ResolveCascadeForElement computes this from the freshly cascaded
    // rs.Transitions + ownership and OUTPUTS it instead of mutating
    // m_TransitioningElements inline, so the cascade compute stage stays
    // manager-container-free (MT-4.2 runs it on a worker). The caller applies
    // it on the UI thread via ApplyTransitionRegistration. Public because the
    // module-internal YogaBuildItem (UIManager_Internal.h) carries it across
    // the compute -> apply seam.
    enum class TransitionRegistration : uint8_t
    {
        None = 0,   // no cascade ran, or ownership disqualified an insert
        Register,   // el owns transitions on this manager -> insert
        Unregister, // el declares no transitions -> erase
    };

    // True after teardown detach notifications invalidate the lifetime token. The element forwarders consult it so the
    // owned tree, which is destroyed from inside the destructor, does not call back into a
    // manager that is tearing down. Only ever asked through an element's current owner,
    // which never dangles (~UIManager detaches every element that outlives it). Anything
    // else that holds a manager it does not own keeps a UIManagerRef.
    bool IsBeingDestroyed() const { return !m_Lifetime; }

    // Cascade-memoization Phase 6: true if any selector in the
    // effective stylesheet set uses `+` or `~`. Class/id mutation sites
    // consult this (via UIManagerUsesSiblingCombinators forwarder) to
    // decide whether following-sibling rule caches need invalidation.
    bool UsesSiblingCombinators() const { return m_StyleAnalysis.UsesSiblingCombinators; }
    // The sibling-position pseudos (:first-child, :nth-last-of-type, ...)
    // any selector uses, as PseudoKindBit bits. Child-list mutations restyle
    // only the siblings whose match these pseudos can change.
    uint32_t GetStructuralPseudoMask() const { return m_StyleAnalysis.StructuralPseudoMask; }
    // True if any selector uses `:empty`. Child-list mutations use this to
    // restyle the parent when it crosses the empty/non-empty boundary.
    bool UsesEmptyPseudo() const { return m_StyleAnalysis.UsesEmptyPseudo; }
    // P4 (C-10): see m_RuleCacheEpoch. Consumed via the UIElement.h
    // forwarder by IsRuleCacheValid and stamped by BuildRuleCache.
    uint32_t GetRuleCacheEpoch() const { return m_RuleCacheEpoch; }

    Rendering::IDevice* GetDevice() const { return m_Device; }

    // Optional platform API (clipboard, etc.)
    void SetPlatform(UI::IPlatformApi* platform) { m_Platform = platform; }
    UI::IPlatformApi* GetPlatform() const { return m_Platform; }

    /// Held modifier keys (same bitmask as UIEvent::mods), for pointer handlers that do not receive mods.
    int GetModifierKeys() const;
    /// Clear held modifiers when the host window loses focus. See
    /// ModifierKeyState::Reset for why the platform's own synthesized key
    /// releases do not cover this.
    void ResetModifierKeys();

    /// Reconcile held modifiers against the platform mask carried by a pointer
    /// event. A modifier held from before this window took focus produces no key
    /// event here, so a click is the first and only chance to observe it.
    void SyncModifierKeys(int mods);

    /// Reconcile held modifiers against a live query of the platform's modifier
    /// state. Unlike SyncModifierKeys this releases a tracked key the platform
    /// no longer reports held. See ModifierKeyState::ApplyLiveMask.
    void ReconcileModifierKeys(int liveMods);

    /// True when focus last moved via Tab/Shift+Tab or while a key event was dispatched (a key handler's
    /// SetFocusById or FocusElement). False after a primary-button press path clears it.
    /// Text fields use this so FocusIn (delivered the frame after mousedown) does not overwrite pointer placement.
    bool IsFocusViaKeyboard() const { return m_FocusViaKeyboard; }

    // Drag/drop (per-window). Cross-window routing is handled by the Editor host.
    UI::Interaction::DragDropManager* GetDragDropManager() const { return m_DragDrop.get(); }
    // Optional JobSystem for background parsing / heavy hotpaths.
    void SetJobSystem(JobSystem::WorkStealingThreadPool* jobSystem) { m_JobSystem = jobSystem; }

    // ---- Design-space probe: level-wave cascade (UIStyleTests/bench ONLY) ---
    // NOT a product code path and never called by Update(). This is the third,
    // previously-untested MT design for the UI style cascade (after MT-4 fork-
    // join and MT-4.3 work-first claim, both killed on measurement). It re-drives the REAL
    // per-element cascade (ResolveCascadeForElement) over the current tree in
    // one of two orders so a harness can A/B them:
    //   levelWave == false : DFS preorder, single shared donor cache — this is
    //                        the product's serial cascade order (BuildYogaRecursive).
    //   levelWave == true  : level-synchronous waves — elements at depth d are
    //                        independent given resolved depth d-1, so per level
    //                        the work items are grouped by sibling-share key and
    //                        driven with ONE ParallelFor over GROUPS (a group's
    //                        donor computes, its sharees copy), then a barrier
    //                        before the next level. Barriers = tree depth.
    // Both orders honour P4 sibling sharing and produce byte-identical resolved
    // styles (the harness asserts this). coldCaches invalidates the per-element
    // rule cache first (full structural rematch); warm leaves it populated.
    struct CascadeProbeResult
    {
        double DriveMs = 0.0;              // wall time of the whole cascade drive
        double GroupingMs = 0.0;           // level-wave only: per-level BFS + share-key bucketing
        uint32_t ElementCount = 0;
        uint32_t DonorComputes = 0;        // elements that ran a full ComputeStyleInto
        uint32_t ShareeCopies = 0;         // elements served from a donor snapshot
        uint32_t LevelCount = 0;           // = tree depth + 1 = level-wave barrier count
        uint32_t MaxLevelWidth = 0;
        uint32_t MaxLevelGroups = 0;
        std::vector<uint32_t> LevelWidths;      // element count per depth level
        std::vector<uint32_t> LevelGroupCounts; // share-group count per level (parallel width)
    };
    void RunLevelWaveCascadeProbe(bool levelWave, bool coldCaches, CascadeProbeResult& out);

    // Cursor callback: invoked when the cursor style should change based on hovered element.
    // The callback receives the CursorStyle from the hovered element's computed style.
    using CursorCallback = std::function<void(CursorStyle)>;
    void SetCursorCallback(CursorCallback callback) { m_CursorCallback = std::move(callback); }

    // Tooltip configuration supplied per-frame by the host application.
    struct TooltipConfig
    {
        bool     Enabled                = true;
        float    HoverDelaySeconds      = 0.5f;
        float    HoverResetDelaySeconds = 0.5f;
        bool     MiddleMouseShow        = false;
        uint32_t ArrowColor             = 0; // 0 = use CSS default
    };
    using TooltipConfigProvider = std::function<TooltipConfig()>;
    void SetTooltipConfigProvider(TooltipConfigProvider provider) { m_TooltipConfigProvider = std::move(provider); }

    /** Immediately hide the active tooltip overlay (e.g. when the hovered target moves under the cursor). */
    void DismissActiveTooltip();

    // Per-frame update. This is the primary entry point for UI time,
    // input dispatch, layout and geometry generation.
    //
    // Update(deltaTime) is shorthand for Update(deltaTime, true).
    //
    // When interactive == false, Update still recomputes layout and hover
    // state but suppresses interactive side effects (focus changes,
    // dockspace rebuilds, control-driven relayout, timers/dispatchers) so
    // callers can render a passive/preview frame.
    void Update(float deltaTime);
    void Update(float deltaTime, bool interactive);

    // Populate each element's ResolvedStyle from the CSS cascade.
    // Called during Update() and before Render() to ensure style data is fresh.
    void ResolveStyles();

    // Refresh the dynamic pseudo-class analysis (hover/active/focus property
    // classification) when the stylesheet set or content changes.
    void RefreshDynamicStyleAnalysis(
        const std::unordered_set<const Stylesheet*>& sheets,
        uint64_t sheetHash);

    // RenderGraph arm (slice 8b): declares the UI overlay pass into an immediate-mode
    // frame, attaching `target` with ClearOps — the UI pass is the target's
    // clearer (it absorbs the old Editor.ClearPresentationTarget). Declared
    // fresh every frame: no retained pass state, no recreate hashing. The
    // pass is declared even with zero primitives or an unready UI (clear-only
    // frame — the target must never present unwritten memory); the exec body
    // keeps the draw early-outs. RenderGraph ids consumed here come ONLY from this
    // frame's PublishExternalTextureRG calls — stale publishes (wrong frame
    // or frame index) are skipped. Returns false (nothing declared) only for
    // an invalid target/device — the caller must then skip its terminal
    // encode/present of the target.
    //
    // `targetSpace` is the HOST's declaration of the attachment's colour
    // space — required, no default: it drives the SDF shader's output arm
    // (HDR paper-white lift) and the subpixel text activation gate, and a
    // space stated by writing nothing is exactly how a wrong value hides
    // (#767 P2a / #784). A display-bound attachment passes
    // UITargetSpace::ForDisplay(activeMode); an offscreen attachment passes
    // what its own consumers expect, never the display's mode.
    // The UI owns/clears the target (editor chrome, backbuffer direct-attach).
    bool RenderRG(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture target,
                  UI::UITargetSpace targetSpace);

    // Load-op overload: pass Load to composite the UI OVER existing content — a
    // game HUD over the scene's FinalColor (Player + Game View) — instead of
    // clearing the target. Two overloads (not a default arg) so RGLoadOp can stay
    // forward-declared here; the Clear default is resolved in the .cpp.
    bool RenderRG(Rendering::RenderGraph::RGFrame& frame, Rendering::RenderGraph::RGTexture target,
                  UI::UITargetSpace targetSpace, Rendering::RenderGraph::RGLoadOp loadOp);

    // Request an immediate relayout pass. This is consumed during Update()
    // to allow controls (splitters, views rebuilding children, etc.) to
    // trigger a second Yoga layout after mutations.
    void RequestRelayout() { m_RequestRelayout = true; }

    // Invalidate Yoga's retained result for a subtree root whose available
    // size changed during post-layout convergence without changing its CSS.
    void InvalidateRetainedLayout(UIElement* subtreeRoot);

    // Debug/telemetry: record that a control requested relayout and who did it.
    void NotifyRelayoutRequested(const UIElement* source);

    // Scroll offset changes are applied by UIManager as a retained "positions-only" patch.
    // Controls (ScrollView/Scrollbar) may change scroll offsets during event dispatch.
    // We track this so Update() avoids ultra-fast early returns that would otherwise defer
    // scroll translation until the next unrelated input.
    void NotifyScrollOffsetsChanged() { m_ScrollOffsetsChanged = true; }

    // Virtualization impact tracking:
    // Virtualized controls (ListView/GridView/TreeView) may rebind pooled cells, adjust layout-override rects,
    // or mutate topology (pool resize / attach). This helps UIManager gate patch-only fast paths and
    // decide when to force an immediate relayout pass for correctness.
    enum VirtualizationImpact : uint32_t
    {
        VirtualizationNone = 0u,
        VirtualizationRebindChanged = 1u << 0,
        VirtualizationLayoutRectsChanged = 1u << 1,
        VirtualizationTopologyChanged = 1u << 2,
    };
    // Pairing invariant: a notify must be accompanied by dirty marks, a
    // relayout request, or enqueued coordinator work. Impact raised before
    // the initial solve is snapshot-consumed once that solve services it
    // (UIManager_Update.cpp), so an unpaired notify alone is not guaranteed
    // to survive to the late-relayout gate.
    void NotifyVirtualizationImpact(uint32_t flags) { m_VirtualizationImpactFlags.fetch_or(flags, std::memory_order_relaxed); }

    // Virtualization scheduling:
    // Controls can enqueue virtualization/binding work to run at a safe point during Update().
    // This is the start of Option A: separate "request work" from "mutate during layout/event dispatch".
    void EnqueueVirtualizationWork(UIElement* ctx, VirtualizationCoordinator::RunFn fn, VirtualizationCoordinator::Reason reason);

    // Lookup by stable per-element instanceId. Used by virtualization scheduling and tooling.
    UIElement* FindElementByInstanceId(uint64_t instanceId) const;


    // One-shot flush of deferred actions posted to the UI tree (safe pre-teardown)
    void DrainDeferredActionsOnce();

    // Pointer state (for editor integration)
    Mathematics::Vector2 GetMousePosition() const { return {m_MouseX, m_MouseY}; }
    // "The cursor is not over this surface." Negative by construction so that
    // IsMousePositionKnown() rejects it and no element's layout rect can contain it.
    static constexpr float kMousePositionUnknown = -1.0f;
    // True when the cursor is inside the window and the position is valid.
    // OnCursorEnter(false) sets the position to kMousePositionUnknown; callers
    // that poll GetMousePosition() should check this before doing hit-tests.
    bool IsMousePositionKnown() const { return m_MouseX >= 0.0f && m_MouseY >= 0.0f; }
    bool IsMouseDown() const { return m_MouseDown; }
    bool IsMouseCaptured() const { return m_MouseCaptured; }

    /// Drops any pointer capture and lets hover re-resolve under the cursor.
    ///
    /// Capture pins hover to the capture element — hover reads
    /// `captured ? captureElement : hitTest(...)` — so anything that takes the
    /// pointer away from the element that captured it must say so. A context
    /// menu opened from a press is the case in point: the press captured, the
    /// release went to the menu instead of back to the capturing element, and
    /// every row then sat under a pointer whose hover was pinned elsewhere.
    void ReleaseMouseCapture();

    const std::string& GetCaptureId() const { return m_CaptureId; }

    // Style/text resources for controls that need to recompute styles or
    // text metrics in response to pointer events (e.g., fields, scrollbars).
    const std::vector<StylesheetHandle>& GetStylesheets() const { return m_GlobalStylesheets; }

    // How many background images have a load or upload in flight. Non-zero means
    // some element is drawing nothing where it will draw once its texture lands,
    // so a capture taken now is of a transient frame. The material-side counter
    // says nothing about this — the two decode paths are separate — so a
    // screenshot of UI chrome reads this one.
    size_t PendingBackgroundImageCount() const { return m_BgUploadInFlight.size(); }
    Rendering::Text::FontAtlas* GetDefaultFontAtlas() const { return m_FontAtlas.get(); }
    Rendering::Text::FontAtlas* ResolveFontForStyle(const ResolvedStyle& style);
    /// The lines `text` takes in `element`'s text face (the font, size, line height and line
    /// breaking of its last layout) wrapped at `widthLogical`: the count the element's own
    /// measure reports for that text. For a control that fits text to a line budget within one
    /// layout. 0 when the element has not been laid out as text yet.
    int MeasureTextLineCount(const UIElement& element, const std::string& text, float widthLogical);
    /// The width, in logical px, `text` takes on one line in `element`'s text face (the font,
    /// size and letter spacing of its last layout): what the element's own measure reports for
    /// that text unwrapped. For a control that sizes itself to a text it has not shown yet,
    /// within one layout. 0 when the element has not been laid out as text yet.
    float MeasureTextWidth(const UIElement& element, std::string_view text);
    const ResolvedStyle* TryGetResolvedStyleFor(const UIElement* el) const;
    float GetTimeSeconds() const { return m_Time; }

    // Slug text rendering tuning.
    // TextContrast is Skia's SK_GAMMA_CONTRAST: the strength of the
    // colour-keyed coverage boost that compensates for source-over blending
    // reproducing a physically correct — and therefore perceptually thin —
    // average of ink and paper. The boost is keyed on the glyph's luminance
    // (full for near-black text, tapering to nothing for near-white text),
    // because that asymmetry, not glyph size, is what the error depends on.
    // Chrome ships 1.0 on Windows; 0 disables the boost entirely.
    // See Shaders/UI/text_mask_gamma.glsl for the model.
    float GetTextContrast() const { return m_TextContrast; }
    void SetTextContrast(float contrast) { m_TextContrast = std::clamp(contrast, 0.0f, 1.0f); }
    // TextBlendGamma selects the TARGET space coverage should blend linearly
    // in, and is a policy switch rather than a taste knob — hence no Settings
    // UI. 0 (default) = linear, i.e. area-exact ink; this is the target Skia's
    // mask gamma is built for with SK_GAMMA_SRGB. 1.0 = display-encoded space,
    // the "gamma-space blending" look. What the target MEANS depends on the
    // attachment's actual blend space: against today's linear attachments the
    // 0 default is the identity retarget and the correction reduces to
    // TextContrast; against an encoded-byte attachment (#767 flip) the same 0
    // selects the full Skia-direction correction. See
    // Shaders/UI/text_mask_gamma.glsl for the bidirectional model.
    float GetTextBlendGamma() const { return m_TextBlendGamma; }
    void SetTextBlendGamma(float gamma) { m_TextBlendGamma = std::clamp(gamma, 0.0f, 4.0f); }
    // What the SDF shader is actually pushed, resolved from the two knobs and
    // the host-declared target space — the ONE place an attachment's blend
    // space becomes a text-correction arm. Pure and static so the gate stays
    // pinned by TextCoverageCorrectionTests.
    struct TextCoverageConstants
    {
        float Contrast;
        float BlendGamma;
        // 1 when the attachment interpolates raw sRGB-encoded bytes — selects
        // the shader's Skia-direction retarget arm. 0 for linear attachments.
        // Exactly one target space resolves 1: UITargetSpace::EncodedSrgb
        // (the #767 flip).
        int BlendSpaceEncoded;
    };
    static TextCoverageConstants ResolveTextCoverageConstants(float contrast, float blendGamma,
                                                              UI::UITargetSpace targetSpace);
    // TextSmoothingGamma is a brightness-neutral edge softening control
    // applied AFTER the colour-keyed correction. The math is a contrast
    // remap centered on 0.5 — endpoints (0/1) and midpoint (0.5) are all
    // preserved, so total coverage area stays constant (no brightness shift).
    //   1.0 = neutral (identity)
    //   > 1.0 softens (edges pulled toward 0.5)
    //   < 1.0 sharpens (edges pushed toward 0/1)
    float GetTextSmoothingGamma() const { return m_TextSmoothingGamma; }
    void SetTextSmoothingGamma(float gamma) { m_TextSmoothingGamma = std::max(0.1f, gamma); }
    // Subpixel RGB text AA (ClearType-style, opt-in, default off). Glyph
    // coverage is evaluated per display subpixel (RGB stripe, R leftmost) and
    // blended per channel via dual-source blending — ~3x horizontal resolution
    // for text on standard-DPI LCDs, at the cost of colour fringes on glyph
    // edges (by design; screenshots carry them too). The setting is a request:
    // ResolveTextSubpixelActive decides per frame whether it actually engages.
    bool GetTextSubpixelAA() const { return m_TextSubpixelAA; }
    void SetTextSubpixelAA(bool enabled) { m_TextSubpixelAA = enabled; }
    // Pure activation gate, pinned by TextCoverageCorrectionTests. The setting
    // is necessary but not sufficient: subpixel additionally requires SDR
    // output (fringes are miscoloured after HDR tone mapping — Windows itself
    // drops ClearType in HDR), dual-source blending on the device, and the
    // subpixel shader variant actually staged — the variant of the target's
    // blend space (the caller passes the availability of the matching twin).
    // Both SDR encodings are eligible: 0 (linear attachment) and 1 (encoded
    // attachment — per-channel source-over on encoded bytes is precisely the
    // ClearType/Skia model, the space subpixel semantics are native to). Any
    // failed condition silently resolves to the grayscale pipeline — never a
    // dropped draw.
    static bool ResolveTextSubpixelActive(bool enabled, int outputEncoding,
                                          bool deviceSupportsDualSourceBlending,
                                          bool subpixelPipelineAvailable)
    {
        return enabled && (outputEncoding == 0 || outputEncoding == 1) &&
               deviceSupportsDualSourceBlending && subpixelPipelineAvailable;
    }
    // Translation of the host-declared target space into the SDF shader's
    // outputEncoding convention (SdfPushConstants: 0 = SDR linear,
    // 1 = SDR encoded bytes, 2 = HDR10 PQ, 3 = HLG, 4 = scRGB). Pure and
    // static, pinned by UITargetSpaceTests; the subpixel gate above consumes
    // its result, so both follow the TARGET, never the display (#784).
    static int ResolveOutputEncoding(UI::UITargetSpace targetSpace);
    // Resolved output state of the most recent RenderRG declare — the
    // TARGET-derived encoding and the subpixel gate it produced. Diagnostics
    // (get_hdr_output) read these to verify the declaration a host actually
    // made (#784); stamped on the main thread at declare time.
    int GetLastResolvedOutputEncoding() const { return m_LastResolvedOutputEncoding; }
    bool GetLastTextSubpixelActive() const { return m_LastTextSubpixelActive; }

    // Host-provided logical layout extent. When both are non-zero, this overrides
    // the swapchain size as the Yoga available-size + SDF target size — for
    // compositing the UI into an offscreen target (e.g. an editor Game View panel)
    // smaller than the OS window. Zero (default) = use the device swapchain size.
    void SetLayoutSizeOverride(uint32_t w, uint32_t h) { m_LayoutSizeOverrideW = w; m_LayoutSizeOverrideH = h; }

    // Resolved target scale (physical pixels per CSS logical pixel).
    // Uses platform DPI by default, or the configured reference viewport.
    float GetContentScale() const { return m_ContentScale; }
    // Layout, text, effects and pointer input share the selected target scale.
    void SetScaleSettings(const UI::UIScaleSettings& settings) { m_ScaleSettings = settings; }
    // Retained-mode diagnostics/safety toggles.
    // Correctness mode setter/getter is declared in the debug controls section below.
    // Kill switch for TryPointerOnlyFrame (also GE_UI_DISABLE_NOOP_FAST_PATH=1):
    // when disabled, bare mouse moves take the heavy pass like any other input.
    void SetFastPathNoOpDisabled(bool disabled) { m_DisableFastPathNoOp = disabled; }

    // Canonical range for the HDR UI black-floor lift (nits). The authoritative
    // clamp lives in ApplyRenderRuntimeConfig below; editor settings and widgets
    // source the C++ bound from here. (The SDF shader keeps its own clamp(0,48)
    // as a GPU-side backstop — that literal can't reference this constant.)
    static constexpr float kHdrUiBlackLiftMinNits = 0.0f;
    static constexpr float kHdrUiBlackLiftMaxNits = 48.0f;

    struct RenderRuntimeConfig
    {
        bool CorrectnessModeEnabled = false;
        bool DisableFastPathNoOp = false;
        // 0 = auto: track the device paper-white each frame (which follows the
        // OS SDR white level when the scene metadata isn't explicitly set).
        float HdrUiPaperWhiteNits = 0.0f;
        // < 0 = auto: lift the dark chrome by the display's black floor
        // (static-metadata min mastering luminance) — ~0 on OLED, the panel
        // floor on LCD. >= 0 = explicit lift in nits.
        float HdrUiBlackLiftNits = -1.0f;
    };
    void ApplyRenderRuntimeConfig(const RenderRuntimeConfig& cfg)
    {
        m_CorrectnessModeEnabled = cfg.CorrectnessModeEnabled;
        m_DisableFastPathNoOp = cfg.DisableFastPathNoOp;
        m_HdrUiPaperWhiteNits = cfg.HdrUiPaperWhiteNits > 0.0f
            ? std::clamp(cfg.HdrUiPaperWhiteNits, 40.0f, 350.0f)
            : 0.0f;
        m_HdrUiBlackLiftNits = cfg.HdrUiBlackLiftNits >= 0.0f
            ? std::clamp(cfg.HdrUiBlackLiftNits, kHdrUiBlackLiftMinNits, kHdrUiBlackLiftMaxNits)
            : -1.0f;
    }
    RenderRuntimeConfig GetRenderRuntimeConfig() const
    {
        RenderRuntimeConfig cfg{};
        cfg.CorrectnessModeEnabled = m_CorrectnessModeEnabled;
        cfg.DisableFastPathNoOp = m_DisableFastPathNoOp;
        cfg.HdrUiPaperWhiteNits = m_HdrUiPaperWhiteNits;
        cfg.HdrUiBlackLiftNits = m_HdrUiBlackLiftNits;
        return cfg;
    }
    void InvalidateRenderPassStateAfterTargetChange();

    struct RenderSlotDiagnostics
    {
        uint64_t ActiveWindowTargetId = 0;
        uint32_t DeviceFrameIndex = 0;
        uint32_t TargetFrameIndex = 0;
        uint32_t UiFrameSlot = 0;
        uint32_t RgPoolSlot = 0;
    };
    RenderSlotDiagnostics GetRenderSlotDiagnostics() const
    {
        RenderSlotDiagnostics d{};
        d.ActiveWindowTargetId = m_RenderDiagActiveWindowTargetId.load(std::memory_order_relaxed);
        d.DeviceFrameIndex = m_RenderDiagDeviceFrameIndex.load(std::memory_order_relaxed);
        d.TargetFrameIndex = m_RenderDiagTargetFrameIndex.load(std::memory_order_relaxed);
        d.UiFrameSlot = m_RenderDiagUiFrameSlot.load(std::memory_order_relaxed);
        d.RgPoolSlot = m_RenderDiagRgPoolSlot.load(std::memory_order_relaxed);
        return d;
    }

    // Asset integration (stubs for now)

    // Asset integration via AssetManager-provided assets
    bool LoadLayoutFromAsset(const UILayoutAsset& asset);
    bool AttachStyleFromAsset(const UIStyleAsset& asset);

    // Asset-driven subtree binding helpers (used by data-driven panels).
    // These bind assets to an element subtree and register hot-reload so
    // changes are applied automatically.
    bool BindLayoutToSubtreeChildrenFromAsset(UIElement* target, const UILayoutAsset& asset);
    bool AttachStyleToSubtreeFromAsset(UIElement* target, const UIStyleAsset& asset);
    // Remove a subtree style previously applied via AttachStyleToSubtreeFromAsset:
    // drops the old style's hot-reload binding AND removes the subtree's attached
    // sheets, in the order a swap needs. Call before attaching a new style so it
    // fully replaces (AttachStyle only adds/dedups, never removes). Owns the swap's
    // "remove old" half so callers don't reimplement the unregister-then-clear order.
    void ClearSubtreeStyle(UIElement* target, const GUID& styleGuid);
    // Status of a subtree style's @import dependencies, for the host's deferred-attach path.
    enum class StyleImportStatus : std::uint8_t
    {
        Resident, // all @imports loaded → AttachStyleToSubtreeFromAsset won't block
        Pending,  // some not loaded yet (this call kicked async loads) → retry next frame
        Failed,   // an @import resolved to a GUID whose load is suppressed (deleted/corrupt)
    };
    // Query a subtree style's @import readiness without blocking: Resident when all are
    // cached, Pending (after kicking async loads) while some load, Failed if any has a
    // permanently-suppressed load. Lets a per-frame caller defer AttachStyleToSubtreeFromAsset
    // until imports are cached (so the attach never blocks the render thread) AND latch on a
    // dead import instead of re-polling + re-kicking the load every frame forever.
    // Mirrors the attach's own top-level @import wait list, so Resident predicts whether the
    // attach BLOCKS — not that the full transitive cascade is resident (a deeply-nested import
    // unresolved at root-parse can still attach a frame late via GetCascadeHandles).
    StyleImportStatus QueryStyleImports(const UIStyleAsset& asset);
    // Resolve an asset-relative CSS path (e.g. "UI/controls/Foldout.css") to a UIStyleAsset
    // and attach it to a subtree root. This does NOT attach globally.
    bool AttachStyleToSubtreeFromAssetPath(UIElement* target, const std::string& assetPath);
    // Resolve an asset-relative layout path (e.g. "UI/controls/FloatingPanel.uxml") to a
    // UILayoutAsset and instantiate its root's classes and children onto `target`, once,
    // with no hot-reload binding. For a control's own chrome: the control re-keys the
    // template ids per instance, so a reconcile pass could not match them, and every
    // instance of the control binds the same asset.
    bool InstantiateLayoutChildrenFromAssetPath(UIElement* target, const std::string& assetPath);

    void LoadLayout(const GUID& /*layoutAssetGuid*/);
    void AttachStyle(const GUID& /*styleAssetGuid*/);

    // Global stylesheet management. Stylesheets added here participate in
    // the same cascade as per-element stylesheets. Re-adding an existing
    // stylesheet moves it to the end of the list so later attachments take
    // precedence.
    void AddStylesheet(const StylesheetHandle& sheet);

    // Hot-reload support: replace a previously-attached block of global stylesheets
    // (identified by pointer identity) with a new block, preserving global ordering
    // for other stylesheets. Used to rebuild a theme's @import cascade when the
    // importer file's import set/order changes.
    void ReplaceGlobalStylesheetBlock(const std::vector<const Stylesheet*>& oldBlock,
                                      const std::vector<StylesheetHandle>& newBlock);

    // Hot-reload support: notify that the rules this manager resolves against
    // changed. Either an asset reload's new snapshots replaced attached ones
    // (a published UIStyleAsset snapshot is never mutated), or a style this
    // manager loaded itself (AttachStyleFromFile) was re-read and rewritten in
    // place. Drops the rule indices built from the previous rules; callers also
    // mark the affected subtree dirty so styles are recomputed.
    void NotifyStylesheetContentChanged();

private:
    // Invalidate every retained element's PersistentSheetSetId and the
    // SheetSetInterner. Required after any change to m_GlobalStylesheets
    // (or anything else that could invalidate previously interned sheet
    // sets) so the next pre-cascade pass re-interns from scratch and the
    // interner doesn't keep entries that reference now-stale sheet
    // pointer lists. Called by ReplaceGlobalStylesheetBlock,
    // AttachStyleFromAsset, AddStylesheet, and the async file-stylesheet
    // attach paths — historically only ReplaceGlobalStylesheetBlock did
    // this, leaving the others to rely on the per-frame full rebuild
    // (now bypassed under primary flip).
    void InvalidateAllInternedSheetSets();

public:
    // Mark a subtree dirty so CSS is recomputed. Intended for hot reload and
    // host-driven stylesheet changes. Marks Style/Layout/Visual dirty on all
    // descendants (best-effort; conservative).
    void MarkStyleDirtySubtree(UIElement* subtreeRoot);
    // Convenience: mark the entire UI tree dirty for style recomputation.
    void MarkStyleDirtyAll();
    // Request background texture. Asset loads are deduped centrally in AssetManager; UI dedupes the GPU upload only.
    // Callback is invoked on the UI thread (via deferred action flush) with (handle,width,height). Handle may be null on failure.
    void RequestBackgroundTexture(const GUID& guid,
                                  std::function<void(Rendering::TextureHandle, uint32_t, uint32_t)> onReady);
    // Resolve a CSS background-image path to an asset GUID. When `sourceAlias`
    // is non-empty, the path is looked up under exactly that asset source
    // ("editor", "project", etc.) — matches CSS `url("editor:path")` syntax.
    // When empty, falls back to implicit source priority (project first).
    GUID ResolveBackgroundImagePath(const std::string& path,
                                    const std::string& sourceAlias = {});
    void EnsureBackgroundTextureUploaded(const GUID& guid);
    // Resolve `path` (+ optional `sourceAlias`, e.g. "editor") like CSS background-image,
    // ensure GPU upload when possible, and register the cached texture into `reg`.
    // Returns UITextureRegistry index 0 if not cached yet (async load) or on failure.
    // Optional outputs are image pixel dimensions when the cache entry exists.
    // Pass a sampler handle to override the default (e.g. GetNearestClampSampler() for pixel-art).
    uint32_t TryRegisterResolvedBackgroundTexture(const std::string& path,
                                                  const std::string& sourceAlias,
                                                  UI::UITextureRegistry& reg,
                                                  uint32_t* outWidth = nullptr,
                                                  uint32_t* outHeight = nullptr,
                                                  Rendering::SamplerHandle sampler = {});
    // Returns the nearest-neighbor (point) clamp sampler used for pixel-art / no-filter rendering.
    Rendering::SamplerHandle GetNearestClampSampler() const { return m_SamplerNearestClamp; }
    // Returns the GPU texture handle and pixel dimensions for a background image path if it is
    // already in the cache (does NOT trigger a load). Returns an invalid handle if not cached.
    Rendering::TextureHandle TryGetBackgroundTextureHandle(const std::string& path,
                                                           uint32_t* outWidth = nullptr,
                                                           uint32_t* outHeight = nullptr) const;
    // As above, resolved by GUID. Triggers an upload if not yet cached (so editor
    // controls — e.g. the 9-slice inspector — can draw a texture they reference by
    // GUID). Returns an invalid handle until the upload completes.
    Rendering::TextureHandle TryGetBackgroundTextureHandleByGuid(const GUID& guid,
                                                                 uint32_t* outWidth = nullptr,
                                                                 uint32_t* outHeight = nullptr);
    // Evict a cached background texture so the next render re-loads it from disk.
    // Call this before re-applying the same path when the file on disk has changed.
    void EvictBackgroundTexture(const std::string& path);
    // Evict by GUID (used by the AssetReloaded hot-reload listener).
    void EvictBackgroundTexture(const GUID& guid);
    // Once per rendered frame: age the background-texture cache and, when it
    // exceeds its byte budget, evict the least-recently-painted entries.
    void EvictStaleBackgroundTextures();

    // Test-only inspection of the background texture cache. Returns true when
    // the cache currently holds a valid handle for `guid`. Used by hot-reload
    // tests to verify that AssetReloaded events evicted the cached entry.
    bool HasBackgroundTextureCachedForTesting(const GUID& guid) const;
    // Test-only seed of the background texture cache. Skips disk load + GPU
    // upload so hot-reload tests can assert eviction without booting a real
    // texture pipeline. The dummy handle is stored as 'sharedOwned' so the
    // destructor / eviction path does not try to destroy it.
    void SeedBackgroundTextureForTesting(const GUID& guid,
                                         Rendering::TextureHandle handle,
                                         uint32_t width = 1,
                                         uint32_t height = 1);
    // Test-only seed of the font GUID -> atlas key map without going through
    // the resolver. Used by C2 tests to assert that a Font reload triggers
    // eviction of the matching atlas entry.
    void SeedFontGuidForTesting(const GUID& fontGuid, const std::string& atlasKey);
    // Test-only inspection of the font atlas registry.
    bool HasFontAtlasKeyForTesting(const std::string& atlasKey) const;
    // Test-only peek at an element's persistent primitive slot range. Returns
    // nullptr when the element owns no range or `index` is out of bounds.
    // Used by drain regression tests to assert baked primitive state.
    const UI::UIPrimitive* PeekPrimitiveForTesting(const UIElement& el, uint16_t index) const;
    // Test-only peek at a persistent clip slot. Returns nullptr when the slot
    // is out of bounds. Used by text-overflow clip regression tests.
    const UI::UIClipRect* PeekClipRectForTesting(uint16_t slot) const;
    // Test-only: position of `el`'s first persistent primitive slot in the
    // frame's draw order, or -1 when the element emitted nothing. Later
    // positions draw on top. Used by overlay-ordering regression tests.
    int FindDrawOrderPosForTesting(const UIElement& el) const;
    // Test-only read of what the whole-stylesheet analysis concluded a
    // `:focus-visible` flip can disturb. Production reads this pair alone in
    // exactly one place -- the modality-flip branch in UIManager::DispatchEvents
    // (UIManager_HoverAndEvents.cpp) -- and every other focus consumer unions
    // it with the :focus aggregate, which hides the two answers from any
    // behavioural probe against a sheet that also carries :focus rules.
    bool FocusVisibleAffectsLayoutForTesting() const
    {
        return m_StyleAnalysis.FocusVisible.AffectsLayout;
    }
    bool FocusVisibleAffectsPaintForTesting() const
    {
        return m_StyleAnalysis.FocusVisible.AffectsPaint;
    }
    // Live persistent clip-slot usage (used = currently allocated, total =
    // allocator high-water). Clip slots share a 16-bit index space with the
    // clip SSBO; exposed so soaks and the debug server can track pressure.
    size_t GetClipSlotsUsed() const { return m_ClipAllocator.GetUsedSlots(); }
    size_t GetClipSlotsTotal() const { return m_ClipAllocator.GetTotalSlots(); }

    // Data-driven helpers (file-based)
    bool LoadLayoutFromFile(const std::string& path);
    bool AttachStyleFromFile(const std::string& path);
    // Asynchronous variants (parse off-thread; apply on UI thread when ready).
    void LoadLayoutFromFileAsync(const std::string& path);
    void AttachStyleFromFileAsync(const std::string& path);
    void SetRoot(std::unique_ptr<UIElement> root);

    // External texture registry for engine-provided textures (e.g., scene view)
    // Optional width/height enable correct background-size (cover/contain) for
    // non-square textures; 0 = unknown, falls back to element size.
    //
    // Every registration states the source space of the pixels it binds. There
    // is no default: a space stated by writing nothing is exactly how a call
    // site expresses a wrong colour, and it is invisible to any sweep keyed on
    // the type name. The space is cross-checked against the texture's format at
    // registration time (UI::CheckTextureSpaceAgainstFormat).

    // Direct device texture overload -- bypasses the render graph entirely.
    // Use for static/asset textures (e.g., color picker) or persistent textures
    // that are no longer written by the RG (e.g., ready thumbnails).
    // The format for the cross-check comes from a live device query.
    void SetExternalTexture(const std::string& name, Rendering::TextureHandle handle,
                            Rendering::SamplerHandle sampler,
                            uint32_t width, uint32_t height,
                            UI::UITextureSpace space);
    void SetExternalTexture(const std::string& name, Rendering::TextureHandle handle,
                            uint32_t width, uint32_t height,
                            UI::UITextureSpace space);
    // RenderGraph arm (slice 8b), two-phase by design. REGISTRATION is persistent —
    // it makes `name` resolvable for primitive generation (size/space for
    // background-size math) and routes the binding through the per-frame
    // publish map. PUBLICATION is per-frame: the host publishes the CURRENT
    // frame's RGTexture after declaring its producers; RenderRG consumes a
    // publish only when its (frame, frameIndex) stamp matches the frame being
    // declared — frame-local ids are never stored across frames.
    //
    // No device handle exists at registration, so the caller passes the format
    // its import declared (or the RG resource desc's format) — the same value
    // the texture it will publish carries.
    void SetExternalTextureRG(const std::string& name, uint32_t width, uint32_t height,
                               UI::UITextureSpace space, Rendering::TextureFormat format);
    void PublishExternalTextureRG(const std::string& name, Rendering::RenderGraph::RGFrame& frame,
                                   Rendering::RenderGraph::RGTexture tex);
    void RemoveExternalTexture(const std::string& name);
    void ClearElementBackgroundTexture(UIElement& el);

    // On-demand external-texture requests. Primitive generation records each
    // engine texture name it cannot resolve this frame; the producer (e.g. the
    // thumbnail service) drains the set to render the missing textures. Consume
    // returns the pending names and clears the set.
    void NoteUnresolvedExternalTextureRequest(const std::string& name);
    std::vector<std::string> ConsumeUnresolvedExternalTextureRequests();

    // A producer that temporarily deferred misses can ask visible primitives
    // to resolve their external images again without rebuilding layout.
    void RequestExternalTextureRefresh() { MarkPrimitivesNeedRegen(0x200u); }

    // ---------------------------------------------------------------------
    // Font loading (host-resolved, async-capable)
    //
    // UI supports CSS `font-family`, but the UI module does not own asset or OS
    // font discovery. Hosts (Editor/tools/games) may register a resolver that
    // maps a requested family name to font bytes (TTF/OTF/etc.), potentially
    // asynchronously.
    //
    // The resolver may invoke the completion callback from any thread. UIManager
    // will marshal completion onto the UI thread via its dispatcher.
    struct FontResolveResult
    {
        std::vector<std::uint8_t> Bytes; // font file bytes (TTF/OTF/etc.)
        int FaceIndex = 0;              // reserved for collections (.ttc); currently unused by FontAtlas
        std::string DebugName;          // optional diagnostics (path, OS family, etc.)
        // Optional asset GUID of the resolved font file. When non-null, UIManager
        // records the (font GUID -> atlas key) mapping so AssetReloaded events
        // for AssetType::Font can evict the matching FontAtlas instance.
        // Resolvers that cannot identify a GUID (e.g. OS-system-font fallbacks)
        // should leave this null.
        GUID AssetGuid = GUID::Null();
    };
    using FontResolveCallback = std::function<void(FontResolveResult)>;
    using FontResolverFn =
        std::function<void(const std::string& family,
                           int weight,
                           FontStyle style,
                           FontVariant variant,
                           FontResolveCallback onReady)>;

    // Install/replace the font resolver. Passing an empty function disables host font loading.
    void SetFontResolver(FontResolverFn&& resolver);

    // Install a synchronous default face used while host-resolved family fonts
    // are still loading. Hosts should provide their primary UI face here to keep
    // first-frame text from falling back to an arbitrary resolved family atlas.
    bool SetDefaultFontBytes(const std::vector<std::uint8_t>& bytes, GUID assetGuid = GUID::Null());

    // Optional prefetch hook: request that a family be loaded for future frames.
    // This is safe to call before the first Update() to avoid initial layout shifts.
    void RequestFontFamily(const std::string& family);

    // No-op under RenderGraph (the old RGTextureRef render-target override is gone);
    // retained because the editor host still calls it on HDR/surface changes.
    void ClearRenderTargetOverride() {}

    // Root access (single root for now)
    UIElement* GetRootElement() const;

    // CSS confirmation:
    // `background-image: url("path/to/asset");` will resolve via AssetManager::ResolveAssetPath(...)
    // and will auto-register the asset if the file exists but is not yet in the registry.
    // The asset must load as a TextureAsset supported by the UI background upload path.

  #if defined(_DEBUG)
    // Debug-only helper: export the current UI tree and resolved styles into a
    // simple XML/HTML-compatible representation for external tools (e.g., Figma).
    // The implementation walks the current root element hierarchy and writes a
    // layout file with inline CSS-style properties derived from ResolvedStyle
    // plus per-element layout rects. This is compiled out in non-debug builds.
    bool DebugExportLayoutAndStyles(const std::string& outDirectory) const;
  #endif

    void ToggleDebugCapture();
    bool IsDebugCaptureEnabled() const { return m_DebugCaptureEnabled; }
    // Input hooks (demo)
    void OnMouseMove(float x, float y);
    // For callers that mutate the tree under a stationary cursor (row removed,
    // rebuilt): re-run the hover hit-test next Update as if the mouse moved.
    // Hover otherwise only re-evaluates on actual mouse motion, so a mutation
    // that lands after the last move leaves stale hover until the next one.
    void RequestHoverRefresh()
    {
        if (IsMousePositionKnown())
            m_MouseMoved = true;
    }
    // Dispatch a button transition immediately, at the OS callback, and report
    // whether a control acted on it. The press targets the element under the
    // current pointer position on the last completed layout, with an active
    // capture outranking it; the answer is what routes the event onward (a
    // consumed press reaches neither the app InputSystem nor a runtime sink).
    bool OnMouseButton(int button, bool down);
    // Terminate an in-flight press whose release will never arrive, because the pointer
    // stream itself ended (the Game View viewport the host reads pointer from was left).
    // Not a release: the armed control is disarmed and capture is dropped, but nothing
    // activates.
    void CancelPress();
    // Called when the OS reports the cursor entering/leaving this window.
    // This helps keep hover state correct even if the cursor leaves the window
    // without generating further cursor-pos callbacks.
    void OnCursorEnter(bool entered);
    bool OnScroll(float xoffset, float yoffset);

    // Keyboard/text input
    // Returns true if the UI intends to consume this character/key (e.g., focused text field or UI hotkey).
    bool OnChar(unsigned int codepoint);
    bool OnKey(int key, int action, int mods);

    // Focus API
    const std::string& GetFocusedElementId() const { return m_FocusId; }
    void ClearFocus();
    // Programmatically set focus to the element with the given id. Made while a key event is
    // dispatched, the move counts as keyboard focus (IsFocusViaKeyboard, :focus-visible).
    void SetFocusById(const std::string& id);
    /// Ensure `el` has an id, then focus it (for controls created without an explicit `SetId`).
    void FocusElement(UIElement* el);
    // Diagnostics: identify the current hovered element. Returns `id` if present, else `tag#instanceId`,
    // else empty string.
    std::string GetHoveredElementDebugName() const;
    // Return the currently hovered element (or null). Use for tooling (e.g. CSS inspector overlay).
    UIElement* GetHoveredElement() const;
    // Return selector string for an element (tag#id.class1.class2). Used by tooling (e.g. CSS inspector).
    std::string GetElementSelector(const UIElement* el) const;
    // Clear hover/ capture state. Useful when large portions of the UI tree are rebuilt
    // or controls destroy and recreate their children (e.g., GridView on folder change).
    void ClearHover();
    // Clear hover/capture state if the current hovered element lies within the
    // specified subtree. Intended to be called from UIElement::RemoveChild.
    void ClearHoverForSubtree(UIElement* subtreeRoot);

  public:
    // UI Dispatcher accessors
    UI::IUiDispatcher* GetDispatcher() { return m_Dispatcher.get(); }
    // For work that posts back from another thread: holding the dispatcher by shared ownership
    // keeps a post made after this manager is destroyed safe, since ~UIManager closes it and a
    // closed dispatcher refuses the post. Work it accepts runs only from this manager's drains.
    // Element post routes (UI::UiPostTarget) hold it the same way.
    friend std::shared_ptr<UI::UiDispatcher> UIManagerSharedDispatcher(const UIManager& owner)
    {
        return owner.m_Dispatcher;
    }
    void PostToUI(std::function<void()> fn)
    {
        if (m_Dispatcher)
            m_Dispatcher->Post(std::move(fn));
    }
    // Scheduler accessors
    Scheduler::IScheduler* GetScheduler() { return m_Scheduler.get(); }

    // Low-frequency, layout-independent refresh tick. A callback registered
    // here fires roughly every kPeriodicRefreshIntervalSeconds from Update,
    // BEFORE the idle gate, so a panel that polls external state (ECS stats,
    // VCS status, file mtimes) still updates while the UI is otherwise idle —
    // the OnPostLayout path only runs on heavy passes and starves an
    // input-idle / IPC-only editor. The callback must be cheap and idempotent
    // (mutate the tree only when its data actually changed) so an unchanged
    // tick still idles. Returns a non-zero token for UnregisterPeriodicRefresh.
    // Prefer holding a PeriodicRefreshHandle over calling these directly.
    uint64_t RegisterPeriodicRefresh(std::function<void()> callback);
    void UnregisterPeriodicRefresh(uint64_t token);

  public:
    // Whether this manager's diagnostic F4-F12 hotkeys are live. The environment
    // opts a developer in, but only the host knows whether its keyboard is the
    // editor's own — a manager driven by a game surface must decline these keys
    // whatever the environment says, or the game silently loses F4-F12.
    void SetDebugKeysEnabled(bool enabled) { m_DebugKeysEnabled = enabled; }

    // Whether a key with nothing focused is offered to the element under the
    // cursor. The editor's chrome wants it — that fallback is what keeps a
    // viewport shortcut firing while an unrelated control holds focus. A game
    // surface must not: on a HUD the cursor rests wherever the player left it,
    // and a control it merely hovers would take keys the game is waiting for.
    void SetKeyHoverFallbackEnabled(bool enabled) { m_KeyHoverFallbackEnabled = enabled; }

    // Whether a wheel with nothing under the pointer is offered to the focused
    // element, and to the first focusable one when nothing is focused. The
    // editor's chrome wants it — that fallback is what keeps the wheel working
    // for a panel driven from the keyboard. A game surface must not: a HUD lets
    // the pointer through to the world it floats over, so nothing is hovered for
    // most of the screen and every tick there would be handed to a HUD list
    // instead of to the game.
    void SetScrollFocusFallbackEnabled(bool enabled) { m_ScrollFocusFallbackEnabled = enabled; }

    // Debug controls / profiling accessors (useful for tests and perf investigation)
    void SetTextDebugOverlayEnabled(bool enabled) { m_TextDebugOverlayEnabled = enabled; }
    bool IsTextDebugOverlayEnabled() const { return m_TextDebugOverlayEnabled; }
    void RequestTextDebugDump() { m_TextDebugDumpRequested = true; }

    // Correctness mode:
    // Forces the heavy pass (Yoga build/solve + full geometry rebuild) and disables fast paths.
    // Intended for debugging invalidation/caching issues in scroll/virtualized views and for CI tests.
    void SetCorrectnessModeEnabled(bool enabled) { m_CorrectnessModeEnabled = enabled; }
    bool IsCorrectnessModeEnabled() const { return m_CorrectnessModeEnabled; }
    void SetCorrectnessModeAssertsEnabled(bool enabled) { m_CorrectnessModeAssertsEnabled = enabled; }
    bool IsCorrectnessModeAssertsEnabled() const { return m_CorrectnessModeAssertsEnabled; }

    // Update profiling (CPU-side). Populated when update profiling is enabled.
    struct UpdateProfileFrame
    {
        uint32_t FrameIndex = 0;
        uint32_t ElementCount = 0;
        double TotalMs = 0.0;
        double SchedulerMs = 0.0;
        double BuildYogaMs = 0.0;
        // BuildYogaMs split for the MT-4 (parallel cascade) go/no-go gate:
        // style-cascade compute vs Yoga style application, build-pass sites
        // only (the converge re-cascade is timed by PostLayoutRetryMs).
        // MT-4 becomes worth doing when CascadeComputeMs sustains >~4ms
        // while CascadeShared/CascadeCalls stays under ~0.30.
        double CascadeComputeMs = 0.0;
        double YogaApplyMs = 0.0;
        double YogaMs = 0.0;
        double PostLayoutMs = 0.0;
        double HitTestMs = 0.0;
        double EventDispatchMs = 0.0;
        double GeometryMs = 0.0;
        double CleanupMs = 0.0;
        // TransitionEngine::Advance wall time (was untimed — folded into the
        // TotalMs tail). Slice D's registry conversion is measured here.
        double TransitionMs = 0.0;
        // 1 when the mode-0 idle gate serviced this frame (no input, nothing
        // pending — Update skipped straight to the tooltip/cursor tail).
        uint32_t IdleFrame = 0;
        // IdleGateDecline value for this frame (0 = Taken). Lets idle-churn
        // regressions be attributed from profile history without relying on
        // the budgeted GE_UI_IDLE_LOG diagnostic.
        uint32_t IdleDecline = 0;
        // Block E (E0/E1) instrumentation. Populated by GenerateAllPrimitives
        // once per frame so per-scenario captures see the per-frame
        // primitive-gen cost (otherwise hidden inside the render path).
        // genAllPrimitivesMs: wall-clock spent in GenerateAllPrimitives.
        // genAllPrimitivesSkipped: 1 if E0 took the early-return path, else 0.
        double GenAllPrimitivesMs       = 0.0;
        uint32_t GenAllPrimitivesSkipped = 0;
        // MT-3 parallel drain shape: how many work items the drain processed,
        // how many emit tasks it forked (1 = sequential), and how many items
        // hit an off-thread tripwire and re-emitted on the UI thread.
        uint32_t DrainItems         = 0;
        uint32_t DrainParallelTasks = 0;
        uint32_t DrainEscalated     = 0;
        // Bitmask of which trigger(s) flipped m_PrimitivesNeedRegen this
        // frame. Lets us see why sweep scenarios force full regen.
        //   1  = MarkDirty (Style/Layout/ChildrenDirty)
        //   2  = treeRebuiltThisFrame (AddChild/Remove/AddElement)
        //   4  = forceGlobalStyle
        //   8  = viewportChanged
        //   16 = relayoutRequestedPre
        //   32 = retired (scroll translation is drain-only)
        //   64 = HasActiveTransitions
        //   128 = writeBack rectChanged with cap == 0 (newly visible)
        //   256 = retired (override-rect patches are drain-only)
        //   1024 = drain bail (invariant break or culled element became visible)
        //   2048 = content mark on an element that owns no primitive range
        //          (never emitted, or emitted nothing and its range was freed)
        uint32_t GenAllPrimitivesRegenCause = 0;
        uint32_t LateRelayoutCount = 0;
        // Time spent specifically in the late-relayout's BuildYoga + Yoga
        // solve pass. The main `buildYogaMs` / `yogaMs` accumulators include
        // both initial and late passes; these expose the late pass alone so
        // we can attribute spike cost to virtualization-driven re-solves.
        double LateRelayoutBuildYogaMs = 0.0;
        double LateRelayoutYogaMs = 0.0;
        // Bitmask of what triggered the late relayout(s) this frame.
        // 1 = lateChildrenDirty (ChildrenDirty set after initial solve)
        // 2 = lateLayoutDirty   (LayoutDirty set after initial solve)
        // 4 = lateStyleDirty    (StyleDirty set after initial solve)
        // 8 = relayoutRequestedLate (RequestRelayout() called after initial solve)
        // 16 = virtualizationLayoutAffectingLate (TopologyChanged | LayoutRectsChanged)
        // OR-folded across all late passes in the frame so diagnosing a
        // spike frame tells you every trigger that contributed.
        uint32_t LateRelayoutReasons = 0;
        uint32_t LayoutOverridePatchCount = 0;
        // Slice 2 subtree-skip fast path instrumentation. Useful for verifying
        // the fast path actually fires and diagnosing why it doesn't.
        uint32_t SubtreeFastPathHits          = 0;  // subtrees served from cache
        uint32_t SubtreeSlowPathWalks         = 0;  // subtrees taking the full slow path
        // Why each slow-path walk happened. The fast-path gate is a long AND
        // chain — when an element falls to slow path, multiple conditions may
        // be true simultaneously, so each bit is OR'd in separately.
        // Counters in the same order let us see which clause dominates.
        //   1   = isFreshNode (no retained yoga node yet)
        //   2   = forceGlobalStyle / computeLayoutSignaturesAll
        //   4   = ChildrenDirty on parent
        //   8   = SubtreeDirty bit
        //   16  = Overrides::NeedsCascadeRerun
        //   32  = Overrides::IsLayoutDirty
        //   64  = Overrides::IsVisualDirty
        //   128 = DisplayMode == None (skip-skip; expected)
        //   256 = !CachedSubtreeValid
        //   512 = LocalSheetsHash mismatch
        //   1024 = ParentSheetsHash mismatch
        //   2048 = StylesheetContentGen mismatch
        uint32_t SubtreeSlowPathReasonUnion   = 0;
        uint32_t SubtreeSlowPathReasonCounts[12] = {};
        // ResolveCascadeForElement call instrumentation. Target of memoization work.
        uint32_t CascadeCalls                 = 0;  // total cascade invocations per frame
        uint32_t CascadeShared                = 0;  // cascades served by sibling style sharing (P4)
        uint32_t CascadeCallsBuildYoga        = 0;  // calls from BuildYogaRecursive slow path
        uint32_t CascadeCallsConverge         = 0;  // calls from ConvergePostLayout re-resolve

        // Split timers for ConvergePostLayout's three phases. Sum to the total
        // postLayoutMs. Used to tell whether a mutation-frame's post-layout
        // cost is in the OnPostLayout dispatch, the retry rebuild, or the
        // StyleDirty re-solve (applyDirtyStylesToYoga).
        double OnPostLayoutDispatchMs = 0.0;  // iterating ctx.nodes and calling OnPostLayout()
        double PostLayoutRetryMs      = 0.0;  // ctx.nodes.clear() + BuildYoga + Yoga solve on retry
        uint32_t PostLayoutRetryCount = 0;    // how many times the retry rebuild fired per frame

        // Broken-out timers for the blocks that were previously lumped into
        // the implicit "untracked" gap (totalMs − sum of other phases).
        // Populated when UI profiling is enabled.
        double PreSolveDrainMs        = 0.0;  // FlushScrollCallbacks + drainVirtualizationQueueBounded
        double IndicesAndClipsMs      = 0.0;  // ApplyScrollTransforms + Clamp + RebuildNodeIndices/ClipCaches/ZIndex
        double SolveAndApplyMs        = 0.0;  // SolveAndApplyLayout (including Yoga + commit)
        double FocusOrderMs           = 0.0;  // BuildFocusOrder + post-rebuild focus refresh

        // Top-N elements that received a layout-affecting dirty flag this
        // frame (only populated when GE_UI_DIRTY_TRACE=1). Used to answer
        // "what's dirtying the tree every frame?" without guessing — the
        // top offenders are what drives SubtreeDirty propagation and the
        // corresponding slow-path walks.
        struct DirtySourceEntry
        {
            uint64_t InstanceId = 0;
            std::string Id;        // el->GetId() — may be empty
            std::string ClassName; // first class or "" if none
            uint32_t Count = 0;    // times MarkDirty(layout-affecting) fired
            unsigned FlagsSeen = 0; // OR-fold of the flags observed
        };
        std::vector<DirtySourceEntry> DirtySources;
    };

    void SetUpdateProfilingEnabled(bool enabled) { m_UpdateProfilingEnabled = enabled; }
    bool IsUpdateProfilingEnabled() const { return m_UpdateProfilingEnabled; }
    void RequestUpdateProfilingDump() { m_UpdateProfilingDumpRequested = true; }
    double GetLastUpdateYogaMs() const
    {
        return m_UpdateProfilingHistory.empty() ? 0.0 : m_UpdateProfilingHistory.back().YogaMs;
    }
    double GetLastUpdateBuildYogaMs() const
    {
        return m_UpdateProfilingHistory.empty() ? 0.0 : m_UpdateProfilingHistory.back().BuildYogaMs;
    }
    double GetLastUpdateGeometryMs() const
    {
        return m_UpdateProfilingHistory.empty() ? 0.0 : m_UpdateProfilingHistory.back().GeometryMs;
    }
    double GetLastUpdateTotalMs() const
    {
        return m_UpdateProfilingHistory.empty() ? 0.0 : m_UpdateProfilingHistory.back().TotalMs;
    }
    uint32_t GetLastSubtreeFastPathHits() const
    {
        return m_UpdateProfilingHistory.empty() ? 0u : m_UpdateProfilingHistory.back().SubtreeFastPathHits;
    }
    uint32_t GetLastSubtreeSlowPathWalks() const
    {
        return m_UpdateProfilingHistory.empty() ? 0u : m_UpdateProfilingHistory.back().SubtreeSlowPathWalks;
    }

    // Returns false if no profiling frame has been recorded yet (profiling disabled or Update never called).
    bool GetLastUpdateProfileFrame(UpdateProfileFrame& out) const
    {
        if (m_UpdateProfilingHistory.empty())
            return false;
        out = m_UpdateProfilingHistory.back();
        return true;
    }

    // Full history access for offline analysis. The ring buffer is capped at
    // 120 frames (see Update()); older frames are evicted front-first so
    // [0] is the oldest, back() is the most recent.
    const std::vector<UpdateProfileFrame>& GetUpdateProfilingHistory() const
    {
        return m_UpdateProfilingHistory;
    }

    // Peak frame in the history ring, selected by totalMs. Returns false if
    // empty. Useful for catching mutation-frame spikes from a polling client
    // that may miss the exact frame via back()-only access.
    bool GetPeakUpdateProfileFrame(UpdateProfileFrame& out) const
    {
        if (m_UpdateProfilingHistory.empty())
            return false;
        const UpdateProfileFrame* best = &m_UpdateProfilingHistory.front();
        for (const auto& f : m_UpdateProfilingHistory)
        {
            if (f.TotalMs > best->TotalMs)
                best = &f;
        }
        out = *best;
        return true;
    }

private:
    // Block E1 helper. Adjust m_DrawOrderIdx on every element whose
    // draw-order slice starts at-or-after `threshold` by `delta`.
    // Walks the tree (children + Mount targets). Used by drain when a
    // count change shifts m_DrawOrder entries downstream of an element.
    void ShiftDrawOrderIndicesAfter(uint32_t threshold, int32_t delta);

    // Block E (E0/E1) helper. Out-of-line so it can call UIElement methods
    // (UIElement is forward-declared in this header).
    //
    // Routes the mutation by flag scope:
    //   * Layout-affecting flags (StyleDirty / LayoutDirty / ChildrenDirty)
    //     → set m_PrimitivesNeedRegen = full DFS regen needed next frame.
    //   * Visual-only (VisualDirty alone) → DON'T set regen; element is
    //     already in m_PrimitiveDataDirty (pushed by NotifyElementDirty
    //     above). DrainPrimitiveDataDirty will refresh just that element's
    //     primitive bytes — no full DFS, no Yoga, no DrawOrder shift.
    //
    // In all cases, elements the emit walk does not enter (EmitWalkEntersSubtree)
    // skip the flag set. A zero layout rect is not that test on its own: an
    // unclipped empty box still descends into children that paint.
    void NotifyDirty_UpdateRegenFlag(UIElement* el, unsigned flags);

    // Block E1 instrumentation. Sets m_PrimitivesNeedRegen and OR-folds
    // `causeBit` into the current frame's regen-cause bitmask so the
    // perf scenarios can attribute "why didn't this frame skip" to a
    // specific trigger. See UpdateProfileFrame::genAllPrimitivesRegenCause.
    void MarkPrimitivesNeedRegen(uint32_t causeBit);
    // The content-only pipeline escalates rangeless elements straight to a
    // full regen (see the definition in UIManager.cpp), which needs the
    // private mark above.
    friend void UIManagerNotifyElementContentDirty(UIManager* owner, UIElement* el);
public:

    // Internal hint: UIElements call into this when they become dirty so Update()
    // can fast-path when nothing changed (retained mode).
    void NotifyElementDirty(UIElement* el, unsigned flags)
    {
        // C-6: the hint flags below are atomic, but the queue push and
        // regen-cause bookkeeping this routes to are not. Off-thread
        // callers must go through the UiDispatcher.
        AssertUiThread();
        m_DirtyHintFlags.fetch_or(flags, std::memory_order_relaxed);
        // release pairs with ResolveStyles' acquire exchange so a marking
        // thread's style-field writes are visible to the resolve walk that
        // consumes the flag (matters on weakly-ordered targets).
        m_ResolvedStylesDirty.store(true, std::memory_order_release);
        // Block E (E0/E1): route the mutation by flag scope. Layout-affecting
        // flags trigger a full regen on next render; visual-only mutations
        // (already pushed onto m_PrimitiveDataDirty below) are handled
        // surgically by DrainPrimitiveDataDirty without touching unrelated
        // elements.
        NotifyDirty_UpdateRegenFlag(el, flags);
        if (m_DirtyTraceEnabled && el)
        {
            // Matches UIElement::{StyleDirty|LayoutDirty|VisualDirty|
            // ChildrenDirty}. Hardcoded so this inline body stays valid under
            // UIElement forward-decl (full definition not included in this
            // header). VisualDirty is traced too: visual-only churn keeps the
            // idle gate declined and the primitive drain re-emitting, so
            // "who dirties this every frame" must include it (FlagsSeen
            // distinguishes the flavors).
            constexpr unsigned kTracedMask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
            const unsigned la = flags & kTracedMask;
            if (la != 0)
            {
                auto& entry = m_PerFrameDirtyCount[el];
                entry.first += 1;
                entry.second |= la;
            }
            // GE_UI_DIRTY_TRACE_ID=<element id>: log a backtrace for the first
            // few marks on that element — attributes "who dirties this every
            // frame" without a debugger. Out-of-line; no cost when unset.
            TraceElementDirtySource(el, flags);
        }
        // Mirror visual marks into the primitive-data queue consumed by
        // DrainPrimitiveDataDirty on the render side. Bit value matches
        // UIElement::VisualDirty; hardcoded for forward-decl friendliness.
        // The push helper is a no-op when the element is already enqueued.
        if (el)
        {
            constexpr unsigned kVisualBit = 1u << 2;
            if (flags & kVisualBit)
                PushPrimitiveDataDirty(el);
        }
    }
    // Internal hint: UIElements call into this when their tree structure changes
    // (children added/removed, Mount target swapped). This is used to detect
    // mid-frame mutations without guessing based on "pending action" counts.
    void NotifyTreeStructureChanged()
    {
        m_TreeStructureGeneration.fetch_add(1, std::memory_order_relaxed);
        // New/reparented elements need an inheritance pass even when no
        // element-level dirty mark accompanies the structural change.
        m_ResolvedStylesDirty.store(true, std::memory_order_release);
    }

    // Monotonic generation bumped by every structural tree change (child add/remove,
    // Mount swap). Lets callers gate "re-resolve an element by id" work on whether the
    // tree could have changed since they last looked, instead of walking every time.
    uint64_t GetTreeStructureGeneration() const
    {
        return m_TreeStructureGeneration.load(std::memory_order_relaxed);
    }

    // Backtrace-on-mark diagnostic behind GE_UI_DIRTY_TRACE_ID (see
    // NotifyElementDirty). Out-of-line in UIManager.cpp.
    void TraceElementDirtySource(UIElement* el, unsigned flags);

    // ------------------------------------------------------------------------

    // Internal: called when a UIElement is attached to this manager (SetOwnerManager).
    // Used to auto-attach cached per-control UIStyle assets (Foldout/Accordion/etc).
    void NotifyElementOwnerChanged(UIElement* el);

    // ---- Attach/detach events (UIManager_AttachEvents.cpp) ------------------
    //
    // Queue a subtree root whose root-reachability may have changed, and settle the
    // queue: walk each queued subtree, compare every element's CURRENT reachability
    // against the last state its subscribers were told, and dispatch
    // kEventAttachedToPanel / kEventDetachedFromPanel only where they differ. Attach is
    // pre-order, detach post-order.
    //
    // Called from the mutation sites via the UI::Detail forwarders (UIElement.h and
    // UI/Internal/AttachDetachInternal.h), and once per frame from Update.
    void EnqueueAttachSettle(UIElement* el);
    void ForgetAttachSettle(UIElement* el);
    void SettleAttachTransitions();
    // Teardown only: drop every back-pointer into this manager before it dies (the way
    // ~UIManager already clears the focus-within chain's), and clear the dispatched-state
    // bit on everything it owns, so an element that OUTLIVES it can be announced again by
    // whichever manager adopts it next.
    void ForgetAttachStateOnTeardown();
    // Called by UI::DispatchDestructionDetach while `doomedRoot`'s subtree is condemned but
    // still whole: when the element last announced FocusIn lies in that subtree it hears
    // FocusOut now, synchronously, because next frame it does not exist and its blur-commit
    // handler would never run; and a focus id that resolves into the subtree is cleared, so
    // the manager stops naming a dead element as focused. Membership follows the ownership
    // links (GetParent), the same set the destruction dispatch condemns: a Mount target
    // under a doomed host survives and keeps its focus.
    void ReleaseFocusInDoomedSubtree(const UIElement* doomedRoot);

  private:
    void DispatchSettledSubtree(UIElement* root, bool attached);
    // Manager teardown: detach the still-alive elements this manager announced but does
    // not own, before its registries go away.
    void AnnounceDetachForSurvivingElements();
    // Manager teardown, after the owned tree is destroyed: clear the owner of every element
    // that outlives this manager, so none keeps a pointer to it once its memory is released.
    void DetachSurvivingElements();

  public:
    // The single point at which m_Root is replaced — see the definition. Every tree-install
    // path routes through it so the outgoing tree is always announced.
    void AdoptRoot(std::unique_ptr<UIElement> newRoot);

    // Event-driven UI rewrite, Stage 1: return persistent render slots
    // owned by `el` (primitive range, clip slot, draw-order slice) to
    // this manager's allocators. Called from ~UIElement when the element
    // owns slots. No-op for elements with no slots allocated. Idempotent.
    void FreeRenderSlots(UIElement* el);

  private:
    // Track elements that own local (subtree-attached) stylesheets so their sheets
    // can seed the dynamic style analysis unconditionally each frame (see
    // m_LocalSheetElements). Idempotent set insert/erase. Not public API: maintained
    // only via the teardown-guarded forwarders below (called from UIElement's
    // header-inline SetOwnerManager, which can't include UIManager.h) and directly
    // from InvalidateSheetSetSubtree.
    void RegisterLocalSheetElement(UIElement* el);
    void UnregisterLocalSheetElement(UIElement* el);
    friend void UIManagerRegisterLocalSheetElement(UIManager*, UIElement*);
    friend void UIManagerUnregisterLocalSheetElement(UIManager*, UIElement*);

    // Drop the hot-reload binding for a (target, styleGuid) subtree style. Internal
    // half of ClearSubtreeStyle; not a standalone public op (clearing a style without
    // also removing its sheets is never what a caller wants).
    void UnregisterSubtreeStyleBinding(UIElement* target, const GUID& styleGuid);

    // Internal helpers
    static Rendering::TextureHandle CreateUIBackgroundTextureFromAsset(Rendering::IDevice* device, const TextureAsset* texA);
    // Read a cached background texture's 9-slice metadata (AssetRegistry KV) for the
    // GUID; returns a disabled NineSlice when absent/unparseable. Internal only —
    // the hot path reads the resolved slice off CachedTexture.Slice.
    NineSlice ResolveNineSlice(const GUID& guid);
    std::string EnsureElementId(UIElement* element);

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // Drop retained-mode layout caches that depend on the prior NodeRec
    // generation (interner, gates). Per-element RetainedYogaNode ownership
    // lives on UIElement::m_YogaState now; the YGNodes themselves get
    // destroyed when their owning UIElements destruct (e.g. when m_Root is
    // reassigned).
    void ResetRetainedYogaTree();
#endif
    bool ConsumeRelayoutRequest()
    {
        const bool v = m_RequestRelayout;
        m_RequestRelayout = false;
        return v;
    }

    std::unique_ptr<Scheduler::IScheduler> m_Scheduler;

  private:
    Rendering::IDevice* m_Device = nullptr;
    AssetManager* m_AssetManager = nullptr;
    std::unique_ptr<UIHotReload> m_HotReload;
    std::unique_ptr<TransitionEngine> m_TransitionEngine;

    // Elements whose ResolvedStyle currently declares transitions.
    // TransitionEngine::Advance iterates this instead of walking the tree;
    // erased on owner change / destruction.
    //
    // MT-4.1: the cascade no longer mutates this set inline. Registration is
    // manager-container state, which the compute stage must stay clear of so
    // MT-4.2 can run the cascade on a worker. ResolveCascadeForElement now
    // OUTPUTS a TransitionRegistration decision (computed from the freshly
    // cascaded rs.Transitions + ownership, exactly as before); each caller
    // applies it on the UI thread via ApplyTransitionRegistration — the build
    // path defers it onto the YogaBuildItem (applied in ApplyElementBuild in
    // DFS/item order), the converge/pseudo re-resolve callers apply it inline.
    std::unordered_set<UIElement*> m_TransitioningElements;

    // Apply a deferred TransitionRegistration decision. UI-thread only (mutates
    // m_TransitioningElements). Preserves #190 semantics exactly: an empty
    // transition set always erases (ownership-independent); a non-empty set
    // inserts only when el is owned by this manager.
    void ApplyTransitionRegistration(UIElement* el, TransitionRegistration decision)
    {
        switch (decision)
        {
        case TransitionRegistration::Unregister: m_TransitioningElements.erase(el); break;
        case TransitionRegistration::Register:   m_TransitioningElements.insert(el); break;
        case TransitionRegistration::None:                                           break;
        }
    }

    // Typed registries: each control registers on owner-gain
    // (OnOwnerManagerChanged) and deregisters on re-home/destruction.
    // Replace the per-frame dynamic_cast walks (RebuildTypedNodeLists) and
    // the per-hover popup-gating walks (GatePointerTargetToOpenPopups).
    // Vectors (not sets) so iteration order is deterministic attach order —
    // the dismissable-popup registry relies on it to resolve "topmost" as the
    // most recently attached open popup.
    std::vector<ScrollView*> m_ScrollViewRegistry;
    std::vector<DismissablePopup*> m_DismissablePopupRegistry;

    // C-8 per-frame provider change pump. Virtualized views register while
    // attached; PumpVirtualizedControlChanges (called once per Update, before
    // the idle/pointer gates) reads each control's ProviderChangeVersion() and,
    // on movement since LastSeenVersion, enqueues its DataChanged coordinator
    // work — waking the idle gate the same frame a MarkChanged lands.
    struct VirtualizedControlPumpEntry
    {
        IVirtualizedControl* Control = nullptr;
        std::uint64_t LastSeenVersion = 0;
    };
    std::vector<VirtualizedControlPumpEntry> m_VirtualizedControlRegistry;

    // instanceId -> element map (slice E1). Insert on owner-gain
    // (NotifyElementOwnerChanged), erase on detach / destruction.
    // FindElementByInstanceId adds a root-reachability check so
    // owned-but-detached elements (inactive dock tabs) stay unfindable,
    // matching the old walk's semantics.
    std::unordered_map<uint64_t, UIElement*> m_ElementsByInstanceId;

    // Attach/detach settle queue: the top of each subtree whose root-reachability may
    // have moved since the last settle (UIManager_AttachEvents.cpp). Raw pointers, kept
    // valid by ~UIElement leaving the queue; a consumed or dead entry is tombstoned to
    // null rather than erased, because the drain indexes into this vector while handlers
    // are running and may append to it.
    std::vector<UIElement*> m_AttachSettleQueue;
    // The subtree currently being dispatched, collected in pre-order BEFORE any handler
    // runs so handler tree edits cannot perturb the traversal. Same tombstone contract as
    // the queue, and a member rather than a local so its capacity persists across frames —
    // after warm-up the settle allocates nothing.
    std::vector<UIElement*> m_AttachSettleWalk;

#ifdef _DEBUG
    // C-6: MarkDirty and the primitive-data queue are UI-thread-only — the
    // hint flags are atomic, but the queue push and regen-cause bookkeeping
    // are not. Captured at the first Update (construction may run on a
    // loader thread), asserted on every dirty notification. Same pattern
    // as UiDispatcher's owner-thread assert.
    std::thread::id m_UiThreadId{};
    bool m_UiThreadCaptured = false;
#endif

  public:
#ifdef _DEBUG
    void AssertUiThread() const
    {
        assert((!m_UiThreadCaptured || std::this_thread::get_id() == m_UiThreadId) &&
               "UI dirty marks must run on the UI thread (C-6)");
    }
#else
    void AssertUiThread() const {}
#endif

    // Called by ~UIElement / SetOwnerManager via forwarder to drop a dying
    // or re-homed element from the declared-transition registry.
    void UnregisterTransitioningElement(UIElement* el) { m_TransitioningElements.erase(el); }

    // Called by ~UIElement / SetOwnerManager via forwarder: drop the id
    // mapping when it still points at the departing element (a reparent to
    // another manager inserts into that manager's own map). Takes the id
    // because UIElement is only forward-declared here.
    void UnregisterElementInstanceId(uint64_t instanceId, UIElement* el)
    {
        auto it = m_ElementsByInstanceId.find(instanceId);
        if (it != m_ElementsByInstanceId.end() && it->second == el)
            m_ElementsByInstanceId.erase(it);
    }

    // Called by the controls' OnOwnerManagerChanged overrides / dtors.
    void RegisterScrollView(ScrollView* sv);
    void UnregisterScrollView(ScrollView* sv);
    void RegisterDismissablePopup(DismissablePopup* popup);
    void UnregisterDismissablePopup(DismissablePopup* popup);

    // Close every open popup that the press at `pressTarget` landed outside
    // of. Returns true when at least one closed, which the caller uses to
    // swallow the press. A press inside any open popup closes nothing.
    // Dismisses every open popup the press landed outside of. Returns whether
    // the caller should CONSUME the press: true only when it landed inside no
    // surviving popup — a press inside a survivor keeps flowing to its target
    // even when it dismissed a sibling layer.
    bool DismissPopupsForOutsidePress(UIElement* pressTarget);
    // Close the topmost (most recently attached) open popup. Returns false
    // when none is open. Drives the Escape fallback, so it runs only after
    // normal bubbling leaves the key unhandled.
    bool DismissTopmostPopup();
    // C-8 pump registry (ListView/GridView/TreeView).
    void RegisterVirtualizedControl(IVirtualizedControl* control);
    void UnregisterVirtualizedControl(IVirtualizedControl* control);

  private:
    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;
    // Shared with the post routes of the elements this manager owns (UI::UiPostTarget), which
    // any thread may read; ~UIManager closes it, so a post that arrives after that is refused
    // instead of queued where nothing will drain it. Never null.
    std::shared_ptr<UI::UiDispatcher> m_Dispatcher = std::make_shared<UI::UiDispatcher>();

    // The lifetime token UIManagerRefs observe: reset after detach notifications and before
    // destroying the owned tree. Owned by this manager alone, so no reference can revive it.
    // UI thread only.
    friend class UIManagerRef;
    std::shared_ptr<const void> m_Lifetime = std::make_shared<bool>(true);

    // Per-window drag/drop manager (UI-level).
    std::unique_ptr<UI::Interaction::DragDropManager> m_DragDrop;

    // Drag/drop overlay widgets (ghost + tooltip).
    std::unique_ptr<UI::Interaction::DragDropOverlay> m_DragDropOverlay;

    // Hover tooltip overlay.
    std::unique_ptr<UI::Interaction::TooltipOverlay> m_TooltipOverlay;
    TooltipConfigProvider m_TooltipConfigProvider;

    // Async hot-reload helpers (best-effort; results are applied on UI thread).
    // False once ~UIManager starts: an async load or parse result still queued for the final
    // drain is dropped rather than applied to a manager that is tearing down. UI thread only;
    // workers never read it, they post through the shared dispatcher, which ~UIManager closes.
    bool m_AppliesAsyncResults = true;
    uint64_t m_AsyncLayoutParseGeneration = 0;
    std::unordered_map<std::string, uint64_t> m_AsyncStyleParseGeneration;

    // Simple demo state
    float m_Time = 0.0f;

    // Low-frequency refresh tick (see RegisterPeriodicRefresh). Callbacks fire
    // from Update before the idle gate at m_NextPeriodicRefreshTime cadence so
    // idle / IPC-only editors still refresh externally-polled panels.
    struct PeriodicRefreshEntry
    {
        uint64_t Token = 0;
        std::function<void()> Callback;
    };
    std::vector<PeriodicRefreshEntry> m_PeriodicRefreshCallbacks;
    uint64_t m_NextPeriodicRefreshToken = 1;
    float m_NextPeriodicRefreshTime = 0.0f;

    float m_TextContrast = 1.0f;
    float m_TextBlendGamma = 0.0f;
    // Default 1.0 is the neutral value; this is the user softness knob, applied
    // after the colour-keyed correction. See SetTextSmoothingGamma.
    float m_TextSmoothingGamma = 1.0f;
    // Opt-in, judged on-display; see SetTextSubpixelAA.
    bool m_TextSubpixelAA = false;
    // Resolved output state of the most recent RenderRG declare (see the
    // getters near ResolveTextSubpixelActive); main-thread only.
    int m_LastResolvedOutputEncoding = 0;
    bool m_LastTextSubpixelActive = false;
    UI::UIScaleSettings m_ScaleSettings;
    float m_ContentScale = 1.0f; // Resolved platform or reference-viewport scale.
    bool m_RequestRelayout = false;
    uint32_t m_RelayoutRequestsSinceLastUpdate = 0;
    uint64_t m_RelayoutLastSourceInstanceId = 0;
    std::atomic<unsigned> m_DirtyHintFlags{0u};
    // ResolveStyles (inheritance + override safety net) is a full-tree walk;
    // this flag lets it run only when something could have changed a resolved
    // style since the last walk: any dirty mark, any structural change, or a
    // heavy Update pass (whose cascade rebuilds styles from scratch).
    // Consumed by ResolveStyles() via exchange(false) — marks raised during
    // the walk re-arm it for the next frame.
    std::atomic<bool> m_ResolvedStylesDirty{true};
    std::atomic<uint64_t> m_TreeStructureGeneration{1u};
    // Snapshot of m_TreeStructureGeneration at the end of the last
    // BuildYogaRecursive pass. Update() compares against
    // m_TreeStructureGeneration.load() to detect "tree changed since
    // last build" without needing a separate StructuralOp drain.
    uint64_t m_LastBuiltStructureGeneration = 0;
    // Generation at which hover/capture instance-id recovery last resolved.
    // Cached pointers can only go stale via a structural change, and every
    // structural change bumps m_TreeStructureGeneration — so recovery (an
    // O(tree) FindElementByInstanceId walk) is skipped while it holds still.
    uint64_t m_HoverRecoveryTreeGen = 0;
    std::atomic<uint32_t> m_VirtualizationImpactFlags{0u};
    VirtualizationCoordinator m_VirtualizationCoordinator;

    // Cached list of ScrollViews (by instanceId) from the last heavy pass.
    // Used to support scroll-only retained updates without re-traversing the full tree.
    std::vector<std::uint64_t> m_CachedScrollViewInstanceIds;
    std::uint64_t m_CachedScrollViewTreeGen = 0;

    // Mouse/input state
    float m_MouseX = -1.0f;
    float m_MouseY = -1.0f;
    bool m_MouseMoved = false;
    bool m_ScrollWheelMoved = false; // set by OnScroll; consumed by Update() for fast-path gating
    bool m_ScrollOffsetsChanged = false; // set by ScrollView; consumed by Update() for fast-path gating
    bool m_MouseDown = false;
    int m_MouseButton = 0; // 0=LMB, 1=RMB, 2=MMB, etc.
    // A button transition (press, release, or CancelPress) happened since the
    // last heavy pass. Dispatch itself is synchronous, so this is not an event
    // queue: it is the per-frame work signal that a transition leaves behind —
    // hover re-evaluation and the :active re-cascade. Sticky rather than derived
    // from a previous-state latch precisely because a press and release that both
    // land inside one frame interval must still raise it exactly once; a latch
    // comparison would see equal endpoints and report no transition at all.
    bool m_ButtonEdgeSinceLastUpdate = false;
    // Focus id as it stood before the button transition currently dispatching,
    // so the transition can re-cascade the elements that gained and lost focus.
    // A member rather than a local because assigning into it reuses its
    // capacity: a local copy would put a heap allocation on every button event
    // once an id outgrows the small-string buffer.
    std::string m_FocusIdBeforeButtonDispatch;
    UIElement* m_Hovered = nullptr;
    // Store instanceId for hover so we can recover safely if the UI tree is rebuilt
    // (avoids dereferencing stale pointers during CSS/style computation).
    uint64_t m_HoveredInstanceId = 0;
    // Early pointer pre-pass bookkeeping (PreProcessPointerState → ProcessHoverChain).
    // When the pre-pass advances m_Hovered before the cascade, the post-solve
    // ProcessHoverChain still needs the true last-frame hover for the enter/leave
    // event diff. Instance ids of the hover's full ancestor chain (leaf first),
    // not pointers: elements can be destroyed between the two passes
    // (virtualization drain), and when the leaf dies the nearest living
    // ancestor is the correct diff base — diffing against null would
    // re-dispatch mouse-enter to ancestors the pointer never left.
    std::vector<uint64_t> m_HoverEventDiffBaseChain;
    uint64_t m_ActivePrePassTargetId = 0;
    bool m_HoverPrePassRan = false;
    bool m_HoverPrePassMarked = false;
    // Set when TryPointerOnlyFrame dispatched the bubbling mouse-move but then
    // escalated to the heavy pass — DispatchEvents skips its own (otherwise
    // unconditional) mouse-move dispatch for that frame.
    bool m_PointerMoveDispatchedThisFrame = false;
    // Cursor callback and tracking
    CursorCallback m_CursorCallback;
    CursorStyle m_LastCursorStyle = CursorStyle::Auto;
    std::string m_FocusId;                            // persistent focus by element id
    std::string m_LastFocusIdNotified;                // to notify BaseField::OnFocusChanged
    // The element last told FocusIn, by instance id, so its FocusOut reaches it even after
    // it has left the tree (an unmounted panel): resolved through m_ElementsByInstanceId,
    // which keeps every live element this manager owns, reachable or not. 0 when no
    // FocusIn is outstanding.
    uint64_t m_FocusNotifiedInstanceId = 0;
    int m_NextAutoElementId = 1;                      // per-manager auto id counter (multi-window safe)

    bool m_FocusViaKeyboard = false; // set when focus arrived via Tab/Shift+Tab or during a key dispatch
    // True while OnKey dispatches a key event to the focus chain (SetFocusById reads it): a key
    // handler's focus move, and a post it drains during that dispatch, count as keyboard focus;
    // work drained later in Update does not.
    bool m_DispatchingKey = false;
    // Set by SetFocusById during a mouse-down event dispatch; read by the post-dispatch
    // focus assignment to defer to handler-driven focus changes (e.g. ListView calling
    // FocusElement(this) via PostSafeAction). Reset at the start of the mouse-down block.
    bool m_FocusExplicitSetDuringDispatch = false;

    // Modifier state reconciled from platform masks and physical modifier-key
    // events, and the source of UIEvent::Mods for every pointer event.
    UI::ModifierKeyState m_Modifiers;

    // Text values by element id (for inputs), and tab focus order
    std::unordered_map<std::string, std::string> m_TextValues;
    std::vector<std::string> m_FocusOrder;

    // Persistent font atlas (loaded once and synced on demand).
    // NOTE: We use shared_ptr so family-specific lookups can alias the default atlas when they
    // resolve to the same font bytes. This prevents multiple FontAtlas instances with the same
    // atlasId from diverging in content and thrashing the renderer cache.
    std::shared_ptr<Rendering::Text::FontAtlas> m_FontAtlas;
    // Set once Update has tried to load the default atlas from the staged engine fonts (the
    // path taken when no host font resolver is installed); the attempt is never repeated.
    bool m_DefaultFontAtlasAttempted = false;
    // Optional additional atlases by (family,weight,style,variant) key for CSS typography.
    // Values may alias m_FontAtlas.
    std::unordered_map<std::string, std::shared_ptr<Rendering::Text::FontAtlas>> m_FontAtlases;
    // Alias map used when multiple (family,weight,style,variant) requests resolve to identical font bytes.
    // This avoids creating multiple FontAtlas instances with the same underlying font bytes which would
    // otherwise share the same FontAtlas::atlasId and cause renderer cache collisions (manifesting as
    // intermittent "gibberish" glyphs when different weights are used).
    std::unordered_map<std::string, Rendering::Text::FontAtlas*> m_FontAtlasAliases;

    // Per-frame memoization of ResolveFontForVisualStyle. Cleared at the start
    // of GenerateAllPrimitives. Linear-scanned; typical frame has <10 unique
    // (family, weight, style, variant) tuples, making it faster than a hashmap.
    struct FontResolveCacheEntry {
        const void* FamilyPtr = nullptr;
        int Weight = 0;
        std::uint8_t Style = 0;
        std::uint8_t Variant = 0;
        Rendering::Text::FontAtlas* Atlas = nullptr;
    };
    std::vector<FontResolveCacheEntry> m_FrameFontResolveCache;

    // Host-provided font resolver + request state (per family/variant key)
    FontResolverFn m_FontResolver;
    uint64_t m_FontResolverGeneration = 1; // incremented when resolver is replaced to ignore stale completions
    std::unordered_set<std::string> m_FontFamilyInFlight; // keys currently being resolved
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_FontFamilyRetryAfter; // negative cache / backoff
    std::unordered_map<std::string, uint32_t> m_FontFamilyFailCount; // used to compute backoff

    // --- Unified SDF renderer ---
    // SDF pipeline and descriptor layouts
    std::vector<uint8_t> m_UiSdfVertShader;
    std::vector<uint8_t> m_UiSdfFragShader;
    // Subpixel RGB AA twins (same sources compiled with UI_SUBPIXEL_DUAL_SRC).
    // Empty when the staged build lacks the variant — the gate then resolves
    // subpixel off and the grayscale pipeline keeps rendering.
    std::vector<uint8_t> m_UiSdfVertShaderSubpixel;
    std::vector<uint8_t> m_UiSdfFragShaderSubpixel;
    // Encoded-blend twins (same fragment sources compiled with
    // UI_BLEND_SPACE_ENCODED; the vertex stages are shared — the define only
    // reshapes fragment colour handling). Selected by an EncodedSrgb target
    // declaration. Empty when unstaged: an encoded declaration then renders
    // clear-only frames — never the linear pipeline into an encoded target.
    std::vector<uint8_t> m_UiSdfFragShaderEncoded;
    std::vector<uint8_t> m_UiSdfFragShaderSubpixelEncoded;
    bool m_SdfPipelineReady = false;
    bool m_SdfSubpixelVariantAvailable = false;
    bool m_SdfEncodedVariantAvailable = false;
    bool m_SdfSubpixelEncodedVariantAvailable = false;
    Rendering::PipelineDesc m_SdfPipelineDesc;
    Rendering::PipelineDesc m_SdfPipelineDescSubpixel;
    Rendering::PipelineDesc m_SdfPipelineDescEncoded;
    Rendering::PipelineDesc m_SdfPipelineDescSubpixelEncoded;
    Rendering::GraphicsPipelineId m_SdfPipelineId{};
    Rendering::GraphicsPipelineId m_SdfPipelineIdSubpixel{};
    Rendering::GraphicsPipelineId m_SdfPipelineIdEncoded{};
    Rendering::GraphicsPipelineId m_SdfPipelineIdSubpixelEncoded{};
    Rendering::DescriptorSetLayoutDesc m_SdfSsboSetLayout;  // set 0: primitive + clip SSBOs

    // SDF push constants (minimal — clips are in SSBO)
    struct SdfPushConstants
    {
        float TargetSize[2];
        float TimeSeconds;
        // Strength of the colour-keyed coverage boost (Skia SK_GAMMA_CONTRAST).
        // See SetTextContrast.
        float TextContrast = 1.0f;
        // TARGET blend space for coverage: 0 = linear, 1.0 = display-encoded.
        // From SetTextBlendGamma via ResolveTextCoverageConstants; which arm
        // interprets it is TextBlendSpaceEncoded below.
        float TextBlendGamma = 0.0f;
        // User edge softness, applied AFTER the colour-keyed correction.
        // Brightness-neutral remap centered on 0.5. 1.0 = neutral, > 1.0 softens.
        float EdgeSoftness = 1.0f;
        // Target-space encoding: 0 = SDR linear, 1 = SDR encoded bytes
        // (EncodedSrgb — the shader variant owns the semantics; the value is
        // diagnostic there), >= 2 = HDR (2 PQ, 3 HLG, 4 scRGB). On linear
        // targets the UI shader outputs LINEAR and the terminal encode owns
        // the OETF; on the encoded target the output bytes already carry the
        // curve and the terminal runs the Finalize contract instead.
        int OutputEncoding = 0;
        // UI white TARGET in nits (the configured UI paper-white). Normalized by
        // DeviceWhiteNits below so UI chrome renders at this absolute nit level,
        // independent of the scene's paper-white.
        float PaperWhiteNits = 120.0f;
        // UI black-floor LIFT in nits, [0,48], 0 = faithful (no subtraction).
        float BlackLiftNits = 0.0f;
        // Device paper-white in nits — the presentation intermediate's 1.0 anchor.
        // UI white is normalized by this so PaperWhiteNits is an absolute target,
        // not a value that cancels itself out.
        float DeviceWhiteNits = 120.0f;
        // Caret blink phase-toggle rate in toggles/second — the reciprocal of
        // the OS caret blink half-period, via UI::CaretPhaseTogglesPerSecond.
        // 0 = never blink, and renders the caret solid.
        float CaretPhaseTogglesPerSecond = 0.0f;
        // 1 when the attachment interpolates raw sRGB-encoded bytes — selects
        // the shader's Skia-direction text retarget arm (see
        // Shaders/UI/text_mask_gamma.glsl). From TextCoverageConstants;
        // exactly UITargetSpace::EncodedSrgb resolves 1.
        int TextBlendSpaceEncoded = 0;
        // First draw-order index of the run this draw covers. Zero on every
        // full-profile draw, which issues one run over the whole order; the
        // compat shader is the only stage that declares it (see ui_sdf.vert for
        // why firstInstance cannot carry it).
        uint32_t DrawBase = 0;
        // Mirrors the compat block's trailing pad, which ends it on a multiple
        // of its largest alignment (vec2: 8) as an MSL struct does.
        uint32_t DrawBasePad = 0;
    };

    // SDF per-frame SSBO buffers (auto-growing)
    std::unique_ptr<UI::FrameBufferRing<UI::UIPrimitive>> m_SdfPrimRing;
    std::unique_ptr<UI::FrameBufferRing<UI::UIClipRect>> m_SdfClipRing;
    // Stage 2 part 3: ring buffer for the DrawOrder SSBO. Size mirrors
    // m_DrawOrder; each entry is a uint32_t slot index into m_PersistentPrimitives.
    std::unique_ptr<UI::FrameBufferRing<uint32_t>> m_SdfDrawOrderRing;

    // SDF texture registry (font atlas pages, background images -> bindless indices)
    std::unique_ptr<UI::UITextureRegistry> m_SdfTextureRegistry;

    // Monotonic per-store versions of the CPU-side persistent stores. Bumped
    // by GenerateAllPrimitives for exactly the store(s) it mutated (full
    // regen bumps all three; a drain bumps only what it touched — a text
    // re-emit bumps primitives without forcing the draw-order and clip
    // buffers to re-upload); the E0 skip path bumps nothing.
    // PrepareSdfFrameData keys each ring's UploadIfChanged on its own store
    // version, and the primitive ring additionally consumes dirty-range
    // marks (MarkRangeDirty at the drain write sites) so a drain frame
    // uploads the touched slot span instead of the whole store.
    uint64_t m_PrimitiveStoreVersion = 1;
    uint64_t m_ClipStoreVersion = 1;
    uint64_t m_DrawOrderStoreVersion = 1;

    // Compat profile only: the per-texture partition of the draw order, and the
    // ring version that keys its upload. The partitioned words depend on BOTH
    // stores — the order decides the spans, the primitives decide which texture
    // each entry names — so the gate watches the pair and bumps a version of
    // its own, which is what the draw-order ring keys on under compat.
    UI::UICompatDrawRunBuilder m_SdfCompatRuns;
    uint64_t m_SdfCompatDrawOrderSource = 0;
    uint64_t m_SdfCompatPrimitiveSource = 0;
    uint64_t m_SdfCompatWordsVersion = 1;
    bool m_SdfCompatProfile = false;

    void EnsureSdfPipelineDesc();
    void InitSdfRenderer();

    // Create the three SSBO ring buffers. Shared by InitSdfRenderer and the
    // post-rebuild heal.
    void CreateSdfRingBuffers();

    // Create the UI-owned samplers and the 1x1 white fallback texture on the
    // current device generation. Shared by the constructor and the heal.
    void CreateDeviceSamplersAndFallbackTexture();

    // Drop every UIManager-owned GPU handle minted on a dead device generation
    // and re-provision, once per rebuild. An in-place rebuild destroys the
    // VkObjects while keeping the IDevice* alive, and a stale handle still
    // reports IsValid(), so without this these keep being fed to the texture
    // registry — and through it into descriptor writes — for the rest of the
    // session. UITextureRegistry heals its own handles the same way.
    void HealDeviceResourcesIfRebuilt();

    // Device generation the UIManager-owned GPU handles belong to.
    uint64_t m_DeviceRebuildGeneration = 0;
    // Graph-agnostic per-frame SDF prep used by RenderRG: lazy init, pipeline
    // desc, style resolve, primitive gen, ring uploads.
    struct SdfFrameData
    {
        bool Ready = false;     // device + root + pipeline desc available
        bool UploadsOk = false; // rings uploaded for this frame (false when DrawCount==0 or a ring failed)
        size_t DrawCount = 0;
        size_t TotalSlots = 0;
        uint32_t TargetW = 0;
        uint32_t TargetH = 0;
    };
    SdfFrameData PrepareSdfFrameData();

    // Debug capture (extracted from Update to reduce file size). Reads
    // m_DebugClipCache directly (populated by RebuildClipCaches) for
    // per-element clip rects; tree traversal walks m_Root + Mount portals.
    void EmitDebugCapture();

    // P4 sibling style sharing: per-build-pass donor snapshots (defined in
    // UIManager_Internal.h; lives behind a pointer so this header only
    // needs the forward declaration). Active only for the duration of a
    // root BuildYogaRecursive pass — ConvergePostLayout re-resolves because
    // inherited CONTENT changed, which the share key cannot see, so sharing
    // is disabled outside the build pass.
    struct CascadeShareCache;
    // The hover-ancestor set lives on this cache (behind the unique_ptr, so it
    // does NOT affect UIManager's member layout — issue #215 keeps the fix
    // ODR-safe: a stale dependent TU compiled against the old header still sees
    // an identical UIManager layout). Do NOT add the hover set as a direct
    // UIManager member — that shifts every member offset and corrupts stale
    // dependents (it deadlocked GameUIHostTests when the Engine lib's
    // GameUIHost.obj was not recompiled).
    std::unique_ptr<CascadeShareCache> m_CascadeShareCache;

    // MT-4.1: the share cache + its active flag threaded explicitly through
    // the cascade compute stage instead of read from manager members. The
    // sequential path binds one context to m_CascadeShareCache for the root
    // build pass; the converge/pseudo re-resolve callers pass nullptr (share
    // inactive). MT-4.2 hands each subtree worker its own donor cache (Entries)
    // but they all share the one Hover pointer — sharing is sibling-scoped, so a
    // per-task cache loses nothing.
    struct CascadeShareContext
    {
        CascadeShareCache* Cache = nullptr;  // per-context donor snapshots (Entries)
        // Hover-ancestor set for the rightmost `:hover` share-key bit. Points at
        // the root cache's HoverChain (m_CascadeShareCache->HoverChain), read-only
        // for the pass; chunks share this pointer instead of copying the set (#215).
        // nullptr disables the hover-chain bit (converge/pseudo re-resolve callers).
        const std::unordered_set<const UIElement*>* Hover = nullptr;
        bool Active = false;
    };

    // Style resolution helpers (shared by BuildYogaRecursive and ConvergePostLayout).
    // Returns true when the cascade was served from the sibling-share cache
    // (P4) instead of a full ComputeStyleInto — callers use it for the
    // CascadeShared profile counter. shareCtx is the sibling-share context
    // (nullptr / Active==false disables sharing). outTransition receives the
    // deferred declared-transition registry decision the caller must apply.
    bool ResolveCascadeForElement(UIElement* el,
        std::span<const Stylesheet* const> sheets,
        std::span<const StylesheetRuleIndex* const> indices,
        const ResolvedStyle* parentStyle,
        CascadeShareContext* shareCtx,
        TransitionRegistration& outTransition);
    void FinalizeElementStyle(UIElement* el);

    // Re-bake one element's FULL cascade in-frame (st.Hover, st.Active, ...) after a
    // hover-target change or mouse-button edge, so :hover/:active paint this frame
    // instead of one transition late. Used by post-cascade callers only (the
    // ProcessHoverChain patch path): on frames where PreProcessPointerState ran,
    // the main cascade already resolved pseudo state with current pointer input,
    // and this only patches elements the solve moved under the cursor afterwards.
    // See its definition for the layout-input guard.
    void ReResolvePseudoStateElement(UIElement* el);

    // The element a mouse press currently targets (for :active): the capture element
    // while a drag is captured, otherwise the hovered element. Callers add the
    // m_MouseDown gate as needed. Single source of the "active/press target" rule.
    UIElement* ActiveTarget() const { return m_MouseCaptured ? m_CaptureElement : m_Hovered; }

    // Yoga tree building (extracted from Update). BuildYogaRecursive is the
    // recursive orchestrator; MT-4.0 split its per-element body into three
    // explicit stages sharing a YogaBuildItem:
    //   1. CollectYogaInputs  — UI thread: ensure the Yoga node exists, resolve
    //      + intern the sheet set, compute sheet hashes. Returns false only on
    //      node-create OOM. May mutate ctx flat tables + the interner/rule-index
    //      caches (all UI-thread state MT-4.2 pre-warms so workers stay read-only).
    //   2. ComputeElementCascade — element-local cascade (ResolveCascadeForElement
    //      + FinalizeElementStyle). No YG* mutation and no manager-container
    //      mutation (MT-4.1): the P4d share cache is now per-context state
    //      threaded via CascadeShareContext, and prof counters / the transition
    //      registry decision are recorded onto the YogaBuildItem. Guarded by the
    //      seam assert.
    //   3. ApplyElementBuild — UI thread: merges the compute stage's deferred
    //      effects (prof counters + declared-transition registry), replays the
    //      Yoga mutations (measure bind, ApplyStyle, mark-dirty, child insert,
    //      display:none detach), and recurses into children. Returns BuiltNode.
    // shareCtx threads the sibling-share context down the recursion; the root
    // entry (parentStyle == nullptr) binds it to m_CascadeShareCache.
    ::BuiltNode BuildYogaRecursive(UIElement* el, const ResolvedStyle* parentStyle,
                                   size_t childIndex, int32_t parentSheetSetIdx,
                                   ::UpdateContext& ctx, CascadeShareContext* shareCtx);
    bool CollectYogaInputs(UIElement* el, int32_t parentSheetSetIdx,
                           ::UpdateContext& ctx, ::YogaBuildItem& item);
    void ComputeElementCascade(UIElement* el, const ResolvedStyle* parentStyle,
                               ::UpdateContext& ctx, ::YogaBuildItem& item,
                               CascadeShareContext* shareCtx, bool forceCascade = false);
    ::BuiltNode ApplyElementBuild(UIElement* el, const ResolvedStyle* parentStyle,
                                  ::UpdateContext& ctx, const ::YogaBuildItem& item,
                                  CascadeShareContext* shareCtx);
    // Subtree-skip gate factored out of BuildYogaRecursive. Returns true when
    // el's subtree is verifiably clean (early-out with outBuilt); false to
    // proceed to compute+apply (side effects: prof reason counters +
    // ctx.subtreeStructureChanges bump, identical to the inline gate).
    bool EvaluateSubtreeSkipGate(UIElement* el, const ::YogaBuildItem& item,
                                 ::UpdateContext& ctx, ::BuiltNode& outBuilt);
    // Element-state + sibling-share-key computation factored out of
    // ResolveCascadeForElement. Single source of truth for the share key so the
    // donor's registered key matches every sharee's lookup key (P4d sibling
    // sharing through the shared cascade cache).
    UIParsing::ElementState BuildCascadeElementState(UIElement* el) const;
    bool ComputeCascadeShareKey(UIElement* el,
        std::span<const StylesheetRuleIndex* const> indices,
        const UIParsing::ElementState& st,
        const std::unordered_set<const UIElement*>* hover,
        uint64_t& outKey, uint16_t& outStateBits, uint32_t& outSheetSetId) const;
    const StylesheetRuleIndex* GetOrBuildRuleIndex(const Stylesheet* s);
    void ApplyElementStyleOverrides(UIElement* el, ResolvedStyle& cs, bool allowLiveTextureRegistration);
    static void ResetFlatSheetTable(::UpdateContext& ctx, UIManager& mgr);
    static void AddSheetForAnalysis(const Stylesheet* s, ::UpdateContext& ctx);

    // Event dispatch (extracted from Update). Returns whether a handler marked
    // the event handled — the consumption answer callers route on. Also accepts
    // any capture a handler requested, which is what arms drag gestures.
    bool DispatchBubbleEvent(UIElement* target, EventId id, int button,
                             bool bubble, bool& treeMutated, uint64_t treeGenBefore);

    // Walk `target` and its ancestors with `ev`, stopping at the first handler
    // that marks it Handled. The chain is snapshotted by instanceId because a
    // handler may rebuild the tree mid-dispatch. Returns ev.Handled.
    bool BubbleEventFrom(UIElement* target, UIEvent& ev);

    // Deliver one keyboard or text event and report whether anything consumed
    // it. The focused element's chain answers first; with hovered fallback
    // enabled, a chain that declines hands the event to the hovered element's
    // chain (how a viewport shortcut still fires while an unrelated control
    // holds focus). Runs the deferred actions handlers posted, so callers do
    // not need to drain the scheduler themselves.
    bool DispatchInputEvent(UIEvent& ev, bool allowHoveredFallback);

    // The two halves of a button gesture, dispatched from OnMouseButton at the
    // OS callback. Both resolve their target the same way — an active capture
    // outranks a fresh hit test at the current pointer position — and both run
    // the focus bookkeeping the transition implies. Return whether a control
    // acted.
    bool DispatchMouseButtonPress();
    bool DispatchMouseButtonRelease();
    // Target for a button transition: the capture element while a capture is
    // held, otherwise whatever the pointer is over on the last completed layout.
    UIElement* ResolveButtonTarget() const;
    // What the pointer is over right now, against the last completed layout:
    // the resolved hover while it is still current for this position, a fresh
    // hit test once motion or a tree change has invalidated it.
    UIElement* PointerTargetForDispatch() const;
    // A synchronous transition changes :active (and often :focus) outside the
    // frame's own pseudo-state diff, which compares against a snapshot taken
    // during Update and would therefore see no change. Re-cascade the elements
    // the transition touched so the styles land on the next frame.
    void MarkPseudoStateForSyncTransition(uint64_t activeBeforeInstanceId,
                                          const std::string& focusIdBefore);

    // Clip / z-index / scroll helpers (extracted from Update).
    void RebuildTypedNodeLists(::UpdateContext& ctx);
    bool FlushScrollCallbacks(::UpdateContext& ctx);

    // The single Yoga->element rect commit (slice E2). Incremental mode
    // routes changed rects to the primitive drain; blunt mode is for
    // post-rebuild/converge callers whose paths trigger a full regen.
    void CommitLayoutRects(UIElement* root, bool incremental);

    // Stage 7 step 6 sub 1: rebuild the per-element effective clip rects
    // consumed by the debug-capture overlay (UIManager_Debug.cpp's
    // getClipFor). Walks the live tree (m_Root + children + Mount portal
    // targets), threading an `overflow:hidden` clip stack down the
    // descent. Output goes into m_DebugClipCache; absence from the map
    // means "no clip applies to this element".
    //
    // Caller must already have gated on m_DebugCaptureEnabled — this
    // function is debug-only and does no work when no consumer exists,
    // but the gate keeps the call site explicit so the work isn't paid
    // for in release frames.
    void RebuildClipCaches();

    // Returns the topmost UIElement under (mx, my): pre-order DFS from
    // m_Root with on-the-fly clip + z-index. Mount targets are walked
    // alongside the host's children so portal subtrees stay hittable.
    // Tiebreak on equal z favors the later DFS visit (later-painted wins).
    //
    // requirePointerEvents=false relaxes the gate so the post-relayout
    // rehover path can reattach to elements that have pointer-events
    // disabled (a known quirk callers depend on); leave it true for
    // every interactive query.
    UIElement* HitTestTree(float mx, float my, bool requirePointerEvents = true) const;
    // Translates committed layout rects for a subtree (following Mount
    // targets). markForDrain additionally queues every node the full
    // primitive DFS would visit for E1 drain re-emission at the new rects
    // (EmitWalkEntersSubtree).
    void TranslateLayoutSubtree(UIElement* root, float dx, float dy, bool markForDrain = false);
    // Mark-only variant of the walk above (Mount-aware, same gates) for
    // patches that resize without translating.
    void MarkSubtreeForDrainReEmit(UIElement* root);

    // --- Emit-path gates, single-sourced -----------------------------------
    // GeneratePrimitivesForElement gates SELF-EMISSION and SUBTREE DESCENT on
    // different predicates, and several other walks have to agree with it:
    // the drain's collect phase pre-warms only what will self-emit, and the
    // mark walks above queue only what the DFS visits. Restating either gate
    // is how they drift apart, so they live here and everyone asks.
    // Defined in UIManager_PrimitiveGen.cpp next to the DFS itself.

    // Whether an element confines its children to its own rect, so an empty
    // rect leaves the whole subtree with nowhere to be seen. This is about
    // CLIPPING only; it says nothing about the element's own paint.
    static bool EmitClipsChildrenToOwnRect(const UIElement& el, const ResolvedStyle& style);
    // Whether everything the element contributes — its children AND its own
    // paint — stays inside its rect, so a rect that misses the ambient clip
    // proves the element is invisible. Strictly stronger than the above: a
    // shadow or glow spills past the rect and defeats that proof. Only the
    // clip-miss cull needs this; a zero-area box paints neither.
    static bool EmitContributionBoundedByOwnRect(const UIElement& el, const ResolvedStyle& style);
    // Whether the full DFS descends into this element's children. A zero-area
    // box paints nothing of its own but still descends: its descendants keep
    // their own geometry. Conservative relative to the DFS in one direction
    // only — the DFS also culls a subtree whose rect misses the ambient clip,
    // which needs a clip stack this predicate does not have.
    static bool EmitWalkEntersSubtree(const UIElement& el);
    // Whether the full DFS emits primitives for the element itself.
    static bool EmitWalkPaintsElement(const UIElement& el);
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    void ConvergePostLayout(YGNode*& rootNode, ::UpdateContext& ctx);
#endif
    bool ApplyScrollTransforms(::UpdateContext& ctx);
    // How pseudo-state transition marking applies styles. MarkOnly raises dirty
    // flags for the frame's upcoming cascade (pre-cascade callers). ResolveInPlace
    // additionally re-bakes each marked element's cascade immediately, unbounded
    // (post-cascade callers on heavy frames — the cascade already ran, and
    // TransitionEngine::Advance still runs later this frame). ResolveInPlaceGated
    // is the pointer-only gate's variant: re-bakes are capped (overflow marks
    // StyleDirty, which the gate escalates to a same-frame heavy pass) and
    // transition-declaring elements always take the batch path — on a fully
    // gated frame Advance never runs, so an in-place re-bake would snap the
    // final value instead of starting the transition.
    enum class PseudoMarkMode { MarkOnly, ResolveInPlace, ResolveInPlaceGated };
    // Returns false when the transition can't affect any style (analysis
    // early-out) or prevHover == newHover; true when the marking walk ran.
    bool MarkHoverTransitionDirty(UIElement* prevHover, UIElement* newHover, PseudoMarkMode mode);
    // Pointer-only frame gate: a bare mouse move over an otherwise-clean tree
    // is fully serviced without the heavy pass — hit test, hover update,
    // in-place pseudo-state re-bake (VisualDirty rides the render-side drain),
    // enter/leave + mouse-move dispatch. Handled → caller updates tooltip/
    // cursor and returns. Escalated → pointer state is already resolved (and
    // events possibly dispatched — ProcessHoverChain/DispatchEvents dedup via
    // the pre-pass bookkeeping); caller continues into the heavy pass.
    // NotApplicable → frame didn't qualify; nothing was touched.
    enum class PointerFrameResult { NotApplicable, Handled, Escalated };
    PointerFrameResult TryPointerOnlyFrame(unsigned dirtyNow, bool interactive, bool envForcesHeavy,
                                           bool profEnabled, UpdateProfileFrame& prof);
    // Why the last TryPointerOnlyFrame call declined (or took) the frame —
    // surfaced by the GE_UI_HEAVY_INPUT_LOG diagnostic so gate misses are
    // attributable without a debugger.
    enum class PointerGateDecline : uint8_t
    {
        Taken, NotPointerFrame, DirtyTree, RelayoutOrRebuild, Virtualization,
        TreeGeneration, StylesheetGeneration, Transitions, DragDrop,
        FocusNotify, FontResolve, EscalatedMark, EscalatedDispatch
    };
    PointerGateDecline m_PointerGateDecline = PointerGateDecline::NotPointerFrame;
    // Why the last TryIdleFrame call declined (or took) the frame — surfaced
    // by the GE_UI_IDLE_LOG diagnostic so "the editor is idle but frames stay
    // heavy" is attributable without a debugger.
    enum class IdleGateDecline : uint8_t
    {
        Taken, Disabled, Viewport, Input, DirtyTree, RelayoutOrRebuild,
        Virtualization, TreeGeneration, StylesheetGeneration, Transitions,
        DragDrop, FocusNotify, FontResolve
    };
    IdleGateDecline m_IdleGateDecline = IdleGateDecline::Disabled;
    // Mode-0 idle gate: true when a frame has no input and no pending work of
    // any kind — the caller skips straight to the tooltip/cursor tail. Pure
    // predicate; performs no work and consumes no state. Host-agnostic: passive
    // (non-interactive) frames qualify identically, since a passive host feeds
    // no input and every decline condition (dirty tree, transitions, font
    // resolve, viewport, virtualization, generations) is interactivity-neutral.
    bool TryIdleFrame(unsigned dirtyNow, bool viewportOrScaleChanged, bool debugCaptureActive);
    // C-8 per-frame provider change pump. Runs once per Update, before the idle/
    // pointer gates: probes each registered virtualized control's provider
    // version and, on movement, enqueues its DataChanged coordinator work so
    // HasPending() declines the idle gate that same frame. O(registered
    // controls), one u64 compare each, zero allocations on the no-change path.
    void PumpVirtualizedControlChanges();
    // Low-frequency refresh tick. Fires registered periodic-refresh callbacks
    // at most every kPeriodicRefreshIntervalSeconds. Called before the idle
    // gate so a callback's tree mutation folds into the frame's dirtyNow and
    // the heavy pass renders it the same frame; an unchanged tick mutates
    // nothing and the frame still idles.
    void PumpPeriodicRefresh();
    // Early pointer pre-pass for heavy frames: updates m_Hovered and marks
    // hover/:active transitions BEFORE the main cascade, so the frame's single
    // style resolution sees current pointer state (and layout-affecting pseudo
    // styles solve this frame instead of next). Returns true when any dirty
    // marks were raised — the caller must fold m_DirtyHintFlags back into its
    // dirtyNow snapshot.
    bool PreProcessPointerState(bool runInteractive);
    void ProcessHoverChain(::UpdateContext& ctx);
    void ApplyLayoutOverrideRects(::UpdateContext& ctx, bool allOverrides);
    void FinalizeSolve(::UpdateContext& ctx);
    // Clamp scroll offsets on all ScrollViews after layout convergence has
    // settled. Called separately from OnPostLayout so intermediate convergence
    // passes (which can produce transient layout rects) never destroy scroll.
    void ClampAllScrollOffsets(::UpdateContext& ctx);
    void DispatchEvents(::UpdateContext& ctx);
    // Tooltip overlay + cursor-style tail, shared by the heavy pass and the
    // pointer-only frame gate's early return.
    void UpdateTooltipAndCursor(bool interactive);
    // Finalize + push the per-frame update profile (TotalMs, dirty-source
    // top-N, history cap). Shared by the heavy tail and the gate's early
    // return so pointer-only frames still appear in profiling history.
    void PublishUpdateProfile(UpdateProfileFrame& prof, bool profEnabled,
                              std::chrono::high_resolution_clock::time_point totalStart);
    void BuildFocusOrder(::UpdateContext& ctx);
    void NotifyFocusChange();
    UIElement* FocusNotifiedElement() const;
    // Drives NotifyFocusChange, which Update reaches only with a device.
    friend struct UIManagerFocusAccess;
    void SolveAndApplyLayout(YGNode*& rootNode, ::UpdateContext& ctx, bool needSolve);

    // SDF primitive generation (Phase 4)
    struct PrimitiveGenContext;
    // MT-3: one entry per drained element — collected on the UI thread,
    // emitted into per-task scratch (possibly on JobSystem workers), applied
    // strictly in queue order on the UI thread.
    struct DrainItem;
    void GenerateAllPrimitives();
    // Block E1: drain the m_PrimitiveDataDirty queue by re-emitting only
    // the listed elements' primitives into their existing slot ranges.
    // Used when no layout/structural change occurred since last full DFS;
    // avoids the full tree walk for visual-only mutations (text, color).
    // Returns true if drain succeeded; false if it had to bail to full
    // regen (e.g., element's primitive count changed unexpectedly).
    // MT-3: structured as collect (UI thread, order-dependent bookkeeping +
    // cache pre-warm) → emit (parallel across JobSystem workers when the
    // drain is large enough) → apply (UI thread, strict queue order).
    bool DrainPrimitiveDataDirty();
    void GeneratePrimitivesForElement(UIElement* el, PrimitiveGenContext& ctx, float parentAbsX, float parentAbsY);
    // Generate one deferred overlay subtree: overlays escape their ancestors'
    // clip and opacity, so the subtree runs against a fresh viewport clip at
    // full opacity.
    void GenerateOverlayPrimitives(UIElement* el, PrimitiveGenContext& ctx, float parentAbsX, float parentAbsY);
    // Emit the hover tooltip's arrow into its dedicated persistent slot. The
    // arrow has no UIElement; GenerateAllPrimitives calls this between the
    // lower overlay layers and the HoverTooltip layer so the arrow draws
    // above dropdowns/modals/drag previews but behind the tooltip body.
    void EmitTooltipArrowSlot(PrimitiveGenContext& ctx);
    // Drain support: physical-px rect test against the nearest ancestor-owned
    // persistent clip slot (viewport when none). Used to decide whether an
    // element the last full DFS skipped has become visible and needs a full
    // regen to re-establish its slots.
    bool ElementRectIntersectsNearestClip(const UIElement* el) const;
    // Drain support: whether a control that renders its own text, and owned
    // no primitive range at its last full DFS, would emit anything now. Emits
    // into the drain context's scratch tail and discards the result.
    bool OwnTextEmissionPaints(UIElement* el, PrimitiveGenContext& ctx);

    // Stage 2: after an element's self-emission has pushed `count` primitives
    // into ctx.primitives starting at scratchStart, finalize them by allocating
    // (or reusing) a persistent slot range in m_PersistentPrimitives via
    // m_PrimitiveAllocator, copying the primitives into that range, pushing
    // the slot indices into m_DrawOrder in DFS pre-order, and popping the
    // scratch entries off ctx.primitives. Frees any prior slot range when
    // count == 0 (visited element produced zero primitives this frame).
    void FinalizeElementSlots(UIElement* el, PrimitiveGenContext& ctx, size_t scratchStart);
    // warmOnly: fill the per-element shape cache + register the font's Slug
    // pages, then return without emitting. Used by the drain's collect phase
    // so worker-side emission runs entirely from warm caches.
    void EmitTextPrimitives(UIElement* el, PrimitiveGenContext& ctx, const ResolvedStyle& style, float x, float y, float w, float h, std::string_view text, bool warmOnly = false);
    // Settles the input's internal horizontal scroll for the frame. The glyph
    // run, the selection highlight and the caret all read m_TextScrollX and
    // must read the same value, so this is the only place on the emit path
    // that chooses it, and it runs before any of this element's primitives are
    // emitted. Not the only writer in the engine: TextField::BuildPointerGeometry
    // clamps the stored value against the current text on the pointer path.
    void RefreshTextInputScroll(TextInput* input, PrimitiveGenContext& ctx, const ResolvedStyle& style, float x, float y, float w, float h, bool isFocused);

    // A text input's overlay primitives, built together because both index the
    // same caret map and share one vertical band. They are emitted apart: CSS
    // paints a selection highlight as a background, so it goes UNDER the
    // glyphs, and the caret goes over them. Emission order is composite order,
    // so the caller pushes Selection, emits the glyph run, then pushes Caret.
    struct TextInputOverlays
    {
        UI::UIPrimitive Selection{};
        UI::UIPrimitive Caret{};
        bool HasSelection = false;
        bool HasCaret = false;
    };
    // Requires RefreshTextInputScroll to have run for this element this frame.
    TextInputOverlays BuildTextInputOverlays(TextInput* input, PrimitiveGenContext& ctx, const ResolvedStyle& style, float x, float y, float w, float h, bool isFocused);
    // `borderWidth` is the element's LOGICAL-px border inset: the background
    // image's positioning area is the padding box (background-origin), while
    // x/y/w/h stay the border box it is clipped to (background-clip).
    void EmitBackgroundImagePrimitive(UIElement* el, PrimitiveGenContext& ctx, const VisualStyle& vs, const Box4& borderWidth, float x, float y, float w, float h, uint16_t clipIdx);
    struct ResolvedBgSlot;
    ResolvedBgSlot ResolveBackgroundTextureSlot(UIElement* el, PrimitiveGenContext& ctx, const BackgroundImageStyle& bgImg, float elW, float elH);
    Rendering::Text::FontAtlas* ResolveFontForVisualStyle(const VisualStyle& vs, PrimitiveGenContext& ctx);
    std::vector<UI::UIPrimitive> m_SdfPrimitiveBuffer;
    std::vector<UI::UIClipRect> m_SdfClipBuffer;

    // Deferred RG texture bindings: populated during GenerateAllPrimitives,
    // consumed during RenderRG. Maps descriptor slot index to the external
    // texture name that needs resolving during the render pass.
    struct DeferredRGBinding
    {
        uint32_t Slot = 0;
        UIElement* Owner = nullptr; // element that reserved this slot (for per-element release on incremental re-emit)
        // RenderGraph arm: resolve through the per-frame publish map at RenderRG
        // declare time. NEVER an RenderGraph id — bindings outlive frames, RenderGraph ids
        // do not.
        std::string ExternalName;
    };
    std::vector<DeferredRGBinding> m_SdfDeferredRGBindings;




    // Last layout size used when building Yoga/layout and geometry so the
    // Render pass can match view size (swapchain may not have changed).
    uint32_t m_LastLayoutWidth = 0;
    uint32_t m_LastLayoutHeight = 0;
    // Host layout-extent override (0 = use device swapchain size). See SetLayoutSizeOverride.
    uint32_t m_LayoutSizeOverrideW = 0;
    uint32_t m_LayoutSizeOverrideH = 0;
    float m_HdrUiPaperWhiteNits = 0.0f;
    float m_HdrUiBlackLiftNits = -1.0f; // < 0 = auto (display black floor)

    // Data-driven state
    std::unique_ptr<UIElement> m_Root;
    std::vector<StylesheetHandle> m_GlobalStylesheets;
    // Stable in-memory stylesheet objects for file-based AttachStyleFromFile hot reload.
    // Key is a normalized path string.
    std::unordered_map<std::string, std::shared_ptr<Stylesheet>> m_FileStylesheets;

    // UI background sampling resources and cache
    Rendering::SamplerHandle m_SamplerRepeat{};  // U:Repeat V:Repeat
    Rendering::SamplerHandle m_SamplerClamp{};   // U:Clamp  V:Clamp
    Rendering::SamplerHandle m_SamplerRepeatX{}; // U:Repeat V:Clamp
    Rendering::SamplerHandle m_SamplerRepeatY{}; // U:Clamp  V:Repeat
    Rendering::SamplerHandle m_SamplerNearestClamp{}; // U:Clamp V:Clamp nearest filter

    struct CachedTexture
    {
        Rendering::TextureHandle Handle = Rendering::INVALID_TEXTURE_HANDLE;
        uint32_t Width = 1;
        uint32_t Height = 1;
        // True when the handle comes from the device-level shared background
        // texture broker. Shared handles are not destroyed by individual
        // UIManager instances.
        bool SharedOwned = false;
        // 9-slice layout authored on the texture asset (resolved from AssetRegistry
        // metadata at upload). Default = un-sliced (single-quad fast path).
        NineSlice Slice{};
        // m_BgCacheFrame value the last time this entry was resolved for paint.
        // EvictStaleBackgroundTextures ages entries on it once the cache
        // exceeds its byte budget.
        uint64_t LastUsedFrame = 0;
    };

    // Path->GUID resolution cache. Keys include the optional asset source
    // alias so editor:Icons/foo.png cannot collide with project Icons/foo.png.
    std::unordered_map<std::string, GUID> m_BgPathToGuid; // caches successful GUIDs

    std::unordered_map<GUID, CachedTexture> m_BgTextureCache;                // GUID -> texture handle + size
    // Monotonic paint-frame counter for m_BgTextureCache aging; advanced by
    // EvictStaleBackgroundTextures once per rendered frame.
    uint64_t m_BgCacheFrame = 0;
    Rendering::TextureHandle m_White1x1 = Rendering::INVALID_TEXTURE_HANDLE; // fallback

    // Asset hot-reload integration.
    // Subscribes to AssetEventDispatcher for Texture/Font reload + modified events
    // and propagates them to the relevant UIManager caches:
    //   - Texture: EvictBackgroundTexture(guid) tears down the cached GPU handle
    //     so the next paint creates a fresh one from disk; also drops the old
    //     UITextureRegistry slot (E7).
    //   - Font:    drops m_FontAtlases / m_FontAtlasAliases entries that resolved
    //     to that font GUID and triggers reflow.
    // Pending lists are populated from the dispatcher (any thread) and drained
    // on the next Update() so all GPU/UIElement mutation happens on the UI
    // thread while holding the dispatcher's normal frame timing.
    uint32_t m_AssetEventCallbackHandle = 0;
    std::mutex m_AssetReloadMutex;
    std::vector<GUID> m_PendingTextureReloads;
    std::vector<GUID> m_PendingFontReloads;
    bool m_PendingTextureCatalogDirty = false;
    // Map from font GUID to the m_FontAtlases keys that resolved to that GUID.
    // Maintained by OnFontResolved when the resolver returns a non-null GUID.
    std::unordered_map<GUID, std::vector<std::string>> m_FontGuidToAtlasKeys;
    // Reverse map: atlas key -> font GUID, so eviction can clean up the forward map.
    std::unordered_map<std::string, GUID> m_AtlasKeyToFontGuid;
    // GUID associated with the default m_FontAtlas (set when a resolver
    // first installs an atlas whose AtlasId matches the default). Lets the
    // reload handler reset the default atlas, which is otherwise not in
    // m_FontAtlases / m_FontAtlasAliases and would survive eviction.
    GUID m_DefaultFontAtlasGuid;
    void InstallAssetReloadCallback();
    void UninstallAssetReloadCallback();
    void DrainPendingAssetReloads();
    void HandleTextureAssetReloaded(const GUID& guid);
    void HandleFontAssetReloaded(const GUID& guid);
    // Direct device textures (name -> TextureHandle+Sampler+size). These bypass the
    // render graph entirely and are used for static/asset textures and persistent
    // textures that are no longer actively written.
    struct ExternalDeviceTexture
    {
        Rendering::TextureHandle Handle{};
        Rendering::SamplerHandle Sampler{};
        uint32_t Width = 0;  // 0 = unknown
        uint32_t Height = 0;
        UI::UITextureSpace Space; // no default: the registration states it
    };
    std::unordered_map<std::string, ExternalDeviceTexture> m_ExternalDeviceTextures;
    // RenderGraph registrations (PERSISTENT: name resolvable for primitive gen) and
    // publications (PER-FRAME: the current frame's id, validity-stamped).
    // Stored as raw RGResourceId + stamps — never a typed RenderGraph handle, so a
    // stale entry is structurally inert (the (frame, frameIndex) compare in
    // RenderRG is the only consumer).
    struct ExternalRGRegistration
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        UI::UITextureSpace Space; // no default: the registration states it
    };
    std::unordered_map<std::string, ExternalRGRegistration> m_ExternalRGRegistrations;
    struct PublishedRGTexture
    {
        Rendering::RenderGraph::RGResourceId TexId = Rendering::RenderGraph::kInvalidId;
        Rendering::RenderGraph::RGFrameStamp For;
    };
    std::unordered_map<std::string, PublishedRGTexture> m_ExternalRGPublished;

    // Engine texture names requested by primitive generation that did not
    // resolve this frame (drained by the producer via Consume...).
    std::unordered_set<std::string> m_UnresolvedExternalTextureRequests;

    // Registration names already reported by the format cross-check, so a
    // per-frame re-registration logs its diagnostic once rather than every frame.
    std::unordered_set<std::string> m_TextureSpaceMismatchReported;

    // Result of resolving an external texture name against the registries. A
    // resolved binding always carries a space; an unresolved name is nullopt,
    // so there is no "resolved with an unset space" state to read past.
    struct ResolvedExternalTexture
    {
        Rendering::TextureHandle DirectTex{};
        Rendering::SamplerHandle DirectSamp{};
        uint32_t Width = 0;  // 0 = unknown
        uint32_t Height = 0;
        UI::UITextureSpace Space;
        // RenderGraph-registered name: primitive gen routes the binding through the
        // per-frame publish map (externalName), no device handle here.
        bool Rg2Registered = false;
    };
    std::optional<ResolvedExternalTexture> ResolveExternalTexture(const std::string& name) const;

    // Cross-checks a stamp against the texture's real format and returns whether
    // the registration may proceed. Diagnoses (once per name) on any pair the
    // check does not accept.
    bool AcceptTextureSpaceForFormat(const std::string& name, Rendering::TextureFormat format,
                                     UI::UITextureSpace space);

    // In-flight GPU upload dedupe and ready-callback fan-out for background textures
    // In-flight upload markers, stamped. The dedupe check reads this before
    // issuing a request, so a completion that never arrives would otherwise
    // block every later attempt for the session — a background image that
    // misses its callback once stays missing. A stale entry is retried instead
    // of believed, and a bounded number of times so a genuinely absent image
    // cannot spin.
    struct BackgroundUploadRequest
    {
        std::chrono::steady_clock::time_point Issued;
        uint32_t Attempts = 0;
    };
    std::unordered_map<GUID, BackgroundUploadRequest> m_BgUploadInFlight;
    std::unordered_map<GUID, std::vector<std::function<void(Rendering::TextureHandle, uint32_t, uint32_t)>>> m_BgReadyCallbacks;

    // Keyboard and text events dispatch synchronously from OnKey/OnChar, so
    // there is no queue to inspect for "was there input this frame". This
    // records it for Update's fast-path gating instead, and is cleared by the
    // interactive event-dispatch pass that acts on it.
    bool m_KeyInputSinceLastUpdate = false;

    // Deferred rebuild to avoid modifying tree mid-frame
    DockspaceElement* m_PendingDockspaceRebuild = nullptr; // not owned
    bool m_RebuildDockspace = false;

    // Split dragging state
    bool m_DraggingSplit = false;
    std::string m_DragSplitPath;                 // '0'/'1' path from root to this split
    std::string m_DragSplitId;                   // element id of the splitter (split:<path>)
    DockspaceElement* m_DragDockspace = nullptr; // not owned
    // Drag metrics captured on mouse-down to avoid initial jump/collapse
    bool m_DragIsRow = true;
    float m_DragStartMouseX = 0.0f;
    float m_DragStartMouseY = 0.0f;
    float m_DragStartAPx = 0.0f;
    float m_DragStartBPx = 0.0f;
    float m_DragAvailPx = 0.0f; // aStart + bStart (excludes splitter)
    float m_DragMinAPx = 100.0f;
    float m_DragMinBPx = 100.0f;
    float m_DragHandlePx = 0.0f;
    float m_DragLastRatio = 0.5f;

    // Simple mouse capture (logical capture within UI, not OS-level)
    bool m_MouseCaptured = false;
    UIElement* m_CaptureElement = nullptr; // preferred pointer for capture routing (works across Mount portals)
    uint64_t m_CaptureInstanceId = 0;
    std::string m_CaptureId;               // element id holding the capture

    // ScrollView post-layout scroll application:
    // We apply scroll as a translation of the scroll-content subtree instead of
    // pushing scroll offsets into Yoga every tick. Track the last scroll offsets
    // that were applied to layout rects so we can apply deltas cheaply and so
    // scroll stays stable across frames.
    struct AppliedScrollOffset
    {
        float X = 0.0f;
        float Y = 0.0f;
        // CommitLayoutRects generation of the scroll CONTENT at the time
        // this offset was applied. A newer stamp on the content means its
        // committed rect was rewritten to the Yoga base since then, so the
        // stored offset is no longer baked into the rect and must be
        // re-applied in full (replaces the old global
        // layoutRectsJustRecomputed flag, which double-translated once
        // E3's pruning let unvisited scroll content keep translated rects).
        uint32_t ContentCommitGen = 0;
    };
    std::unordered_map<uint64_t, AppliedScrollOffset> m_AppliedScrollByInstanceId;

    // Bumped once per CommitLayoutRects call; visited elements' retained
    // nodes are stamped with it ("when was this rect last committed").
    uint32_t m_LayoutCommitGeneration = 0;

    bool m_DebugCaptureEnabled = false;
    std::ofstream m_DebugCaptureOut;
    std::string m_DebugCapturePath;
    uint64_t m_DebugCaptureSeq = 0;

  public:
    // Per-element effective clip rect, populated by RebuildClipCaches
    // whenever debug capture is enabled. Element absent from the map
    // means "no clip applies" (i.e. the prior NodeRec.hasClip == false).
    // Cleared at the start of each RebuildClipCaches call.
    //
    // Public so the recursive walk helper in UIManager_Layout.cpp can
    // reference the type by name; the storage itself stays private.
    struct DebugClipRect
    {
        float X = 0.0f, Y = 0.0f, W = 0.0f, H = 0.0f;
    };

  private:
    std::unordered_map<UIElement*, DebugClipRect> m_DebugClipCache;

    bool m_CorrectnessModeEnabled = false;
    bool m_CorrectnessModeAssertsEnabled = false;

    // Optional debug toggles (env-controlled) to bisect invalidation issues without full correctness mode.
    bool m_DisableFastPathNoOp = false;

    // GE_UI_DEBUG_KEYS=1 hands the bare F4-F12 presses to the UI's diagnostic
    // toggles. Off by default, and off regardless for a host that declines them
    // (SetDebugKeysEnabled): an ungated function key is one the playing game,
    // and every application behind the UI, silently never receives.
    bool m_DebugKeysEnabled = false;

    // A key with nothing focused bubbles from the hovered element
    // (SetKeyHoverFallbackEnabled). On by default for the editor's chrome; a
    // game surface's host turns it off so hover never claims the game's keys.
    bool m_KeyHoverFallbackEnabled = true;

    // A wheel with nothing hovered falls back to the focused element, then to the
    // first focusable one (SetScrollFocusFallbackEnabled). On by default for the
    // editor's chrome; a game surface's host turns it off so a HUD control never
    // takes a tick aimed at the world.
    bool m_ScrollFocusFallbackEnabled = true;
    std::atomic<uint64_t> m_RenderDiagActiveWindowTargetId{0};
    std::atomic<uint32_t> m_RenderDiagDeviceFrameIndex{0};
    std::atomic<uint32_t> m_RenderDiagTargetFrameIndex{0};
    std::atomic<uint32_t> m_RenderDiagUiFrameSlot{0};
    std::atomic<uint32_t> m_RenderDiagRgPoolSlot{0};

    UI::IPlatformApi* m_Platform = nullptr;

    // Per-frame text debug overlay + dump request
    bool m_TextDebugOverlayEnabled = false;
    bool m_TextDebugDumpRequested = false;
    bool m_TextDebugDisableElementScissor = false;
    bool m_TextDebugDisableClipStack = false;
    int m_TextDebugExpandScissorPx = 0;

    // Layout diagnostics (controlled by hotkey)
    int m_LayoutDiagFrames = 0; // when >0, print tree/grid widths for N frames

    // Stylesheet / dynamic pseudo-state analysis (computed during layout/style builds).
    struct DynamicStyleAnalysis
    {
        struct PseudoInfo
        {
            bool Any = false;
            bool AffectsLayout = false;
            bool AffectsPaint = false;
            bool MayAffectDescendants = false;
            bool MayAffectSiblings = false;
            // True when rules referencing this pseudo can affect inheritable layout inputs
            // (e.g. font-size / line-height / font-family). In those cases proving a "no-op"
            // hover change without a full style pass is difficult.
            bool MayAffectInheritedLayout = false;
        };
        PseudoInfo Hover{};
        PseudoInfo Active{};
        PseudoInfo Focus{};
        PseudoInfo FocusVisible{};
        PseudoInfo FocusWithin{};
        // :checked — flips with a Toggle/Checkbox value change (ToggleBase::SetValue).
        PseudoInfo Checked{};
        // Aggregate across every custom-state (`:name`) rule in the stylesheet,
        // mirroring how Hover is whole-stylesheet-aggregated. Custom-state rules
        // reach the runtime via the lexbor overlay that preserves unknown
        // pseudo-classes (cmake/ports/lexbor patch 0002); this info is populated
        // from those parsed rules. Per-name splitting would add a
        // StringId->PseudoInfo map plus a lookup on every state flip for no
        // editor benefit (the editor CSS uses zero custom-state selectors) and no
        // share-key benefit (P4d already excludes custom-states from the share
        // key). AddCustomState/RemoveCustomState consult this one info.
        PseudoInfo CustomState{};
        bool UsesSiblingCombinators = false; // '+' or '~' anywhere in selectors
        // PseudoKindBit bits of every pseudo whose match depends on sibling
        // position or count (nth-child, nth-of-type, first-/last-/only-child,
        // first-/last-/only-of-type and the nth-last forms), including uses
        // nested in :is / :where / :not.
        uint32_t StructuralPseudoMask = 0;
        bool UsesEmptyPseudo = false;       // `:empty` specifically — invalidate parent on add/remove first/last child
    };
    DynamicStyleAnalysis m_StyleAnalysis{};
    // Elements that currently own local (subtree-attached) stylesheets — e.g. a
    // game-UI HUD .css attached per-entity via AttachStyleToSubtreeFromAsset. Their
    // sheets seed the dynamic style analysis every frame (mirroring m_GlobalStylesheets):
    // the subtree-skip fast path can bypass the Yoga-walk's AddSheetForAnalysis on
    // clean frames, which would otherwise leave the analysis incomplete and silently
    // drop :hover/:active for those subtrees. Maintained by Register/UnregisterLocalSheetElement
    // (called from UIElement::SetOwnerManager + Add/RemoveStylesheet), so every entry is
    // a live element currently owned by this manager.
    std::unordered_set<UIElement*> m_LocalSheetElements;
    // Stylesheet tracking:
    //  - m_StylesheetSetGeneration: bumped when the *set/order* of global stylesheets changes.
    //  - m_StylesheetContentGeneration: bumped when an already-attached stylesheet's contents
    //    change in-place (e.g., UIStyleAsset hot reload) so analysis and caches can refresh.
    uint64_t m_StylesheetSetGeneration = 1;
    uint64_t m_StylesheetContentGeneration = 1;
    // P4 (C-10): epoch stamped into per-element rule caches at build time.
    // Bumped alongside the generations above whenever sheet content or the
    // sheet set changes; caches from an older epoch are never trusted
    // (invalidation walks miss detached subtrees, whose caches would
    // otherwise hold dangling CSSRule pointers after a hot reload).
    uint32_t m_RuleCacheEpoch = 1;
    uint64_t m_StyleAnalysisGeneration = 0;        // last content generation analyzed
    uint64_t m_StyleAnalysisSheetHash = 0;         // last analyzed stylesheet-set hash (includes subtree sheets)
    uint64_t m_AppliedStylesheetSetGeneration = 0; // last set generation applied to Yoga styles
    uint64_t m_AppliedStylesheetContentGeneration = 0; // last content generation applied to Yoga styles

    // Stylesheet runtime caches keyed by Stylesheet* identity. Cleared on stylesheet content changes.
    std::unordered_map<const Stylesheet*, std::unique_ptr<StylesheetRuleIndex>> m_StylesheetRuleIndexCache;
    uint64_t m_StylesheetRuleIndexCacheGeneration = 0;

    // Update profiling (CPU-side)
    bool m_UpdateProfilingEnabled = false;
    bool m_UpdateProfilingDumpRequested = false;
    uint32_t m_UpdateProfilingFrameCounter = 0;
    std::vector<UpdateProfileFrame> m_UpdateProfilingHistory; // capped in Update()

    // Dirty-source tracing. Off by default (enable via env GE_UI_DIRTY_TRACE=1).
    // When enabled, every layout-affecting MarkDirty call bumps a per-element
    // counter; at Update() exit the top-N entries are snapshot into
    // UpdateProfileFrame::dirtySources. Used to identify which elements /
    // controls are dirtying the tree every frame and driving slow-path walks.
    bool m_DirtyTraceEnabled = false;
    std::unordered_map<UIElement*, std::pair<uint32_t, unsigned>> m_PerFrameDirtyCount;

    // Font resolution can complete asynchronously (posted via the dispatcher). A font completion
    // affects text measurement and rendering, but forcing a full rebuild for *every* completion
    // can cause rebuild storms during editor startup when multiple font keys resolve over time.
    //
    // We coalesce: mark that a font resolved, and only invalidate cached geometry once the
    // current in-flight batch finishes (in-flight becomes empty).
    bool m_FontResolvedDirty = false;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
  public:
    // These nested types are public so internal .cpp files (via
    // UIManager_Internal.h) can reference them by name — they're still
    // implementation detail.

    // Text measurement context for Yoga measure callbacks. One instance per
    // RetainedYogaNode that has a measure function attached. Lives until the
    // owning UIElement is destroyed. Previously pooled in a thread_local that
    // got reset each frame — a fast-path that skipped YGNodeSetContext would
    // then point at a reused pool slot. Owned per-retained-node here, so the
    // Yoga context pointer stays stable across frames.
    struct TextMeasureCtx
    {
        Rendering::Text::FontAtlas* Atlas = nullptr;
        std::string Text;
        // Device size (font size x content scale), fractional — see
        // FontAtlas::GetLineMetrics for why the line box rounds here.
        float PixelSize = 20.0f;
        float LineHeight = 0.0f;
        float FontSize = 0.0f;
        // letter-spacing in device px (CSS px x content scale), the unit the
        // FontAtlas measure/shape entry points take.
        float LetterSpacingPx = 0.0f;
        float ContentScale = 1.0f;
        bool HasNewlines = false;
        bool AllowWrap = false;
        // word-break and overflow-wrap collapsed to one line-breaking policy
        // (ResolveTextBreakPolicy), not the CSS word-break value.
        WordBreak BreakPolicy = GameEngine::WordBreak::Normal;
        std::uint64_t TextHash = 0;
        std::uint64_t CachedKey = 0;
        bool CachedValid = false;
        float CachedW = 0.0f;
        float CachedH = 0.0f;
        // Lines the last reported measurement covers. The baseline callback
        // needs it: one line is centred in the content box, a block of them is
        // top-aligned, and the first line's baseline follows the box it sits in.
        // Held alongside CachedW/CachedH so a cache hit keeps all three in step.
        int MeasuredLineCount = 1;
        // Last wrap-width constraint (physical px) Yoga measured with, and
        // whether any measure has run yet. The parallel pre-measure pass
        // predicts the next solve's constraint from this — a wrong prediction
        // is only a cache miss (synchronous re-measure inside the solve).
        float LastWrapWidthPhysical = 0.0f;
        bool HasMeasured = false;
    };

    struct MeasureInputs
    {
        bool HasMeasure = false;
        uint64_t TextHash = 0;
        float FontSize = 0.0f;
        float LineHeight = 0.0f;
        float LetterSpacing = 0.0f;
        float VisualFontSize = 0.0f;
        float VisualLineHeight = 0.0f;
        bool AllowWrap = false;
        // Resolved line-breaking policy, as on TextMeasureCtx.
        WordBreak BreakPolicy = GameEngine::WordBreak::Normal;
        // Atlas identity: a font-family/weight/style swap changes the atlas
        // while every other input stays equal — without this, the node is
        // never re-dirtied and Yoga serves a size measured with the old font.
        // Content-derived id (FontAtlas::GetAtlasId, regenerated by every
        // LoadFontBytes), NOT the pointer: a pointer survives in-place
        // reloads and can be reused by the allocator across destroy/recreate.
        uint32_t AtlasId = 0;

        bool operator==(const MeasureInputs&) const = default;
    };

  public:
    // Persistent interner for effective stylesheet sets. An element's
    // merged (parent+local) sheet list, canonicalized by MergeSheets,
    // is interned to a stable u32 ID; elements sharing the same merged
    // list share the same ID. IDs persist across frames so the effective
    // sheet set for a dirty element resolves without a top-down walk.
    //
    // ID 0 is reserved for "empty" (no sheets). Subsequent IDs
    // allocated densely in order of first intern.
    //
    // Entries grow monotonically per UIManager lifetime; cleared on
    // ResetRetainedYogaTree. Dedup by hash + equality on the sheet
    // pointer list (Stylesheet* identity; lifetime guaranteed by
    // StylesheetHandles held elsewhere).
    class SheetSetInterner
    {
      public:
        SheetSetInterner();

        // Step 4: takes a span of handles instead of raw pointers.
        // The interner holds StylesheetHandle copies internally so the
        // referenced Stylesheets stay alive as long as the entry is in
        // the interner — eliminates the per-frame Clear() workaround
        // for dangling pointers.
        // `mgr` is used to build StylesheetRuleIndex pointers for
        // entries that don't yet have them; the mgr reference is not
        // retained. Returns a stable ID.
        uint32_t Intern(std::span<const StylesheetHandle> handles,
                        UIManager& mgr);

        std::span<const Stylesheet* const>
            GetSheets(uint32_t id) const;
        std::span<const StylesheetHandle>
            GetHandles(uint32_t id) const;
        std::span<const StylesheetRuleIndex* const>
            GetIndices(uint32_t id) const;

        uint32_t Size() const { return (uint32_t)m_Entries.size(); }
        // Advances on every Clear(). A RetainedYogaNode stamps it when its
        // PersistentSheetSetId is interned; a mismatch means the id predates
        // the clear and resolves to nothing until BuildYogaRecursive
        // re-primes it.
        uint32_t Generation() const { return m_Generation; }
        // Drops every entry; only safe when no element references the
        // interner IDs anymore (scene reload, ~UIManager). Step 4's
        // whole point is that handle ownership keeps entries valid
        // otherwise — call sites outside those paths should not need this.
        void Clear();

      private:
        struct Entry
        {
            std::vector<const Stylesheet*> Sheets;     // raw; used for hash + equality
            std::vector<StylesheetHandle> Handles;     // keep-alives; mirrors sheets
            std::vector<const StylesheetRuleIndex*> Indices;
            uint64_t Hash = 0;
        };
        std::vector<Entry> m_Entries;
        std::unordered_map<uint64_t, std::vector<uint32_t>> m_HashBuckets;
        uint32_t m_Generation = 1;
    };

    // Exposed publicly so the interner can be unit-tested directly.
    SheetSetInterner& GetSheetSetInterner() { return m_SheetSetInterner; }
    const SheetSetInterner& GetSheetSetInterner() const { return m_SheetSetInterner; }

    // Invalidate the persistent sheet-set cache for a subtree. Walks
    // m_Children + Mount targets, zeroing PersistentSheetSetId on each
    // element's RetainedYogaNode and marking StyleDirty. Called from
    // UIElement's stylesheet-attach/detach sites because MarkDirtySubtree
    // doesn't propagate through Mount portals and doesn't touch the
    // interner-backed sheet cache. The next BuildYogaRecursive visit
    // re-interns the subtree's effective sheet lists from scratch
    // (parent + local merged).
    // A parent's stylesheet change would otherwise leave descendants with
    // stale sheet-set IDs; the rebuild re-interns against the invalidated
    // cache.
    void InvalidateSheetSetSubtree(UIElement* root);

  private:
    SheetSetInterner m_SheetSetInterner;

    // Event-driven UI rewrite, Stage 1 (additive, currently unused at
    // runtime): persistent CPU mirrors of the GPU SSBOs, paired with
    // their slot allocators. Stage 2 starts wiring PrimitiveGen output
    // into m_PersistentPrimitives via m_PrimitiveAllocator; Stage 3
    // mirrors clips; Stage 4 introduces m_DrawOrder + its allocator
    // and switches the render pipeline to read through these fields.
    //
    // Today these stay default-constructed and are not referenced by
    // any code path. Their presence is purely so UIElement's render
    // handle fields (m_PrimitiveRangeStart / m_ClipSlotIdx / etc.) have
    // an addressable destination, and so ~UIElement can route through
    // UIManagerFreeRenderSlots when those handles do start getting set.
    std::vector<UI::UIPrimitive> m_PersistentPrimitives;
    UI::SlotAllocator            m_PrimitiveAllocator;
    std::vector<UI::UIClipRect>  m_PersistentClipRects;
    UI::SimpleSlotAllocator      m_ClipAllocator;
    std::vector<uint32_t>        m_DrawOrder;
    UI::SlotAllocator            m_DrawOrderAllocator;

    // Stage 2: PrimitiveGen frame counter, paired with UIElement::
    // m_LastPrimitiveGenFrame for re-entrancy detection. Bumped at the
    // top of every GenerateAllPrimitives. Skips 0 on wrap so default-
    // initialized element counters cannot collide with the current value.
    uint32_t m_PrimitiveGenFrame = 0;

    // Counter value of the last FULL primitive DFS (drains bump
    // m_PrimitiveGenFrame too, so this is recorded separately). Elements
    // stamped with this value (UIElement::m_LastFullDfsFrame) were processed
    // by that walk; the drain rejects surgery on any element that wasn't.
    uint32_t m_LastFullDfsGenFrame = 0;

    // Block E (E0) — idle-frame skip for GenerateAllPrimitives.
    // Set true by NotifyElementDirty (via MarkDirty on any element of this
    // manager) and by Update for the few mutation paths that don't route
    // through MarkDirty (Yoga solve, dock rebuild, viewport / scroll,
    // sheet hot-reload, transitions). Cleared at the end of a successful
    // GenerateAllPrimitives. When false (and m_DrawOrder non-empty),
    // the render path skips PrimitiveGen and re-uploads the existing snapshot.
    //
    // Default true so the first frame always generates. Atomic because
    // MarkDirty can fire from worker threads (e.g. async font / asset
    // callbacks) before being marshalled onto the UI thread.
    std::atomic<bool> m_PrimitivesNeedRegen{true};

    // Stage 2: dedicated slot for the tooltip overlay arrow primitive.
    // The arrow is emitted by m_TooltipOverlay (not a UIElement), so it
    // cannot route through the per-element slot path. Allocated lazily
    // on first emission, retained for the lifetime of this UIManager.
    // SlotAllocator::kInvalidSlot when not yet allocated.
    uint32_t m_TooltipArrowSlot = 0xFFFFFFFFu;

    // Stage 3: dedicated clip slot for the anonymous viewport clip used by
    // deferred-overlay rendering (tooltips, dropdowns, modals). Lifecycle
    // mirrors m_TooltipArrowSlot — allocated lazily, retained for the
    // lifetime of this UIManager. Per-element clips are owned by the
    // element via UIElement::m_ClipSlotIdx.
    uint32_t m_ViewportClipSlot = 0xFFFFFFFFu;

    // Stage 5 Block D: :focus-within chain. Set of (focused element +
    // every ancestor up to the root). Recomputed by RebuildFocusWithinChain()
    // when focus changes; consulted by ResolveCascadeForElement to populate
    // ElementState::FocusWithin.
    //
    // The chain holds raw UIElement pointers. Entries are removed via
    // OnFocusChainElementDestroyed when an element in the chain is
    // destroyed; otherwise a destroyed element would leave a dangling
    // pointer in the set. The set is also rebuilt from scratch on every
    // focus change so transient leaks (an element destroyed without
    // explicit focus removal) self-heal at the next focus event.
    std::unordered_set<UIElement*> m_FocusWithinChain;

  public:
    // Enqueue an element for the render-side primitive-data drain
    // (DrainPrimitiveDataDirty, called from GenerateAllPrimitives): its
    // primitive bytes are re-emitted in place into its persistent slot
    // range, no tree walk. No-op if already enqueued (dedup'd via
    // UIElement::m_InQueueFlags). Cheap to call from any mutation site.
    void PushPrimitiveDataDirty(UIElement* el);

    // Called by ~UIElement to remove the element from the primitive-data
    // queue (tombstones the entry to nullptr; the drain skips nullptr) and
    // from the :focus-within chain.
    void RemoveFromDirtyQueues(UIElement* el);

    // Stage 5 Block D: recompute m_FocusWithinChain from the current m_FocusId
    // (focused element + every ancestor). Marks the symmetric difference of
    // the OLD and NEW chain elements through MarkPseudoStateScope so
    // :focus/:focus-visible/:focus-within rules re-cascade (and relayout when
    // the analysis says they affect layout). Cheap when there's no focus change
    // (empty symmetric difference).
    void RebuildFocusWithinChain();

    // Single analysis-consulting decision shared by every focus-family pseudo
    // marker (the :focus / :focus-visible / :active blocks in DispatchEvents and
    // the :focus-within chain rebuild). Given the style analysis for a pseudo
    // (or a combined pair — a focus-target change flips :focus and
    // :focus-visible together), it raises exactly the dirty bits that transition
    // demands: StyleDirty|VisualDirty always, plus LayoutDirty when
    // info.AffectsLayout, and fans the mark out to the element's subtree when
    // info.MayAffectDescendants and to its siblings when info.MayAffectSiblings.
    // Replaces the hand-rolled per-site bit choices that had drifted — the
    // :focus-within rebuild raised StyleDirty only and never consulted the
    // analysis, so a layout-affecting or descendant/sibling :focus-within rule
    // could paint at stale geometry or miss re-cascade entirely. narrowBits
    // gates the per-element mark when the rule cache drives the cascade (an
    // element no matching rule references for these pseudos is a safe skip).
    void MarkPseudoStateScope(UIElement* el,
                              const DynamicStyleAnalysis::PseudoInfo& info,
                              std::uint16_t narrowBits);

    // Union of two pseudo infos, used where one transition activates more than
    // one pseudo-class at once (a focus-target change flips :focus and
    // :focus-visible; the :focus-within chain leaf additionally flips those).
    static DynamicStyleAnalysis::PseudoInfo CombinePseudoInfo(
        const DynamicStyleAnalysis::PseudoInfo& a,
        const DynamicStyleAnalysis::PseudoInfo& b);

    // Self-pseudo marker entry points for the two element-driven state flips
    // that MarkPseudoStateScope did not previously cover: a Toggle/Checkbox
    // value change (:checked) and a custom-state add/remove. Both route the
    // element's own scope through the same analysis-driven decision the focus
    // family uses, so the flip raises LayoutDirty only when the corresponding
    // rules affect layout and fans out per their descendant/sibling
    // combinators. Reached from UIElement / ToggleBase via the
    // UIManagerMark{Checked,CustomState}Scope free-function forwarders.
    void MarkCheckedStateScope(UIElement* el);
    void MarkCustomStateScope(UIElement* el);

  private:
    // Elements with pending visual-only mutations, consumed by
    // DrainPrimitiveDataDirty on the render side. Never cleared at Update
    // entry — event handlers queue into it between the previous frame's
    // drain and this Update.
    std::vector<UIElement*> m_PrimitiveDataDirty;

    // Force the next Update to rebuild from scratch. Set by
    // ResetRetainedYogaTree, SetRoot, and the structural-op drain
    // when ops were enqueued this frame. Consumed at the top of
    // Update.
    bool m_ForceFullRebuildNextFrame = false;

    // Tactical gate (path B): bumped on AddStylesheet / RemoveStylesheet /
    // m_GlobalStylesheets change / asset-reload. Compared against
    // m_StylesheetGenAtLastClear to decide whether the per-frame
    // interner-clear + PSSID-zero + pre-cascade re-intern triple needs
    // to run this frame.
    uint64_t m_StylesheetGen = 1;
    uint64_t m_StylesheetGenAtLastClear = 0;

  public:
    // Trigger a full rebuild on the next Update. Called from SetRoot etc.
    // once the primary flip lands; currently a no-op hint (full rebuild
    // runs unconditionally).
    void RequestFullRebuild() { m_ForceFullRebuildNextFrame = true; }
  private:

    // Master toggle for the subtree-skip fast path (Slice 2). Reads env
    // GE_UI_SUBTREE_SKIP once in the ctor ("0"/"false"/"off" disable).
    // Default ON once step 2 has been validated; can be flipped off at
    // runtime via SetSubtreeSkipEnabled for A/B comparisons.
  public:
    void SetSubtreeSkipEnabled(bool v) { m_SubtreeSkipEnabled = v; }
    bool IsSubtreeSkipEnabled() const { return m_SubtreeSkipEnabled; }
  private:
    bool m_SubtreeSkipEnabled = true;
  public:
    // IndicesAndClips-rebuild skip. When structure is stable and layout wasn't
    // solved, the derived maps and clip caches from the previous frame are
    // still valid — rebuilding them is pure waste. Env: GE_UI_INDICES_CLIPS_SKIP=0
    // disables (forces the rebuild every frame, matches pre-fix behaviour).
    void SetIndicesAndClipsSkipEnabled(bool v) { m_IndicesAndClipsSkipEnabled = v; }
    bool IsIndicesAndClipsSkipEnabled() const { return m_IndicesAndClipsSkipEnabled; }
  private:
    // Master skip knob (env: GE_UI_INDICES_CLIPS_SKIP=0 disables). Reused
    // post-Stage-7-step-7 as a global gate for the FocusOrder rebuild skip
    // — the original index-map/clip skip it was named for is gone.
    bool m_IndicesAndClipsSkipEnabled = true;
    // Same one-shot prime flag for m_FocusOrder — required because the
    // vector is empty on the first frame even though structure-change
    // counter might be 0 (nothing dirtied yet).
    bool m_FocusOrderPrimed = false;
#endif

    // Normalize a CSS font family name for internal lookup/dedup (case-insensitive, trimmed).
    static std::string NormalizeFontFamilyKey(const std::string& family);

    // Returns an already-loaded atlas for (family,weight,style,variant), or (if missing) kicks off a host
    // resolution request (deduped) and returns nullptr until ready.
    Rendering::Text::FontAtlas* GetOrRequestFontFamilyInternal(const std::string& family,
                                                               int weight,
                                                               FontStyle style,
                                                               FontVariant variant);

    // Apply a completed resolution result on the UI thread.
    void OnFontResolved(const std::string& fontKey, FontResolveResult&& result, uint64_t resolverGeneration);

    // A failed resolve only arms a wall-clock backoff; the retry is issued by
    // text re-resolution during a heavy build, which a converged idle tree
    // never runs. Schedules a restyle at backoff expiry so the retry can
    // actually fire (the scheduler drains before the idle gate).
    void ScheduleFontRetryRestyle(std::chrono::milliseconds backoff);

    // Internal helper: run scheduler/dispatcher, rebuild Yoga layout, handle
    // input + events, and generate per-frame geometry into the CPU caches.
    // Returns true when geometry is valid for rendering this frame.
    bool BuildLayoutAndGeometry(bool interactive);
};

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
// Per-element retained Yoga state. Owned by UIElement::m_YogaState (one
// allocation per element). Previously held in
// std::unordered_map<UIElement*, std::unique_ptr<RetainedYogaNode>> on
// UIManager — Stage 7 step 8 moved ownership to the element to eliminate
// the per-frame map lookup on the cascade hot path.
//
// Defined at namespace scope (rather than nested inside UIManager) so
// UIElement.h can forward-declare it without depending on UIManager.h.
struct RetainedYogaNode
{
    RetainedYGNodeRef Node = nullptr;
    uint64_t InstanceId = 0;
    LayoutInputs PrevLayout{};
    UIManager::MeasureInputs PrevMeasure{};
    float PrevFlexGrowOverride = 0.0f;
    // Whether the container established block flow at the last push. The
    // effective flex-shrink depends on it (ApplyBlockFlowShrinkDefault), so a
    // parent that flips display leaves this element's own PrevLayout unchanged
    // while its Yoga input has moved — without this the re-push is skipped.
    bool PrevParentBlockFlow = false;
    bool HasPrevInputs = false;

    // UIManager::m_LayoutCommitGeneration at the last CommitLayoutRects
    // visit — "when was this element's rect last rewritten from Yoga".
    // ApplyScrollTransforms compares it to decide whether its applied
    // offset is still baked into the committed rect (E3 pruning means a
    // solve no longer implies every rect was recommitted).
    uint32_t LastCommitGen = 0;

    // Subtree-skip gate state (Stage 7 step 7 option 2). Set true at the
    // end of a successful slow-path BuildYogaRecursive run; the early-exit
    // gate at the top of the function refuses to skip until this becomes
    // true. Cleared by ResetRetainedYogaTree (implicit — node is destroyed),
    // InvalidateSheetSetSubtree, InvalidateAllInternedSheetSets, and the
    // display:none early-return branch in BuildYogaRecursive. All other
    // invalidations (SubtreeDirty propagation, hash mismatch, content-gen
    // mismatch, Mount-in-subtree) are detected naturally by the gate
    // predicate without explicit clearing.
    bool                                       CachedSubtreeValid          = false;
    // Generation counter snapshot at last successful slow-path build.
    // Compared against UIManager::m_StylesheetContentGeneration in the
    // gate; mismatch forces a slow-path rebuild after stylesheet content
    // hot-reload (NotifyStylesheetContentChanged advances the counter).
    uint64_t                                   CachedStylesheetContentGen  = 0;

    // Per-node text measurement context (Slice 2 step 3). Allocated lazily
    // when this element first needs a Yoga measure function; persists
    // across frames, so fast-path appends don't need to re-bind the
    // Yoga context pointer.
    std::unique_ptr<UIManager::TextMeasureCtx> MeasureCtx;

    // 64-bit fold of el->GetStylesheets() pointer list observed on the
    // previous slow-path visit. Used by the subtree-skip gate at the top
    // of BuildYogaRecursive: a hash mismatch means the local stylesheet
    // list changed (attach/detach/reorder) since the last build, so the
    // gate forces a slow-path rebuild even when no dirty flag fired.
    // Initialised 0 (matches "no local sheets" on a fresh node).
    uint64_t PrevLocalSheetsHash = 0;
    // 64-bit fold of the inherited (parent) sheet span observed on the
    // previous slow-path visit. A change anywhere above this element
    // (parent attached/detached a sheet) shifts our merged sheet list,
    // which we can't detect purely from local state — hash the parent
    // span and treat a delta as a reason to fall through to slow path.
    uint64_t PrevParentSheetsHash = 0;

    // Last observed DisplayMode == None state. display:none transitions
    // (via a StyleDirty-driven cascade that flips DisplayMode) change
    // ctx.nodes size — descendants are included or excluded via the
    // display:none early-return branch — without firing ChildrenDirty
    // on any element. Tracking the flag here lets BuildYogaRecursive
    // bump the structure counter on the transition frame, which is
    // how the IndicesAndClips-skip gate learns the node list changed.
    bool PrevDisplayModeNone = false;

    // Persistent sheet-set ID (SheetSetInterner). Populated by
    // BuildYogaRecursive at each visit. 0 = "not yet interned" OR
    // "inherits root set" — caller must interpret based on context.
    // Survives across frames so the effective sheet set for a dirty
    // element resolves without a top-down walk.
    uint32_t PersistentSheetSetId = 0;
    // SheetSetInterner::Generation() at the time PersistentSheetSetId was
    // interned. 0 = never interned. The in-place cascade callers compare it
    // against the live generation before trusting the id.
    uint32_t SheetSetGeneration = 0;

    // Cached parent PSSID at the time PersistentSheetSetId was last
    // computed by the pre-cascade re-intern walk. The walk uses this
    // as a content-based gate: if the parent's *current* PSSID
    // matches AND our own PSSID is non-zero, skip the merge+intern
    // step — the interner is content-addressed, so a matching
    // parent PSSID implies the same parent handle list, which would
    // re-intern to the same id. The gate is sound only when every
    // path that changes effective sheets zeros affected PSSIDs:
    // InvalidateSheetSetSubtree handles the per-element/subtree
    // case; InvalidateAllInternedSheetSets handles every global-
    // sheet add (m_GlobalStylesheets mutation). UINT32_MAX = "no
    // cached value yet, force a walk".
    uint32_t LastSeenParentPSSID = UINT32_MAX;

    RetainedYogaNode() = default;

    // Out-of-line in UIManager_Layout.cpp so it can call YogaAdapter
    // free functions without pulling yoga/Yoga.h into this header.
    // Detaches `Node` from any parent, removes any remaining children,
    // then destroys it. Idempotent on already-null `Node`.
    ~RetainedYogaNode();

    RetainedYogaNode(const RetainedYogaNode&) = delete;
    RetainedYogaNode& operator=(const RetainedYogaNode&) = delete;
    RetainedYogaNode(RetainedYogaNode&&) = delete;
    RetainedYogaNode& operator=(RetainedYogaNode&&) = delete;
};
#endif

} // namespace GameEngine
