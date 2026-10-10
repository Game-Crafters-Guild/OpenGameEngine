#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

#include <nlohmann/json.hpp>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "UI/UITextureSpace.h"

namespace GameEngine
{
class UIElement;
class UIManager;
class ListView;
class ScrollView;
class GridView;
class TreeView;
namespace Rendering
{
class IDevice;
class RGReadbackTicket;
namespace RenderGraph
{
class RGFrame;
}
}

// Scripted, deterministic UI input replay + telemetry capture.
//
// Intended for self-serve debugging and automated regressions:
// - inject input into UIManager each frame
// - log per-frame UI timings and key UI state to JSONL
// - auto-exit after N frames (or earlier on failure)
class UiReplayRunner
{
  public:
    using InvokeCommandFn = bool (*)(void* userCtx, std::uint32_t commandId, std::string* outError);
    using ResizeWindowFn =
        bool (*)(void* userCtx, std::uint32_t windowIndex, std::uint32_t width, std::uint32_t height, std::string* outError);
    using QueryWindowCountFn = std::uint32_t (*)(void* userCtx);

    struct Config
    {
        std::filesystem::path scenarioPath;
        std::filesystem::path outputJsonlPath;
        std::uint64_t exitAfterFrames = 0; // 0 = run until scenario ends
        std::uint32_t windowIndex = 0;     // which Editor window context to target
        // Optional: allow deterministic invocation of Editor commands without relying on native OS menus.
        // Used by scripted UI replays to drive entity creation, etc.
        void* invokeCommandCtx = nullptr;
        InvokeCommandFn invokeCommand = nullptr;
        // Optional: allow deterministic window resizing (swapchain recreation stress).
        void* resizeWindowCtx = nullptr;
        ResizeWindowFn resizeWindow = nullptr;
        // Optional: allow deterministic host window-count waits (tear-out / close coverage).
        void* queryWindowCountCtx = nullptr;
        QueryWindowCountFn queryWindowCount = nullptr;
    };

    bool IsEnabled() const { return m_Enabled; }
    bool HasFailed() const { return m_Failed; }
    int ExitCode() const { return m_Failed ? 2 : 0; }
    bool ShouldExitNow() const { return m_ShouldExit; }
    std::uint32_t GetTargetWindowIndex() const { return m_Cfg.windowIndex; }
    // True only when the pixelHash probe will actually read pixels this
    // session. Since 8e-7 such frames stay PURE — the driver forces the
    // FinalLinear composite into the RenderGraph frame and the probe reads it back
    // via tickets (TickBeforeRenderRG).
    bool WantsPixelReadbackComposite() const
    {
        return m_Enabled && !m_ShouldExit && m_ProbePixelHash.enabled;
    }

    // Initialize from config; loads scenario, opens output file.
    bool Initialize(const Config& cfg, std::string* outError);

    // Called once per frame before UIManager::Update for the target window.
    void TickBeforeUiUpdate(UIManager& ui);

    // Called once per frame after UIManager::Update for the target window.
    void TickAfterUiUpdate(UIManager& ui, double deltaSeconds);

    // RenderGraph twin (8e-7): polls finished pixel tickets, then declares this
    // frame's region readback of `sourceId` (a frame-local RenderGraph texture id —
    // the rg2Capture raw-id publish convention; typed handles never cross
    // headers). `sourceSpace` is what the host declared that attachment holds; when
    // it is un-encoded the consume path applies linear→sRGB so hash guardrail
    // thresholds calibrated on sRGB bytes keep working. Declaration scope:
    // after the UI pass is declared, before Execute.
    void TickBeforeRenderRG(UIManager& ui, Rendering::RenderGraph::RGFrame& frame,
                            std::uint32_t sourceId, UI::UITextureSpace sourceSpace,
                            Rendering::IDevice* dev);

    // Cancel + release all pending pixel readbacks (both arms). Call during
    // shutdown BEFORE render teardown: ticket buffer destroys are
    // timeline-deferred and need the device alive.
    void CancelPendingReadbacks();

  private:
    struct AssertConfig
    {
        // Assert that ListView virtualization updates (firstVisible changes) while a scrollbar drag is captured.
        // This catches regressions where scroll offsets change but virtualization/visuals lag until mouse-up/leave.
        bool enabled = false;
        std::string listViewUnderId; // typically "mount:Log"
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
    };

    struct TreeViewUpdatesDuringCaptureAssertConfig
    {
        bool enabled = false;
        std::string treeViewUnderId;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
    };

    struct GridViewUpdatesDuringCaptureAssertConfig
    {
        // Assert that GridView virtualization updates (firstVisibleRow changes) while a scrollbar drag is captured.
        // This catches regressions where scroll offsets change but grid virtualization/visuals lag until mouse-up/leave.
        bool enabled = false;
        std::string gridViewUnderId; // typically "assets-grid"
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
    };

    struct PaintOffsetsMatchAssertConfig
    {
        // Assert that cached paint-command offsets match element layout positions within a subtree.
        // This catches "layout looks correct, but cached geometry/patch offsets are wrong" bugs in retained mode.
        bool enabled = false;
        std::string elementId; // subtree root (can be a Mount id; UIManager debug helpers expand it)
        std::string requiredClass; // optional: restrict to paint commands whose element has this class
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t maxCommands = 20000;
        std::uint32_t maxMismatchCount = 0;
    };

    struct PerfBudgetAssertConfig
    {
        bool enabled = false;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        double maxTotalMs = 0.0;
        double maxBuildYogaMs = 0.0;
        double maxGeometryMs = 0.0;
    };

    struct ElementVisibleAssertConfig
    {
        bool enabled = false;
        std::string elementId;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
    };

    struct NoHeavyPassAssertConfig
    {
        bool enabled = false;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
    };

    struct ElementTextChangesAssertConfig
    {
        bool enabled = false;
        std::string elementId;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t minChanges = 1;
    };

    struct LayoutConvergedAssertConfig
    {
        bool enabled = false;
        std::string elementId; // subtree root
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        bool includeStyleDirty = true;
    };

    struct SelectedCountAssertConfig
    {
        bool enabled = false;
        std::string subtreeId;
        std::string requiredClass; // optional filter (e.g. "tree-item", "grid-cell")
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t exact = 0xFFFFFFFFu; // if != 0xFFFFFFFF, require exact count
        std::uint32_t atLeast = 0;         // min count
    };

    struct TextContainsAssertConfig
    {
        bool enabled = false;
        std::string subtreeId;
        std::string requiredClass; // optional filter (e.g. "grid-title", "tree-title")
        std::string needle;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t exact = 0xFFFFFFFFu; // if != 0xFFFFFFFF, require exact count
        std::uint32_t atLeast = 1;
    };

    struct TreeViewBoundRowsContainAssertConfig
    {
        struct ExpectedRow
        {
            std::string label;
            int depth = -1; // -1 = ignore depth
        };

        bool enabled = false;
        std::string treeViewUnderId;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        // Require the condition to be true in at least this many frames within [frameStart..frameEnd].
        std::uint32_t minFrames = 1;
        std::vector<ExpectedRow> rows;

        // Runtime state
        std::uint32_t satisfiedFrames = 0;
    };

    // Assert that UIManager's captured-move fast path was exercised in a frame range.
    // This is used for SceneView input regressions (camera look/orbit) in optimized builds.
    struct CapturedMoveFastPathAssertConfig
    {
        bool enabled = false;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t minFrames = 1;
    };

    // Assert that scroll input in a frame range was serviced by the retained
    // scroll path rather than a full relayout. Counts frames on which a scroll
    // event was dispatched and the frame stayed light (BuildYogaMs and
    // GeometryMs below their thresholds). A full tree rebuild costs ~15ms of
    // BuildYoga plus geometry regen, so a small threshold cleanly separates the
    // retained scroll path from a regression that forces a rebuild per scroll.
    struct ScrollOnlyPathAtLeastAssertConfig
    {
        bool enabled = false;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t minFrames = 1;
        double maxBuildYogaMs = 2.0;
        double maxGeometryMs = 0.5;
    };

    // Assert that text elements which are actually inside a ScrollView viewport are not fully clipped.
    // This catches regressions where virtualized rows/cells accidentally clip their children to a too-small
    // container (manifesting as “missing columns” during horizontal scroll).
    struct VisibleTextNotFullyClippedAssertConfig
    {
        bool enabled = false;
        std::string subtreeId;        // subtree root to scan (typically the ScrollView id, e.g. "assets-list")
        std::string scrollViewUnderId; // ScrollView id whose clip viewport defines "visible"
        std::vector<std::string> requiredClasses; // if non-empty, element must have at least one of these classes
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        float minVisiblePx = 2.0f;   // require at least this much overlap with viewport in both axes
        std::uint32_t maxChecks = 2000; // safety cap
    };

    struct ScheduledEvent
    {
        std::uint64_t frame = 0;
        // Stored as JSON for flexibility; interpreted at runtime.
        // Expected schema:
        // - type: mouseMove/mouseDown/mouseUp/scroll/keyDown/keyUp/char/invokeCommand/waitWindowCount
        // - target: (optional) { kind: elementId|scrollbarVUnderId|scrollViewUnderId|listViewUnderId, id/underId, relX, relY }
        // - button/key/mods/dx/dy/codepoint...
        std::string type;
        std::shared_ptr<nlohmann::json> payload;
    };

    bool LoadScenario(const std::filesystem::path& p, std::string* outError);
    void WriteJsonl(const nlohmann::json& j);
    void Fail(const std::string& msg);

    // Target resolution helpers.
    UIElement* FindById(UIManager& ui, const std::string& id) const;
    ScrollView* FindScrollViewUnder(UIElement* root) const;
    ListView* FindListViewUnder(UIElement* root) const;
    GridView* FindGridViewUnder(UIElement* root) const;
    TreeView* FindTreeViewUnder(UIElement* root) const;

    bool ResolveTarget(UIManager& ui, const nlohmann::json& targetSpec, float& outX, float& outY, std::string* outError);

  private:
    struct PixelHashProbeConfig
    {
        bool enabled = false;
        std::uint64_t frameStart = 0;
        std::uint64_t frameEnd = 0; // inclusive
        std::uint32_t sampleWidth = 256;  // copied region width in pixels (clamped to backbuffer)
        std::uint32_t sampleHeight = 256; // copied region height in pixels (clamped to backbuffer)
        std::uint32_t everyNFrames = 1;   // 1 = every frame
        bool center = true;              // if true, sample region is centered; otherwise uses srcX/srcY
        std::uint32_t srcX = 0;
        std::uint32_t srcY = 0;
        std::uint32_t maxPending = 3; // avoid unbounded readback queue
        // GPU readback is asynchronous; allow some frames of delay before mapping the buffer.
        // If too low, fast mode can map before the GPU copy completes, yielding intermittent black/partial captures.
        std::uint32_t readbackDelayFrames = 2;
        // If true, force GPU idle before mapping the readback buffer.
        // This is deterministic but slow; intended for automated replays/tests.
        bool waitForIdle = false;
        bool dumpPpm = false;         // if true, write sampled region as .ppm next to the jsonl log
        // If true, fail the replay if the sampled region is fully black (all bytes == 0).
        // Useful to catch cases where the app presents an uninitialized/cleared backbuffer.
        bool failOnAllBlack = false;
        // Optional "nearly black" guardrails. Disabled by default.
        // - failIfMeanByteBelow: if >= 0, fail if meanByte < threshold
        // - failIfNonZeroBytesBelow: if > 0, fail if nonZeroBytes < threshold
        double failIfMeanByteBelow = -1.0;
        std::uint64_t failIfNonZeroBytesBelow = 0;
        // Optional normalized region (overrides sampleWidth/sampleHeight/center/srcX/srcY when enabled).
        // Coordinates are normalized to the backbuffer: x/y in [0..1], w/h in [0..1].
        bool useNormalizedRegion = false;
        float normX = 0.0f;
        float normY = 0.0f;
        float normW = 1.0f;
        float normH = 1.0f;
    };

    std::vector<SelectedCountAssertConfig> m_AssertSelectedCounts;
    std::vector<TextContainsAssertConfig> m_AssertTextContains;
    std::vector<TreeViewBoundRowsContainAssertConfig> m_AssertTreeBoundRowsContain;

    // Per-event state for "wait*" event types (keyed by scheduled event index).
    std::unordered_map<std::size_t, std::uint64_t> m_WaitStartFrames;

    bool m_Enabled = false;
    bool m_Failed = false;
    bool m_ShouldExit = false;

    Config m_Cfg{};

    std::uint64_t m_FrameIndex = 0;
    std::uint64_t m_MinFrames = 0;
    float m_MouseX = 0.0f;
    float m_MouseY = 0.0f;
    bool m_CursorEntered = false;
    // Wall-clock UIManager::Update time (for fast paths too).
    std::chrono::steady_clock::time_point m_UpdateStartTime{};
    bool m_UpdateStartValid = false;

    // Scenario data
    std::string m_ScenarioName;
    std::vector<ScheduledEvent> m_Events;
    size_t m_NextEventIndex = 0;

    // Optional probes configured by scenario
    std::optional<std::string> m_ProbeScrollViewUnderId;
    std::optional<std::string> m_ProbeListViewUnderId;
    std::optional<std::string> m_ProbeGridViewUnderId;
    // When set (probe.requireGridViewLive), fail the scenario if the gridView
    // probe never resolves to a sized viewport — catches scenarios that scroll a
    // hidden/empty grid (e.g. the Assets panel defaulting to list view) and would
    // otherwise pass their grid asserts vacuously.
    bool m_ProbeGridViewRequireLive = false;
    bool m_ProbeGridViewEverLive = false;
    std::optional<std::string> m_ProbeTreeViewUnderId;
    std::optional<std::string> m_ProbeScrollbarVUnderId;
    std::optional<std::string> m_ProbeScrollbarHUnderId;
    std::optional<std::string> m_ProbeElementId;
    bool m_ProbeDragDrop = false;
    // Optional CPU profiler probe: record top-N samples filtered by prefixes.
    bool m_ProbeCpuProfilerEnabled = false;
    std::vector<std::string> m_ProbeCpuProfilerPrefixes;
    std::uint32_t m_ProbeCpuProfilerTopN = 24;
    std::uint32_t m_ProbeCpuProfilerEveryNFrames = 1;
    bool m_ProbeDumpTabIds = false;
    std::optional<std::string> m_ProbeDumpIdsContaining;
    std::optional<std::uint64_t> m_ProbeDumpIdsContainingFrame;
    bool m_ProbeRenderSyncTimings = false;
    PixelHashProbeConfig m_ProbePixelHash{};

    // When enabled by replay scenario, we temporarily enable the CPU profiler.
    bool m_CpuProfilerWasEnabled = false;

    // Assertions configured by scenario
    AssertConfig m_AssertListViewUpdatesDuringCapture{};
    TreeViewUpdatesDuringCaptureAssertConfig m_AssertTreeViewUpdatesDuringCapture{};
    GridViewUpdatesDuringCaptureAssertConfig m_AssertGridViewUpdatesDuringCapture{};
    PaintOffsetsMatchAssertConfig m_AssertPaintOffsetsMatch{};
    PerfBudgetAssertConfig m_AssertPerfBudget{};
    ElementVisibleAssertConfig m_AssertElementVisible{};
    NoHeavyPassAssertConfig m_AssertNoHeavyPass{};
    ElementTextChangesAssertConfig m_AssertElementTextChanges{};
    LayoutConvergedAssertConfig m_AssertLayoutConverged{};
    CapturedMoveFastPathAssertConfig m_AssertCapturedMoveFastPath{};
    ScrollOnlyPathAtLeastAssertConfig m_AssertScrollOnlyPath{};
    VisibleTextNotFullyClippedAssertConfig m_AssertVisibleTextNotFullyClipped{};
    bool m_AssertSawCaptureInRange = false;
    bool m_AssertSawFirstVisibleChangeInRange = false;
    int m_AssertFirstVisibleAtRangeStart = -999999;

    bool m_AssertSawTreeCaptureInRange = false;
    bool m_AssertSawTreeFirstIndexChangeInRange = false;
    int m_AssertTreeFirstIndexAtRangeStart = -999999;

    bool m_AssertSawGridCaptureInRange = false;
    bool m_AssertSawGridFirstRowChangeInRange = false;
    int m_AssertGridFirstRowAtRangeStart = -999999;

    // Paint offset assertion tracking (captures max mismatch over the range).
    std::uint32_t m_AssertPaintOffsetMaxCmdCount = 0;
    std::uint32_t m_AssertPaintOffsetMaxClassCmdCount = 0;
    std::uint32_t m_AssertPaintOffsetMaxMismatchCount = 0;
    float m_AssertPaintOffsetMaxAbsDx = 0.0f;
    float m_AssertPaintOffsetMaxAbsDy = 0.0f;

    std::uint32_t m_AssertCapturedMoveFastPathCount = 0;

    // scrollOnlyPathAtLeast tracking.
    std::uint32_t m_AssertScrollOnlyLightFrames = 0;  // scroll frames that stayed on the retained path
    std::uint32_t m_AssertScrollOnlyScrollFrames = 0; // scroll frames observed in the assert range
    // Set when a scroll event is dispatched in TickBeforeUiUpdate; consumed by
    // the scrollOnlyPathAtLeast evaluation in TickAfterUiUpdate. Reset each frame.
    bool m_ScrollEventDispatchedThisFrame = false;

    // Element text change tracking for ElementTextChangesAssertConfig.
    std::string m_AssertElementTextPrev;
    std::uint32_t m_AssertElementTextChangeCount = 0;

    std::ofstream m_Out;

    struct PendingPixelReadback
    {
        std::uint64_t frame = 0;
        Rendering::IDevice* device = nullptr;
        // If valid, signaled when the GPU copy into readbackBuffer has completed.
        Rendering::IDevice::GpuSyncToken completionToken{};
        Rendering::BufferHandle readbackBuffer{};
        std::uint32_t srcX = 0;
        std::uint32_t srcY = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t format = 0; // Rendering::TextureFormat (stored as uint32)
        std::uint32_t bytesPerPixel = 4;
        // Debug: sometimes the backbuffer texture can report 0x0 size (transient swapchain issues).
        // In that case we mark the readback as invalid and log a skip event rather than underflowing
        // srcX/srcY and writing misleading 1x1 PPMs.
        bool invalid = false;
        std::uint32_t invalidReason = 0; // 1 = zero_texture_size
        std::uint32_t texW = 0;
        std::uint32_t texH = 0;
        bool pending = false;
        bool ready = false;
        bool consumed = false;
    };
    std::vector<PendingPixelReadback> m_PendingPixelReadbacks;

    // RenderGraph ticket entries (8e-7). Pending across frames until the ticket
    // resolves (TryGet) or reports cancelled (IsConsumed without data).
    struct PendingPixelTicket
    {
        std::shared_ptr<Rendering::RGReadbackTicket> Ticket;
        std::uint64_t Frame = 0;
        std::uint32_t SrcX = 0;
        std::uint32_t SrcY = 0;
        std::uint32_t Width = 0;
        std::uint32_t Height = 0;
        std::uint32_t TexW = 0;
        std::uint32_t TexH = 0;
        // What the declaring host said the source attachment holds. Engaged for
        // every entry the declare path pushes; disengaged is a programming error,
        // not a "linear" default.
        std::optional<UI::UITextureSpace> Space;
    };
    std::vector<PendingPixelTicket> m_PendingPixelTickets;

    // Shared consume metadata for ConsumePixelRgb — both arms feed it.
    struct PixelConsumeMeta
    {
        std::uint64_t Frame = 0;
        std::uint32_t SrcX = 0;
        std::uint32_t SrcY = 0;
        std::uint32_t TexW = 0;
        std::uint32_t TexH = 0;
        std::uint32_t Format = 0; // Rendering::TextureFormat as uint32
        bool HasCompletionToken = false;
        // Required: the transfer-curve decision comes from this, never from Format.
        std::optional<UI::UITextureSpace> Space;
        const char* Arm = "old";        // "old" | "rg2"
        const char* Src = "backbuffer"; // "backbuffer" | "final_linear"
        nlohmann::json Extra;           // arm-specific diagnostic fields, merged into the JSONL
    };
    // Canonicalize the sampled region to RGB8 (with linear→sRGB when the
    // source is un-encoded), hash, run the guardrails, dump PPM on demand,
    // emit the ui_replay_pixel_hash JSONL. Shared by both arms.
    void ConsumePixelRgb(const std::uint8_t* data, std::uint32_t width, std::uint32_t height,
                         const PixelConsumeMeta& meta);
    // The probe's region selection (normalized / fixed / centered) against a
    // texture extent. Returns false for a degenerate extent.
    bool ComputeProbeRegion(std::uint32_t texW, std::uint32_t texH, std::uint32_t& outSrcX,
                            std::uint32_t& outSrcY, std::uint32_t& outW,
                            std::uint32_t& outH) const;
};

} // namespace GameEngine

