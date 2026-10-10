#include "UI/UIManager.h"
#include "UIManager_Internal.h"

#include "UI/UIHotReload.h"
#include "UI/TransitionEngine.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UI/VirtualizationCoordinator.h"
#include "UI/UIFrameBufferRing.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureRegistry.h"

#include "Logger/Logger.h"
#include "Platform/SystemFonts.h"

#include "UI/Controls/DockspaceElement.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/IVirtualizedControl.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/TreeView.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DragDropOverlay.h"
#include "UI/Interaction/TooltipOverlay.h"
#include "UI/UIElement.h"
#include "UIAttributeAccess.h"

#include "Types/ColorUtils.h"
#include "UI/Controls/Mount.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "UI/Layout/YogaLayout.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/StyleUtil.h"
#include "UI/UIEvents.h"
#include "UI/UIStyle.h"

#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"

#include "Core/CpuProfiler.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
// <memory_resource> removed: all PMR containers replaced with static thread_local vectors

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
// ThreadLocalScratchResource.h removed: PMR scratch allocator no longer used in this file

#include <cmath>
#include <vector>

#include <unordered_map>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>

#include "Assets/AssetManager.h"
#include "Assets/TextureAsset.h"

#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/StyleOverrides.h"

#include "Core/Application.h" // PathUtils::GetInstallAssetsRoot

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Geometry;
using namespace GameEngine::UIParsing;
using namespace GameEngine::Rendering::Text;


static constexpr bool kTextDebugOverlay = false; // disabled by default; enable when needed

static constexpr bool kTextDebugHeader = false;   // disabled by default; enable when needed
static constexpr bool kTextDebugFill = false;     // keep off unless debugging layout boxes
static constexpr bool kTextDebugConsole = false;  // quiet by default; turn on for triage
static constexpr bool kTextDebugDumpFile = false; // no dump unless explicitly debugging

// Root padding diagnostics: optional, off-by-default helpers used when
// validating that :root / .root selectors and Yoga root padding agree.
static constexpr bool kRootPaddingDebugOverlay = false; // draw outlines for root border/content boxes
static constexpr bool kRootPaddingDebugConsole = false; // print computed root padding + layout and selector info

// Pointer diagnostics: disabled by default to avoid spam; can be turned on
// locally when debugging pointer routing. In most cases, more targeted
// diagnostics inside individual controls are sufficient.
[[maybe_unused]] static int s_UiTextDebugFrame = 0;
static bool s_DebugDumped = false;

// Tree-walk helper: count visible elements in the (children + Mount portal)
// hierarchy. Used for spike-log + profile-frame element counts after Stage 7
// step 7 deleted the m_Nodes flat vector.
static uint32_t CountTreeElements(UIElement* root)
{
    uint32_t n = 0;
    std::function<void(UIElement*)> walk = [&](UIElement* el) {
        if (!el) return;
        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            return;
        ++n;
        for (const auto& ch : el->GetChildren())
            walk(ch.get());
        if (el->Kind() == UIElementKind::Mount)
            if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                walk(tgt);
    };
    walk(root);
    return n;
}

using namespace GameEngine::UILayout;

namespace
{
class ScopedUpdateEventDispatchGuard final
{
  public:
    ScopedUpdateEventDispatchGuard() { UIElement::SetInEventDispatch(true); }
    ~ScopedUpdateEventDispatchGuard() { UIElement::SetInEventDispatch(false); }

    ScopedUpdateEventDispatchGuard(const ScopedUpdateEventDispatchGuard&) = delete;
    ScopedUpdateEventDispatchGuard& operator=(const ScopedUpdateEventDispatchGuard&) = delete;
};


} // namespace


void UIManager::Update(float deltaTime)
{
    Update(deltaTime, /*interactive=*/true);
}

#if defined(_MSC_VER)
// /analyze sometimes overestimates stack usage for this function due to large inlined
// templates and lambdas. Suppress C6262 on this function specifically.
#pragma warning(suppress : 6262)
#endif
void UIManager::Update(float deltaTime, bool interactive)
{
    GE_CPU_PROFILE_SCOPE("UIManager.Update");

#ifdef _DEBUG
    // C-6: capture the UI thread at the first Update (construction may run
    // on a loader thread); AssertUiThread checks against it thereafter.
    if (!m_UiThreadCaptured)
    {
        m_UiThreadId = std::this_thread::get_id();
        m_UiThreadCaptured = true;
    }
#endif

    // Advance global UI time for animations, timers, etc.
    m_Time += deltaTime;

    // Stage 5 Block D: refresh the :focus-within chain. Marks any element
    // entering or leaving the chain dirty so its style re-cascades with
    // the new FocusWithin flag. Cheap when m_FocusId is stable — the
    // FindById walk + parent ascent is O(N + depth) and only the symmetric
    // difference triggers MarkDirty calls.
    RebuildFocusWithinChain();

    // Diagnostics for redundant-BuildYoga investigation. Populated throughout
    // this frame and dumped by the spike logger at the end when total frame
    // time exceeds GE_UI_SPIKE_LOG_MS. Gives us per-invocation ms and the
    // specific trigger that caused each extra rebuild beyond the initial one.
    bool rebuildAfterPreSolveFired = false;
    bool rebuildAfterPreSolveByFlush = false;
    bool rebuildAfterPreSolveByVirt = false;
    double rebuildInitialMs = 0.0;
    double rebuildAfterPreSolveMs = 0.0;
    double preSolveFlushCallbacksMs = 0.0;
    double preSolveVirtualizationMs = 0.0;

    bool rebuildLateRelayoutFired = false;
    // Bitmask: 1=childrenDirty 2=layoutDirty 4=styleDirty 8=relayoutReq 16=virt
    unsigned rebuildLateRelayoutReasons = 0;
    double rebuildLateRelayoutMs = 0.0;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // Without a device or loaded layout there is nothing to update.
    if (!m_Device || !m_Root)
        return;

    const bool profEnabled = IsUpdateProfilingEnabled();
    UpdateProfileFrame prof{};
    using ProfClock = std::chrono::high_resolution_clock;
    // Optional spike logger: set GE_UI_SPIKE_LOG_MS to a threshold in ms (e.g. 25)
    // to log a compact summary when UIManager::Update exceeds that time.
    static const double s_UiSpikeLogThresholdMs = []() -> double
    {
        const char* env = std::getenv("GE_UI_SPIKE_LOG_MS");
        if (!env || !*env)
            return 0.0;
        char* end = nullptr;
        const double v = std::strtod(env, &end);
        if (end == env)
            return 0.0;
        return (v > 0.0) ? v : 0.0;
    }();
    const bool spikeLogEnabled = (s_UiSpikeLogThresholdMs > 0.0);

    ProfClock::time_point totalStart{};
    if (profEnabled || spikeLogEnabled)
    {
        if (profEnabled)
        {
            prof.FrameIndex = ++m_UpdateProfilingFrameCounter;
        }
        totalStart = ProfClock::now();
    }

    // (Step 3's drain-and-count moved down — now happens just before the
    //  full rebuild as part of the Step 4 surgical preview, so both paths
    //  consume the same drained op list.)

    // Geometry is now retained across frames. We only rebuild when something
    // changes (layout, paint, animations). Do not clear cached frame data here.

    // Update owns input dispatch and layout. When interactive == false we
    // still recompute layout and hover state but suppress interactive side
    // effects (focus changes, dockspace rebuilds, control-driven relayout,
    // etc.). This is used by the editor for passive/preview renders.
    const bool runInteractive = interactive;

    // Bind TLS UiContext so any UI calls from timers/dispatchers during
    // Update are routed to this manager.
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    // Apply any queued asset-driven hot reloads before building Yoga/layout.
    if (m_HotReload)
    {
        m_HotReload->Pump();
    }
    // C1/C2/E7: drain Texture/Font asset reload events queued from the asset
    // pipeline's event dispatcher. Runs after UIHotReload::Pump so style/layout
    // reloads observe the freshly-evicted texture/font caches in the same frame.
    DrainPendingAssetReloads();
    // Prepare a default font atlas for text measurement (same atlas that Render uses for drawing).
    //
    // A UIManager with no host font resolver — tests, tools, and the editor's own resolver-less
    // windows such as ColorPickerWindow — loads a default font atlas synchronously from the staged
    // engine fonts. One attempt per UIManager: a font that is not staged is not going to appear
    // on a later frame, and Update must not probe the filesystem every frame.
    if (!m_FontAtlas && !m_FontResolver && !m_DefaultFontAtlasAttempted)
    {
        m_DefaultFontAtlasAttempted = true;
        std::vector<uint8_t> fontBytes;
        auto tryLoad = [&](const char* p)
        {
            if (fontBytes.empty())
                fontBytes = Utils::ReadFile(p);
        };

        {
            const std::string stagedFont =
                (PathUtils::GetInstallAssetsRoot() / "Fonts" / "Roboto-Regular.ttf").string();
            tryLoad(stagedFont.c_str());
        }
#if !defined(__APPLE__)
        if (fontBytes.empty())
        {
            Platform::SystemFontFile sf;
            if (Platform::TryResolveSystemFontFile("sans-serif", 400, Platform::SystemFontStyle::Normal, sf))
                tryLoad(sf.path.string().c_str());
        }
#endif
        if (!fontBytes.empty())
        {
            auto fa = std::make_unique<FontAtlas>();
            ConfigureUiFontAtlas(*fa);
            if (fa->LoadFontBytes(fontBytes.data(), fontBytes.size(), kUiFontAtlasPx))
            {
                m_FontAtlas = std::move(fa);
            }
        }
    }

    // Font-family loader shared by layout measurement and element renderers.
    // This is intentionally available during Yoga measurement so measured text
    // height matches the font that will be used for rendering (prevents label
    // overlap/cropping when themes specify font-family).
    auto getOrLoadFamily = [this](const std::string& family,
                                  int weight,
                                  FontStyle style,
                                  FontVariant variant) -> FontAtlas*
    {
        return GetOrRequestFontFamilyInternal(family, weight, style, variant);
    };

    // NOTE: text measure contexts are stored in the thread-local pool above.

    // Ensure a layout has been provided by the app/example
    UIElement* rootEl = m_Root.get();

    // Retained-mode dirty hints:
    // - UIElement::MarkDirty/MarkDirtySubtree notifies the owning UIManager via m_DirtyHintFlags.
    // - We exchange(0) once at the start of Update so we can detect "dirty since last frame"
    //   without scanning the whole tree, and then later re-clear before the heavy pass so any
    //   dirty marks during the heavy pass are preserved for the next frame.
    const unsigned dirtyPrev = m_DirtyHintFlags.exchange(0u, std::memory_order_relaxed);

    // NOTE: We intentionally keep Update() semantics general: no editor-specific
    // fast paths. Performance work should be achieved via retained caching,
    // incremental rebuilds, and better dirty tracking.

    // Run any due scheduled tasks (timers/next-tick) and flush deferred UI actions.
    //
    // Correctness note:
    // - Both the scheduler and dispatcher are explicitly non-reentrant: work scheduled/posted while
    //   processing a batch is deferred to the *next* ProcessDue()/Drain() call.
    // - In practice, many UI workflows rely on a small amount of same-frame convergence (e.g.,
    //   a PostAction enqueues another PostAction, or a scheduled callback posts UI work).
    // - If we only ProcessDue()/Drain() once per Update(), some mutations can be delayed until the
    //   next *user input*, leading to the exact "layout is wrong until refresh" issues weâ€™re trying
    //   to eliminate in normal mode.
    //
    // Therefore: do a small, bounded convergence loop each frame.
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.SchedulerAndDispatcher");
        ScopedSectionTimer _tSched(profEnabled, &prof.SchedulerMs);

        constexpr int kMaxDrainIterations = 4;
        for (int iter = 0; iter < kMaxDrainIterations; ++iter)
        {
            std::size_t ranScheduler = 0;
            if (runInteractive && m_Scheduler)
            {
                ranScheduler = m_Scheduler->ProcessDue();
            }

            std::size_t pendingBefore = 0;
            if (m_Dispatcher)
            {
                pendingBefore = m_Dispatcher->PendingCount();
                if (pendingBefore)
                {
                    m_Dispatcher->Drain();
                }
            }

            const std::size_t pendingAfter = m_Dispatcher ? m_Dispatcher->PendingCount() : 0;
            if (ranScheduler == 0 && pendingBefore == 0 && pendingAfter == 0)
            {
                break;
            }
        }
    }

    // Attach/detach events for every tree change since the last settle. Placed HERE for
    // three reasons: after m_HotReload->Pump() above, so a reconcile has finished and is
    // judged as the single tree edit it is rather than mid-flight; after the
    // scheduler/dispatcher drain, so a deferred RemoveChild has already run and the tree
    // the settle reads is the one this frame will lay out; and before both of Update's
    // early-return gates below (the idle gate and the pointer-only gate), which would
    // otherwise strand the queue for as long as the UI sits still — precisely the frames
    // after a HUD is built.
    SettleAttachTransitions();

    bool dockspaceRebuiltBeforeLayout = false;

    // Apply any pending dockspace rebuild before building Yoga so layout
    // reflects changes requested by UI controls in the previous frame.
    if (runInteractive && m_RebuildDockspace && m_PendingDockspaceRebuild)
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.DockspaceRebuild");
        auto resetPrimitiveGenFrames = [&](auto& self, UIElement* el) -> void {
            if (!el)
                return;
            el->m_LastPrimitiveGenFrame = 0;
            for (const auto& child : el->GetChildren())
                self(self, child.get());
            if (UIElement* target = el->GetMountTarget())
                self(self, target);
        };

        resetPrimitiveGenFrames(resetPrimitiveGenFrames, m_PendingDockspaceRebuild);
        // Rebuilding the dockspace destroys and recreates large portions of the UI tree.
        // Drop retained Yoga nodes up-front so we don't hold stale UIElement* keys.
        ResetRetainedYogaTree();
        m_PendingDockspaceRebuild->RebuildFromModel();
        InvalidateSheetSetSubtree(m_PendingDockspaceRebuild);
        resetPrimitiveGenFrames(resetPrimitiveGenFrames, m_PendingDockspaceRebuild);
        dockspaceRebuiltBeforeLayout = true;
        m_PendingDockspaceRebuild = nullptr;
        m_RebuildDockspace = false;
        // Safety: input state may reference elements in the rebuilt subtree
        m_Hovered = nullptr;
        m_HoveredInstanceId = 0;
        m_MouseCaptured = false;
        m_CaptureElement = nullptr;
        m_CaptureInstanceId = 0;
        m_CaptureId.clear();
    }

    // Non-RAII scope for the "PreBuild" phase — the prep work between DockspaceRebuild and the
    // first BuildYoga invocation (hover recovery, viewport/content-scale checks,
    // stylesheet flat-table construction). Released explicitly before the first BuildYoga scope
    // so BuildYoga becomes a sibling in the call tree, not a child. unique_ptr is used because
    // the code in between declares many locals that are used later in the function, ruling out a
    // plain `{}` block wrapper.
    auto preBuildScope =
        std::make_unique<::GameEngine::Profiling::ScopedCpuProfile>("UIManager.Update.PreBuild");

    // Harden hover/capture pointers: UI controls (inspector, virtualization, docking) may rebuild
    // large subtrees between frames. Never dereference stale UIElement* during CSS/style passes.
    // Recover by instanceId (or id string for capture) if possible, otherwise clear safely.
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.HoverRecovery");
        UIElement* root = GetRootElement();
        if (!root)
        {
            m_Hovered = nullptr;
            m_HoveredInstanceId = 0;
            m_MouseCaptured = false;
            m_CaptureElement = nullptr;
            m_CaptureInstanceId = 0;
            m_CaptureId.clear();
        }
        else if (const uint64_t recoveryTreeGen = m_TreeStructureGeneration.load(std::memory_order_relaxed);
                 recoveryTreeGen != m_HoverRecoveryTreeGen)
        {
            // Instance-id resolution is an O(tree) walk. Cached pointers can
            // only go stale through a structural change (child add/remove,
            // Mount target swap), and every structural change bumps
            // m_TreeStructureGeneration — so the walk is skipped while the
            // generation holds still.
            m_HoverRecoveryTreeGen = recoveryTreeGen;
            if (m_HoveredInstanceId != 0)
            {
                UIElement* h = FindElementByInstanceId(m_HoveredInstanceId);
                if (h)
                {
                    m_Hovered = h;
                }
                else
                {
                    m_Hovered = nullptr;
                    m_HoveredInstanceId = 0;
                }
            }
            else
            {
                m_Hovered = nullptr;
            }

            if (m_MouseCaptured)
            {
                UIElement* cap = nullptr;
                if (m_CaptureInstanceId != 0)
                {
                    cap = FindElementByInstanceId(m_CaptureInstanceId);
                }
                if (!cap && !m_CaptureId.empty())
                {
                    cap = root->FindById(m_CaptureId);
                }
                if (cap)
                {
                    m_CaptureElement = cap;
                    m_CaptureInstanceId = cap->GetInstanceId();
                }
                else
                {
                    m_MouseCaptured = false;
                    m_CaptureElement = nullptr;
                    m_CaptureInstanceId = 0;
                    m_CaptureId.clear();
                }
            }
        }
    }

    // Fire the low-frequency periodic-refresh callbacks (VCS/ECS-stat panels)
    // before the idle gate. A callback that mutates the tree raises dirty hints
    // that fold into dirtyNow just below, so the change renders this frame; an
    // unchanged tick mutates nothing and the frame still idles. This is what
    // lets an input-idle / IPC-only editor keep externally-polled panels fresh
    // (OnPostLayout, the previous driver, never fires on idle frames).
    PumpPeriodicRefresh();

    // Carry dirtyPrev (the frame-start exchange(0) at the top of Update) into dirtyNow. Without
    // it, a LayoutDirty/ChildrenDirty hint raised on the previous frame (e.g. a control resizing
    // itself in an event handler) is lost here -- the layout-signature gate below keys off
    // dirtyNow, so the deferred relayout never solves and the change does not take effect until
    // some other input forces a solve.
    unsigned dirtyNow = dirtyPrev | m_DirtyHintFlags.load(std::memory_order_relaxed);
    bool forceLayoutSignaturesThisFrame = false;

    const bool globalStylesheetSetChanged = (m_AppliedStylesheetSetGeneration != m_StylesheetSetGeneration);
    const bool debugCaptureActive = m_DebugCaptureEnabled && m_DebugCaptureOut.is_open();
    const bool anyInput = m_MouseMoved || m_ScrollWheelMoved || m_ScrollOffsetsChanged ||
                          m_ButtonEdgeSinceLastUpdate || m_KeyInputSinceLastUpdate;
    uint32_t viewportW = 0u;
    uint32_t viewportH = 0u;
    {
        constexpr uint32_t kDefaultViewportW = 1280u;
        constexpr uint32_t kDefaultViewportH = 720u;
        viewportW = kDefaultViewportW;
        viewportH = kDefaultViewportH;
        if (m_LayoutSizeOverrideW > 0 && m_LayoutSizeOverrideH > 0)
        {
            // A host compositing the UI into a sub-window target (GameUIHost over a
            // panel-sized FinalColor) sizes layout to that extent, not the swapchain.
            // Honored here so it works in every build, not just the fast path.
            viewportW = m_LayoutSizeOverrideW;
            viewportH = m_LayoutSizeOverrideH;
        }
        else
        {
            m_Device->GetSwapchainSize(viewportW, viewportH);
            if (viewportW == 0 || viewportH == 0)
            {
                if (m_LastLayoutWidth > 0 && m_LastLayoutHeight > 0)
                {
                    viewportW = m_LastLayoutWidth;
                    viewportH = m_LastLayoutHeight;
                }
                else
                {
                    viewportW = kDefaultViewportW;
                    viewportH = kDefaultViewportH;
                }
            }
        }
    }
    const bool viewportChanged = (viewportW != m_LastLayoutWidth || viewportH != m_LastLayoutHeight);

    // Resolve target scaling (physical px per CSS logical px) once per frame.
    // m_LastLayoutWidth/Height stay in physical pixels for rendering; Yoga receives
    // logical dimensions (physical / scale) so that CSS px maps to the correct
    // physical size on HiDPI displays.
    const float contentScale = m_ScaleSettings.Resolve(
        viewportW, viewportH, m_Platform ? m_Platform->GetContentScale() : 1.0f);
    const bool contentScaleChanged = std::abs(contentScale - m_ContentScale) > 1.0e-3f;
    m_ContentScale = contentScale;

    // Put Yoga's rounding grid on this frame's scale before anything builds a
    // node or solves. Layout carries values that are not whole logical pixels —
    // text sized in device px, CSS lengths at a fractional scale — and the
    // default grid rounds them away (YogaAdapter::SetContentScale). Set here
    // rather than beside the solve: node creation reads the config too, and
    // several solve sites share this one frame.
    UILayout::YogaAdapter::SetContentScale(contentScale);

    // When the user changes Additional UI scale, the logical viewport handed
    // to Yoga changes (physical / scale). contentScaleChanged forces the full
    // relayout this frame (via computeLayoutSignaturesAll below and the
    // pointer-only gate's env check) so the next paint uses correctly-sized
    // elements — otherwise we paint the previous layout at the new scale,
    // which shows as a brief visual zoom until the next dirty event re-solves.

    // C-8: pump virtualized-control provider versions BEFORE the gates. A
    // MarkChanged that landed since the last frame enqueues DataChanged work
    // here, so HasPending() declines the idle gate below and the heavy pass
    // services the rebind the same frame — an idle-gated panel no longer waits
    // for a scroll/viewport event to repaint. No-op (one u64 compare per
    // registered control) when nothing changed, so it never breaks the idle gate.
    PumpVirtualizedControlChanges();

    // Mode-0 idle gate: a frame with no input and nothing pending anywhere in
    // the pipeline has no Update-side work at all — skip straight to the
    // tooltip/cursor tail. Tooltip delay timers still advance there (a
    // tooltip firing mutates the tree, which declines the gate next frame).
    // The render side pairs with this: ResolveStyles and the SSBO uploads are
    // gated on their own dirty state, so an idle UI does no per-frame tree
    // walks and uploads zero bytes.
    //
    // Host-agnostic: passive (non-interactive) frames take this same early-out.
    // A passive host (Player/Game-View HUD, editor background tool window) feeds
    // no input and marks style/layout dirty exactly like the interactive path,
    // so a clean passive frame has nothing to build/cascade/solve either. Every
    // decline condition below is interactivity-neutral, so a passive frame with
    // real work pending (dirty tree, active transitions, pending font resolve,
    // viewport change, queued virtualization) still declines and runs the heavy
    // pass — the C-8 pump above having already surfaced provider changes.
    const bool idleFrameTaken =
        TryIdleFrame(dirtyNow, viewportChanged || contentScaleChanged, debugCaptureActive);
    prof.IdleDecline = (uint32_t)m_IdleGateDecline;
    if (idleFrameTaken)
    {
        prof.IdleFrame = 1u;
        UpdateTooltipAndCursor(runInteractive);
        PublishUpdateProfile(prof, profEnabled, totalStart);
        return;
    }

    // Diagnostics for zero-input frames that reach the heavy pass — the
    // idle-gate analogue of GE_UI_HEAVY_INPUT_LOG below. Entries here while
    // the app sits untouched name the state that keeps frames heavy (e.g. a
    // control re-marking itself every frame). Enable with GE_UI_IDLE_LOG=1.
    static const bool s_IdleLog = []() -> bool
    {
        const char* e = std::getenv("GE_UI_IDLE_LOG");
        return (e && e[0] == '1');
    }();
    static int s_IdleLogBudget = 64;
    if (s_IdleLog && !anyInput && s_IdleLogBudget > 0)
    {
        --s_IdleLogBudget;
        static constexpr const char* kIdleDeclineNames[] = {
            "taken", "disabled", "viewport", "input", "dirtyTree", "relayoutOrRebuild",
            "virtualization", "treeGeneration", "stylesheetGeneration", "transitions",
            "dragDrop", "focusNotify", "fontResolve"};
        static_assert(sizeof(kIdleDeclineNames) / sizeof(kIdleDeclineNames[0]) ==
                          (size_t)IdleGateDecline::FontResolve + 1,
                      "kIdleDeclineNames must cover every IdleGateDecline value");
        Logger::Log::Warning(
            "[UI IdleGate] declined: gate={} dirtyNow={} treeGen={}/{} virtPending={} transitions={}",
            kIdleDeclineNames[(size_t)m_IdleGateDecline],
            dirtyNow,
            m_TreeStructureGeneration.load(std::memory_order_relaxed),
            m_LastBuiltStructureGeneration,
            m_VirtualizationCoordinator.HasPending() ? 1 : 0,
            (m_TransitionEngine && m_TransitionEngine->HasActiveTransitions()) ? 1 : 0);
    }

    // Pointer-only frame gate: a bare mouse move over an otherwise-clean tree
    // is fully serviced here — hit test, pseudo-state re-bake (render-side
    // drain repaints), enter/leave + mouse-move dispatch — skipping
    // BuildYoga/cascade/solve entirely. Everything else falls through to the
    // heavy pass.
    const PointerFrameResult pointerFrame = TryPointerOnlyFrame(
        dirtyNow, runInteractive,
        viewportChanged || contentScaleChanged || debugCaptureActive,
        profEnabled, prof);
    if (pointerFrame == PointerFrameResult::Handled)
    {
        UpdateTooltipAndCursor(runInteractive);
        PublishUpdateProfile(prof, profEnabled, totalStart);
        return;
    }
    if (pointerFrame == PointerFrameResult::NotApplicable)
    {
        // Early pointer pre-pass: advance m_Hovered and mark hover/:active
        // transitions BEFORE the cascade below, so this frame's single style
        // resolution runs with current pointer state (and layout-affecting
        // pseudo styles solve this frame). Any marks it raises are folded into
        // dirtyNow here, ahead of the hint-clear below that consumes them.
        if (PreProcessPointerState(runInteractive))
            dirtyNow |= m_DirtyHintFlags.load(std::memory_order_relaxed);
    }
    else
    {
        // Escalated: pointer state is already resolved and events may already
        // be dispatched (ProcessHoverChain / DispatchEvents dedup via the
        // pre-pass bookkeeping). Fold the gate's marks into this frame's
        // dirty set and run the heavy pass.
        dirtyNow |= m_DirtyHintFlags.load(std::memory_order_relaxed);
    }

    // Diagnostics for input frames that reach the heavy pass. With the
    // pointer-only gate above, a bare mouse move should be serviced before
    // this point — an entry with mouseMoved=1 and every other field 0 means
    // the gate declined a frame it should have taken.
    // Enable by setting GE_UI_HEAVY_INPUT_LOG=1.
    static const bool s_HeavyInputLog = []() -> bool
    {
        const char* e = std::getenv("GE_UI_HEAVY_INPUT_LOG");
        return (e && e[0] == '1');
    }();
    static int s_HeavyInputLogBudget = 32;
    if (s_HeavyInputLog && anyInput && s_HeavyInputLogBudget > 0)
    {
        --s_HeavyInputLogBudget;
        static constexpr const char* kGateDeclineNames[] = {
            "taken", "notPointerFrame", "dirtyTree", "relayoutOrRebuild", "virtualization",
            "treeGeneration", "stylesheetGeneration", "transitions", "dragDrop",
            "focusNotify", "fontResolve", "escalatedMark", "escalatedDispatch"};
        static_assert(sizeof(kGateDeclineNames) / sizeof(kGateDeclineNames[0]) ==
                          (size_t)PointerGateDecline::EscalatedDispatch + 1,
                      "kGateDeclineNames must cover every PointerGateDecline value");
        const std::string hoverTagStr = m_Hovered ? UIAttributeAccess::GetDebugTypeName(*m_Hovered) : std::string();
        const char* hoverTag = m_Hovered ? hoverTagStr.c_str() : "<null>";
        const char* hoverId = "<null>";
        if (m_Hovered)
            hoverId = m_Hovered->GetId().empty() ? "<no-id>" : m_Hovered->GetId().c_str();
        Logger::Log::Warning(
            "[UI HeavyInput] mouseMoved={} mouseEdge={} keyInput={} captured={} dirtyNow={} gate={} hovered(tag='{}' id='{}')",
            m_MouseMoved ? 1 : 0,
            m_ButtonEdgeSinceLastUpdate ? 1 : 0,
            m_KeyInputSinceLastUpdate ? 1 : 0,
            m_MouseCaptured ? 1 : 0,
            dirtyNow,
            kGateDeclineNames[(size_t)m_PointerGateDecline],
            hoverTag,
            hoverId);
    }


    // Coalesce font-resolution invalidations: while fonts stream in, both
    // gates above decline (m_FontResolvedDirty), so every install lands in a
    // heavy pass whose BuildYoga measure-input diff picks up the new atlas.
    // Cleared only once the in-flight batch is complete AND a heavy pass is
    // actually running — clearing before the gates would let the batch's
    // final install be idle-gated away without ever reflowing.
    if (m_FontResolvedDirty && m_FontFamilyInFlight.empty())
    {
        m_FontResolvedDirty = false;
    }

    // We are about to do a heavy pass that will handle all outstanding dirty. Clear any dirty hints
    // observed so far; new dirty marks during this pass will be preserved for the next frame.
    m_DirtyHintFlags.store(0u, std::memory_order_relaxed);
    m_MouseMoved = false;
    m_ScrollWheelMoved = false;
    m_ScrollOffsetsChanged = false;

    // Snapshot viewport size for rendering (physical pixels).
    // Yoga layout uses logical dimensions (ctx.viewportW/H) instead.
    m_LastLayoutWidth = viewportW;
    m_LastLayoutHeight = viewportH;
    // PERF gate for the layout-signature pass. See UpdateContext's layout-signature
    // doc block (UIManager_Internal.h) for the invariants; don't widen these flags
    // without reading it — Fix A (a3355481) measured a 2.4× regression from a
    // single extra `||`.
    const bool relayoutRequestedPre = ConsumeRelayoutRequest();
    const bool computeLayoutSignaturesAll =
        viewportChanged ||
        contentScaleChanged ||
        globalStylesheetSetChanged;
    bool computeLayoutSignaturesThisFrame =
        computeLayoutSignaturesAll ||
        relayoutRequestedPre ||
        forceLayoutSignaturesThisFrame ||
        ((dirtyNow & (UIElement::LayoutDirty | UIElement::ChildrenDirty)) != 0u);

    // Build Yoga tree and collect nodes for layout and event dispatch.
    // NOTE: all containers in this hot path are static thread_local to retain
    // capacity across frames and avoid per-frame heap allocations entirely.
    // The PMR scratch allocator was removed because MSVC's monotonic_buffer_resource
    // consistently routes through the upstream (operator new) regardless of available
    // buffer space, negating the benefit of the pre-allocated arena.
    // Flat-table layout for effective stylesheet sets (sheets + aligned rule indices).
    // All sheet and index pointers live in two contiguous flat buffers; each unique
    // set is a {offset, count} range into them.  Elements share sets by index,
    // avoiding per-element vector allocations entirely.
    //
    // PERF: static thread_local vectors retain capacity across frames, eliminating
    // all heap allocations after the first heavy-path frame.
    static thread_local std::vector<const Stylesheet*> flatSheets;
    // Step 4: parallel-indexed handles. Mirrors flatSheets so the
    // interner can be fed handle spans (it now stores keep-alives).
    static thread_local std::vector<StylesheetHandle> flatHandles;
    static thread_local std::vector<const StylesheetRuleIndex*> flatIndices;
    static thread_local std::vector<SheetSetRef> sheetSetRefs;
    flatSheets.clear();
    flatHandles.clear();
    flatIndices.clear();
    sheetSetRefs.clear();

    // Helper: look up a SheetSetRef by index (-1 = none).
    auto getSheetSpan = [&](int32_t idx) -> std::span<const Stylesheet* const>
    {
        if (idx < 0)
            return {};
        const auto& ref = sheetSetRefs[(size_t)idx];
        return {flatSheets.data() + ref.offset, ref.count};
    };
    auto getIndexSpan = [&](int32_t idx) -> std::span<const StylesheetRuleIndex* const>
    {
        if (idx < 0)
            return {};
        const auto& ref = sheetSetRefs[(size_t)idx];
        return {flatIndices.data() + ref.offset, ref.count};
    };

    // Consume the force-rebuild fallback flag at the TOP of Update.
    // Setters in prior frames (ResetRetainedYogaTree, RequestFullRebuild)
    // need the flag to survive across the frame boundary.
    m_ForceFullRebuildNextFrame = false;

    // Debug: track pixel sizes used across all text runs this frame (when typing)
    static thread_local std::vector<unsigned> dbgPixelSizes;
    dbgPixelSizes.clear();

    // Track the set of stylesheets currently attached/active so we can
    // (a) cache dynamic pseudo-state analysis and
    // (b) avoid work when hover/focus changes cannot affect style.
    //
    // PERF: reuse the hash table across frames to avoid per-frame allocations.
    static thread_local std::unordered_set<const Stylesheet*> analysisSheets;
    analysisSheets.clear();
    analysisSheets.reserve(m_GlobalStylesheets.size() + 32);
    uint64_t analysisSheetHash = 0;

    // These will be aliased via UpdateContext (constructed later).
    // Defined here so they're available for the ctx aggregate init.
    bool anyLayoutSignatureChanged = false;
    bool anyChildrenDirty = false;
    bool didSolveLayout = false;
    uint32_t cssRecomputeWhileSignaturePassDisabled = 0;

    // Dirty flag counters observed during Yoga build (for optional spike diagnostics).
    uint32_t dirtyStyleSeen = 0;
    uint32_t dirtyLayoutSeen = 0;
    uint32_t dirtyVisualSeen = 0;
    uint32_t dirtyChildrenSeen = 0;

    // Effective stylesheet list propagated top-down. Start from global sheets.
    // Stored as StylesheetHandles (shared_ptrs) so the Stylesheet objects remain alive
    // for the entire Update() frame even if m_GlobalStylesheets is modified mid-frame
    // (e.g. by a hot-reload ReplaceGlobalStylesheetBlock call during event dispatch).
    static thread_local std::vector<StylesheetHandle> rootSheets;
    rootSheets.clear();
    for (const StylesheetHandle& styleHandle : m_GlobalStylesheets)
    {
        if (styleHandle)
        {
            if (analysisSheets.insert(styleHandle.get()).second)
                analysisSheetHash ^= HashPtr64(styleHandle.get());
            rootSheets.push_back(styleHandle);
        }
    }
    // Subtree-attached sheets (e.g. a game-UI HUD .css attached per-entity via
    // AttachStyleToSubtreeFromAsset) live on element-local sheet lists, not in
    // m_GlobalStylesheets. They otherwise reach the analysis ONLY via the Yoga
    // walk's AddSheetForAnalysis, which the subtree-skip fast path bypasses on
    // clean frames — leaving hover/active.affectsPaint wrongly false and silently
    // dropping :hover/:active for those subtrees. Seed them unconditionally here,
    // mirroring globals, so analysisSheets (and its hash) is complete + stable
    // every frame regardless of the skip gate. NOT pushed to rootSheets: they
    // apply only to their own subtree (per-element merge), not the global root set.
    for (UIElement* sheetEl : m_LocalSheetElements)
    {
        if (!sheetEl)
            continue;
        for (const StylesheetHandle& sheetHandle : sheetEl->GetStylesheets())
        {
            if (sheetHandle && analysisSheets.insert(sheetHandle.get()).second)
                analysisSheetHash ^= HashPtr64(sheetHandle.get());
        }
    }
    const bool forceGlobalStyle =
        (m_AppliedStylesheetSetGeneration != m_StylesheetSetGeneration) ||
        (m_AppliedStylesheetContentGeneration != m_StylesheetContentGeneration);

    // Create the root (global) stylesheet set.
    {
        SheetSetRef rootRef;
        rootRef.offset = (uint32_t)flatSheets.size();
        rootRef.count = (uint32_t)rootSheets.size();
        for (const StylesheetHandle& sh : rootSheets)
        {
            flatSheets.push_back(sh.get());
            flatHandles.push_back(sh);                     // Step 4: parallel handle
            flatIndices.push_back(GetOrBuildRuleIndex(sh.get()));
        }
        sheetSetRefs.push_back(rootRef);
    }
    const int32_t rootSheetSetIdx = 0; // first entry

    // PERF: avoid std::function recursion overhead/allocations in this hot path.
    // Use a recursive lambda (Y-combinator style) instead.

    // Thread-local containers needed by UpdateContext and layout helper methods.
    // Declared here (before ctx) so they're in scope; static thread_local retains capacity.
    //
    // Stage 7 step 7.1/7.2: nodeIndexByYG / nodeIndexByElement /
    // nodesByInstanceId all removed. The first two were write-only;
    // virtualization drain (the only nodesByInstanceId reader) now resolves
    // via UIManager::FindElementByInstanceId (tree walk).
    static thread_local std::vector<ScrollView*> scrollViews;
    static thread_local std::vector<LayoutOverridePatchRec> layoutOverrideNodes;

    // Reset the text measure pool for this frame (pool lives in UIManager_Layout.cpp).
    GetTextMeasureCtxUsed() = 0;
    TrimTextMeasureCtxPool();

    // Hover chain scratch vectors: thread_local retains capacity across frames.
    static thread_local std::vector<UIElement*> s_HoverLeft;
    static thread_local std::vector<UIElement*> s_HoverEntered;
    s_HoverLeft.clear();
    s_HoverEntered.clear();

    // Construct UpdateContext for methods extracted from Update().
    // Must be after stylesheet setup (rootSheetSetIdx, forceGlobalStyle, etc. are set).
    UpdateContext ctx{
        flatSheets, flatHandles, flatIndices, sheetSetRefs, rootSheets, rootSheetSetIdx,
        analysisSheets, analysisSheetHash,
        scrollViews, layoutOverrideNodes,
        dirtyStyleSeen, dirtyLayoutSeen, dirtyVisualSeen, dirtyChildrenSeen, anyChildrenDirty,
        computeLayoutSignaturesThisFrame, computeLayoutSignaturesAll, relayoutRequestedPre, anyLayoutSignatureChanged, cssRecomputeWhileSignaturePassDisabled,
        didSolveLayout, forceGlobalStyle,
        // Yoga receives logical (CSS) pixels; layout rects are scaled back to physical after solve.
        (uint32_t)std::lround((float)viewportW / contentScale),
        (uint32_t)std::lround((float)viewportH / contentScale),
        contentScale,
        GetTextMeasureCtxUsed(),
        {}, false, nullptr, s_HoverLeft, s_HoverEntered, 0, false, false, runInteractive,
        profEnabled, &prof
    };


    // End the PreBuild scope so BuildYoga appears as a sibling in the call tree.
    preBuildScope.reset();

    // Snapshot the structure-change counter BEFORE BuildYoga so we can tell
    // at end-of-Update whether anything structural happened. If the counter
    // didn't advance (and layout wasn't solved), the node list is shape-
    // identical to the previous frame — the derived maps and clip caches
    // from last frame are still valid and the rebuild can be skipped entirely.
    const uint64_t structCountBeforeBuild = ctx.subtreeStructureChanges;

    BuiltNode rootBuilt{};
    YGNodeRef rootNode = nullptr;
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.BuildYoga.Initial");
        ScopedSectionTimer _tBuild(profEnabled, &prof.BuildYogaMs);
        const auto tStart = ProfClock::now();
        rootBuilt = BuildYogaRecursive(rootEl, nullptr, 0, rootSheetSetIdx, ctx, /*shareCtx=*/nullptr);
        rebuildInitialMs = std::chrono::duration<double, std::milli>(ProfClock::now() - tStart).count();
        rootNode = rootBuilt.node;
    }
    // Snapshot the tree generation at the end of the build so the next
    // Update can detect mid-frame mutations vs. truly idle frames.
    m_LastBuiltStructureGeneration = m_TreeStructureGeneration.load(std::memory_order_relaxed);

    // PERF: resolving virtualization work items by instanceId should be O(1).
    // Build a per-frame lookup table so DrainOnce doesn't scan nodes linearly.
    // (nodesByInstanceId declared earlier for UpdateContext.)

    // PERF: avoid repeated RTTI scans over all nodes. Build typed lists once per Yoga build.
    // (scrollViews and layoutOverrideNodes declared earlier for UpdateContext.)

    RebuildTypedNodeLists(ctx);

    // Cache ScrollView instanceIds for scroll-only retained updates.
    // Safe because instanceIds are stable; we revalidate against treeStructureGeneration.
    {
        m_CachedScrollViewInstanceIds.clear();
        m_CachedScrollViewInstanceIds.reserve(scrollViews.size());
        for (ScrollView* sv : scrollViews)
        {
            if (!sv)
                continue;
            const std::uint64_t id = sv->GetInstanceId();
            if (id != 0)
                m_CachedScrollViewInstanceIds.push_back(id);
        }
        m_CachedScrollViewTreeGen = m_TreeStructureGeneration.load(std::memory_order_relaxed);
    }

    // Shared helpers: keep the "flush scroll callbacks + drain virtualization" pattern consistent
    // across phases, and avoid duplicating resolver/iteration code.

    auto drainVirtualizationQueueBounded = [&](int maxIterations) -> bool
    {
        bool anyRan = false;
        // Stage 7 step 7.1: virtualization resolves elements via UIManager::
        // FindElementByInstanceId (a tree walk). The previous nodesByInstanceId
        // map was an O(1) cache rebuilt every frame; the walk is cheap enough
        // (few k entries max) that the per-frame map maintenance was net loss
        // for this drain path.
        for (int it = 0; it < maxIterations; ++it)
        {
            if (!m_VirtualizationCoordinator.HasPending())
                break;
            if (!m_VirtualizationCoordinator.DrainOnce(
                    *this,
                    /*userCtx=*/nullptr,
                    +[](void* /*userCtx*/, UIManager& ui, std::uint64_t id) -> UIElement*
                    {
                        UIElement* el = ui.FindElementByInstanceId(id);
                        if (!el)
                            return nullptr;
                        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
                            return nullptr;
                        return el;
                    }))
                break;
            anyRan = true;
        }
        return anyRan;
    };

    // Option A (phase separation): flush deferred ScrollView callbacks and drain virtualization work
    // before the first Yoga solve so layout sees a stable, up-to-date tree.
    //
    // NOTE: We only have the per-frame 'nodes' list after buildYoga(), so we flush callbacks here.
    bool preSolveChildrenDirty = false;
    bool preSolveLayoutDirty = false;
    bool preSolveRelayoutRequested = false;
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.PreSolveDrain");
        ScopedSectionTimer _tPreSolve(profEnabled, &prof.PreSolveDrainMs);
        const uint64_t treeGenBeforePreSolve = m_TreeStructureGeneration.load(std::memory_order_relaxed);
        bool preSolveAnyScrollCallbacksFlushed = false;
        bool preSolveAnyVirtualizationRan = false;

        constexpr int kMaxVirtualizationDrainIterationsPreSolve = 4;

        // Attribute the tree-gen delta to FlushScrollCallbacks vs the virtualization drain
        // so we can tell which one is forcing the AfterPreSolveDrain rebuild. Costs one
        // atomic load per checkpoint — negligible.
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.PreSolveDrain.FlushScrollCallbacks");
            const auto t0 = ProfClock::now();
            preSolveAnyScrollCallbacksFlushed = preSolveAnyScrollCallbacksFlushed || FlushScrollCallbacks(ctx);
            preSolveFlushCallbacksMs += std::chrono::duration<double, std::milli>(ProfClock::now() - t0).count();
        }
        const uint64_t treeGenAfterFlush = m_TreeStructureGeneration.load(std::memory_order_relaxed);

        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.PreSolveDrain.VirtualizationDrain");
            const auto t0 = ProfClock::now();
            preSolveAnyVirtualizationRan = preSolveAnyVirtualizationRan || drainVirtualizationQueueBounded(kMaxVirtualizationDrainIterationsPreSolve);
            preSolveVirtualizationMs += std::chrono::duration<double, std::milli>(ProfClock::now() - t0).count();
        }
        const uint64_t treeGenAfterPreSolve = m_TreeStructureGeneration.load(std::memory_order_relaxed);

        // Walk the tree to detect any layout-affecting dirty marks set during the
        // drain (e.g. virtualized cell rebinds via BindCell -> MarkDirtySubtree).
        // BindCell writes new Width/Height/Position overrides on cell wrappers
        // but doesn't bump m_TreeStructureGeneration, so a topology-only trigger
        // would let the upcoming Yoga solve run with stale node styles. Probing
        // here lets us include layout-dirty as a rebuild trigger below. Style-
        // only dirties are intentionally NOT a trigger (paint-only changes
        // don't need a Yoga rebuild — see same convention in the late-relayout
        // probe at the end of this function).
        //
        // IMPORTANT: gate the probe on "virt or scroll-callback actually ran".
        // Otherwise stale LayoutDirty/ChildrenDirty flags from previous frames
        // (e.g. display:none-pruned subtrees BuildYogaRecursive didn't visit,
        // or behind a Mount whose target wasn't reached) can fire the rebuild
        // trigger every frame on idle and burn redundant Yoga rebuilds.
        //
        // Bail early once both flags are set; the walk prunes subtrees lacking
        // SubtreeDirty so it stays cheap.
        if (preSolveAnyVirtualizationRan || preSolveAnyScrollCallbacksFlushed)
        {
            auto dirtyProbe = [&](auto& self, UIElement* el) -> void {
                if (!el) return;
                if (preSolveChildrenDirty && preSolveLayoutDirty) return;
                if (el->IsDirty(UIElement::ChildrenDirty))
                    preSolveChildrenDirty = true;
                if (el->IsDirty(UIElement::LayoutDirty))
                    preSolveLayoutDirty = true;
                if (!el->IsDirty(UIElement::SubtreeDirty))
                    return;
                for (const auto& ch : el->GetChildren())
                    self(self, ch.get());
                if (el->Kind() == UIElementKind::Mount)
                    if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                        self(self, tgt);
            };
            dirtyProbe(dirtyProbe, rootEl);
        }
        preSolveRelayoutRequested = m_RequestRelayout;

        // If virtualization (or any scroll callback) mutated topology OR set
        // layout-affecting dirties on existing nodes (BindCell rebinds), rebuild
        // the Yoga tree now so the upcoming solve sees up-to-date node styles.
        // Without this, e.g. wheel-scrolling a virtualized list rebinds cells
        // with new Width/Height/Position overrides that never reach Yoga,
        // leaving cells rendered at the previous frame's metrics.
        const bool topologyMutatedDuringPreSolve = (treeGenAfterPreSolve != treeGenBeforePreSolve);
        const bool layoutDirtyDuringPreSolve = (preSolveChildrenDirty || preSolveLayoutDirty);
        if (topologyMutatedDuringPreSolve || layoutDirtyDuringPreSolve)
        {
            rebuildAfterPreSolveFired = true;
            rebuildAfterPreSolveByFlush = (treeGenAfterFlush != treeGenBeforePreSolve);
            rebuildAfterPreSolveByVirt = topologyMutatedDuringPreSolve
                                             ? (treeGenAfterPreSolve != treeGenAfterFlush)
                                             : true;

            ResetFlatSheetTable(ctx, *this);
            {
                GE_CPU_PROFILE_SCOPE("UIManager.Update.BuildYoga.AfterPreSolveDrain");
                ScopedSectionTimer _tBuild2(profEnabled, &prof.BuildYogaMs);
                const auto tStart = ProfClock::now();
                rootBuilt = BuildYogaRecursive(rootEl, nullptr, 0, rootSheetSetIdx, ctx, /*shareCtx=*/nullptr);
                rebuildAfterPreSolveMs = std::chrono::duration<double, std::milli>(ProfClock::now() - tStart).count();
            }
            rootNode = rootBuilt.node;
            RebuildTypedNodeLists(ctx);

            // Re-run one more bounded flush/drain pass in case newly-attached scroll views
            // or controls need to observe initial scroll/viewport state.
            {
                GE_CPU_PROFILE_SCOPE("UIManager.Update.PreSolveDrain.FlushScrollCallbacks");
                const auto t0 = ProfClock::now();
                preSolveAnyScrollCallbacksFlushed = preSolveAnyScrollCallbacksFlushed || FlushScrollCallbacks(ctx);
                preSolveFlushCallbacksMs += std::chrono::duration<double, std::milli>(ProfClock::now() - t0).count();
            }
            {
                GE_CPU_PROFILE_SCOPE("UIManager.Update.PreSolveDrain.VirtualizationDrain");
                const auto t0 = ProfClock::now();
                preSolveAnyVirtualizationRan = preSolveAnyVirtualizationRan || drainVirtualizationQueueBounded(kMaxVirtualizationDrainIterationsPreSolve);
                preSolveVirtualizationMs += std::chrono::duration<double, std::milli>(ProfClock::now() - t0).count();
            }
        }

        if (preSolveAnyVirtualizationRan || preSolveAnyScrollCallbacksFlushed)
        {
            // If any work ran here, it may have introduced intrinsic layout changes; ensure
            // signature contexts are ready and the initial solve can observe updated state.
            computeLayoutSignaturesThisFrame = true;
        }
    }

    // Snapshot-consume the virtualization impact accumulated up to (and by) the
    // pre-solve drain: the initial solve below services exactly this work, so
    // leaving it armed would re-fire the late-relayout gate for a second
    // whole-tree rebuild + solve per virtualized scroll tick. A point-in-time
    // clear after the solve would instead wipe impact raised DURING the solve
    // (OnPostLayout handlers inside ConvergePostLayout) or by focus callbacks —
    // snapshotting here keeps those armed for the late gate. If no solve ends
    // up running this frame, the snapshot is re-raised below.
    const uint32_t virtImpactPreSolve =
        m_VirtualizationImpactFlags.exchange(0u, std::memory_order_relaxed);

    if (forceGlobalStyle)
    {
        m_AppliedStylesheetSetGeneration = m_StylesheetSetGeneration;
        m_AppliedStylesheetContentGeneration = m_StylesheetContentGeneration;
    }

    // Refresh dynamic stylesheet analysis when stylesheet content changes or when
    // the attached stylesheet set changes (including subtree-attached sheets).
    analysisSheetHash ^= (uint64_t)analysisSheets.size() * 0x9e3779b97f4a7c15ULL;
    if (m_StyleAnalysisGeneration != m_StylesheetContentGeneration || m_StyleAnalysisSheetHash != analysisSheetHash)
        RefreshDynamicStyleAnalysis(analysisSheets, analysisSheetHash);

    NotifyFocusChange();

    ctx.focusIdForYoga = m_FocusId;
    ctx.focusViaKeyboardForYoga = m_FocusViaKeyboard;
    ctx.activeTargetForYoga = nullptr;
    if (m_MouseDown)
    {
        ctx.activeTargetForYoga = ActiveTarget();
    }

    const bool needInitialLayoutSolve =
        viewportChanged ||
        contentScaleChanged ||
        relayoutRequestedPre ||
        anyChildrenDirty ||
        anyLayoutSignatureChanged ||
        preSolveChildrenDirty ||
        preSolveLayoutDirty ||
        preSolveRelayoutRequested;

    {
        ScopedSectionTimer _tSolve(profEnabled, &prof.SolveAndApplyMs);
        SolveAndApplyLayout(rootNode, ctx, needInitialLayoutSolve);
    }

    // No solve ran, so the pre-solve impact snapshot was not serviced after
    // all — re-raise it for the late-relayout gate (conservative: matches the
    // pre-snapshot behavior for producers that notify without marking dirt).
    if (!needInitialLayoutSolve && virtImpactPreSolve != 0u)
        m_VirtualizationImpactFlags.fetch_or(virtImpactPreSolve, std::memory_order_relaxed);

    // Shared "nothing to do" signal for focus/indices/clips: when the tree
    // is structurally unchanged since the last build and layout wasn't
    // solved, every derived cache (focus order, per-node index maps, clip
    // rects, z-index) matches the previous frame verbatim. Scroll change
    // check is included in the indices/clips gate only — scroll shifts
    // layout rects, which affects clips but not focus order.
    const bool structureUnchangedSinceLastBuild =
        (ctx.subtreeStructureChanges == structCountBeforeBuild);

    {
        ScopedSectionTimer _tFocus(profEnabled, &prof.FocusOrderMs);
        // Skip focus-order rebuild when the tree is stable. Focus order is
        // derived from (tabIndex, isEnabled, isFocusable, visibility) × node
        // list. None of those change without a style/class/tree mutation
        // that bumps subtreeStructureChanges (class changes mark StyleDirty,
        // which forces a slow walk somewhere in the subtree).
        // Edge case: SetTabIndex()/SetFocusable() don't currently dirty
        // anything — if a user calls those without any other mutation, the
        // cached focus order will lag by one frame until something else
        // moves the counter. Acceptable trade for eliminating a 0.11 ms
        // rebuild on every idle frame; safe to tighten later by having
        // those setters bump a focus-dirty flag.
        const bool canSkipFocusOrder =
            m_IndicesAndClipsSkipEnabled &&  // reused as master skip knob
            structureUnchangedSinceLastBuild &&
            !needInitialLayoutSolve &&
            m_FocusOrderPrimed;
        if (!canSkipFocusOrder)
        {
            BuildFocusOrder(ctx);
            m_FocusOrderPrimed = true;
        }
    }

    {
        ScopedSectionTimer _tIndicesAndClips(profEnabled, &prof.IndicesAndClipsMs);
        ApplyScrollTransforms(ctx);
        // Clamp scroll offsets now that layout has converged and scroll transforms
        // are applied. This is deliberately separated from OnPostLayout (which fires
        // during intermediate convergence passes with potentially transient rects).
        ClampAllScrollOffsets(ctx);

        // Stage 7 step 6.1: clip cache is debug-overlay-only; gate on
        // debug capture being active. The legacy RebuildNodeIndices /
        // canSkipMaps gate (Stage 7 step 7.2) is gone — nodeIndexByYG /
        // nodeIndexByElement / nodesByInstanceId all retired.
        if (m_DebugCaptureEnabled)
            RebuildClipCaches();
    }

    ProcessHoverChain(ctx);

    ctx.treeGenBeforeEvents = m_TreeStructureGeneration.load(std::memory_order_relaxed);
    ctx.treeMutatedDuringEvents = false;
    ctx.treeRebuiltThisFrame = false;

    DispatchEvents(ctx);

    // Focus/active pseudo-state dirty marking is handled inside DispatchEvents.
    bool& treeMutatedDuringEvents = ctx.treeMutatedDuringEvents;
    bool& treeRebuiltThisFrame = ctx.treeRebuiltThisFrame;
    if (dockspaceRebuiltBeforeLayout)
        treeRebuiltThisFrame = true;

    // If the UI tree was mutated during event dispatch, rebuild Yoga/layout
    // from the now-stable tree state so we can still produce geometry for
    // this frame without using stale node pointers.
    if (treeMutatedDuringEvents)
    {
        treeRebuiltThisFrame = true;
        const bool hadCaptureRebuild = m_MouseCaptured;
        const std::string captureIdRebuild = m_CaptureId;

        // Tree mutated during event dispatch. Rebuild the Yoga view of the tree
        // using retained Yoga nodes (no per-frame create/free).
        ResetFlatSheetTable(ctx, *this);
        // No-op: text measure contexts are pooled (thread-local).

        // Safety: hover/capture may reference elements that were replaced.
        m_Hovered = nullptr;
        m_HoveredInstanceId = 0;
        m_MouseCaptured = false;
        m_CaptureElement = nullptr;
        m_CaptureInstanceId = 0;
        m_CaptureId = captureIdRebuild; // keep id so we can restore capture if possible

        // Rebuild Yoga tree using the current UI tree.
        // IMPORTANT: The decision to compute layout signatures was made at the start of Update,
        // but event dispatch can introduce layout-affecting changes (display toggles, new nodes,
        // text changes) in the same frame. When we rebuild the Yoga tree here, force the
        // signature/measure path so new retained nodes get correct Yoga styles + measure funcs.
        computeLayoutSignaturesThisFrame = true;
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.BuildYoga.AfterEventDispatch");
            ScopedSectionTimer _tBuild(profEnabled, &prof.BuildYogaMs);
            rootNode = BuildYogaRecursive(rootEl, nullptr, 0, rootSheetSetIdx, ctx, /*shareCtx=*/nullptr).node;
        }
        RebuildTypedNodeLists(ctx);
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.YogaSolve.Primary");
            ScopedSectionTimer _tYoga(profEnabled, &prof.YogaMs);
            YogaAdapter::CalculateLayout(rootNode, (float)ctx.viewportW, (float)ctx.viewportH);
        }
        didSolveLayout = true;

        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.CommitLayoutRects.Primary");
            CommitLayoutRects(rootEl, /*incremental=*/false);
        }

        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.ConvergePostLayout");
            ConvergePostLayout(rootNode, ctx);
        }
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.FinalizeSolve");
            FinalizeSolve(ctx);
        }

        // Rebuild tab focus order and hovered element using the rebuilt tree.
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.RebuildFocusOrder");
            m_FocusOrder.clear();
            struct FocusCandidate
            {
                std::string id;
                int tabIndex;
                size_t domOrder;
            };
            static thread_local std::vector<FocusCandidate> focusCandidatesRebuild;
            focusCandidatesRebuild.clear();
            // Tree walk (skipping display:none subtrees) collects focus
            // candidates in DFS order — domOrder mirrors the legacy
            // m_Nodes index for stable-sort ties.
            size_t domOrder = 0;
            auto walk = [&](auto& self, UIElement* el) -> void {
                if (!el) return;
                const ResolvedStyle& rs = el->GetResolvedStyle();
                if (rs.Layout.DisplayMode == DisplayMode::None)
                    return;
                if (!el->IsEnabled())
                    return;  // disabled: the element and its contents leave the tab order
                if (rs.Visual.Visible && el->IsFocusable())
                {
                    int tabIndex = el->GetTabIndex();
                    if (tabIndex >= 0)
                    {
                        std::string id = el->GetId();
                        if (id.empty())
                            id = EnsureElementId(el);
                        focusCandidatesRebuild.push_back({id, tabIndex, domOrder});
                    }
                }
                ++domOrder;
                for (const auto& ch : el->GetChildren())
                    self(self, ch.get());
                if (el->Kind() == UIElementKind::Mount)
                    if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                        self(self, tgt);
            };
            walk(walk, rootEl);
            std::stable_sort(focusCandidatesRebuild.begin(), focusCandidatesRebuild.end(), [](const FocusCandidate& a, const FocusCandidate& b)
                             {
                                 const auto rankA = (a.tabIndex > 0) ? 0 : 1;
                                 const auto rankB = (b.tabIndex > 0) ? 0 : 1;
                                 if (rankA != rankB) return rankA < rankB;
                                 if (rankA == 0 && a.tabIndex != b.tabIndex) return a.tabIndex < b.tabIndex;
                                 return a.domOrder < b.domOrder; });
            for (const auto& c : focusCandidatesRebuild)
                m_FocusOrder.push_back(c.id);
        }

        // Stage 7 step 7.2: RebuildNodeIndices retired. The post-rebuild
        // scope only refreshes scroll transforms + (debug-only) clip cache
        // now.
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.RebuildIndicesAndClips.Primary");

            // Rebuild per-node parent indices + effective overflow clip for the
            // rebuilt tree so hit-testing and geometry generation use up-to-date
            // layout rectangles.
            // NOTE: Layout rects were just rewritten from Yoga; re-apply scroll
            // translations before computing overflow clips.
            ApplyScrollTransforms(ctx);
            ClampAllScrollOffsets(ctx);
            if (m_DebugCaptureEnabled)
                RebuildClipCaches();
        }

        // Post-rebuild rehover: re-derive m_Hovered from a fresh hit walk
        // because layout rects (and the elements themselves) may have moved.
        // requirePointerEvents=false because some callers (legacy splitter
        // capture restoration, in particular) rely on rehover snapping back
        // to elements that have pointer-events disabled.
        UIElement* newHoveredRebuild = IsMousePositionKnown()
            ? HitTestTree(m_MouseX, m_MouseY, /*requirePointerEvents=*/false)
            : nullptr;
        m_Hovered = newHoveredRebuild;
        m_HoveredInstanceId = newHoveredRebuild ? newHoveredRebuild->GetInstanceId() : 0;

        // Restore capture if the captured element still exists. This prevents
        // splitter drags from being interrupted by rebuilds (e.g., GridView column changes).
        if (hadCaptureRebuild && m_MouseDown && !captureIdRebuild.empty())
        {
            UIElement* restored = nullptr;
            auto findById = [&](auto& self, UIElement* el) -> void {
                if (!el || restored) return;
                if (el->GetId() == captureIdRebuild)
                {
                    restored = el;
                    return;
                }
                for (const auto& ch : el->GetChildren())
                    self(self, ch.get());
                if (el->Kind() == UIElementKind::Mount)
                    if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                        self(self, tgt);
            };
            findById(findById, rootEl);
            if (restored)
            {
                m_MouseCaptured = true;
                m_CaptureElement = restored;
                m_CaptureInstanceId = restored ? restored->GetInstanceId() : 0;
                m_CaptureId = captureIdRebuild;
            }
            else
            {
                m_MouseCaptured = false;
                m_CaptureElement = nullptr;
                m_CaptureInstanceId = 0;
                m_CaptureId.clear();
            }
        }
        else
        {
            // If we cannot restore safely, clear capture state.
            if (!hadCaptureRebuild)
                m_CaptureId.clear();
        }
        treeMutatedDuringEvents = false;
    }

    // Check for control-issued dockspace rebuild requests via tree walk.
    if (runInteractive && !m_RebuildDockspace)
    {
        auto findDs = [&](auto& self, UIElement* el) -> void {
            if (!el || m_RebuildDockspace) return;
            if (auto* ds = dynamic_cast<DockspaceElement*>(el))
            {
                if (ds->ConsumeRebuildRequest())
                {
                    m_PendingDockspaceRebuild = ds;
                    m_RebuildDockspace = true;
                    return;
                }
            }
            for (const auto& ch : el->GetChildren())
                self(self, ch.get());
            if (el->Kind() == UIElementKind::Mount)
                if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                    self(self, tgt);
        };
        findDs(findDs, rootEl);
    }

    // Scrollbar drags / scroll-wheel handlers can mutate scroll offsets during
    // event dispatch without requesting a Yoga relayout. Apply any remaining
    // scroll deltas now (and refresh overflow clip caches) so geometry built
    // this frame matches the scrolled layout.
    bool scrollTranslatedAfterEvents = ApplyScrollTransforms(ctx);
    if (scrollTranslatedAfterEvents)
    {
        ClampAllScrollOffsets(ctx);
        if (m_DebugCaptureEnabled)
            RebuildClipCaches();
    }

    // Flush any deferred ScrollView scroll callbacks now that scroll transforms have been applied.
    // This allows virtualized controls (ListView/GridView/TreeView) to rebind items outside of
    // event dispatch and in the same frame as the scroll movement.
    bool anyScrollCallbacksFlushed = FlushScrollCallbacks(ctx);

    // Drain virtualization work requested by scroll callbacks (or other sources) at a stable point
    // outside event dispatch. This keeps the scheduling logic centralized and prepares us to
    // move virtualization earlier in the frame (pre-Yoga) without changing individual controls.
    bool anyVirtualizationRan = false;
    // Bounded draining: a virtualization pass may enqueue follow-up work (e.g. data+scroll changes).
    constexpr int kMaxVirtualizationDrainIterations = 4;
    anyVirtualizationRan = drainVirtualizationQueueBounded(kMaxVirtualizationDrainIterations);

    // Flushing callbacks may clamp scroll offsets (via SetContentSize/SetViewportSize), which can
    // require an additional translation pass. Apply any resulting deltas and refresh clip caches.
    if (anyScrollCallbacksFlushed)
    {
        const bool translated2 = ApplyScrollTransforms(ctx);
        if (translated2)
        {
            if (m_DebugCaptureEnabled)
                RebuildClipCaches();
            scrollTranslatedAfterEvents = true;
        }
    }

    // Virtualized controls can expand their pools during scroll callbacks (e.g., first scroll
    // after mount or when the viewport height changes). Those mutations happen after the
    // per-frame 'nodes' traversal was built. Detect that here so we can rebuild the retained
    // Yoga/tree view and avoid a 1-frame "blank until mouse move" artifact.
    if (!treeMutatedDuringEvents) { if (m_TreeStructureGeneration.load(std::memory_order_relaxed) != ctx.treeGenBeforeEvents) treeMutatedDuringEvents = true; }

    FinalizeSolve(ctx);

    // Dockspace rebuild requested during event dispatch: perform it now, before
    // the late dirty-flag scan, so the re-solve below gives new elements valid
    // layout rects in this same frame (avoiding a black-frame flash).
    if (runInteractive && m_RebuildDockspace && m_PendingDockspaceRebuild)
    {
        auto invalidateRenderSlots = [&](auto& self, UIElement* el) -> void {
            if (!el)
                return;
            FreeRenderSlots(el);
            el->m_LastPrimitiveGenFrame = 0;
            for (const auto& child : el->GetChildren())
                self(self, child.get());
            if (UIElement* target = el->GetMountTarget())
                self(self, target);
        };

        invalidateRenderSlots(invalidateRenderSlots, m_PendingDockspaceRebuild);
        ResetRetainedYogaTree();
        m_PendingDockspaceRebuild->RebuildFromModel();
        InvalidateSheetSetSubtree(m_PendingDockspaceRebuild);
        invalidateRenderSlots(invalidateRenderSlots, m_PendingDockspaceRebuild);
        RequestRelayout();
        treeRebuiltThisFrame = true;
        m_PendingDockspaceRebuild = nullptr;
        m_RebuildDockspace = false;
        m_Hovered = nullptr;
        m_HoveredInstanceId = 0;
        m_MouseCaptured = false;
        m_CaptureElement = nullptr;
        m_CaptureInstanceId = 0;
        m_CaptureId.clear();
    }

    // Late tree mutations / relayout requests:
    // Virtualized controls may attach/detach pooled children or mutate text (layout) during the
    // deferred scroll-changed callback phase (after the initial Yoga build/solve). Those dirty marks
    // happen *during* this heavy pass (m_DirtyHintFlags was cleared), so we must explicitly detect
    // them and rebuild Yoga/layout now to avoid 1+ frame lag and "snap" artifacts.
    bool lateChildrenDirty = false;
    bool lateLayoutDirty = false;
    bool lateStyleDirty = false;
    {
        auto probe = [&](auto& self, UIElement* el) -> void {
            if (!el) return;
            if (lateChildrenDirty && lateLayoutDirty && lateStyleDirty) return;
            if (el->IsDirty(UIElement::ChildrenDirty))
                lateChildrenDirty = true;
            if (el->IsDirty(UIElement::LayoutDirty))
                lateLayoutDirty = true;
            if (el->IsDirty(UIElement::StyleDirty))
                lateStyleDirty = true;
            if (!el->IsDirty(UIElement::SubtreeDirty))
                return;
            for (const auto& ch : el->GetChildren())
                self(self, ch.get());
            if (el->Kind() == UIElementKind::Mount)
                if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                    self(self, tgt);
        };
        probe(probe, rootEl);
    }
    const bool relayoutRequestedLate = ConsumeRelayoutRequest();
    const uint32_t virtualizationImpactThisFrame = m_VirtualizationImpactFlags.exchange(0u, std::memory_order_relaxed);
    const bool virtualizationLayoutAffectingLate =
        (virtualizationImpactThisFrame &
         (UIManager::VirtualizationTopologyChanged | UIManager::VirtualizationLayoutRectsChanged)) != 0u;
    // NOTE: lateStyleDirty is intentionally NOT a trigger here. Style-only
    // dirties set between ConvergePostLayout and this scan (typically from
    // virtualization-driven cell rebinds calling SetClass — fires every frame
    // when rapidly hovering across a virtualized list) don't affect layout
    // and don't need a full-tree rebuild. The next frame's initial BuildYoga
    // cascade picks them up via its normal StyleDirty path at ~10× lower cost.
    // Record the reason for telemetry so a paint-only style change that does
    // turn out to need layout can be reported cleanly, but gate the rebuild
    // on layout-affecting triggers only.
    const bool layoutAffectingLate =
        lateChildrenDirty || lateLayoutDirty ||
        relayoutRequestedLate || virtualizationLayoutAffectingLate;
    if (profEnabled)
    {
        if (lateChildrenDirty)               prof.LateRelayoutReasons |= 1u;
        if (lateLayoutDirty)                 prof.LateRelayoutReasons |= 2u;
        if (lateStyleDirty)                  prof.LateRelayoutReasons |= 4u;
        if (relayoutRequestedLate)           prof.LateRelayoutReasons |= 8u;
        if (virtualizationLayoutAffectingLate) prof.LateRelayoutReasons |= 16u;
    }
    if (layoutAffectingLate)
    {
        if (profEnabled)
            ++prof.LateRelayoutCount;

        rebuildLateRelayoutFired = true;
        if (lateChildrenDirty)               rebuildLateRelayoutReasons |= 1u;
        if (lateLayoutDirty)                 rebuildLateRelayoutReasons |= 2u;
        if (relayoutRequestedLate)           rebuildLateRelayoutReasons |= 8u;
        if (virtualizationLayoutAffectingLate) rebuildLateRelayoutReasons |= 16u;

        // Ensure layout signatures and intrinsic measurement contexts are refreshed for the
        // now-dirty nodes (e.g. Label text changes update Yoga measure contexts).
        computeLayoutSignaturesThisFrame = true;

        ResetFlatSheetTable(ctx, *this);

        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.BuildYoga.LateRelayout");
            ScopedSectionTimer _tBuild2(profEnabled, &prof.BuildYogaMs);
            ScopedSectionTimer _tBuildLate(profEnabled, &prof.LateRelayoutBuildYogaMs);
            const auto tStart = ProfClock::now();
            rootNode = BuildYogaRecursive(rootEl, nullptr, 0, rootSheetSetIdx, ctx, /*shareCtx=*/nullptr).node;
            rebuildLateRelayoutMs = std::chrono::duration<double, std::milli>(ProfClock::now() - tStart).count();
        }
        RebuildTypedNodeLists(ctx);
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.YogaSolve.LateRelayout");
            ScopedSectionTimer _tYoga2(profEnabled, &prof.YogaMs);
            ScopedSectionTimer _tYogaLate(profEnabled, &prof.LateRelayoutYogaMs);
            YogaAdapter::CalculateLayout(rootNode, (float)ctx.viewportW, (float)ctx.viewportH);
        }
        didSolveLayout = true;
        CommitLayoutRects(rootEl, /*incremental=*/false);

        // Converge after relayout so scrollbars, popups, etc. settle in this frame.
        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.ConvergePostLayout");
            ConvergePostLayout(rootNode, ctx);
        }

        // Layout may have changed (or even rebuilt). Refresh parent/clip caches so
        // hit-testing and geometry generation use up-to-date overflow clipping.
        ApplyScrollTransforms(ctx);
        ClampAllScrollOffsets(ctx);
        if (m_DebugCaptureEnabled)
            RebuildClipCaches();

        // Relayout may clamp scroll offsets; flush any resulting scroll callbacks and re-apply.
        bool anyScrollCallbacksFlushed2 = FlushScrollCallbacks(ctx);
        // Drain virtualization work enqueued during the relayout scroll flush.
        anyVirtualizationRan = anyVirtualizationRan || drainVirtualizationQueueBounded(kMaxVirtualizationDrainIterations);
        if (anyScrollCallbacksFlushed2)
        {
            const bool translated3 = ApplyScrollTransforms(ctx);
            if (translated3)
            {
                ClampAllScrollOffsets(ctx);
                if (m_DebugCaptureEnabled)
                    RebuildClipCaches();
                scrollTranslatedAfterEvents = true;
            }
        }

        FinalizeSolve(ctx);
    }

    // Retire the transition signal only on a pass that actually serviced it.
    // Dispatch itself already happened at the callback and can never be eaten
    // here, but the follow-up work this flag asks for — hover re-evaluation and
    // the :active re-cascade — is interactive-only, so clearing it on a passive
    // pass would drop a pressed style on the floor.
    if (interactive)
        m_ButtonEdgeSinceLastUpdate = false;

    // Stage 7 step 8: retained Yoga nodes are now owned per-UIElement via
    // m_YogaState; their YGNodes get destroyed automatically when the
    // owning element destructs (e.g. on RemoveChild / TakeChild / scene
    // teardown). No side-table sweep is needed.

    if (debugCaptureActive)
        EmitDebugCapture();


    // Optional spike logger (GE_UI_SPIKE_LOG_MS). We log *after* all dirty/clear steps so we can
    // include both the observed dirty flags and which expensive sub-stages ran.
    if (spikeLogEnabled)
    {
        const double totalMs = std::chrono::duration<double, std::milli>(ProfClock::now() - totalStart).count();
        if (totalMs >= s_UiSpikeLogThresholdMs)
        {
            static ProfClock::time_point s_LastSpikeLog{};
            const auto now = ProfClock::now();
            // Throttle to avoid log spam when a UI is consistently slow.
            if (s_LastSpikeLog.time_since_epoch().count() == 0 ||
                std::chrono::duration<double>(now - s_LastSpikeLog).count() > 0.25)
            {
                // IMPORTANT: our Logger uses "{}" placeholders only (not fmt-style "{:.2f}" / "{:X}").
                // Pre-format using snprintf so spike logs are reliable.
                char line[1024];
                std::snprintf(
                    line,
                    sizeof(line),
                    "[UI Spike] total=%.2fms elems=%u dirtyPrev=0x%X dirtyNow=0x%X anyInput=%d viewportChanged=%d relayoutReq=%d layoutSigPass=%d layoutSigChanged=%d childrenDirty=%d didSolveLayout=%d treeChanged=%d hover(layout/paint/desc/sib)=%d/%d/%d/%d usesSib=%d dirtySeen(style/layout/visual/children)=%u/%u/%u/%u textMeasureNodes=%u",
                    totalMs,
                    (unsigned)CountTreeElements(rootEl),
                    (unsigned)dirtyPrev,
                    (unsigned)dirtyNow,
                    anyInput ? 1 : 0,
                    viewportChanged ? 1 : 0,
                    relayoutRequestedPre ? 1 : 0,
                    computeLayoutSignaturesThisFrame ? 1 : 0,
                    anyLayoutSignatureChanged ? 1 : 0,
                    anyChildrenDirty ? 1 : 0,
                    didSolveLayout ? 1 : 0,
                    treeRebuiltThisFrame ? 1 : 0,
                    (m_StyleAnalysis.Hover.AffectsLayout ? 1 : 0),
                    (m_StyleAnalysis.Hover.AffectsPaint ? 1 : 0),
                    (m_StyleAnalysis.Hover.MayAffectDescendants ? 1 : 0),
                    (m_StyleAnalysis.Hover.MayAffectSiblings ? 1 : 0),
                    (m_StyleAnalysis.UsesSiblingCombinators ? 1 : 0),
                    (unsigned)dirtyStyleSeen,
                    (unsigned)dirtyLayoutSeen,
                    (unsigned)dirtyVisualSeen,
                    (unsigned)dirtyChildrenSeen,
                    (unsigned)ctx.textMeasureCtxUsed);
                Logger::Log::Warning("{}", line);

                // Second line: redundant-BuildYoga attribution. Emitted only when a
                // post-initial rebuild fired so we know which mutation source caused it.
                if (rebuildAfterPreSolveFired || rebuildLateRelayoutFired)
                {
                    char rebuildLine[512];
                    std::snprintf(
                        rebuildLine,
                        sizeof(rebuildLine),
                        "[UI Rebuild] initial=%.2fms preSolve(flush=%.2fms virt=%.2fms) afterPreSolve=%.2fms(fired=%d byFlush=%d byVirt=%d) lateRelayout=%.2fms(fired=%d reasons=0x%X ch=%d lay=%d sty=%d req=%d virt=%d)",
                        rebuildInitialMs,
                        preSolveFlushCallbacksMs,
                        preSolveVirtualizationMs,
                        rebuildAfterPreSolveMs,
                        rebuildAfterPreSolveFired ? 1 : 0,
                        rebuildAfterPreSolveByFlush ? 1 : 0,
                        rebuildAfterPreSolveByVirt ? 1 : 0,
                        rebuildLateRelayoutMs,
                        rebuildLateRelayoutFired ? 1 : 0,
                        rebuildLateRelayoutReasons,
                        (rebuildLateRelayoutReasons & 1u) ? 1 : 0,
                        (rebuildLateRelayoutReasons & 2u) ? 1 : 0,
                        (rebuildLateRelayoutReasons & 4u) ? 1 : 0,
                        (rebuildLateRelayoutReasons & 8u) ? 1 : 0,
                        (rebuildLateRelayoutReasons & 16u) ? 1 : 0);
                    Logger::Log::Warning("{}", rebuildLine);
                }

                s_LastSpikeLog = now;
            }
        }
    }

    // Finalize update profiling (optional, enabled via F9/F10).
    PublishUpdateProfile(prof, profEnabled, totalStart);
    if (profEnabled)
    {

        if (m_UpdateProfilingDumpRequested)
        {
            m_UpdateProfilingDumpRequested = false;

            const size_t n = std::min<size_t>(m_UpdateProfilingHistory.size(), 60);
            if (n > 0)
            {
                double total = 0, sched = 0, build = 0, yoga = 0, post = 0, hit = 0, ev = 0, geo = 0, clean = 0;
                uint32_t elems = 0;
                for (size_t i = m_UpdateProfilingHistory.size() - n; i < m_UpdateProfilingHistory.size(); ++i)
                {
                    const auto& f = m_UpdateProfilingHistory[i];
                    total += f.TotalMs;
                    sched += f.SchedulerMs;
                    build += f.BuildYogaMs;
                    yoga += f.YogaMs;
                    post += f.PostLayoutMs;
                    hit += f.HitTestMs;
                    ev += f.EventDispatchMs;
                    geo += f.GeometryMs;
                    clean += f.CleanupMs;
                    elems = f.ElementCount; // last frame count
                }
                const double inv = 1.0 / (double)n;
                Logger::Log::Info(
                    "[UI] Update avg over {} frames (elements={}): total={:.3f}ms  sched={:.3f}ms  build={:.3f}ms  yoga={:.3f}ms  post={:.3f}ms  hit={:.3f}ms  events={:.3f}ms  geo={:.3f}ms  cleanup={:.3f}ms",
                    (uint32_t)n,
                    elems,
                    total * inv,
                    sched * inv,
                    build * inv,
                    yoga * inv,
                    post * inv,
                    hit * inv,
                    ev * inv,
                    geo * inv,
                    clean * inv);
            }
        }
    }
    else if (m_UpdateProfilingDumpRequested)
    {
        // Clear stale dump requests when profiling is disabled.
        m_UpdateProfilingDumpRequested = false;
    }

    // Correctness mode asserts: after Update completes, there should be no remaining
    // topology/layout dirties on live elements. If there are, something mutated at an
    // unsafe time or failed to converge within this frame.
    if (m_CorrectnessModeEnabled && m_CorrectnessModeAssertsEnabled)
    {
        uint32_t layoutDirtyCount = 0;
        uint32_t childrenDirtyCount = 0;
        if (UIElement* root = GetRootElement())
        {
            std::vector<UIElement*> stack;
            stack.reserve(512);
            stack.push_back(root);
            while (!stack.empty())
            {
                UIElement* el = stack.back();
                stack.pop_back();
                if (!el)
                    continue;
                if (el->IsDirty(UIElement::LayoutDirty))
                    ++layoutDirtyCount;
                if (el->IsDirty(UIElement::ChildrenDirty))
                    ++childrenDirtyCount;
                for (const auto& ch : el->GetChildren())
                    if (ch)
                        stack.push_back(ch.get());
                if (auto* m = dynamic_cast<Mount*>(el))
                    if (UIElement* tgt = m->GetTarget())
                        stack.push_back(tgt);
            }
        }
        if (layoutDirtyCount || childrenDirtyCount)
        {
            Logger::Log::Error(
                "[UI Correctness] Dirty flags remain after Update: layout={} children={}. "
                "This indicates unsafe late mutations or missing convergence.",
                layoutDirtyCount,
                childrenDirtyCount);
        }
    }


    // Update cursor based on hovered element's computed style.
    // We check cursor style each frame and invoke the callback when it changes.
    // Populate ResolvedStyle on each element from the style cascade.
    // This runs after all layout and geometry so the resolved style is
    // guaranteed to be set on every element.
    // A heavy pass rebuilt styles above (cascade/overrides), so the resolve
    // walk must run regardless of when the last dirty mark landed. Setting
    // the flag keeps heavy-frame behavior identical to the ungated path;
    // ResolveStyles consumes it, so the render-side call becomes a no-op
    // unless something (e.g. TransitionEngine below) dirties styles again.
    m_ResolvedStylesDirty.store(true, std::memory_order_relaxed);
    ResolveStyles();

    if (!m_TransitionEngine)
        m_TransitionEngine = std::make_unique<TransitionEngine>();
    {
        ScopedSectionTimer _tTransitions(profEnabled, &prof.TransitionMs);
        m_TransitionEngine->Advance(m_Root.get(), m_TransitioningElements, m_Time);
    }

    // Transition interpolation runs after the cascade so it can compare the
    // previous and current CSS targets. Paint properties can be consumed
    // directly by rendering, but layout properties must also be copied into
    // Yoga and solved before committed geometry reflects the interpolated
    // value. Without this pass, `top`, width, padding, etc. snap to the CSS
    // target while only ResolvedStyle contains the animation in between.
    const auto& activeLayoutTransitionElements =
        m_TransitionEngine->GetActiveLayoutElements();
    if (!activeLayoutTransitionElements.empty())
    {
        for (UIElement* element : activeLayoutTransitionElements)
        {
            if (!element || !element->m_YogaState || !element->m_YogaState->Node)
                continue;

            const ResolvedStyle* parentStyle = nullptr;
            if (UIElement* dfsParent = element->GetDfsParent())
                parentStyle = &dfsParent->GetResolvedStyle();

            RetainedYogaNode& retained = *element->m_YogaState;
            retained.PrevLayout = element->GetResolvedStyle().Layout;
            retained.PrevParentBlockFlow =
                parentStyle && EstablishesBlockFlow(parentStyle->Layout.DisplayMode);
            retained.HasPrevInputs = true;
            YogaAdapter::ApplyStyle(retained.Node, element->GetResolvedStyle());
            ApplyBlockFlowShrinkDefault(retained.Node, element->GetResolvedStyle(), parentStyle);
            ApplyWeightedPaneFlexGrowOverride(element, retained.Node);
        }

        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.YogaSolve.LayoutTransitions");
            ScopedSectionTimer _tYoga(profEnabled, &prof.YogaMs);
            YogaAdapter::CalculateLayout(rootNode, (float)ctx.viewportW, (float)ctx.viewportH);
        }
        didSolveLayout = true;
        CommitLayoutRects(rootEl, /*incremental=*/false);
        FinalizeSolve(ctx);
        ApplyScrollTransforms(ctx);
        ClampAllScrollOffsets(ctx);
        if (m_DebugCaptureEnabled)
            RebuildClipCaches();
    }

    for (const auto& ct : m_TransitionEngine->GetCompletedTransitions())
    {
        UIEvent e{};
        e.Id = kEventTransitionEnd;
        e.Target = ct.Element;
        e.CurrentTarget = ct.Element;
        e.TransitionProperty = ct.Property;
        ct.Element->DispatchEvent(e);
    }

    UpdateTooltipAndCursor(runInteractive);

    // Block E (E0/E1): mutation paths that bypass MarkDirty get mirrored
    // into the regen flag here so the render side always sees a correct
    // signal.
    //
    // didSolveLayout is intentionally NOT in this set (E1 step 3): the
    // Yoga commit walk in SolveAndApplyLayout already pushed elements
    // whose rect actually changed onto m_PrimitiveDataDirty, and set
    // m_PrimitivesNeedRegen itself only for newly-visible elements that
    // need full DFS to allocate their slot range.
    if (treeRebuiltThisFrame)        MarkPrimitivesNeedRegen(0x2u);
    if (forceGlobalStyle)            MarkPrimitivesNeedRegen(0x4u);
    if (viewportChanged)             MarkPrimitivesNeedRegen(0x8u);
    if (relayoutRequestedPre)        MarkPrimitivesNeedRegen(0x10u);
    // Scroll translation is drain-only: ApplyScrollTransforms queues the
    // translated subtree and the drain re-emits it at the patched rects.
    if (m_TransitionEngine && m_TransitionEngine->HasActiveTransitions())
        MarkPrimitivesNeedRegen(0x40u);

#else
    (void)interactive;
#endif
}

// ---------------------------------------------------------------------------
// PumpVirtualizedControlChanges: C-8 per-frame provider change pump. For each
// attached virtualized control, compare its provider's change version to the
// value last seen by the pump; on movement, enqueue the control's DataChanged
// coordinator work (the same work its scroll path uses). This is what turns a
// bare MarkChanged into a repaint on an idle-gated panel: the enqueue makes
// VirtualizationCoordinator::HasPending() true, and both the idle and pointer
// gates already decline on that. Zero allocations and no enqueue on the common
// no-change path, so it never keeps an idle frame from idling.
void UIManager::PumpVirtualizedControlChanges()
{
    for (auto& entry : m_VirtualizedControlRegistry)
    {
        if (!entry.Control)
            continue;
        const std::uint64_t version = entry.Control->ProviderChangeVersion();
        if (version == entry.LastSeenVersion)
            continue;
        entry.LastSeenVersion = version;
        entry.Control->EnqueuePumpWork(*this);
    }
}

// TryIdleFrame: mode-0 idle gate. True when the frame has no input and no
// pending work of any kind, so Update can skip straight to the tooltip/cursor
// tail. Pure predicate — no state is consumed; every condition is re-checked
// next frame. The qualification set is a superset of TryPointerOnlyFrame's
// (minus the pointer-motion requirement, plus viewport and font-resolve):
// anything the heavy pass would service must decline the gate, or that work
// starves for as long as the app sits idle.
//
// Host-agnostic by design (no interactive flag): a passive host feeds no input
// and every condition below is interactivity-neutral, so a clean passive frame
// idles on exactly the same terms as a clean interactive one. Passive-only side
// effects the heavy pass would otherwise suppress (scheduler, dockspace rebuild,
// hover/event dispatch) are precisely the work a clean frame has none of, so
// skipping the heavy pass changes nothing a passive frame needed to do.
// ---------------------------------------------------------------------------
bool UIManager::TryIdleFrame(unsigned dirtyNow, bool viewportOrScaleChanged, bool debugCaptureActive)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    m_IdleGateDecline = IdleGateDecline::Disabled;
    if (!m_Root)
        return false;
    if (m_CorrectnessModeEnabled || m_DisableFastPathNoOp || debugCaptureActive)
        return false;
    m_IdleGateDecline = IdleGateDecline::Viewport;
    if (viewportOrScaleChanged)
        return false;
    m_IdleGateDecline = IdleGateDecline::Input;
    if (m_MouseMoved || m_ScrollWheelMoved || m_ScrollOffsetsChanged)
        return false;
    if (m_ButtonEdgeSinceLastUpdate || m_MouseCaptured)
        return false;
    if (m_KeyInputSinceLastUpdate)
        return false;
    // Stricter than the pointer gate: even VisualDirty declines. A visual-only
    // mark with no input is rare, and letting the heavy pass service it keeps
    // this gate trivially correct.
    m_IdleGateDecline = IdleGateDecline::DirtyTree;
    if (dirtyNow != 0u)
        return false;
    m_IdleGateDecline = IdleGateDecline::RelayoutOrRebuild;
    if (m_RequestRelayout || m_ForceFullRebuildNextFrame || m_RebuildDockspace)
        return false;
    m_IdleGateDecline = IdleGateDecline::Virtualization;
    if (m_VirtualizationImpactFlags.load(std::memory_order_relaxed) != 0u)
        return false;
    if (m_VirtualizationCoordinator.HasPending())
        return false;
    m_IdleGateDecline = IdleGateDecline::TreeGeneration;
    if (m_TreeStructureGeneration.load(std::memory_order_relaxed) != m_LastBuiltStructureGeneration)
        return false;
    m_IdleGateDecline = IdleGateDecline::StylesheetGeneration;
    if (m_AppliedStylesheetSetGeneration != m_StylesheetSetGeneration ||
        m_AppliedStylesheetContentGeneration != m_StylesheetContentGeneration)
        return false;
    m_IdleGateDecline = IdleGateDecline::Transitions;
    if (m_TransitionEngine && m_TransitionEngine->HasActiveTransitions())
        return false;
    m_IdleGateDecline = IdleGateDecline::DragDrop;
    if (m_DragDrop && m_DragDrop->IsDragging())
        return false;
    m_IdleGateDecline = IdleGateDecline::FocusNotify;
    if (m_FocusId != m_LastFocusIdNotified)
        return false;
    // Fonts installed by async resolution raise no dirty mark; the heavy
    // pass's BuildYoga measure-input diff is what detects the new atlas and
    // reflows text. Idling past a pending install would freeze fallback-font
    // layout on screen indefinitely.
    m_IdleGateDecline = IdleGateDecline::FontResolve;
    if (m_FontResolvedDirty)
        return false;

    m_IdleGateDecline = IdleGateDecline::Taken;
    return true;
#else
    (void)dirtyNow;
    (void)viewportOrScaleChanged;
    (void)debugCaptureActive;
    return false;
#endif
}

void UIManager::UpdateTooltipAndCursor(bool interactive)
{
    // Hover tooltip overlay
    if (interactive)
    {
        if (!m_TooltipOverlay)
            m_TooltipOverlay = std::make_unique<UI::Interaction::TooltipOverlay>();

        // Pull runtime config from editor settings when available.
        // The lambda avoids a hard dependency on the editor in the UI module —
        // callers can set this via SetTooltipConfigProvider().
        if (m_TooltipConfigProvider)
        {
            TooltipConfig cfg = m_TooltipConfigProvider();
            if (!cfg.Enabled)
            {
                m_TooltipOverlay->Hide();
            }
            else
            {
                m_TooltipOverlay->SetHoverDelay(cfg.HoverDelaySeconds);
                m_TooltipOverlay->SetHoverResetDelay(cfg.HoverResetDelaySeconds);
                m_TooltipOverlay->SetArrowColor(cfg.ArrowColor);
                if (UIElement* root = GetRootElement())
                    m_TooltipOverlay->Update(root, m_MouseDown ? nullptr : m_Hovered, m_MouseX, m_MouseY, m_Time);
            }
        }
        else
        {
            if (UIElement* root = GetRootElement())
                m_TooltipOverlay->Update(root, m_MouseDown ? nullptr : m_Hovered, m_MouseX, m_MouseY, m_Time);
        }
    }
    else if (m_TooltipOverlay)
    {
        m_TooltipOverlay->Hide();
    }

    if (m_CursorCallback)
    {
        CursorStyle desiredCursor = CursorStyle::Auto;
        if (m_Hovered)
        {
            // Walk up the hover chain to find the first element with a non-Auto cursor.
            for (UIElement* e = m_Hovered; e; e = e->GetParent())
            {
                if (e->GetResolvedStyle().Visual.Cursor != CursorStyle::Auto)
                {
                    desiredCursor = e->GetResolvedStyle().Visual.Cursor;
                    break;
                }
            }
        }
        if (desiredCursor != m_LastCursorStyle)
        {
            m_LastCursorStyle = desiredCursor;
            m_CursorCallback(desiredCursor);
        }
    }
}

void UIManager::PublishUpdateProfile(UpdateProfileFrame& prof, bool profEnabled,
                                     std::chrono::high_resolution_clock::time_point totalStart)
{
    if (!profEnabled)
        return;

    prof.ElementCount = CountTreeElements(GetRootElement());
    prof.TotalMs = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - totalStart).count();

    // Snapshot dirty-source top-N into the profile frame when tracing
    // is on, then clear the per-frame map. Each entry records the
    // element's id/first-class + flag bitmask + hit count so polling
    // consumers can attribute slow-path walks to specific controls.
    if (m_DirtyTraceEnabled && !m_PerFrameDirtyCount.empty())
    {
        struct Row
        {
            UIElement* el;
            uint32_t count;
            unsigned flags;
        };
        std::vector<Row> rows;
        rows.reserve(m_PerFrameDirtyCount.size());
        for (auto& kv : m_PerFrameDirtyCount)
            rows.push_back({kv.first, kv.second.first, kv.second.second});
        const size_t keep = std::min<size_t>(rows.size(), 10);
        std::partial_sort(
            rows.begin(), rows.begin() + keep, rows.end(),
            [](const Row& a, const Row& b) { return a.count > b.count; });
        prof.DirtySources.reserve(keep);
        for (size_t i = 0; i < keep; ++i)
        {
            UpdateProfileFrame::DirtySourceEntry e;
            e.InstanceId = rows[i].el ? rows[i].el->GetInstanceId() : 0;
            e.Id = rows[i].el ? rows[i].el->GetId() : std::string{};
            if (rows[i].el)
            {
                const auto& cls = rows[i].el->GetClasses();
                if (!cls.empty())
                    e.ClassName = *cls.begin();
            }
            e.Count = rows[i].count;
            e.FlagsSeen = rows[i].flags;
            prof.DirtySources.push_back(std::move(e));
        }
        m_PerFrameDirtyCount.clear();
    }

    m_UpdateProfilingHistory.push_back(prof);
    constexpr size_t kMaxHistory = 120;
    if (m_UpdateProfilingHistory.size() > kMaxHistory)
    {
        m_UpdateProfilingHistory.erase(m_UpdateProfilingHistory.begin(),
                                       m_UpdateProfilingHistory.begin() + (m_UpdateProfilingHistory.size() - kMaxHistory));
    }
}

void UIManager::DismissActiveTooltip()
{
    if (m_TooltipOverlay)
        m_TooltipOverlay->Hide();
}
