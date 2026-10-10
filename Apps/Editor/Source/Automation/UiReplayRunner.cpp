#include "Automation/UiReplayRunner.h"

#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include "Mathematics/HalfFloat.h"
#include "Panels/LogView.h"
#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/ResolvedStyle.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/TreeView.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/Label.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Registration/ElementRegistration.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <filesystem>

namespace GameEngine
{
using nlohmann::json;

static std::uint64_t HashFnv1a64(const std::uint8_t* data, size_t size)
{
    // 64-bit FNV-1a
    std::uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= (std::uint64_t)data[i];
        h *= 1099511628211ull;
    }
    return h;
}

static std::string ToHex64(std::uint64_t v)
{
    static const char* kHex = "0123456789abcdef";
    char buf[16];
    for (int i = 15; i >= 0; --i)
    {
        buf[i] = kHex[(int)(v & 0xFULL)];
        v >>= 4;
    }
    return std::string(buf, buf + 16);
}

static void R11G11B10ToRgb01(std::uint32_t v, float& outR, float& outG, float& outB)
{
    // R11G11B10_FLOAT (unsigned). 5-bit exponent, mantissa: 6/6/5.
    auto decode = [](std::uint32_t mant, std::uint32_t exp, int mantBits) -> float
    {
        constexpr int bias = 15;
        if (exp == 0u)
        {
            if (mant == 0u)
                return 0.0f;
            // subnormal: mantissa / 2^mantBits * 2^(1-bias)
            const float m = (float)mant / (float)(1u << mantBits);
            return std::ldexp(m, 1 - bias);
        }
        if (exp == 31u)
        {
            // inf/nan -> clamp
            return 1e9f;
        }
        // normal: (1 + mant/2^mantBits) * 2^(exp-bias)
        const float m = 1.0f + ((float)mant / (float)(1u << mantBits));
        return std::ldexp(m, (int)exp - bias);
    };

    const std::uint32_t rMant = (v >> 0) & 0x3Fu;
    const std::uint32_t rExp = (v >> 6) & 0x1Fu;
    const std::uint32_t gMant = (v >> 11) & 0x3Fu;
    const std::uint32_t gExp = (v >> 17) & 0x1Fu;
    const std::uint32_t bMant = (v >> 22) & 0x1Fu;
    const std::uint32_t bExp = (v >> 27) & 0x1Fu;
    outR = decode(rMant, rExp, 6);
    outG = decode(gMant, gExp, 6);
    outB = decode(bMant, bExp, 5);
}

static float LinearToSrgb01(float v)
{
    if (!(v >= 0.0f))
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

bool UiReplayRunner::Initialize(const Config& cfg, std::string* outError)
{
    m_Cfg = cfg;
    m_Enabled = false;
    m_Failed = false;
    m_ShouldExit = false;
    m_FrameIndex = 0;
    m_NextEventIndex = 0;
    m_CursorEntered = false;
    m_MouseX = 0.0f;
    m_MouseY = 0.0f;
    m_MinFrames = 0;
    m_ProbeScrollViewUnderId.reset();
    m_ProbeListViewUnderId.reset();
    m_ProbeGridViewUnderId.reset();
    m_ProbeGridViewRequireLive = false;
    m_ProbeGridViewEverLive = false;
    m_ProbeTreeViewUnderId.reset();
    m_ProbeScrollbarVUnderId.reset();
    m_ProbeScrollbarHUnderId.reset();
    m_ProbeElementId.reset();
    m_ProbeCpuProfilerEnabled = false;
    m_ProbeCpuProfilerPrefixes.clear();
    m_ProbeCpuProfilerTopN = 24;
    m_ProbeCpuProfilerEveryNFrames = 1;
    m_ProbeDumpTabIds = false;
    m_ProbeDumpIdsContaining.reset();
    m_ProbeDumpIdsContainingFrame.reset();
    m_ProbeRenderSyncTimings = false;
    m_ProbePixelHash = PixelHashProbeConfig{};
    m_PendingPixelReadbacks.clear();
    m_CpuProfilerWasEnabled = false;
    m_AssertListViewUpdatesDuringCapture = AssertConfig{};
    m_AssertTreeViewUpdatesDuringCapture = TreeViewUpdatesDuringCaptureAssertConfig{};
    m_AssertGridViewUpdatesDuringCapture = GridViewUpdatesDuringCaptureAssertConfig{};
    m_AssertPaintOffsetsMatch = PaintOffsetsMatchAssertConfig{};
    m_AssertPerfBudget = PerfBudgetAssertConfig{};
    m_AssertElementVisible = ElementVisibleAssertConfig{};
    m_AssertNoHeavyPass = NoHeavyPassAssertConfig{};
    m_AssertElementTextChanges = ElementTextChangesAssertConfig{};
    m_AssertLayoutConverged = LayoutConvergedAssertConfig{};
    m_AssertCapturedMoveFastPath = CapturedMoveFastPathAssertConfig{};
    m_AssertScrollOnlyPath = ScrollOnlyPathAtLeastAssertConfig{};
    m_AssertSawCaptureInRange = false;
    m_AssertSawFirstVisibleChangeInRange = false;
    m_AssertFirstVisibleAtRangeStart = -999999;
    m_AssertSawTreeCaptureInRange = false;
    m_AssertSawTreeFirstIndexChangeInRange = false;
    m_AssertTreeFirstIndexAtRangeStart = -999999;
    m_AssertSawGridCaptureInRange = false;
    m_AssertSawGridFirstRowChangeInRange = false;
    m_AssertGridFirstRowAtRangeStart = -999999;
    m_AssertPaintOffsetMaxCmdCount = 0;
    m_AssertPaintOffsetMaxClassCmdCount = 0;
    m_AssertPaintOffsetMaxMismatchCount = 0;
    m_AssertPaintOffsetMaxAbsDx = 0.0f;
    m_AssertPaintOffsetMaxAbsDy = 0.0f;
    m_AssertCapturedMoveFastPathCount = 0;
    m_AssertScrollOnlyLightFrames = 0;
    m_AssertScrollOnlyScrollFrames = 0;
    m_ScrollEventDispatchedThisFrame = false;
    m_AssertElementTextPrev.clear();
    m_AssertElementTextChangeCount = 0;
    m_AssertSelectedCounts.clear();
    m_AssertTextContains.clear();
    m_AssertTreeBoundRowsContain.clear();
    m_WaitStartFrames.clear();

    if (m_Cfg.scenarioPath.empty())
    {
        if (outError)
            *outError = "UiReplayRunner: scenarioPath is empty";
        return false;
    }
    if (m_Cfg.outputJsonlPath.empty())
    {
        if (outError)
            *outError = "UiReplayRunner: outputJsonlPath is empty";
        return false;
    }

    std::string err;
    if (!LoadScenario(m_Cfg.scenarioPath, &err))
    {
        if (outError)
            *outError = err;
        return false;
    }

    // Ensure parent directory exists (best-effort).
    {
        std::error_code ec;
        auto parent = m_Cfg.outputJsonlPath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
    }

    m_Out.open(m_Cfg.outputJsonlPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_Out.is_open())
    {
        if (outError)
            *outError = "UiReplayRunner: failed to open output log: " + m_Cfg.outputJsonlPath.string();
        return false;
    }

    m_Enabled = true;

    Logger::Log::Info("UIReplay: enabled scenario='{}' events={} out='{}' exitAfterFrames={}",
                      m_ScenarioName,
                      (int)m_Events.size(),
                      m_Cfg.outputJsonlPath.string(),
                      (unsigned long long)m_Cfg.exitAfterFrames);

    // Header record
    json hdr;
    hdr["kind"] = "ui_replay_begin";
    hdr["scenario"] = m_ScenarioName;
    hdr["scenarioPath"] = m_Cfg.scenarioPath.string();
    hdr["output"] = m_Cfg.outputJsonlPath.string();
    hdr["exitAfterFrames"] = m_Cfg.exitAfterFrames;
    WriteJsonl(hdr);

    return true;
}

void UiReplayRunner::Fail(const std::string& msg)
{
    if (m_Failed)
        return;
    m_Failed = true;
    Logger::Log::Error("UIReplay: FAIL: {}", msg);
    json j;
    j["kind"] = "ui_replay_fail";
    j["frame"] = m_FrameIndex;
    j["message"] = msg;
    WriteJsonl(j);
    // Keep running until exit condition triggers; but mark for non-zero exit.
}

void UiReplayRunner::WriteJsonl(const json& j)
{
    if (!m_Out.is_open())
        return;
    m_Out << j.dump() << "\n";
    // Flush so crashes still leave useful breadcrumbs.
    m_Out.flush();
}

bool UiReplayRunner::LoadScenario(const std::filesystem::path& p, std::string* outError)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
    {
        if (outError)
            *outError = "UiReplayRunner: failed to open scenario: " + p.string();
        return false;
    }

    // Parse without throwing (debugger can break on thrown exceptions), and allow C++-style
    // comments for convenience in authored scenarios.
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json root = json::parse(text, /*callback=*/nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (root.is_discarded())
    {
        if (outError)
            *outError = "UiReplayRunner: scenario JSON parse error (invalid JSON): " + p.string();
        return false;
    }

    m_ScenarioName = root.value("name", p.filename().string());
    m_MinFrames = root.value("minFrames", 0ull);
    m_Events.clear();
    m_NextEventIndex = 0;
    m_ProbeScrollViewUnderId.reset();
    m_ProbeListViewUnderId.reset();
    m_ProbeGridViewUnderId.reset();
    m_ProbeGridViewRequireLive = false;
    m_ProbeGridViewEverLive = false;
    m_ProbeTreeViewUnderId.reset();
    m_ProbeScrollbarVUnderId.reset();
    m_ProbeScrollbarHUnderId.reset();
    m_ProbeElementId.reset();
    m_ProbeCpuProfilerEnabled = false;
    m_ProbeCpuProfilerPrefixes.clear();
    m_ProbeCpuProfilerTopN = 24;
    m_ProbeCpuProfilerEveryNFrames = 1;
    m_ProbeDumpTabIds = false;
    m_ProbeDumpIdsContaining.reset();
    m_ProbeDumpIdsContainingFrame.reset();
    m_ProbeRenderSyncTimings = false;
    m_AssertListViewUpdatesDuringCapture = AssertConfig{};
    m_AssertTreeViewUpdatesDuringCapture = TreeViewUpdatesDuringCaptureAssertConfig{};
    m_AssertGridViewUpdatesDuringCapture = GridViewUpdatesDuringCaptureAssertConfig{};
    m_AssertPaintOffsetsMatch = PaintOffsetsMatchAssertConfig{};
    m_AssertPerfBudget = PerfBudgetAssertConfig{};
    m_AssertElementVisible = ElementVisibleAssertConfig{};
    m_AssertNoHeavyPass = NoHeavyPassAssertConfig{};
    m_AssertElementTextChanges = ElementTextChangesAssertConfig{};
    m_AssertLayoutConverged = LayoutConvergedAssertConfig{};
    m_AssertCapturedMoveFastPath = CapturedMoveFastPathAssertConfig{};
    m_AssertScrollOnlyPath = ScrollOnlyPathAtLeastAssertConfig{};
    m_AssertSawCaptureInRange = false;
    m_AssertSawFirstVisibleChangeInRange = false;
    m_AssertFirstVisibleAtRangeStart = -999999;
    m_AssertSawTreeCaptureInRange = false;
    m_AssertSawTreeFirstIndexChangeInRange = false;
    m_AssertTreeFirstIndexAtRangeStart = -999999;
    m_AssertSawGridCaptureInRange = false;
    m_AssertSawGridFirstRowChangeInRange = false;
    m_AssertGridFirstRowAtRangeStart = -999999;
    m_AssertPaintOffsetMaxCmdCount = 0;
    m_AssertPaintOffsetMaxClassCmdCount = 0;
    m_AssertPaintOffsetMaxMismatchCount = 0;
    m_AssertPaintOffsetMaxAbsDx = 0.0f;
    m_AssertPaintOffsetMaxAbsDy = 0.0f;
    m_AssertCapturedMoveFastPathCount = 0;
    m_AssertScrollOnlyLightFrames = 0;
    m_AssertScrollOnlyScrollFrames = 0;
    m_ScrollEventDispatchedThisFrame = false;
    m_AssertElementTextPrev.clear();
    m_AssertElementTextChangeCount = 0;
    m_AssertSelectedCounts.clear();
    m_AssertTextContains.clear();
    m_AssertTreeBoundRowsContain.clear();
    m_WaitStartFrames.clear();
    m_CpuProfilerWasEnabled = false;

    if (auto itProbe = root.find("probe"); itProbe != root.end() && itProbe->is_object())
    {
        const json& probe = *itProbe;
        m_ProbeDumpTabIds = probe.value("dumpTabIds", false);
        if (auto it = probe.find("dragDrop"); it != probe.end())
        {
            if (it->is_boolean())
                m_ProbeDragDrop = it->get<bool>();
            else if (it->is_object())
                m_ProbeDragDrop = it->value("enabled", false);
        }
        if (auto it = probe.find("dumpIdsContaining"); it != probe.end())
        {
            if (it->is_string())
            {
                m_ProbeDumpIdsContaining = it->get<std::string>();
                m_ProbeDumpIdsContainingFrame = 0;
            }
            else if (it->is_object())
            {
                const json& obj = *it;
                if (auto itNeedle = obj.find("needle"); itNeedle != obj.end() && itNeedle->is_string())
                    m_ProbeDumpIdsContaining = itNeedle->get<std::string>();
                if (auto itFrame = obj.find("frame"); itFrame != obj.end() && itFrame->is_number_unsigned())
                    m_ProbeDumpIdsContainingFrame = itFrame->get<std::uint64_t>();
                else
                    m_ProbeDumpIdsContainingFrame = 0;
            }
        }
        if (auto it = probe.find("scrollViewUnderId"); it != probe.end() && it->is_string())
            m_ProbeScrollViewUnderId = it->get<std::string>();
        if (auto it = probe.find("listViewUnderId"); it != probe.end() && it->is_string())
            m_ProbeListViewUnderId = it->get<std::string>();
        if (auto it = probe.find("gridViewUnderId"); it != probe.end() && it->is_string())
            m_ProbeGridViewUnderId = it->get<std::string>();
        m_ProbeGridViewRequireLive = probe.value("requireGridViewLive", false);
        if (auto it = probe.find("treeViewUnderId"); it != probe.end() && it->is_string())
            m_ProbeTreeViewUnderId = it->get<std::string>();
        if (auto it = probe.find("scrollbarVUnderId"); it != probe.end() && it->is_string())
            m_ProbeScrollbarVUnderId = it->get<std::string>();
        if (auto it = probe.find("scrollbarHUnderId"); it != probe.end() && it->is_string())
            m_ProbeScrollbarHUnderId = it->get<std::string>();
        if (auto it = probe.find("elementId"); it != probe.end() && it->is_string())
            m_ProbeElementId = it->get<std::string>();

        m_ProbeRenderSyncTimings = probe.value("renderSyncTimings", false);

        if (auto it = probe.find("cpuProfiler"); it != probe.end() && it->is_object())
        {
            const json& cp = *it;
            m_ProbeCpuProfilerEnabled = cp.value("enabled", true);
            m_ProbeCpuProfilerTopN = cp.value("topN", 24u);
            m_ProbeCpuProfilerEveryNFrames = std::max(1u, cp.value("everyNFrames", 1u));
            m_ProbeCpuProfilerPrefixes.clear();
            if (auto itP = cp.find("prefixes"); itP != cp.end() && itP->is_array())
            {
                for (const auto& pfx : *itP)
                {
                    if (pfx.is_string())
                        m_ProbeCpuProfilerPrefixes.push_back(pfx.get<std::string>());
                }
            }
        }

        if (auto it = probe.find("pixelHash"); it != probe.end())
        {
            if (it->is_boolean())
            {
                m_ProbePixelHash.enabled = it->get<bool>();
            }
            else if (it->is_object())
            {
                const json& ph = *it;
                m_ProbePixelHash.enabled = ph.value("enabled", true);
                m_ProbePixelHash.frameStart = ph.value("frameStart", 0ull);
                m_ProbePixelHash.frameEnd = ph.value("frameEnd", 0ull);
                m_ProbePixelHash.sampleWidth = ph.value("sampleWidth", 256u);
                m_ProbePixelHash.sampleHeight = ph.value("sampleHeight", 256u);
                m_ProbePixelHash.everyNFrames = std::max(1u, ph.value("everyNFrames", 1u));
                m_ProbePixelHash.center = ph.value("center", true);
                m_ProbePixelHash.srcX = ph.value("srcX", 0u);
                m_ProbePixelHash.srcY = ph.value("srcY", 0u);
                m_ProbePixelHash.maxPending = std::max(1u, ph.value("maxPending", 3u));
                // GPU readback is asynchronous; delay mapping to avoid intermittent black/partial captures,
                // especially when the editor is running extremely fast (cached mode).
                m_ProbePixelHash.readbackDelayFrames = ph.value("readbackDelayFrames", m_ProbePixelHash.readbackDelayFrames);
                // Deterministic but slow option for tests.
                m_ProbePixelHash.waitForIdle = ph.value("waitForIdle", m_ProbePixelHash.waitForIdle);
                m_ProbePixelHash.dumpPpm = ph.value("dumpPpm", false);
                m_ProbePixelHash.failOnAllBlack = ph.value("failOnAllBlack", false);
                m_ProbePixelHash.failIfMeanByteBelow = ph.value("failIfMeanByteBelow", m_ProbePixelHash.failIfMeanByteBelow);
                // JSON stores numbers as signed; clamp to >= 0.
                {
                    const std::int64_t nz = ph.value("failIfNonZeroBytesBelow", (std::int64_t)m_ProbePixelHash.failIfNonZeroBytesBelow);
                    m_ProbePixelHash.failIfNonZeroBytesBelow = (nz > 0) ? (std::uint64_t)nz : 0ull;
                }
                if (auto itReg = ph.find("normalizedRegion"); itReg != ph.end() && itReg->is_object())
                {
                    const json& r = *itReg;
                    m_ProbePixelHash.useNormalizedRegion = true;
                    m_ProbePixelHash.normX = r.value("x", 0.0f);
                    m_ProbePixelHash.normY = r.value("y", 0.0f);
                    m_ProbePixelHash.normW = r.value("w", 1.0f);
                    m_ProbePixelHash.normH = r.value("h", 1.0f);
                }
            }

            // If frameEnd wasn't specified, treat it as "forever".
            if (m_ProbePixelHash.enabled && m_ProbePixelHash.frameEnd == 0ull)
                m_ProbePixelHash.frameEnd = 0xFFFFFFFFFFFFFFFFull;
        }
    }

    // If the scenario wants CPU profiler data, enable it for the duration of the replay.
    if (m_ProbeCpuProfilerEnabled)
    {
        auto& prof = GameEngine::Profiling::CpuProfiler::Get();
        m_CpuProfilerWasEnabled = prof.IsEnabled();
        prof.SetEnabled(true);
    }

    // Assertions (optional).
    // Back-compat:
    //  - "assert": { ...single assertion... }
    // New:
    //  - "asserts": [ { ... }, { ... } ]
    bool scenarioOk = true;
    auto parseAssertObj = [&](const json& a)
    {
        if (!a.is_object())
            return;
        const std::string kind = a.value("kind", std::string());
        if (kind == "listViewUpdatesDuringCapture")
        {
            m_AssertListViewUpdatesDuringCapture.enabled = true;
            m_AssertListViewUpdatesDuringCapture.listViewUnderId = a.value("listViewUnderId", std::string());
            m_AssertListViewUpdatesDuringCapture.frameStart = a.value("frameStart", 0ull);
            m_AssertListViewUpdatesDuringCapture.frameEnd = a.value("frameEnd", 0ull);
        }
        else if (kind == "elementVisible")
        {
            m_AssertElementVisible.enabled = true;
            m_AssertElementVisible.elementId = a.value("elementId", std::string());
            m_AssertElementVisible.frameStart = a.value("frameStart", 0ull);
            m_AssertElementVisible.frameEnd = a.value("frameEnd", 0ull);
        }
        else if (kind == "noHeavyPass")
        {
            m_AssertNoHeavyPass.enabled = true;
            m_AssertNoHeavyPass.frameStart = a.value("frameStart", 0ull);
            m_AssertNoHeavyPass.frameEnd = a.value("frameEnd", 0ull);
        }
        else if (kind == "elementTextChanges")
        {
            m_AssertElementTextChanges.enabled = true;
            m_AssertElementTextChanges.elementId = a.value("elementId", std::string());
            m_AssertElementTextChanges.frameStart = a.value("frameStart", 0ull);
            m_AssertElementTextChanges.frameEnd = a.value("frameEnd", 0ull);
            m_AssertElementTextChanges.minChanges = a.value("minChanges", 1u);
        }
        else if (kind == "layoutConverged")
        {
            m_AssertLayoutConverged.enabled = true;
            m_AssertLayoutConverged.elementId = a.value("elementId", std::string());
            m_AssertLayoutConverged.frameStart = a.value("frameStart", 0ull);
            m_AssertLayoutConverged.frameEnd = a.value("frameEnd", 0ull);
            m_AssertLayoutConverged.includeStyleDirty = a.value("includeStyleDirty", true);
        }
        else if (kind == "capturedMoveFastPathAtLeast")
        {
            m_AssertCapturedMoveFastPath.enabled = true;
            m_AssertCapturedMoveFastPath.frameStart = a.value("frameStart", 0ull);
            m_AssertCapturedMoveFastPath.frameEnd = a.value("frameEnd", 0ull);
            m_AssertCapturedMoveFastPath.minFrames = a.value("minFrames", 1u);
            m_AssertCapturedMoveFastPathCount = 0;
        }
        else if (kind == "scrollOnlyPathAtLeast")
        {
            m_AssertScrollOnlyPath.enabled = true;
            m_AssertScrollOnlyPath.frameStart = a.value("frameStart", 0ull);
            m_AssertScrollOnlyPath.frameEnd = a.value("frameEnd", 0ull);
            m_AssertScrollOnlyPath.minFrames = a.value("minFrames", 1u);
            m_AssertScrollOnlyPath.maxBuildYogaMs = a.value("maxBuildYogaMs", 2.0);
            m_AssertScrollOnlyPath.maxGeometryMs = a.value("maxGeometryMs", 0.5);
            m_AssertScrollOnlyLightFrames = 0;
            m_AssertScrollOnlyScrollFrames = 0;
        }
        else if (kind == "treeViewUpdatesDuringCapture")
        {
            m_AssertTreeViewUpdatesDuringCapture.enabled = true;
            m_AssertTreeViewUpdatesDuringCapture.treeViewUnderId = a.value("treeViewUnderId", std::string());
            m_AssertTreeViewUpdatesDuringCapture.frameStart = a.value("frameStart", 0ull);
            m_AssertTreeViewUpdatesDuringCapture.frameEnd = a.value("frameEnd", 0ull);
        }
        else if (kind == "gridViewUpdatesDuringCapture")
        {
            m_AssertGridViewUpdatesDuringCapture.enabled = true;
            m_AssertGridViewUpdatesDuringCapture.gridViewUnderId = a.value("gridViewUnderId", std::string());
            m_AssertGridViewUpdatesDuringCapture.frameStart = a.value("frameStart", 0ull);
            m_AssertGridViewUpdatesDuringCapture.frameEnd = a.value("frameEnd", 0ull);
        }
        else if (kind == "paintOffsetsMatch")
        {
            m_AssertPaintOffsetsMatch.enabled = true;
            m_AssertPaintOffsetsMatch.elementId = a.value("elementId", std::string());
            m_AssertPaintOffsetsMatch.requiredClass = a.value("requiredClass", std::string());
            m_AssertPaintOffsetsMatch.frameStart = a.value("frameStart", 0ull);
            m_AssertPaintOffsetsMatch.frameEnd = a.value("frameEnd", 0ull);
            m_AssertPaintOffsetsMatch.maxCommands = a.value("maxCommands", 20000u);
            m_AssertPaintOffsetsMatch.maxMismatchCount = a.value("maxMismatchCount", 0u);
            // Reset tracking (in case multiple scenarios reuse runner instance).
            m_AssertPaintOffsetMaxCmdCount = 0;
            m_AssertPaintOffsetMaxClassCmdCount = 0;
            m_AssertPaintOffsetMaxMismatchCount = 0;
            m_AssertPaintOffsetMaxAbsDx = 0.0f;
            m_AssertPaintOffsetMaxAbsDy = 0.0f;
        }
        else if (kind == "visibleTextNotFullyClipped")
        {
            m_AssertVisibleTextNotFullyClipped.enabled = true;
            m_AssertVisibleTextNotFullyClipped.subtreeId = a.value("subtreeId", std::string());
            m_AssertVisibleTextNotFullyClipped.scrollViewUnderId = a.value("scrollViewUnderId", std::string());
            m_AssertVisibleTextNotFullyClipped.frameStart = a.value("frameStart", 0ull);
            m_AssertVisibleTextNotFullyClipped.frameEnd = a.value("frameEnd", 0ull);
            m_AssertVisibleTextNotFullyClipped.minVisiblePx = a.value("minVisiblePx", 2.0f);
            m_AssertVisibleTextNotFullyClipped.maxChecks = a.value("maxChecks", 2000u);
            m_AssertVisibleTextNotFullyClipped.requiredClasses.clear();
            if (auto it = a.find("requiredClasses"); it != a.end() && it->is_array())
            {
                for (const auto& c : *it)
                {
                    if (c.is_string())
                        m_AssertVisibleTextNotFullyClipped.requiredClasses.push_back(c.get<std::string>());
                }
            }
        }
        else if (kind == "selectedCount")
        {
            SelectedCountAssertConfig c{};
            c.enabled = true;
            c.subtreeId = a.value("subtreeId", std::string());
            c.requiredClass = a.value("requiredClass", std::string());
            c.frameStart = a.value("frameStart", 0ull);
            c.frameEnd = a.value("frameEnd", 0ull);
            // If exact isn't provided, default is "unset".
            if (a.contains("exact"))
                c.exact = a.value("exact", 0u);
            c.atLeast = a.value("atLeast", 0u);
            m_AssertSelectedCounts.push_back(std::move(c));
        }
        else if (kind == "textContains")
        {
            TextContainsAssertConfig c{};
            c.enabled = true;
            c.subtreeId = a.value("subtreeId", std::string());
            c.requiredClass = a.value("requiredClass", std::string());
            c.needle = a.value("needle", std::string());
            c.frameStart = a.value("frameStart", 0ull);
            c.frameEnd = a.value("frameEnd", 0ull);
            // If exact isn't provided, default is "unset".
            if (a.contains("exact"))
                c.exact = a.value("exact", 0u);
            c.atLeast = a.value("atLeast", 1u);
            m_AssertTextContains.push_back(std::move(c));
        }
        else if (kind == "treeBoundRowsContain")
        {
            TreeViewBoundRowsContainAssertConfig c{};
            c.enabled = true;
            c.treeViewUnderId = a.value("treeViewUnderId", a.value("underId", std::string()));
            c.frameStart = a.value("frameStart", 0ull);
            c.frameEnd = a.value("frameEnd", 0ull);
            c.minFrames = a.value("minFrames", 1u);
            if (auto it = a.find("rows"); it != a.end() && it->is_array())
            {
                for (const auto& r : *it)
                {
                    if (!r.is_object())
                        continue;
                    TreeViewBoundRowsContainAssertConfig::ExpectedRow er{};
                    er.label = r.value("label", std::string());
                    er.depth = r.value("depth", -1);
                    if (!er.label.empty())
                        c.rows.push_back(std::move(er));
                }
            }
            if (c.treeViewUnderId.empty() || c.rows.empty())
            {
                scenarioOk = false;
                if (outError && outError->empty())
                    *outError = "assert(treeBoundRowsContain) requires treeViewUnderId/rows";
                return;
            }
            m_AssertTreeBoundRowsContain.push_back(std::move(c));
        }
    };

    if (auto itAssert = root.find("assert"); itAssert != root.end() && itAssert->is_object())
    {
        parseAssertObj(*itAssert);
    }
    if (auto itAsserts = root.find("asserts"); itAsserts != root.end() && itAsserts->is_array())
    {
        for (const auto& a : *itAsserts)
        {
            parseAssertObj(a);
        }
    }
    if (!scenarioOk)
        return false;

    // Performance budget assertion (optional).
    // Example:
    // "assertPerfBudget": { "frameStart":150, "frameEnd":220, "maxTotalMs":1.0, "maxBuildYogaMs":0.05, "maxGeometryMs":0.05 }
    if (auto itPerf = root.find("assertPerfBudget"); itPerf != root.end() && itPerf->is_object())
    {
        const json& perf = *itPerf;
        m_AssertPerfBudget.enabled = true;
        m_AssertPerfBudget.frameStart = perf.value("frameStart", 0ull);
        m_AssertPerfBudget.frameEnd = perf.value("frameEnd", 0ull);
        m_AssertPerfBudget.maxTotalMs = perf.value("maxTotalMs", 0.0);
        m_AssertPerfBudget.maxBuildYogaMs = perf.value("maxBuildYogaMs", 0.0);
        m_AssertPerfBudget.maxGeometryMs = perf.value("maxGeometryMs", 0.0);
    }

    std::uint32_t winIndex = root.value("windowIndex", 0u);
    m_Cfg.windowIndex = winIndex;

    auto itEv = root.find("events");
    if (itEv == root.end() || !itEv->is_array())
    {
        if (outError)
            *outError = "UiReplayRunner: scenario missing 'events' array";
        return false;
    }

    for (const auto& e : *itEv)
    {
        if (!e.is_object())
            continue;
        const std::uint64_t frame = e.value("frame", 0ull);
        const std::string type = e.value("type", std::string());
        if (type.empty())
            continue;

        ScheduledEvent se;
        se.frame = frame;
        se.type = type;
        se.payload = std::make_shared<json>(e);
        m_Events.push_back(std::move(se));
    }

    std::sort(m_Events.begin(), m_Events.end(), [](const ScheduledEvent& a, const ScheduledEvent& b)
              { return a.frame < b.frame; });

    return true;
}

UIElement* UiReplayRunner::FindById(UIManager& ui, const std::string& id) const
{
    if (id.empty())
        return nullptr;
    if (UIElement* root = ui.GetRootElement())
        return root->FindById(id);
    return nullptr;
}

template <typename T>
static T* FindFirstDescendantOfType(UIElement* root)
{
    if (!root)
        return nullptr;
    std::vector<UIElement*> stack;
    stack.reserve(128);
    stack.push_back(root);
    while (!stack.empty())
    {
        UIElement* el = stack.back();
        stack.pop_back();
        if (!el)
            continue;
        if (auto* t = dynamic_cast<T*>(el))
            return t;
        for (const auto& ch : el->GetChildren())
        {
            if (ch)
                stack.push_back(ch.get());
        }
        if (auto* m = dynamic_cast<Mount*>(el))
        {
            if (UIElement* tgt = m->GetTarget())
                stack.push_back(tgt);
        }
    }
    return nullptr;
}

ScrollView* UiReplayRunner::FindScrollViewUnder(UIElement* root) const
{
    return FindFirstDescendantOfType<ScrollView>(root);
}

ListView* UiReplayRunner::FindListViewUnder(UIElement* root) const
{
    return FindFirstDescendantOfType<ListView>(root);
}

GridView* UiReplayRunner::FindGridViewUnder(UIElement* root) const
{
    return FindFirstDescendantOfType<GridView>(root);
}

TreeView* UiReplayRunner::FindTreeViewUnder(UIElement* root) const
{
    return FindFirstDescendantOfType<TreeView>(root);
}

bool UiReplayRunner::ResolveTarget(UIManager& ui, const json& targetSpec, float& outX, float& outY, std::string* outError)
{
    if (!targetSpec.is_object())
    {
        if (outError)
            *outError = "target must be an object";
        return false;
    }

    const std::string kind = targetSpec.value("kind", std::string());
    const float relX = targetSpec.value("relX", 0.5f);
    const float relY = targetSpec.value("relY", 0.5f);

    // Absolute mouse position in window space (pixels).
    // Useful for reproducing hover issues that depend on specific screen regions.
    if (kind == "abs")
    {
        if (!targetSpec.contains("x") || !targetSpec.contains("y"))
        {
            if (outError)
                *outError = "abs target requires x and y";
            return false;
        }
        outX = targetSpec.value("x", 0.0f);
        outY = targetSpec.value("y", 0.0f);
        return true;
    }

    auto pointOn = [&](UIElement* el) -> bool
    {
        if (!el)
            return false;
        const float x = el->GetLayoutX();
        const float y = el->GetLayoutY();
        const float w = el->GetLayoutWidth();
        const float h = el->GetLayoutHeight();
        outX = x + std::clamp(relX, 0.0f, 1.0f) * w;
        outY = y + std::clamp(relY, 0.0f, 1.0f) * h;
        return true;
    };

    if (kind == "elementId")
    {
        const std::string id = targetSpec.value("id", std::string());
        UIElement* el = FindById(ui, id);
        if (!el || !pointOn(el))
        {
            if (outError)
                *outError = "elementId target not found or has no layout: id=" + id;
            return false;
        }
        return true;
    }

    // Click the visible element carrying a given class. Some toolbars exist in two
    // copies (only one shown, the other Display:None per prefs), so an id-based
    // click would land on the hidden one. This picks the largest-area element that
    // actually has layout, making toggles (e.g. the Assets grid/list view icons,
    // class "grid-view-icon") robust to which toolbar is currently visible.
    if (kind == "visibleElementByClass")
    {
        const std::string cls = targetSpec.value("class", std::string());
        const std::string underId = targetSpec.value("underId", std::string());
        if (cls.empty())
        {
            if (outError)
                *outError = "visibleElementByClass requires class";
            return false;
        }
        UIElement* root = underId.empty() ? ui.GetRootElement() : FindById(ui, underId);
        if (!root)
        {
            if (outError)
                *outError = "visibleElementByClass: root not found (underId=" + underId + ")";
            return false;
        }

        UIElement* best = nullptr;
        float bestArea = 0.0f;
        std::vector<UIElement*> stack;
        stack.push_back(root);
        std::uint32_t safety = 0;
        while (!stack.empty())
        {
            UIElement* cur = stack.back();
            stack.pop_back();
            if (!cur)
                continue;
            if (++safety > 20000u)
                break;
            if (cur->HasClass(cls))
            {
                const float w = cur->GetLayoutWidth();
                const float h = cur->GetLayoutHeight();
                if (w > 0.5f && h > 0.5f && (w * h) > bestArea)
                {
                    bestArea = w * h;
                    best = cur;
                }
            }
            for (const auto& ch : cur->GetChildren())
            {
                if (ch)
                    stack.push_back(ch.get());
            }
            if (auto* m = dynamic_cast<Mount*>(cur))
            {
                if (UIElement* tgt = m->GetTarget())
                    stack.push_back(tgt);
            }
        }

        if (!best || !pointOn(best))
        {
            if (outError)
                *outError = "visibleElementByClass: no visible element with class=" + cls;
            return false;
        }
        return true;
    }

    // Find a Label under a subtree (underId) by text match and optional class filter.
    // Useful for robust UIReplay scripts that shouldn't depend on hardcoded row coordinates.
    if (kind == "labelTextUnderId")
    {
        const std::string underId = targetSpec.value("underId", std::string());
        const std::string needle = targetSpec.value("needle", std::string());
        const std::string requiredClass = targetSpec.value("requiredClass", std::string());
        if (underId.empty() || needle.empty())
        {
            if (outError)
                *outError = "labelTextUnderId requires underId and needle";
            return false;
        }
        UIElement* root = FindById(ui, underId);
        if (!root)
        {
            if (outError)
                *outError = "labelTextUnderId: underId not found: " + underId;
            return false;
        }

        auto contains = [](const std::string& haystack, const std::string& n) -> bool
        {
            if (n.empty())
                return true;
            return haystack.find(n) != std::string::npos;
        };

        Label* found = nullptr;
        std::vector<UIElement*> stack;
        stack.push_back(root);
        std::uint32_t safety = 0;
        while (!stack.empty() && !found)
        {
            UIElement* cur = stack.back();
            stack.pop_back();
            if (!cur)
                continue;
#if defined(_MSC_VER)
            __analysis_assume(cur != nullptr);
#endif
            if (++safety > 20000u)
                break;

            Label* lbl = dynamic_cast<Label*>(cur);
            if (lbl)
            {
                if (!requiredClass.empty() && !lbl->HasClass(requiredClass))
                {
                    // skip
                }
                else
                {
                    const std::string& txt = lbl->GetText();
                    if (contains(txt, needle) && lbl->GetLayoutWidth() > 0.5f && lbl->GetLayoutHeight() > 0.5f)
                    {
                        found = lbl;
                        break;
                    }
                }
            }

            const auto& kids = cur->GetChildren();
            for (const auto& ch : kids)
            {
                if (ch)
                    stack.push_back(ch.get());
            }
            if (auto* m = dynamic_cast<Mount*>(cur))
            {
                if (UIElement* tgt = m->GetTarget())
                    stack.push_back(tgt);
            }
        }

        if (!found || !pointOn(found))
        {
            if (outError)
                *outError = "labelTextUnderId: label not found: underId=" + underId + " needle=" + needle;
            return false;
        }
        return true;
    }

    // Find a Label by text, then return a point on an ancestor element with a given class.
    // This is useful when the clickable/capturing surface is the row/cell container (not the Label itself).
    if (kind == "ancestorOfLabelTextUnderId")
    {
        const std::string underId = targetSpec.value("underId", std::string());
        const std::string needle = targetSpec.value("needle", std::string());
        const std::string labelClass = targetSpec.value("labelClass", std::string());
        const std::string ancestorClass = targetSpec.value("ancestorClass", std::string());
        if (underId.empty() || needle.empty() || ancestorClass.empty())
        {
            if (outError)
                *outError = "ancestorOfLabelTextUnderId requires underId, needle, and ancestorClass";
            return false;
        }

        json tmp = targetSpec;
        tmp["kind"] = "labelTextUnderId";
        if (!labelClass.empty())
            tmp["requiredClass"] = labelClass;

        float lx = 0.0f, ly = 0.0f;
        std::string err;
        if (!ResolveTarget(ui, tmp, lx, ly, &err))
        {
            if (outError)
                *outError = "ancestorOfLabelTextUnderId: failed to resolve label: " + err;
            return false;
        }

        // Re-find the label element itself so we can walk to its ancestor.
        UIElement* root = FindById(ui, underId);
        if (!root)
        {
            if (outError)
                *outError = "ancestorOfLabelTextUnderId: underId not found: " + underId;
            return false;
        }

        Label* found = nullptr;
        std::vector<UIElement*> stack;
        stack.push_back(root);
        std::uint32_t safety = 0;
        while (!stack.empty() && !found)
        {
            UIElement* cur = stack.back();
            stack.pop_back();
            if (!cur)
                continue;
            if (++safety > 20000u)
                break;
            Label* lbl = dynamic_cast<Label*>(cur);
            if (lbl)
            {
                if (!labelClass.empty() && !lbl->HasClass(labelClass))
                {
                    // skip
                }
                else if (lbl->GetText().find(needle) != std::string::npos)
                {
                    found = lbl;
                    break;
                }
            }
            if (cur)
            {
                for (const auto& ch : cur->GetChildren())
                {
                    if (ch)
                        stack.push_back(ch.get());
                }
                if (auto* m = dynamic_cast<Mount*>(cur))
                {
                    if (UIElement* tgt = m->GetTarget())
                        stack.push_back(tgt);
                }
            }
        }

        UIElement* clickEl = found ? found->GetParent() : nullptr;
        while (clickEl && !clickEl->HasClass(ancestorClass))
            clickEl = clickEl->GetParent();

        if (!clickEl || !pointOn(clickEl))
        {
            if (outError)
                *outError = "ancestorOfLabelTextUnderId: ancestor not found (ancestorClass=" + ancestorClass + " needle=" + needle + ")";
            return false;
        }
        return true;
    }

    if (kind == "scrollViewUnderId")
    {
        const std::string underId = targetSpec.value("underId", std::string());
        UIElement* root = FindById(ui, underId);
        ScrollView* sv = FindScrollViewUnder(root);
        if (!sv || !pointOn(sv))
        {
            if (outError)
                *outError = "scrollViewUnderId target not found: underId=" + underId;
            return false;
        }
        return true;
    }

    if (kind == "scrollbarVUnderId")
    {
        const std::string underId = targetSpec.value("underId", std::string());
        UIElement* root = FindById(ui, underId);
        ScrollView* sv = FindScrollViewUnder(root);
        UIElement* sb = sv ? sv->GetVerticalScrollbar() : nullptr;
        if (!sb || !pointOn(sb))
        {
            if (outError)
                *outError = "scrollbarVUnderId target not found: underId=" + underId;
            return false;
        }
        return true;
    }

    if (kind == "scrollbarHUnderId")
    {
        const std::string underId = targetSpec.value("underId", std::string());
        UIElement* root = FindById(ui, underId);
        ScrollView* sv = FindScrollViewUnder(root);
        UIElement* sb = sv ? sv->GetHorizontalScrollbar() : nullptr;
        if (!sb || !pointOn(sb))
        {
            if (outError)
                *outError = "scrollbarHUnderId target not found: underId=" + underId;
            return false;
        }
        return true;
    }

    if (outError)
        *outError = "unknown target kind: " + kind;
    return false;
}

// Scenario events are driven straight into the target UIManager — OnMouseMove,
// OnMouseButton, OnScroll, OnKey, OnChar — with no WindowInputRouter in between,
// and that is deliberate. A replay is a closed deterministic stream over one
// named manager: routing it would also run the editor's key pre-hooks, so a
// scenario's F11 or Ctrl+Z would fire an editor command, and would accumulate
// edges in the application InputSystem that no scenario asked for. The other
// half of the same rule keeps real input out — while a scenario is loaded every
// window's getUi answers null, so the router delivers nothing to these managers
// and this stream is the only one they see.
void UiReplayRunner::TickBeforeUiUpdate(UIManager& ui)
{
    if (!m_Enabled)
        return;

    m_UpdateStartTime = std::chrono::steady_clock::now();
    m_UpdateStartValid = true;

    // Keep UI profiling enabled during replay runs.
    if (!ui.IsUpdateProfilingEnabled())
        ui.SetUpdateProfilingEnabled(true);

    if (!m_CursorEntered)
    {
        ui.OnCursorEnter(true);
        m_CursorEntered = true;
    }

    // Dispatch any events scheduled for this frame.
    // NOTE: We execute events whose scheduled frame is <= the current frame so scenarios
    // remain robust under load (e.g. longer frame times, async asset discovery, GPU stalls).
    // This is critical for any "wait" event types and prevents tests from becoming frame-perfect.
    // Consumed by scrollOnlyPathAtLeast in TickAfterUiUpdate; scoped to this frame.
    m_ScrollEventDispatchedThisFrame = false;

    std::uint32_t safetyEventsThisFrame = 0;
    while (m_NextEventIndex < m_Events.size() && m_Events[m_NextEventIndex].frame <= m_FrameIndex)
    {
        if (++safetyEventsThisFrame > 2000u)
        {
            Fail("too many events executed in a single frame (possible infinite loop)");
            break;
        }

        const ScheduledEvent& ev = m_Events[m_NextEventIndex];
        const json& payload = *ev.payload;

        if (ev.type == "mouseMove")
        {
            if (auto itT = payload.find("target"); itT != payload.end())
            {
                std::string err;
                float x = 0.0f, y = 0.0f;
                if (!ResolveTarget(ui, *itT, x, y, &err))
                {
                    Fail("mouseMove ResolveTarget failed: " + err);
                }
                else
                {
                    m_MouseX = x;
                    m_MouseY = y;
                    ui.OnMouseMove(m_MouseX, m_MouseY);
                }
            }
            else
            {
                // Relative move
                const float dx = payload.value("dx", 0.0f);
                const float dy = payload.value("dy", 0.0f);
                m_MouseX += dx;
                m_MouseY += dy;
                ui.OnMouseMove(m_MouseX, m_MouseY);
            }
        }
        else if (ev.type == "mouseDown")
        {
            const int button = payload.value("button", 0);
            ui.OnMouseMove(m_MouseX, m_MouseY);
            ui.OnMouseButton(button, true);
        }
        else if (ev.type == "mouseUp")
        {
            const int button = payload.value("button", 0);
            ui.OnMouseMove(m_MouseX, m_MouseY);
            ui.OnMouseButton(button, false);
        }
        else if (ev.type == "invokeCommand")
        {
            const std::uint32_t commandId = payload.value("commandId", 0u);
            if (commandId == 0u)
            {
                Fail("invokeCommand missing/invalid commandId");
            }
            else if (!m_Cfg.invokeCommand)
            {
                Fail("invokeCommand not supported by host (cfg.invokeCommand is null)");
            }
            else
            {
                std::string err;
                if (!m_Cfg.invokeCommand(m_Cfg.invokeCommandCtx, commandId, &err))
                {
                    if (err.empty())
                        err = "unknown failure";
                    Fail("invokeCommand failed: cmd=" + std::to_string(commandId) + " err=" + err);
                }
                else
                {
                    json j;
                    j["kind"] = "ui_replay_event";
                    j["frame"] = m_FrameIndex;
                    j["eventIndex"] = m_NextEventIndex;
                    j["type"] = "invokeCommand";
                    j["commandId"] = commandId;
                    if (m_Cfg.queryWindowCount)
                        j["windowCount"] = m_Cfg.queryWindowCount(m_Cfg.queryWindowCountCtx);
                    WriteJsonl(j);
                }
            }
        }
        else if (ev.type == "waitTextContains")
        {
            const std::string subtreeId = payload.value("subtreeId", payload.value("underId", std::string()));
            const std::string requiredClass = payload.value("requiredClass", std::string());
            const std::string needle = payload.value("needle", std::string());
            const std::uint32_t atLeast = payload.value("atLeast", 1u);
            const std::uint64_t timeoutFrames = payload.value("timeoutFrames", 600ull);
            if (subtreeId.empty() || needle.empty())
            {
                Fail("waitTextContains requires subtreeId/underId and needle");
                ++m_NextEventIndex;
                continue;
            }

            UIElement* rootEl = FindById(ui, subtreeId);
            auto countText = [](UIElement* root, const std::string& requiredClass, const std::string& needle) -> std::uint32_t
            {
                if (!root || needle.empty())
                    return 0;
                std::uint32_t count = 0;
                std::vector<UIElement*> stack;
                stack.reserve(256);
                stack.push_back(root);
                std::uint32_t safety = 0;
                while (!stack.empty())
                {
                    UIElement* cur = stack.back();
                    stack.pop_back();
                    if (!cur)
                        continue;
#if defined(_MSC_VER)
                    __analysis_assume(cur != nullptr);
#endif
                    if (++safety > 200000u)
                        break;

                    Label* lbl = dynamic_cast<Label*>(cur);
                    if (lbl)
                    {
                        if (requiredClass.empty() || lbl->HasClass(requiredClass))
                        {
                            if (lbl->GetText().find(needle) != std::string::npos)
                                ++count;
                        }
                    }
                    if (cur)
                    {
                        for (const auto& ch : cur->GetChildren())
                        {
                            if (ch)
                                stack.push_back(ch.get());
                        }
                        if (auto* m = dynamic_cast<Mount*>(cur))
                        {
                            if (UIElement* tgt = m->GetTarget())
                                stack.push_back(tgt);
                        }
                    }
                }
                return count;
            };

            const std::uint64_t startFrame =
                m_WaitStartFrames.emplace(m_NextEventIndex, m_FrameIndex).first->second;

            if (!rootEl)
            {
                if (timeoutFrames > 0 && (m_FrameIndex - startFrame) >= timeoutFrames)
                {
                    Fail("waitTextContains timed out: subtree not found (id='" + subtreeId + "')");
                    ++m_NextEventIndex;
                }
                // keep waiting
                break;
            }

            const std::uint32_t got = countText(rootEl, requiredClass, needle);
            if (got >= atLeast)
            {
                // Satisfied; continue to next event.
                m_WaitStartFrames.erase(m_NextEventIndex);
            }
            else
            {
                if (timeoutFrames > 0 && (m_FrameIndex - startFrame) >= timeoutFrames)
                {
                    Fail("waitTextContains timed out: expected atLeast=" + std::to_string(atLeast) + " got=" + std::to_string(got) +
                         " needle='" + needle + "' (subtreeId='" + subtreeId + "')");
                    ++m_NextEventIndex;
                }
                // keep waiting
                break;
            }
        }
        else if (ev.type == "waitSelectedCount")
        {
            const std::string subtreeId = payload.value("subtreeId", payload.value("underId", std::string()));
            const std::string requiredClass = payload.value("requiredClass", std::string());
            const std::uint32_t atLeast = payload.value("atLeast", 0u);
            const std::uint32_t exact = payload.value("exact", 0xFFFFFFFFu);
            const std::uint64_t timeoutFrames = payload.value("timeoutFrames", 600ull);
            if (subtreeId.empty())
            {
                Fail("waitSelectedCount requires subtreeId/underId");
                ++m_NextEventIndex;
                continue;
            }

            UIElement* rootEl = FindById(ui, subtreeId);
            auto countSelected = [](UIElement* root, const std::string& requiredClass) -> std::uint32_t
            {
                if (!root)
                    return 0;
                std::uint32_t count = 0;
                std::vector<UIElement*> stack;
                stack.reserve(256);
                stack.push_back(root);
                std::uint32_t safety = 0;
                while (!stack.empty())
                {
                    UIElement* cur = stack.back();
                    stack.pop_back();
                    if (!cur)
                        continue;
                    if (++safety > 200000u)
                        break;
                    if (cur->HasClass("selected"))
                    {
                        if (requiredClass.empty() || cur->HasClass(requiredClass))
                            ++count;
                    }
                    for (const auto& ch : cur->GetChildren())
                    {
                        if (ch)
                            stack.push_back(ch.get());
                    }
                    if (auto* m = dynamic_cast<Mount*>(cur))
                    {
                        if (UIElement* tgt = m->GetTarget())
                            stack.push_back(tgt);
                    }
                }
                return count;
            };

            const std::uint64_t startFrame =
                m_WaitStartFrames.emplace(m_NextEventIndex, m_FrameIndex).first->second;

            if (!rootEl)
            {
                if (timeoutFrames > 0 && (m_FrameIndex - startFrame) >= timeoutFrames)
                {
                    Fail("waitSelectedCount timed out: subtree not found (id='" + subtreeId + "')");
                    ++m_NextEventIndex;
                }
                break;
            }

            const std::uint32_t got = countSelected(rootEl, requiredClass);
            const bool ok = (exact != 0xFFFFFFFFu) ? (got == exact) : (got >= atLeast);
            if (ok)
            {
                m_WaitStartFrames.erase(m_NextEventIndex);
            }
            else
            {
                if (timeoutFrames > 0 && (m_FrameIndex - startFrame) >= timeoutFrames)
                {
                    if (exact != 0xFFFFFFFFu)
                        Fail("waitSelectedCount timed out: expected exact=" + std::to_string(exact) + " got=" + std::to_string(got) +
                             " (subtreeId='" + subtreeId + "')");
                    else
                        Fail("waitSelectedCount timed out: expected atLeast=" + std::to_string(atLeast) + " got=" + std::to_string(got) +
                             " (subtreeId='" + subtreeId + "')");
                    ++m_NextEventIndex;
                }
                break;
            }
        }
        else if (ev.type == "waitFrames")
        {
            const std::uint64_t frames = payload.value("frames", 0ull);
            if (frames > 0)
            {
                const std::uint64_t startFrame =
                    m_WaitStartFrames.emplace(m_NextEventIndex, m_FrameIndex).first->second;
                if ((m_FrameIndex - startFrame) < frames)
                {
                    // keep waiting
                    break;
                }
                m_WaitStartFrames.erase(m_NextEventIndex);
            }
        }
        else if (ev.type == "waitWindowCount")
        {
            const std::uint32_t atLeast = payload.value("atLeast", 0u);
            const std::uint32_t exact = payload.value("exact", 0xFFFFFFFFu);
            const std::uint64_t timeoutFrames = payload.value("timeoutFrames", 600ull);

            if (!m_Cfg.queryWindowCount)
            {
                Fail("waitWindowCount not supported by host (cfg.queryWindowCount is null)");
                ++m_NextEventIndex;
                continue;
            }

            const std::uint64_t startFrame =
                m_WaitStartFrames.emplace(m_NextEventIndex, m_FrameIndex).first->second;
            const std::uint32_t count = m_Cfg.queryWindowCount(m_Cfg.queryWindowCountCtx);
            const bool ok = (exact != 0xFFFFFFFFu) ? (count == exact) : (count >= atLeast);
            if (ok)
            {
                json j;
                j["kind"] = "ui_replay_event";
                j["frame"] = m_FrameIndex;
                j["eventIndex"] = m_NextEventIndex;
                j["type"] = "waitWindowCount";
                if (exact != 0xFFFFFFFFu)
                    j["exact"] = exact;
                else
                    j["atLeast"] = atLeast;
                j["windowCount"] = count;
                j["waitFrames"] = (m_FrameIndex >= startFrame) ? (m_FrameIndex - startFrame) : 0ull;
                j["result"] = "satisfied";
                WriteJsonl(j);
                m_WaitStartFrames.erase(m_NextEventIndex);
            }
            else
            {
                if (timeoutFrames > 0 && (m_FrameIndex - startFrame) >= timeoutFrames)
                {
                    if (exact != 0xFFFFFFFFu)
                    {
                        Fail("waitWindowCount timed out: expected exact=" + std::to_string(exact) +
                             " got=" + std::to_string(count));
                    }
                    else
                    {
                        Fail("waitWindowCount timed out: expected atLeast=" + std::to_string(atLeast) +
                             " got=" + std::to_string(count));
                    }
                    ++m_NextEventIndex;
                }
                // keep waiting
                break;
            }
        }
        else if (ev.type == "scroll")
        {
            const float dx = payload.value("dx", 0.0f);
            const float dy = payload.value("dy", 0.0f);
            (void)ui.OnScroll(dx, dy);
            m_ScrollEventDispatchedThisFrame = true;
        }
        else if (ev.type == "keyDown")
        {
            const int key = payload.value("key", 0);
            const int mods = payload.value("mods", 0);
            // GLFW-like actions: 1=press
            (void)ui.OnKey(key, /*action=*/1, mods);
        }
        else if (ev.type == "keyUp")
        {
            const int key = payload.value("key", 0);
            const int mods = payload.value("mods", 0);
            // GLFW-like actions: 0=release
            (void)ui.OnKey(key, /*action=*/0, mods);
        }
        else if (ev.type == "uiTextDebugDump")
        {
            // Directly request a text debug dump (writes UI_TextDebug_*.txt) without relying on
            // keyboard bindings. Useful for automation when the host may remap or consume keys.
            ui.RequestTextDebugDump();
        }
        else if (ev.type == "emitLogLines")
        {
            // Deterministically inject log lines so the Log panel/ListView is scrollable in automation.
            // Example payload:
            // { "type":"emitLogLines", "count":200, "level":"warning", "prefix":"[UIReplay]" }
            const int countReq = payload.value("count", 200);
            const int count = std::max(0, std::min(2000, countReq));
            const std::string levelStr = payload.value("level", std::string("warning"));
            const std::string prefix = payload.value("prefix", std::string("[UIReplay]"));

            enum class Lvl { Trace, Debug, Info, Warning, Error, Critical };
            auto parseLvl = [&](const std::string& s) -> Lvl
            {
                std::string t;
                t.reserve(s.size());
                for (char c : s)
                {
                    if (c >= 'A' && c <= 'Z')
                        t.push_back((char)(c - 'A' + 'a'));
                    else
                        t.push_back(c);
                }
                if (t == "trace") return Lvl::Trace;
                if (t == "debug") return Lvl::Debug;
                if (t == "info") return Lvl::Info;
                if (t == "warning" || t == "warn") return Lvl::Warning;
                if (t == "error" || t == "err") return Lvl::Error;
                if (t == "critical" || t == "crit") return Lvl::Critical;
                return Lvl::Warning;
            };
            const Lvl lvl = parseLvl(levelStr);

            // Ensure the injected lines actually go through even in builds where
            // the global logger level filters out Info/Warning by default.
            const Logger::LogLevel prevMin = Logger::Log::GetLogLevel();
            Logger::Log::SetLogLevel(Logger::LogLevel::Trace);

            LogView* logView = nullptr;
            if (UIElement* lvEl = FindById(ui, "LogView"))
                logView = dynamic_cast<LogView*>(lvEl);

            for (int i = 0; i < count; ++i)
            {
                const std::string msg = prefix + " log line " + std::to_string(i + 1) + "/" + std::to_string(count);
                switch (lvl)
                {
                    case Lvl::Trace:    Logger::Log::Trace(msg); break;
                    case Lvl::Debug:    Logger::Log::Debug(msg); break;
                    case Lvl::Info:     Logger::Log::Info(msg); break;
                    case Lvl::Warning:  Logger::Log::Warning(msg); break;
                    case Lvl::Error:    Logger::Log::Error(msg); break;
                    case Lvl::Critical: Logger::Log::Critical(msg); break;
                }

                // Also inject directly into the Log panel if it exists.
                // Some panel update paths rely on per-panel Update() calls to flush pending messages,
                // which may not run consistently during automation. This keeps the replay deterministic.
                if (logView)
                {
                    Logger::LogLevel ll = Logger::LogLevel::Warning;
                    switch (lvl)
                    {
                        case Lvl::Trace: ll = Logger::LogLevel::Trace; break;
                        case Lvl::Debug: ll = Logger::LogLevel::Debug; break;
                        case Lvl::Info: ll = Logger::LogLevel::Info; break;
                        case Lvl::Warning: ll = Logger::LogLevel::Warning; break;
                        case Lvl::Error: ll = Logger::LogLevel::Error; break;
                        case Lvl::Critical: ll = Logger::LogLevel::Critical; break;
                    }
                    Logger::LogMessage lm(ll, msg);
                    logView->AddMessage(lm);
                }
            }

            Logger::Log::SetLogLevel(prevMin);

            if (logView)
                logView->FlushPendingMessages();
        }
        else if (ev.type == "char")
        {
            const unsigned int cp = payload.value("codepoint", 0u);
            (void)ui.OnChar(cp);
        }
        else if (ev.type == "windowResize")
        {
            const std::uint32_t w = payload.value("width", payload.value("w", 0u));
            const std::uint32_t h = payload.value("height", payload.value("h", 0u));
            if (w == 0u || h == 0u)
            {
                Fail("windowResize missing/invalid width/height");
            }
            else if (!m_Cfg.resizeWindow)
            {
                Fail("windowResize not supported by host (cfg.resizeWindow is null)");
            }
            else
            {
                std::string err;
                if (!m_Cfg.resizeWindow(m_Cfg.resizeWindowCtx, m_Cfg.windowIndex, w, h, &err))
                {
                    if (err.empty())
                        err = "unknown failure";
                    Fail("windowResize failed: w=" + std::to_string(w) + " h=" + std::to_string(h) + " err=" + err);
                }
            }
        }
        else
        {
            Fail("unknown event type: " + ev.type);
        }

        ++m_NextEventIndex;
    }
}

void UiReplayRunner::TickAfterUiUpdate(UIManager& ui, double deltaSeconds)
{
    if (!m_Enabled)
        return;

    // Emit per-frame telemetry.
    json j;
    j["kind"] = "ui_replay_frame";
    j["scenario"] = m_ScenarioName;
    j["frame"] = m_FrameIndex;
    j["dt"] = deltaSeconds;

    // UI timings:
    // - Always record wall-clock duration of UIManager::Update (covers fast paths too).
    // - Record per-section breakdown only when UIManager reports a heavy-profile frame.
    UIManager::UpdateProfileFrame profFrame{};

    double updateWallMs = 0.0;
    if (m_UpdateStartValid)
    {
        const auto end = std::chrono::steady_clock::now();
        updateWallMs = std::chrono::duration<double, std::milli>(end - m_UpdateStartTime).count();
    }
    m_UpdateStartValid = false;

    if (ui.GetLastUpdateProfileFrame(profFrame))
    {
        j["ui"] = {
            {"totalMs", updateWallMs},
            {"profileTotalMs", profFrame.TotalMs},
            {"schedulerMs", profFrame.SchedulerMs},
            {"buildYogaMs", profFrame.BuildYogaMs},
            {"cascadeComputeMs", profFrame.CascadeComputeMs},
            {"cascadeCalls", profFrame.CascadeCalls},
            {"cascadeShared", profFrame.CascadeShared},
            {"cascadeCallsBuildYoga", profFrame.CascadeCallsBuildYoga},
            {"yogaApplyMs", profFrame.YogaApplyMs},
            {"yogaMs", profFrame.YogaMs},
            {"postLayoutMs", profFrame.PostLayoutMs},
            {"hitTestMs", profFrame.HitTestMs},
            {"eventDispatchMs", profFrame.EventDispatchMs},
            {"geometryMs", profFrame.GeometryMs},
            {"cleanupMs", profFrame.CleanupMs},
            {"transitionMs", profFrame.TransitionMs},
            {"elementCount", profFrame.ElementCount},
            {"idleFrame", profFrame.IdleFrame},
            {"idleDecline", profFrame.IdleDecline},
        };
    }
    else
    {
        j["ui"] = {
            {"totalMs", updateWallMs},
            {"buildYogaMs", 0.0},
            {"yogaMs", 0.0},
            {"geometryMs", 0.0},
        };
    }

    if (m_ProbeScrollViewUnderId.has_value())
    {
        UIElement* under = FindById(ui, *m_ProbeScrollViewUnderId);
        ScrollView* sv = FindScrollViewUnder(under);
        if (sv)
        {
            j["probe"]["scrollView"] = {
                {"underId", *m_ProbeScrollViewUnderId},
                {"scrollX", sv->GetScrollX()},
                {"scrollY", sv->GetScrollY()},
                {"viewportW", sv->GetViewportWidth()},
                {"viewportH", sv->GetViewportHeight()},
                {"contentW", sv->GetContentWidth()},
                {"contentH", sv->GetContentHeight()},
            };
        }
    }

    if (m_ProbeListViewUnderId.has_value())
    {
        UIElement* under = FindById(ui, *m_ProbeListViewUnderId);
        ListView* lv = FindListViewUnder(under);
        if (lv)
        {
            j["probe"]["listView"] = {
                {"underId", *m_ProbeListViewUnderId},
                {"scrollY", lv->GetScrollOffset()},
                {"firstVisible", lv->GetFirstVisibleIndex()},
                {"visibleCount", lv->GetVisibleCount()},
                {"cellPoolSize", lv->GetCellPoolSize()},
                {"lastItemCount", lv->GetLastKnownItemCount()},
            };
        }
    }

    if (m_ProbeGridViewUnderId.has_value())
    {
        UIElement* under = FindById(ui, *m_ProbeGridViewUnderId);
        GridView* gv = FindGridViewUnder(under);
        if (gv)
        {
            if (gv->GetViewportHeight() > 0.5f)
                m_ProbeGridViewEverLive = true;
            j["probe"]["gridView"] = {
                {"underId", *m_ProbeGridViewUnderId},
                {"scrollY", gv->GetScrollOffsetY()},
                {"viewportW", gv->GetViewportWidth()},
                {"viewportH", gv->GetViewportHeight()},
                {"contentW", gv->GetContentWidth()},
                {"contentH", gv->GetContentHeight()},
                {"columns", gv->GetColumnCount()},
                {"firstRow", gv->GetFirstVisibleRow()},
                {"desiredCells", gv->GetDesiredCellCount()},
                {"lastItemCount", gv->GetLastKnownItemCount()},
                {"lastColumns", gv->GetLastKnownColumns()},
                {"lastContentHeightPx", gv->GetLastContentHeightPx()},
            };
        }
    }

    if (m_ProbeTreeViewUnderId.has_value())
    {
        UIElement* under = FindById(ui, *m_ProbeTreeViewUnderId);
        TreeView* tv = FindTreeViewUnder(under);
        if (tv)
        {
            const auto dbgTv = tv->GetDebugRowLayoutInfo();
            const auto dbgBoundRows = tv->DebugGetBoundRows(/*maxRows=*/32);
            json rows = json::array();
            for (const auto& r : dbgBoundRows)
            {
                rows.push_back({
                    {"slot", r.slotIndex},
                    {"index", r.boundIndex},
                    {"id", r.boundId},
                    {"depth", r.depth},
                    {"attached", r.attached},
                    {"label", r.label},
                });
            }
            j["probe"]["treeView"] = {
                {"underId", *m_ProbeTreeViewUnderId},
                {"scrollX", tv->GetScrollOffsetX()},
                {"scrollY", tv->GetScrollOffsetY()},
                {"viewportW", tv->GetViewportWidth()},
                {"viewportH", tv->GetViewportHeight()},
                {"contentW", tv->GetContentWidth()},
                {"contentH", tv->GetContentHeight()},
                {"firstIndex", tv->GetFirstVisibleIndex()},
                {"desiredRows", tv->GetDesiredRowCount()},
                {"flatSize", tv->GetLastKnownFlatSize()},
                {"lastViewportWPx", tv->GetLastViewportWPx()},
                {"dbgBoundVisibleRows", dbgTv.boundVisibleRows},
                {"dbgOverlaps", dbgTv.overlaps},
                {"dbgMinDeltaY", dbgTv.minDeltaY},
                {"dbgMinRowH", dbgTv.minRowH},
                {"dbgMaxRowH", dbgTv.maxRowH},
                {"dbgBoundRows", std::move(rows)},
            };
        }
    }

    if (m_ProbeDragDrop)
    {
        if (auto* dd = ui.GetDragDropManager())
        {
            json ddj;
            ddj["dragging"] = dd->IsDragging();
            ddj["payloadTypeId"] = dd->GetPayload().TypeId;
            ddj["mods"] = dd->GetCurrentMods();
            ddj["allowed"] = dd->GetCurrentFeedback().Allowed;
            ddj["reason"] = dd->GetCurrentFeedback().Reason;
            ddj["hitTargetId"] = dd->GetCurrentHit().TargetId;
            ddj["hitLocation"] = (int)dd->GetCurrentHit().Location;
            ddj["hitIndentDepth"] = dd->GetCurrentHit().IndentDepth;
            if (UIElement* el = dd->GetCurrentTargetElement())
                ddj["targetElementId"] = el->GetId();
            j["probe"]["dragDrop"] = std::move(ddj);
        }
    }

    if (m_ProbeScrollbarVUnderId.has_value())
    {
        UIElement* under = FindById(ui, *m_ProbeScrollbarVUnderId);
        ScrollView* sv = FindScrollViewUnder(under);
        UIElement* sb = sv ? sv->GetVerticalScrollbar() : nullptr;
        if (sb)
        {
            const ResolvedStyle& st = sb->GetResolvedStyle();
            j["probe"]["scrollbarV"] = {
                {"underId", *m_ProbeScrollbarVUnderId},
                {"x", sb->GetLayoutX()},
                {"y", sb->GetLayoutY()},
                {"w", sb->GetLayoutWidth()},
                {"h", sb->GetLayoutHeight()},
                {"displayNone", st.Layout.DisplayMode == DisplayMode::None},
                {"visible", st.Visual.Visible},
                {"pointerEvents", st.Visual.PointerEvents},
            };
        }
    }

    if (m_ProbeScrollbarHUnderId.has_value())
    {
        UIElement* under = FindById(ui, *m_ProbeScrollbarHUnderId);
        ScrollView* sv = FindScrollViewUnder(under);
        UIElement* sb = sv ? sv->GetHorizontalScrollbar() : nullptr;
        if (sb)
        {
            const ResolvedStyle& st = sb->GetResolvedStyle();
            j["probe"]["scrollbarH"] = {
                {"underId", *m_ProbeScrollbarHUnderId},
                {"x", sb->GetLayoutX()},
                {"y", sb->GetLayoutY()},
                {"w", sb->GetLayoutWidth()},
                {"h", sb->GetLayoutHeight()},
                {"displayNone", st.Layout.DisplayMode == DisplayMode::None},
                {"visible", st.Visual.Visible},
                {"pointerEvents", st.Visual.PointerEvents},
            };
        }
    }

    if (m_ProbeElementId.has_value())
    {
        const std::string& id = *m_ProbeElementId;
        UIElement* el = FindById(ui, id);
        if (el)
        {
            const ResolvedStyle& st = el->GetResolvedStyle();
            j["probe"]["element"] = {
                {"id", id},
                {"exists", true},
                {"tag", UIRegistration::ElementFactoryRegistry::Instance().GetTagForType(typeid(*el))},
                {"text", el->GetTextContent()},
                {"x", el->GetLayoutX()},
                {"y", el->GetLayoutY()},
                {"w", el->GetLayoutWidth()},
                {"h", el->GetLayoutHeight()},
                {"displayNone", st.Layout.DisplayMode == DisplayMode::None},
                {"visible", st.Visual.Visible},
                {"pointerEvents", st.Visual.PointerEvents},
                {"hasClassOpen", el->HasClass("open")},
            };
        }
        else
        {
            j["probe"]["element"] = {
                {"id", id},
                {"exists", false},
            };
        }
    }

    if (m_ProbeCpuProfilerEnabled && ((m_FrameIndex % (std::uint64_t)m_ProbeCpuProfilerEveryNFrames) == 0ull))
    {
        auto& cpuProf = GameEngine::Profiling::CpuProfiler::Get();
        std::vector<std::pair<std::string_view, GameEngine::Profiling::CpuProfiler::Sample>> snap;
        cpuProf.CopyFrameSamples(snap);

        auto startsWith = [](std::string_view s, std::string_view pre) -> bool
        {
            return s.size() >= pre.size() && s.substr(0, pre.size()) == pre;
        };

        std::vector<std::pair<std::string_view, GameEngine::Profiling::CpuProfiler::Sample>> filtered;
        filtered.reserve(snap.size());
        if (m_ProbeCpuProfilerPrefixes.empty())
        {
            filtered = std::move(snap);
        }
        else
        {
            for (const auto& kv : snap)
            {
                bool match = false;
                for (const auto& pre : m_ProbeCpuProfilerPrefixes)
                {
                    if (!pre.empty() && startsWith(kv.first, pre))
                    {
                        match = true;
                        break;
                    }
                }
                if (match)
                    filtered.push_back(kv);
            }
        }

        std::sort(filtered.begin(), filtered.end(),
                  [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });

        const std::size_t n = std::min<std::size_t>(filtered.size(), (std::size_t)m_ProbeCpuProfilerTopN);
        json arr = json::array();
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& [name, s] = filtered[i];
            arr.push_back({
                {"name", std::string(name)},
                {"ms", s.totalMs},
                {"count", s.count},
            });
        }

        j["probe"]["cpuProfiler"] = {
            {"everyNFrames", m_ProbeCpuProfilerEveryNFrames},
            {"top", arr},
        };
    }

    if (m_ProbeRenderSyncTimings)
    {
        if (auto* dev = ui.GetDevice())
        {
            Rendering::IDevice::FrameSyncTimings st{};
            if (dev->GetLastFrameSyncTimings(st))
            {
                j["probe"]["renderSync"] = {
                    {"frameIndex", st.frameIndex},
                    {"beginFrameWaitMs", st.beginFrameWaitMs},
                    {"waitedGraphicsFence", st.waitedGraphicsFence},
                    {"waitedComputeFence", st.waitedComputeFence},
                    {"waitedTransferFence", st.waitedTransferFence},
                    {"acquireMs", st.acquireMs},
                    {"acquireTimedOut", st.acquireTimedOut},
                    {"presentTransitionSubmitMs", st.presentTransitionSubmitMs},
                    {"presentMs", st.presentMs},
                };
            }
        }
    }

    if (m_ProbeDumpTabIds && m_FrameIndex == 0)
    {
        json tabIds = json::array();
        if (UIElement* root = ui.GetRootElement())
        {
            std::vector<UIElement*> stack;
            stack.reserve(256);
            stack.push_back(root);
            while (!stack.empty() && tabIds.size() < 64)
            {
                UIElement* el = stack.back();
                stack.pop_back();
                if (!el)
                    continue;
                const std::string& id = el->GetId();
                if (id.rfind("tab:", 0) == 0)
                {
                    tabIds.push_back(id);
                }
                for (const auto& ch : el->GetChildren())
                {
                    if (ch)
                        stack.push_back(ch.get());
                }
            }
        }
        j["probe"]["tabIds"] = tabIds;
    }

    if (m_ProbeDumpIdsContaining.has_value())
    {
        const std::string& needle = *m_ProbeDumpIdsContaining;
        const std::uint64_t targetFrame = m_ProbeDumpIdsContainingFrame.value_or(0);
        if (m_FrameIndex == targetFrame)
        {
            json ids = json::array();
            if (UIElement* root = ui.GetRootElement())
            {
                std::vector<UIElement*> stack;
                stack.reserve(256);
                stack.push_back(root);
                while (!stack.empty() && ids.size() < 64)
                {
                    UIElement* el = stack.back();
                    stack.pop_back();
                    if (!el)
                        continue;
                    const std::string& id = el->GetId();
                    if (!id.empty() && id.find(needle) != std::string::npos)
                    {
                        ids.push_back(id);
                    }
                    for (const auto& ch : el->GetChildren())
                    {
                        if (ch)
                            stack.push_back(ch.get());
                    }
                    if (auto* m = dynamic_cast<Mount*>(el))
                    {
                        if (UIElement* tgt = m->GetTarget())
                            stack.push_back(tgt);
                    }
                }
            }
            j["probe"]["idsContaining"] = {{"needle", needle}, {"ids", ids}};
        }
    }

    j["mouse"] = {{"x", m_MouseX}, {"y", m_MouseY}};
    j["focusId"] = ui.GetFocusedElementId();
    j["hoverId"] = ui.GetHoveredElementDebugName();
    j["capture"] = {{"captured", ui.IsMouseCaptured()}, {"captureId", ui.GetCaptureId()}};

    WriteJsonl(j);

    // Run scenario-configured assertions.
    if (!m_Failed && m_AssertListViewUpdatesDuringCapture.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertListViewUpdatesDuringCapture.frameStart && f <= m_AssertListViewUpdatesDuringCapture.frameEnd)
        {
            if (ui.IsMouseCaptured())
                m_AssertSawCaptureInRange = true;

            UIElement* under = FindById(ui, m_AssertListViewUpdatesDuringCapture.listViewUnderId);
            ListView* lv = FindListViewUnder(under);
            if (lv)
            {
                const int curFirst = lv->GetFirstVisibleIndex();
                if (f == m_AssertListViewUpdatesDuringCapture.frameStart)
                {
                    m_AssertFirstVisibleAtRangeStart = curFirst;
                }
                else if (m_AssertFirstVisibleAtRangeStart != -999999 && curFirst != m_AssertFirstVisibleAtRangeStart)
                {
                    m_AssertSawFirstVisibleChangeInRange = true;
                }
            }
        }

        // If we've passed the end frame, finalize.
        if (f == m_AssertListViewUpdatesDuringCapture.frameEnd)
        {
            if (!m_AssertSawCaptureInRange)
            {
                Fail("assert(listViewUpdatesDuringCapture) failed: no mouse capture observed in frame range");
            }
            else if (!m_AssertSawFirstVisibleChangeInRange)
            {
                Fail("assert(listViewUpdatesDuringCapture) failed: ListView firstVisible did not change during captured drag frame range");
            }
        }
    }

    if (!m_Failed && m_AssertTreeViewUpdatesDuringCapture.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertTreeViewUpdatesDuringCapture.frameStart && f <= m_AssertTreeViewUpdatesDuringCapture.frameEnd)
        {
            if (ui.IsMouseCaptured())
                m_AssertSawTreeCaptureInRange = true;

            UIElement* under = FindById(ui, m_AssertTreeViewUpdatesDuringCapture.treeViewUnderId);
            TreeView* tv = FindTreeViewUnder(under);
            if (tv)
            {
                const int curFirst = tv->GetFirstVisibleIndex();
                if (f == m_AssertTreeViewUpdatesDuringCapture.frameStart)
                {
                    m_AssertTreeFirstIndexAtRangeStart = curFirst;
                }
                else if (m_AssertTreeFirstIndexAtRangeStart != -999999 && curFirst != m_AssertTreeFirstIndexAtRangeStart)
                {
                    m_AssertSawTreeFirstIndexChangeInRange = true;
                }
            }
        }

        if (f == m_AssertTreeViewUpdatesDuringCapture.frameEnd)
        {
            if (!m_AssertSawTreeCaptureInRange)
            {
                Fail("assert(treeViewUpdatesDuringCapture) failed: no mouse capture observed in frame range");
            }
            else if (!m_AssertSawTreeFirstIndexChangeInRange)
            {
                Fail("assert(treeViewUpdatesDuringCapture) failed: TreeView firstIndex did not change during captured drag frame range");
            }
        }
    }

    if (!m_Failed && m_AssertGridViewUpdatesDuringCapture.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertGridViewUpdatesDuringCapture.frameStart && f <= m_AssertGridViewUpdatesDuringCapture.frameEnd)
        {
            if (ui.IsMouseCaptured())
                m_AssertSawGridCaptureInRange = true;

            UIElement* under = FindById(ui, m_AssertGridViewUpdatesDuringCapture.gridViewUnderId);
            GridView* gv = FindGridViewUnder(under);
            if (gv)
            {
                const int curFirstRow = gv->GetFirstVisibleRow();
                if (f == m_AssertGridViewUpdatesDuringCapture.frameStart)
                {
                    m_AssertGridFirstRowAtRangeStart = curFirstRow;
                }
                else if (m_AssertGridFirstRowAtRangeStart != -999999 && curFirstRow != m_AssertGridFirstRowAtRangeStart)
                {
                    m_AssertSawGridFirstRowChangeInRange = true;
                }
            }
        }

        if (f == m_AssertGridViewUpdatesDuringCapture.frameEnd)
        {
            if (!m_AssertSawGridCaptureInRange)
            {
                Fail("assert(gridViewUpdatesDuringCapture) failed: no mouse capture observed in frame range");
            }
            else if (!m_AssertSawGridFirstRowChangeInRange)
            {
                Fail("assert(gridViewUpdatesDuringCapture) failed: GridView firstVisibleRow did not change during captured drag frame range");
            }
        }
    }

    if (!m_Failed && m_AssertScrollOnlyPath.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertScrollOnlyPath.frameStart && f <= m_AssertScrollOnlyPath.frameEnd)
        {
            if (m_ScrollEventDispatchedThisFrame)
            {
                ++m_AssertScrollOnlyScrollFrames;
                UIManager::UpdateProfileFrame pf{};
                const bool haveProf = ui.GetLastUpdateProfileFrame(pf);
                const double buildYogaMs = haveProf ? pf.BuildYogaMs : 0.0;
                const double geometryMs = haveProf ? pf.GeometryMs : 0.0;
                if (buildYogaMs <= m_AssertScrollOnlyPath.maxBuildYogaMs &&
                    geometryMs <= m_AssertScrollOnlyPath.maxGeometryMs)
                {
                    ++m_AssertScrollOnlyLightFrames;
                }
            }
        }

        if (f == m_AssertScrollOnlyPath.frameEnd)
        {
            const std::uint32_t need = std::max(1u, m_AssertScrollOnlyPath.minFrames);
            if (m_AssertScrollOnlyScrollFrames == 0)
            {
                Fail("assert(scrollOnlyPathAtLeast) failed: no scroll events dispatched in frame range [" +
                     std::to_string(m_AssertScrollOnlyPath.frameStart) + ".." +
                     std::to_string(m_AssertScrollOnlyPath.frameEnd) +
                     "] - scenario cannot exercise the scroll path");
            }
            else if (m_AssertScrollOnlyLightFrames < need)
            {
                Fail("assert(scrollOnlyPathAtLeast) failed: lightScrollFrames=" +
                     std::to_string(m_AssertScrollOnlyLightFrames) + " of scrollFrames=" +
                     std::to_string(m_AssertScrollOnlyScrollFrames) + " need>=" + std::to_string(need) +
                     " (maxBuildYogaMs=" + std::to_string(m_AssertScrollOnlyPath.maxBuildYogaMs) +
                     " maxGeometryMs=" + std::to_string(m_AssertScrollOnlyPath.maxGeometryMs) + ")");
            }
        }
    }

    if (!m_Failed && m_AssertPerfBudget.enabled && ui.GetLastUpdateProfileFrame(profFrame))
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertPerfBudget.frameStart && f <= m_AssertPerfBudget.frameEnd)
        {
            if (m_AssertPerfBudget.maxTotalMs > 0.0 && profFrame.TotalMs > m_AssertPerfBudget.maxTotalMs)
            {
                Fail("assert(perfBudget) failed: totalMs exceeded budget (totalMs=" + std::to_string(profFrame.TotalMs) +
                     " budget=" + std::to_string(m_AssertPerfBudget.maxTotalMs) + ")");
            }
            if (m_AssertPerfBudget.maxBuildYogaMs > 0.0 && profFrame.BuildYogaMs > m_AssertPerfBudget.maxBuildYogaMs)
            {
                Fail("assert(perfBudget) failed: buildYogaMs exceeded budget (buildYogaMs=" + std::to_string(profFrame.BuildYogaMs) +
                     " budget=" + std::to_string(m_AssertPerfBudget.maxBuildYogaMs) + ")");
            }
            if (m_AssertPerfBudget.maxGeometryMs > 0.0 && profFrame.GeometryMs > m_AssertPerfBudget.maxGeometryMs)
            {
                Fail("assert(perfBudget) failed: geometryMs exceeded budget (geometryMs=" + std::to_string(profFrame.GeometryMs) +
                     " budget=" + std::to_string(m_AssertPerfBudget.maxGeometryMs) + ")");
            }
        }
    }

    if (!m_Failed && m_AssertElementVisible.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertElementVisible.frameStart && f <= m_AssertElementVisible.frameEnd)
        {
            UIElement* el = FindById(ui, m_AssertElementVisible.elementId);
            if (!el)
            {
                Fail("assert(elementVisible) failed: element not found (id='" + m_AssertElementVisible.elementId + "')");
            }
            else
            {
                const ResolvedStyle& st = el->GetResolvedStyle();

                const bool displayNone = (st.Layout.DisplayMode == DisplayMode::None);
                const bool visible = st.Visual.Visible;
                const float w = el->GetLayoutWidth();
                const float h = el->GetLayoutHeight();
                if (displayNone || !visible || w <= 0.0f || h <= 0.0f)
                {
                    Fail("assert(elementVisible) failed: element not visible (id='" + m_AssertElementVisible.elementId +
                         "' displayNone=" + std::string(displayNone ? "true" : "false") +
                         " visible=" + std::string(visible ? "true" : "false") +
                         " w=" + std::to_string(w) +
                         " h=" + std::to_string(h) + ")");
                }
            }
        }
    }


    if (!m_Failed && m_AssertLayoutConverged.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertLayoutConverged.frameStart && f <= m_AssertLayoutConverged.frameEnd)
        {
            UIElement* root = FindById(ui, m_AssertLayoutConverged.elementId);
            if (!root)
            {
                Fail("assert(layoutConverged) failed: element not found (id='" + m_AssertLayoutConverged.elementId + "')");
            }
            else
            {
                unsigned flags = UIElement::LayoutDirty | UIElement::ChildrenDirty;
                if (m_AssertLayoutConverged.includeStyleDirty)
                    flags |= UIElement::StyleDirty;

                std::vector<UIElement*> stack;
                stack.reserve(256);
                stack.push_back(root);
                while (!stack.empty())
                {
                    UIElement* el = stack.back();
                    stack.pop_back();
                    if (!el)
                        continue;

                    // Skip subtrees that are display:none. These elements are intentionally excluded
                    // from layout/geometry work and can carry dirties that won't be cleared until
                    // they become visible again.
                    if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
                    {
                        continue;
                    }

                    if (el->IsDirty(flags))
                    {
                        const bool ld = el->IsDirty(UIElement::LayoutDirty);
                        const bool cd = el->IsDirty(UIElement::ChildrenDirty);
                        const bool sd = el->IsDirty(UIElement::StyleDirty);
                        std::string id = el->GetId();
                        if (id.empty())
                            id = UIRegistration::ElementFactoryRegistry::Instance().GetTagForType(typeid(*el)) + "#" + std::to_string(el->GetInstanceId());
                        std::string why = "layout=" + std::string(ld ? "1" : "0") +
                                          " children=" + std::string(cd ? "1" : "0");
                        if (m_AssertLayoutConverged.includeStyleDirty)
                            why += " style=" + std::string(sd ? "1" : "0");
                        Fail("assert(layoutConverged) failed: dirty element '" + id + "' (" + why + ") at frame " + std::to_string(f));
                        break;
                    }

                    for (const auto& ch : el->GetChildren())
                    {
                        if (ch)
                            stack.push_back(ch.get());
                    }
                    if (auto* m = dynamic_cast<Mount*>(el))
                    {
                        if (UIElement* tgt = m->GetTarget())
                            stack.push_back(tgt);
                    }
                }
            }
        }
    }

    if (!m_Failed && m_AssertElementTextChanges.enabled)
    {
        const std::uint64_t f = m_FrameIndex;
        if (f >= m_AssertElementTextChanges.frameStart && f <= m_AssertElementTextChanges.frameEnd)
        {
            UIElement* el = FindById(ui, m_AssertElementTextChanges.elementId);
            if (!el)
            {
                Fail("assert(elementTextChanges) failed: element not found (id='" + m_AssertElementTextChanges.elementId + "')");
            }
            else
            {
                const std::string cur = el->GetTextContent();
                if (f == m_AssertElementTextChanges.frameStart)
                {
                    m_AssertElementTextPrev = cur;
                }
                else
                {
                    if (cur != m_AssertElementTextPrev)
                    {
                        ++m_AssertElementTextChangeCount;
                        m_AssertElementTextPrev = cur;
                    }
                }
            }
        }

        if (f == m_AssertElementTextChanges.frameEnd && !m_Failed)
        {
            if (m_AssertElementTextChangeCount < m_AssertElementTextChanges.minChanges)
            {
                Fail("assert(elementTextChanges) failed: text changed " + std::to_string(m_AssertElementTextChangeCount) +
                     " times (minChanges=" + std::to_string(m_AssertElementTextChanges.minChanges) +
                     ", id='" + m_AssertElementTextChanges.elementId + "')");
            }
        }
    }

    // Selected-count assertions.
    if (!m_Failed && !m_AssertSelectedCounts.empty())
    {
        auto countSelected = [](UIElement* root, const std::string& requiredClass) -> std::uint32_t
        {
            if (!root)
                return 0;
            std::uint32_t count = 0;
            std::vector<UIElement*> stack;
            stack.reserve(256);
            stack.push_back(root);
            std::uint32_t safety = 0;
            while (!stack.empty())
            {
                UIElement* cur = stack.back();
                stack.pop_back();
                if (!cur)
                    continue;
#if defined(_MSC_VER)
                __analysis_assume(cur != nullptr);
#endif
                if (++safety > 200000u)
                    break;

                if (cur->HasClass("selected"))
                {
                    if (requiredClass.empty() || cur->HasClass(requiredClass))
                        ++count;
                }
                if (cur)
                {
                    for (const auto& ch : cur->GetChildren())
                    {
                        if (ch)
                            stack.push_back(ch.get());
                    }
                    if (auto* m = dynamic_cast<Mount*>(cur))
                    {
                        if (UIElement* tgt = m->GetTarget())
                            stack.push_back(tgt);
                    }
                }
            }
            return count;
        };

        const std::uint64_t f = m_FrameIndex;
        for (const auto& a : m_AssertSelectedCounts)
        {
            if (!a.enabled)
                continue;
            if (f < a.frameStart || f > a.frameEnd)
                continue;
            UIElement* rootEl = FindById(ui, a.subtreeId);
            if (!rootEl)
            {
                Fail("assert(selectedCount) failed: subtree not found (id='" + a.subtreeId + "')");
                continue;
            }
            const std::uint32_t got = countSelected(rootEl, a.requiredClass);
            if (a.exact != 0xFFFFFFFFu)
            {
                if (got != a.exact)
                    Fail("assert(selectedCount) failed: expected exact=" + std::to_string(a.exact) + " got=" + std::to_string(got) +
                         " (subtreeId='" + a.subtreeId + "')");
            }
            else if (got < a.atLeast)
            {
                Fail("assert(selectedCount) failed: expected atLeast=" + std::to_string(a.atLeast) + " got=" + std::to_string(got) +
                     " (subtreeId='" + a.subtreeId + "')");
            }
        }
    }

    // Text-contains assertions.
    if (!m_Failed && !m_AssertTextContains.empty())
    {
        auto countText = [](UIElement* root, const std::string& requiredClass, const std::string& needle) -> std::uint32_t
        {
            if (!root || needle.empty())
                return 0;
            std::uint32_t count = 0;
            std::vector<UIElement*> stack;
            stack.reserve(256);
            stack.push_back(root);
            std::uint32_t safety = 0;
            while (!stack.empty())
            {
                UIElement* cur = stack.back();
                stack.pop_back();
                if (!cur)
                    continue;
                if (++safety > 200000u)
                    break;

                Label* lbl = dynamic_cast<Label*>(cur);
                if (lbl)
                {
                    if (requiredClass.empty() || lbl->HasClass(requiredClass))
                    {
                        if (lbl->GetText().find(needle) != std::string::npos)
                            ++count;
                    }
                }
                if (cur)
                {
                    for (const auto& ch : cur->GetChildren())
                    {
                        if (ch)
                            stack.push_back(ch.get());
                    }
                    if (auto* m = dynamic_cast<Mount*>(cur))
                    {
                        if (UIElement* tgt = m->GetTarget())
                            stack.push_back(tgt);
                    }
                }
            }
            return count;
        };

        const std::uint64_t f = m_FrameIndex;
        for (const auto& a : m_AssertTextContains)
        {
            if (!a.enabled)
                continue;
            if (f < a.frameStart || f > a.frameEnd)
                continue;
            UIElement* rootEl = FindById(ui, a.subtreeId);
            if (!rootEl)
            {
                Fail("assert(textContains) failed: subtree not found (id='" + a.subtreeId + "')");
                continue;
            }
            const std::uint32_t got = countText(rootEl, a.requiredClass, a.needle);
            if (a.exact != 0xFFFFFFFFu && got != a.exact)
            {
                Fail("assert(textContains) failed: expected exact=" + std::to_string(a.exact) + " got=" + std::to_string(got) +
                     " needle='" + a.needle + "' (subtreeId='" + a.subtreeId + "')");
                continue;
            }
            if (got < a.atLeast)
            {
                Fail("assert(textContains) failed: expected atLeast=" + std::to_string(a.atLeast) + " got=" + std::to_string(got) +
                     " needle='" + a.needle + "' (subtreeId='" + a.subtreeId + "')");
            }
        }
    }

    // TreeView bound-row structure assertions (uses TreeView debug row binding).
    if (!m_Failed && !m_AssertTreeBoundRowsContain.empty())
    {
        const std::uint64_t f = m_FrameIndex;
        for (auto& a : m_AssertTreeBoundRowsContain)
        {
            if (!a.enabled)
                continue;
            if (f < a.frameStart || f > a.frameEnd)
                continue;

            UIElement* under = FindById(ui, a.treeViewUnderId);
            TreeView* tv = FindTreeViewUnder(under);
            if (!tv)
            {
                Fail("assert(treeBoundRowsContain) failed: TreeView not found (underId='" + a.treeViewUnderId + "')");
                continue;
            }

            const auto dbgBoundRows = tv->DebugGetBoundRows(/*maxRows=*/128);

            auto matches = [&](const TreeViewBoundRowsContainAssertConfig::ExpectedRow& er) -> bool
            {
                for (const auto& r : dbgBoundRows)
                {
                    if (!r.attached)
                        continue;
                    if (r.label != er.label)
                        continue;
                    if (er.depth >= 0 && r.depth != er.depth)
                        return false;
                    return true;
                }
                return false;
            };

            bool ok = true;
            for (const auto& er : a.rows)
            {
                if (!matches(er))
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                a.satisfiedFrames++;

            if (f == a.frameEnd)
            {
                const std::uint32_t need = std::max(1u, a.minFrames);
                if (a.satisfiedFrames < need)
                {
                    Fail("assert(treeBoundRowsContain) failed: satisfiedFrames=" + std::to_string(a.satisfiedFrames) +
                         " need=" + std::to_string(need) + " (underId='" + a.treeViewUnderId + "')");
                }
            }
        }
    }

    // Exit policy.
    const bool eventsDone = (m_NextEventIndex >= m_Events.size());
    const bool hitMaxFrames = (m_Cfg.exitAfterFrames > 0 && (m_FrameIndex + 1) >= m_Cfg.exitAfterFrames);
    const bool metMinFrames = ((m_FrameIndex + 1) >= m_MinFrames);
    if ((eventsDone && metMinFrames) || hitMaxFrames)
    {
        m_ShouldExit = true;

        // A scenario that opted into requireGridViewLive but whose gridView probe
        // never resolved to a sized viewport scrolled a hidden/empty grid — its
        // grid asserts were vacuous. Fail loudly so this can't silently no-op.
        if (!m_Failed && m_ProbeGridViewRequireLive && !m_ProbeGridViewEverLive)
        {
            Fail("gridView probe under '" + m_ProbeGridViewUnderId.value_or(std::string()) +
                 "' never became live (viewport height stayed 0) - the scenario scrolled a "
                 "hidden/empty grid; drive the Assets panel to grid view at scenario start");
        }

        json end;
        end["kind"] = "ui_replay_end";
        end["scenario"] = m_ScenarioName;
        end["frame"] = m_FrameIndex;
        end["result"] = m_Failed ? "fail" : "pass";
        end["reason"] = hitMaxFrames ? "max_frames" : "events_done";
        if (m_MinFrames > 0)
            end["minFrames"] = m_MinFrames;
        WriteJsonl(end);

        // Restore CPU profiler enabled state if the scenario changed it.
        if (m_ProbeCpuProfilerEnabled)
        {
            GameEngine::Profiling::CpuProfiler::Get().SetEnabled(m_CpuProfilerWasEnabled);
        }
    }

    ++m_FrameIndex;
}

bool UiReplayRunner::ComputeProbeRegion(std::uint32_t texW, std::uint32_t texH,
                                        std::uint32_t& outSrcX, std::uint32_t& outSrcY,
                                        std::uint32_t& outW, std::uint32_t& outH) const
{
    if (texW == 0 || texH == 0)
        return false;
    std::uint32_t copyW = 1;
    std::uint32_t copyH = 1;
    std::uint32_t srcX = 0;
    std::uint32_t srcY = 0;
    if (m_ProbePixelHash.useNormalizedRegion)
    {
        auto clamp01 = [](float v) -> float { return std::max(0.0f, std::min(1.0f, v)); };
        const float nx = clamp01(m_ProbePixelHash.normX);
        const float ny = clamp01(m_ProbePixelHash.normY);
        const float nw = clamp01(m_ProbePixelHash.normW);
        const float nh = clamp01(m_ProbePixelHash.normH);
        srcX = (std::uint32_t)std::floor(nx * (float)texW);
        srcY = (std::uint32_t)std::floor(ny * (float)texH);
        copyW = (std::uint32_t)std::floor(nw * (float)texW);
        copyH = (std::uint32_t)std::floor(nh * (float)texH);
        if (copyW == 0)
            copyW = 1;
        if (copyH == 0)
            copyH = 1;
        if (srcX >= texW)
            srcX = texW - 1;
        if (srcY >= texH)
            srcY = texH - 1;
        if (srcX + copyW > texW)
            copyW = texW - srcX;
        if (srcY + copyH > texH)
            copyH = texH - srcY;
    }
    else
    {
        copyW = (m_ProbePixelHash.sampleWidth > 0) ? std::min(m_ProbePixelHash.sampleWidth, texW) : texW;
        copyH = (m_ProbePixelHash.sampleHeight > 0) ? std::min(m_ProbePixelHash.sampleHeight, texH) : texH;
        if (copyW == 0)
            copyW = 1;
        if (copyH == 0)
            copyH = 1;
        if (m_ProbePixelHash.center)
        {
            srcX = (texW > copyW) ? ((texW - copyW) / 2u) : 0u;
            srcY = (texH > copyH) ? ((texH - copyH) / 2u) : 0u;
        }
        else
        {
            srcX = std::min(m_ProbePixelHash.srcX, (texW > copyW) ? (texW - copyW) : 0u);
            srcY = std::min(m_ProbePixelHash.srcY, (texH > copyH) ? (texH - copyH) : 0u);
        }
    }
    outSrcX = srcX;
    outSrcY = srcY;
    outW = copyW;
    outH = copyH;
    return true;
}

void UiReplayRunner::ConsumePixelRgb(const std::uint8_t* data, std::uint32_t width,
                                     std::uint32_t height, const PixelConsumeMeta& meta)
{
    // Hash a canonical RGB8 view of the sampled region (not raw GPU bytes).
    // This avoids false mismatches when formats differ in packing/alpha bits
    // but the visible pixels are identical (e.g. RGB10A2 alpha/padding).
    static thread_local std::vector<std::uint8_t> rgbScratch;
    const size_t rgbBytes = static_cast<size_t>(width) * height * 3ull;
    rgbScratch.clear();
    rgbScratch.resize(rgbBytes, 0u);

    if (!meta.Space)
    {
        json j;
        j["kind"] = "ui_replay_pixel_hash_skipped";
        j["scenario"] = m_ScenarioName;
        j["frame"] = meta.Frame;
        j["reason"] = 5u; // no_source_space
        j["arm"] = meta.Arm;
        WriteJsonl(j);
        return;
    }
    // The declared space alone decides the transfer curve; the format below only
    // decides how the bytes unpack.
    const bool sourceIsLinear = !UI::IsEncodedAtRest(*meta.Space);

    const std::uint8_t* src = data;
    const auto fmt = (Rendering::TextureFormat)meta.Format;
    const bool isBgra =
        (fmt == Rendering::TextureFormat::BGRA8_UNORM) || (fmt == Rendering::TextureFormat::BGRA8_SRGB);
    const bool isRgba8 =
        (fmt == Rendering::TextureFormat::RGBA8_UNORM) || (fmt == Rendering::TextureFormat::RGBA8_SRGB) || isBgra;
    const bool isRgb10A2 = (fmt == Rendering::TextureFormat::RGB10A2_UNORM);
    const bool isRgba16f = (fmt == Rendering::TextureFormat::R16G16B16A16_FLOAT);
    const bool isR11G11B10 = (fmt == Rendering::TextureFormat::R11G11B10_FLOAT);

    if (isRgba8)
    {
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::uint8_t* rowBytes = src + (size_t)y * (size_t)width * 4ull;
            std::uint8_t* out = rgbScratch.data() + (size_t)y * (size_t)width * 3ull;
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::uint8_t* px = rowBytes + (size_t)x * 4ull;
                if (isBgra)
                {
                    out[0] = px[2]; // R
                    out[1] = px[1]; // G
                    out[2] = px[0]; // B
                }
                else
                {
                    out[0] = px[0]; // R
                    out[1] = px[1]; // G
                    out[2] = px[2]; // B
                }
                out += 3;
            }
        }
    }
    else if (isRgb10A2)
    {
        auto to8 = [](std::uint32_t c10) -> std::uint8_t
        {
            // map [0..1023] -> [0..255] with rounding
            return (std::uint8_t)((c10 * 255u + 511u) / 1023u);
        };
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::uint32_t* row = reinterpret_cast<const std::uint32_t*>(src + (size_t)y * (size_t)width * 4ull);
            std::uint8_t* out = rgbScratch.data() + (size_t)y * (size_t)width * 3ull;
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::uint32_t v = row[x];
                out[0] = to8((v >> 0) & 0x3FFu);
                out[1] = to8((v >> 10) & 0x3FFu);
                out[2] = to8((v >> 20) & 0x3FFu);
                out += 3;
            }
        }
    }
    else if (isRgba16f)
    {
        auto to8 = [](float v) -> std::uint8_t
        {
            // Clamp to [0..1] (we're validating parity, not HDR tonemap).
            if (!(v >= 0.0f))
                v = 0.0f;
            if (v > 1.0f)
                v = 1.0f;
            return (std::uint8_t)std::lround(v * 255.0f);
        };
        // An un-encoded linear source (the RenderGraph FinalLinear composite) must be
        // sRGB-encoded before byte thresholds apply — guardrails like
        // failIfMeanByteBelow were calibrated on sRGB bytes (sRGB 30 ≈ linear
        // 3, far under the smoke thresholds).
        const bool encode = sourceIsLinear;
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::uint16_t* row = reinterpret_cast<const std::uint16_t*>(src + (size_t)y * (size_t)width * 8ull);
            std::uint8_t* out = rgbScratch.data() + (size_t)y * (size_t)width * 3ull;
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::uint16_t* px = row + (size_t)x * 4ull;
                float r = Mathematics::HalfToFloat(px[0]);
                float g = Mathematics::HalfToFloat(px[1]);
                float b = Mathematics::HalfToFloat(px[2]);
                if (encode)
                {
                    r = LinearToSrgb01(r);
                    g = LinearToSrgb01(g);
                    b = LinearToSrgb01(b);
                }
                out[0] = to8(r);
                out[1] = to8(g);
                out[2] = to8(b);
                out += 3;
            }
        }
    }
    else if (isR11G11B10)
    {
        auto to8 = [](float v) -> std::uint8_t
        {
            if (!(v >= 0.0f))
                v = 0.0f;
            if (v > 1.0f)
                v = 1.0f;
            return (std::uint8_t)std::lround(v * 255.0f);
        };
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::uint32_t* row = reinterpret_cast<const std::uint32_t*>(src + (size_t)y * (size_t)width * 4ull);
            std::uint8_t* out = rgbScratch.data() + (size_t)y * (size_t)width * 3ull;
            for (std::uint32_t x = 0; x < width; ++x)
            {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                R11G11B10ToRgb01(row[x], r, g, b);
                if (sourceIsLinear)
                {
                    r = LinearToSrgb01(r);
                    g = LinearToSrgb01(g);
                    b = LinearToSrgb01(b);
                }
                out[0] = to8(r);
                out[1] = to8(g);
                out[2] = to8(b);
                out += 3;
            }
        }
    }
    else
    {
        // Unhandled formats: keep zeros (black). Still yields stable hash.
    }

    std::uint64_t nonZeroBytes = 0;
    std::uint64_t sumBytes = 0;
    for (std::uint8_t b : rgbScratch)
    {
        sumBytes += (std::uint64_t)b;
        if (b != 0u)
            ++nonZeroBytes;
    }
    const double meanByte = rgbScratch.empty() ? 0.0 : (double)sumBytes / (double)rgbScratch.size();

    const std::uint64_t h = HashFnv1a64(rgbScratch.data(), rgbScratch.size());

    bool failingPixelHash = false;
    std::string failReason;
    if (!rgbScratch.empty())
    {
        if (m_ProbePixelHash.failOnAllBlack && nonZeroBytes == 0)
        {
            failingPixelHash = true;
            failReason = "pixelHash region is fully black (all sampled bytes are 0)";
        }
        if (!failingPixelHash && m_ProbePixelHash.failIfNonZeroBytesBelow > 0 &&
            nonZeroBytes < m_ProbePixelHash.failIfNonZeroBytesBelow)
        {
            failingPixelHash = true;
            failReason = "pixelHash region is too sparse (nonZeroBytes below threshold)";
        }
        if (!failingPixelHash && m_ProbePixelHash.failIfMeanByteBelow >= 0.0 &&
            meanByte < m_ProbePixelHash.failIfMeanByteBelow)
        {
            failingPixelHash = true;
            failReason = "pixelHash region is too dark (meanByte below threshold)";
        }
    }

    std::string ppmPathStr;
    const bool shouldDumpPpm = (m_ProbePixelHash.dumpPpm || failingPixelHash) && !m_Cfg.outputJsonlPath.empty();
    if (shouldDumpPpm)
    {
        // Dump a small sampled region for manual inspection (binary PPM P6, RGB).
        // File will be written next to the jsonl log, and name is derived from the log stem.
        try
        {
            const std::filesystem::path outDir = m_Cfg.outputJsonlPath.parent_path();
            const std::string stem = m_Cfg.outputJsonlPath.stem().string();
            const std::filesystem::path ppmPath =
                outDir / (stem + ".frame" + std::to_string(meta.Frame) + ".x" + std::to_string(meta.SrcX) + ".y" +
                          std::to_string(meta.SrcY) + ".w" + std::to_string(width) + ".h" +
                          std::to_string(height) + ".ppm");

            std::ofstream ppm(ppmPath, std::ios::binary);
            if (ppm.is_open())
            {
                ppm << "P6\n" << width << " " << height << "\n255\n";
                for (std::uint32_t y = 0; y < height; ++y)
                {
                    const std::uint8_t* row = rgbScratch.data() + (size_t)y * (size_t)width * 3ull;
                    ppm.write(reinterpret_cast<const char*>(row), (std::streamsize)((size_t)width * 3ull));
                }
            }
            ppmPathStr = ppmPath.string();
        }
        catch (...)
        {
            // Best-effort dump; ignore errors.
        }
    }

    json j;
    j["kind"] = "ui_replay_pixel_hash";
    j["scenario"] = m_ScenarioName;
    j["frame"] = meta.Frame;
    j["hash64"] = ToHex64(h);
    j["arm"] = meta.Arm;
    j["src"] = meta.Src;
    j["sourceIsLinear"] = sourceIsLinear;
    j["hasCompletionToken"] = meta.HasCompletionToken;
    j["x"] = meta.SrcX;
    j["y"] = meta.SrcY;
    j["w"] = width;
    j["h"] = height;
    j["format"] = meta.Format;
    j["texW"] = meta.TexW;
    j["texH"] = meta.TexH;
    j["nonZeroBytes"] = nonZeroBytes;
    j["meanByte"] = meanByte;
    if (meta.Extra.is_object())
    {
        for (auto it = meta.Extra.begin(); it != meta.Extra.end(); ++it)
            j[it.key()] = it.value();
    }
    if (!ppmPathStr.empty())
        j["ppmPath"] = ppmPathStr;
    WriteJsonl(j);

    if (failingPixelHash)
    {
        std::string msg = failReason + ".";
        msg += " meanByte=" + std::to_string(meanByte);
        msg += " nonZeroBytes=" + std::to_string(nonZeroBytes) + "/" + std::to_string((std::uint64_t)rgbScratch.size());
        if (m_ProbePixelHash.failIfMeanByteBelow >= 0.0)
            msg += " minMean=" + std::to_string(m_ProbePixelHash.failIfMeanByteBelow);
        if (m_ProbePixelHash.failIfNonZeroBytesBelow > 0)
            msg += " minNonZero=" + std::to_string(m_ProbePixelHash.failIfNonZeroBytesBelow);
        if (!ppmPathStr.empty())
            msg += " ppm=" + ppmPathStr;
        Fail(msg);
    }
}

void UiReplayRunner::TickBeforeRenderRG(UIManager& /*ui*/, Rendering::RenderGraph::RGFrame& frame,
                                        std::uint32_t sourceId, UI::UITextureSpace sourceSpace,
                                        Rendering::IDevice* dev)
{
    if (!m_Enabled || m_ShouldExit || !dev)
        return;

    const std::uint64_t renderedFrame = (m_FrameIndex > 0) ? (m_FrameIndex - 1) : 0ull;

    // Poll pending tickets first. Resolved tickets consume; tickets reported
    // consumed WITHOUT data were cancelled by a re-begun frame incarnation —
    // drop them or the maxPending gate counts ghosts and the replay silently
    // stops checking pixels.
    for (auto it = m_PendingPixelTickets.begin(); it != m_PendingPixelTickets.end();)
    {
        Rendering::ViewReadbackResult result;
        if (it->Ticket && it->Ticket->TryGet(result))
        {
            PixelConsumeMeta meta{};
            meta.Frame = it->Frame;
            meta.SrcX = it->SrcX;
            meta.SrcY = it->SrcY;
            meta.TexW = it->TexW;
            meta.TexH = it->TexH;
            meta.Format = (std::uint32_t)result.format;
            meta.HasCompletionToken = true;
            meta.Space = it->Space;
            meta.Arm = "rg2";
            // The rg2 arm reads exactly one source — the frame's UI composite
            // (Editor.FinalLinear), whatever blend space it was declared in.
            // The space rides meta.Space; it is not this label's job.
            meta.Src = "final_linear";
            ConsumePixelRgb(result.pixels.data(), result.width, result.height, meta);
            it = m_PendingPixelTickets.erase(it);
            continue;
        }
        if (!it->Ticket || it->Ticket->IsConsumed())
        {
            json j;
            j["kind"] = "ui_replay_pixel_hash_skipped";
            j["scenario"] = m_ScenarioName;
            j["frame"] = it->Frame;
            j["reason"] = 3u; // cancelled_incarnation
            j["arm"] = "rg2";
            WriteJsonl(j);
            it = m_PendingPixelTickets.erase(it);
            continue;
        }
        ++it;
    }

    if (!m_ProbePixelHash.enabled)
        return;
    if (renderedFrame < m_ProbePixelHash.frameStart || renderedFrame > m_ProbePixelHash.frameEnd)
        return;
    if ((renderedFrame % (std::uint64_t)m_ProbePixelHash.everyNFrames) != 0ull)
        return;

    // The pending cap spans BOTH arms — excursion frames still use the old
    // list and the budget is one probe pipeline.
    std::uint32_t pendingCount = (std::uint32_t)m_PendingPixelTickets.size();
    for (const auto& p : m_PendingPixelReadbacks)
    {
        if (!p.consumed)
            ++pendingCount;
    }
    if (pendingCount >= m_ProbePixelHash.maxPending)
        return;

    // Region resolved at DECLARE time from the frame-local resource desc (no
    // exec-time size discovery: RenderGraph descs are authoritative at declaration).
    const auto& desc = frame.Graph().ResourceDesc(sourceId);
    std::uint32_t srcX = 0, srcY = 0, copyW = 1, copyH = 1;
    if (!ComputeProbeRegion(desc.Width, desc.Height, srcX, srcY, copyW, copyH))
    {
        json j;
        j["kind"] = "ui_replay_pixel_hash_skipped";
        j["scenario"] = m_ScenarioName;
        j["frame"] = renderedFrame;
        j["reason"] = 1u; // zero_texture_size
        j["arm"] = "rg2";
        WriteJsonl(j);
        return;
    }

    auto ticket = Rendering::RequestTextureRegionReadbackRG(
        dev, frame, Rendering::RenderGraph::RGTexture{sourceId}, srcX, srcY, copyW, copyH,
        "UiReplay.PixelHash");
    if (!ticket)
    {
        json j;
        j["kind"] = "ui_replay_pixel_hash_skipped";
        j["scenario"] = m_ScenarioName;
        j["frame"] = renderedFrame;
        j["reason"] = 4u; // declare_failed
        j["arm"] = "rg2";
        WriteJsonl(j);
        return;
    }

    PendingPixelTicket pending{};
    pending.Ticket = std::move(ticket);
    pending.Frame = renderedFrame;
    pending.SrcX = srcX;
    pending.SrcY = srcY;
    pending.Width = copyW;
    pending.Height = copyH;
    pending.TexW = desc.Width;
    pending.TexH = desc.Height;
    pending.Space = sourceSpace;
    m_PendingPixelTickets.push_back(std::move(pending));
}

void UiReplayRunner::CancelPendingReadbacks()
{
    for (auto& t : m_PendingPixelTickets)
    {
        if (t.Ticket)
            t.Ticket->Cancel();
    }
    m_PendingPixelTickets.clear();
    for (auto& pr : m_PendingPixelReadbacks)
    {
        if (!pr.consumed && pr.readbackBuffer.IsValid() && pr.device)
        {
            pr.device->DestroyBuffer(pr.readbackBuffer);
            pr.readbackBuffer = Rendering::BufferHandle{};
        }
        pr.consumed = true;
    }
}

} // namespace GameEngine

