// Measurement harness for the UI primitive-generation path.
//
// These are DISABLED_ by the repo's convention for cost-opt-in benchmarks: they
// drive hundreds of full-tree regenerations and have no place in the normal
// suite. Run them with --gtest_also_run_disabled_tests.
//
// Every number is printed with its config, sample count, median and spread. The
// profiler's own per-phase timers are the instrument (SetUpdateProfilingEnabled),
// and each scenario also takes an independent steady_clock wall reading around
// the same frames so the instrument can be checked against something that does
// not share its clock or its enable flag.
//
// The workload is an editor-shaped panel — rows of icon + label + value, real
// cascade, real Yoga solve, real shaping — not a synthetic loop, because the
// hypotheses under test are all about cache behaviour and cache behaviour is
// exactly what a microbenchmark manufactures away.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using GameEngine::UIElement;
using GameEngine::UIManager;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

#ifdef NDEBUG
constexpr const char* kConfig = "Release";
#else
constexpr const char* kConfig = "Debug";
#endif

// Enough rows that the tree is the same order as the editor's recorded ~1,037
// elements: 200 * (row + icon + 2 labels) + panel + root.
constexpr int kRows = 200;
constexpr int kWarmupFrames = 12;
constexpr int kSampleFrames = 60;

struct Stats
{
    double Median = 0.0;
    double Min = 0.0;
    double Max = 0.0;
    double Mean = 0.0;
    size_t N = 0;
};

Stats Summarize(std::vector<double> v)
{
    Stats s{};
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    s.N = v.size();
    s.Min = v.front();
    s.Max = v.back();
    s.Median = v[v.size() / 2];
    double sum = 0.0;
    for (double d : v)
        sum += d;
    s.Mean = sum / static_cast<double>(v.size());
    return s;
}

void PrintRow(const char* label, const Stats& s)
{
    std::printf("  %-34s  %8.4f  %8.4f  %8.4f  %8.4f   %zu\n", label, s.Median, s.Mean, s.Min,
                s.Max, s.N);
}

void PrintHeader(const char* title, float scale)
{
    std::printf("\n=== %s | config=%s | contentScale=%.2f | machine=win11-x64 ===\n", title,
                kConfig, static_cast<double>(scale));
    std::printf("  %-34s  %8s  %8s  %8s  %8s   %s\n", "metric (ms unless noted)", "median", "mean",
                "min", "max", "n");
}

// An editor-shaped panel: a scrolling column of property rows.
std::string BuildPanelXml(int rows)
{
    std::string xml = "<uielement id=\"root\">\n<uielement id=\"panel\">\n";
    for (int i = 0; i < rows; ++i)
    {
        const std::string n = std::to_string(i);
        xml += "<uielement class=\"row\" id=\"row" + n + "\">";
        xml += "<uielement class=\"icon\"/>";
        xml += "<label class=\"lbl\" text=\"Property Name " + n + "\"/>";
        xml += "<label class=\"val\" text=\"Value " + n + "\"/>";
        xml += "</uielement>\n";
    }
    xml += "</uielement>\n</uielement>";
    return xml;
}

// `rowOverflow` is the one variable the culling experiment changes.
std::string PanelCss(const char* rowOverflow)
{
    return std::string(R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#panel {
  display: flex; flex-direction: column;
  width: 780px; height: 560px;
  overflow: hidden;
  background-color: #1e1e1e;
}
.row {
  display: flex; flex-direction: row;
  width: 760px; height: 24px;
  padding: 2px;
  border-width: 1px;
  border-color: #333333;
  background-color: #272727;
  overflow: )") +
           rowOverflow + R"(;
}
.icon { width: 16px; height: 16px; background-color: #4488cc; border-radius: 3px; }
.lbl {
  width: 400px; height: 20px;
  font-family: Roboto; font-size: 12px; color: #dddddd;
  white-space: nowrap;
}
.val {
  width: 300px; height: 20px;
  font-family: Roboto; font-size: 12px; color: #99ccff;
  white-space: nowrap;
}
)";
}

const UIManager::UpdateProfileFrame& LastFrame(const UIManager& ui)
{
    return ui.GetUpdateProfilingHistory().back();
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario 1: phase-level cost of a forced full regen, and the control frames
// (idle, drain) it is supposed to be compared against.
// ---------------------------------------------------------------------------
void RunFullRegenProfile(float scale)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(scale, BuildPanelXml(kRows), PanelCss("visible"));
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    ui.SetUpdateProfilingEnabled(true);

    // The instrument has to see the font we think it does; a silent fallback
    // face changes every shaping cost in this file.
    EXPECT_EQ(fx.ResolvedFontFamily("row0"), "Roboto");

    for (int i = 0; i < kWarmupFrames; ++i)
    {
        ui.MarkStyleDirtyAll();
        fx.StepFrame();
    }

    std::vector<double> genMs, totalMs, yogaMs, buildYogaMs, cascadeMs, geometryMs, hitTestMs,
        wallMs;
    uint32_t elementCount = 0;
    uint32_t skippedCount = 0;

    for (int i = 0; i < kSampleFrames; ++i)
    {
        ui.MarkStyleDirtyAll();
        const auto t0 = std::chrono::steady_clock::now();
        fx.StepFrame();
        const auto t1 = std::chrono::steady_clock::now();

        const auto& f = LastFrame(ui);
        genMs.push_back(f.GenAllPrimitivesMs);
        totalMs.push_back(f.TotalMs);
        yogaMs.push_back(f.YogaMs);
        buildYogaMs.push_back(f.BuildYogaMs);
        cascadeMs.push_back(f.CascadeComputeMs);
        geometryMs.push_back(f.GeometryMs);
        hitTestMs.push_back(f.HitTestMs);
        wallMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        elementCount = f.ElementCount;
        skippedCount += f.GenAllPrimitivesSkipped;
    }

    PrintHeader("FULL REGEN (MarkStyleDirtyAll every frame)", scale);
    PrintRow("GenAllPrimitivesMs", Summarize(genMs));
    PrintRow("BuildYogaMs", Summarize(buildYogaMs));
    PrintRow("  of which CascadeComputeMs", Summarize(cascadeMs));
    PrintRow("YogaMs", Summarize(yogaMs));
    PrintRow("GeometryMs", Summarize(geometryMs));
    PrintRow("HitTestMs", Summarize(hitTestMs));
    PrintRow("TotalMs (profiler)", Summarize(totalMs));
    PrintRow("wall Update+Render (independent)", Summarize(wallMs));
    std::printf("  elements=%u  regens_skipped=%u/%d\n", elementCount, skippedCount, kSampleFrames);

    // CONTROL: nothing dirty. If this is not ~0 and skipped, the workload is
    // dirtying something on its own and every attribution above is void.
    std::vector<double> idleGen, idleTotal;
    uint32_t idleSkipped = 0;
    for (int i = 0; i < kSampleFrames; ++i)
    {
        fx.StepFrame();
        const auto& f = LastFrame(ui);
        idleGen.push_back(f.GenAllPrimitivesMs);
        idleTotal.push_back(f.TotalMs);
        idleSkipped += f.GenAllPrimitivesSkipped;
    }
    PrintHeader("CONTROL: idle frames (nothing dirty)", scale);
    PrintRow("GenAllPrimitivesMs", Summarize(idleGen));
    PrintRow("TotalMs (profiler)", Summarize(idleTotal));
    std::printf("  regens_skipped=%u/%d  <-- expect %d\n", idleSkipped, kSampleFrames,
                kSampleFrames);
}

TEST(UIEmitPerf, DISABLED_FullRegenProfileScale100) { RunFullRegenProfile(1.0f); }
TEST(UIEmitPerf, DISABLED_FullRegenProfileScale150) { RunFullRegenProfile(1.5f); }

// ---------------------------------------------------------------------------
// Scenario 2 (H1): does an active CSS transition force a full regen?
// ---------------------------------------------------------------------------
TEST(UIEmitPerf, DISABLED_TransitionRegenBehaviour)
{
    const std::string css = PanelCss("visible") + R"(
.row { transition: background-color 0.15s ease; }
.row.hot { background-color: #884444; }
)";

    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, BuildPanelXml(kRows), css);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    ui.SetUpdateProfilingEnabled(true);
    for (int i = 0; i < kWarmupFrames; ++i)
        fx.StepFrame();

    // Baseline: steady state, nothing animating.
    std::vector<double> baseGen;
    uint32_t baseSkipped = 0;
    for (int i = 0; i < 20; ++i)
    {
        fx.StepFrame();
        baseGen.push_back(LastFrame(ui).GenAllPrimitivesMs);
        baseSkipped += LastFrame(ui).GenAllPrimitivesSkipped;
    }

    // Start a paint-only transition on ONE row, exactly as a hover would.
    UIElement* row = fx.Element("row0");
    ASSERT_NE(row, nullptr);
    row->AddClass("hot");

    std::vector<double> transGen;
    uint32_t transSkipped = 0;
    uint32_t causeOr = 0;
    uint32_t drainFrames = 0;
    for (int i = 0; i < 12; ++i) // 0.15s at 16ms/frame ~= 9 frames
    {
        fx.StepFrame();
        const auto& f = LastFrame(ui);
        transGen.push_back(f.GenAllPrimitivesMs);
        transSkipped += f.GenAllPrimitivesSkipped;
        causeOr |= f.GenAllPrimitivesRegenCause;
        if (f.DrainItems > 0)
            ++drainFrames;
    }

    PrintHeader("H1: one row transitions background-color", 1.0f);
    PrintRow("GenAllPrimitivesMs (steady state)", Summarize(baseGen));
    PrintRow("GenAllPrimitivesMs (transitioning)", Summarize(transGen));
    std::printf("  steady: skipped=%u/20   transition: skipped=%u/12  drainFrames=%u\n",
                baseSkipped, transSkipped, drainFrames);
    std::printf("  regenCause bitmask over transition frames = 0x%X"
                "  (0x1=MarkDirty 0x40=HasActiveTransitions)\n",
                causeOr);
}

// ---------------------------------------------------------------------------
// Scenario 3 (H2): is offscreen content culled? One variable: row overflow.
// ---------------------------------------------------------------------------
void RunCullExperiment(float scale)
{
    // Both arms have identical element counts and identical text. The ONLY
    // difference is whether a row declares overflow:hidden, which is what the
    // emit path's cull gate tests before it will consider a rect comparison.
    // The panel is 560px tall and holds 200 * 24px rows, so ~77% of the rows
    // are outside the clip in both arms.
    struct Arm
    {
        const char* Name;
        const char* RowOverflow;
    };
    const Arm arms[] = {{"rows overflow:visible (default)", "visible"},
                        {"rows overflow:hidden", "hidden"}};

    PrintHeader("H2: identical trees, 77% of rows outside the parent clip", scale);
    for (const Arm& arm : arms)
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, BuildPanelXml(kRows), PanelCss(arm.RowOverflow));
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();

        UIManager& ui = fx.Manager();
        ui.SetUpdateProfilingEnabled(true);
        for (int i = 0; i < kWarmupFrames; ++i)
        {
            ui.MarkStyleDirtyAll();
            fx.StepFrame();
        }

        std::vector<double> gen;
        for (int i = 0; i < kSampleFrames; ++i)
        {
            ui.MarkStyleDirtyAll();
            fx.StepFrame();
            gen.push_back(LastFrame(ui).GenAllPrimitivesMs);
        }
        PrintRow(arm.Name, Summarize(gen));
    }
}

TEST(UIEmitPerf, DISABLED_CullExperimentScale100) { RunCullExperiment(1.0f); }

// ---------------------------------------------------------------------------
// Scenario 4 (H4): the TextArea line-break cache thrash.
//
// The alternation needs BOTH callers live in the same frame:
//   - OnPostLayout uses ComputeMetrics(out) -> GetLayoutWidth(), LOGICAL, and
//     only runs when the TextArea has a parent ScrollView;
//   - OnGeneratePrimitives passes the PHYSICAL w.
// It also needs wrapping ON: under nowrap/pre, ComputeMetrics forces
// WrapWidth = 0 in BOTH paths, the keys agree, and there is nothing to thrash.
// ---------------------------------------------------------------------------
void RunTextAreaThrash(float scale)
{
    std::string doc;
    for (int i = 0; i < 300; ++i)
        doc += "The quick brown fox jumps over the lazy dog and keeps running onward. ";

    const std::string xml = "<uielement id=\"root\"><scrollview id=\"sv\">"
                            "<textarea id=\"area\" value=\"" +
                            doc + "\"/></scrollview></uielement>";

    const char* css = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#sv { width: 780px; height: 560px; overflow: hidden; }
#area {
  width: 700px; height: 5000px;
  font-family: Roboto; font-size: 14px; color: #dddddd;
  padding: 8px;
  border-width: 1px;
  white-space: normal;
  background-color: #272727;
}
)";

    IsolatedUIFixture fx;
    const bool built = fx.Build(scale, xml, css);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    ui.SetUpdateProfilingEnabled(true);
    for (int i = 0; i < kWarmupFrames; ++i)
    {
        ui.MarkStyleDirtyAll();
        fx.StepFrame();
    }

    std::vector<double> gen, total, wall;
    for (int i = 0; i < kSampleFrames; ++i)
    {
        ui.MarkStyleDirtyAll();
        const auto t0 = std::chrono::steady_clock::now();
        fx.StepFrame();
        const auto t1 = std::chrono::steady_clock::now();
        gen.push_back(LastFrame(ui).GenAllPrimitivesMs);
        total.push_back(LastFrame(ui).TotalMs);
        wall.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    PrintHeader("H4: wrapping TextArea in a ScrollView (emit + OnPostLayout)", scale);
    PrintRow("GenAllPrimitivesMs", Summarize(gen));
    PrintRow("TotalMs (profiler)", Summarize(total));
    PrintRow("wall Update+Render (independent)", Summarize(wall));
    std::printf("  document bytes=%zu\n", doc.size());
}

TEST(UIEmitPerf, DISABLED_TextAreaThrashScale100) { RunTextAreaThrash(1.0f); }
TEST(UIEmitPerf, DISABLED_TextAreaThrashScale150) { RunTextAreaThrash(1.5f); }

// ---------------------------------------------------------------------------
// H4 instrument check. A null timing result is only worth reporting if the
// specimen actually exercises the mechanism, so this observes the mechanism's
// visible consequence rather than its cost.
//
// If the emit path wraps at a PHYSICAL width while shaping at a LOGICAL size,
// the number of wrapped lines it produces must CHANGE with the content scale:
// at cs 1.5 the same text is given 1.5x the wrap width but the same glyph
// sizes, so it fits on fewer lines. A line count that only tracks the scale
// mapping (identical rows, 1.5x apart) would mean the specimen never reached
// the mixed-space path and the timing above measured nothing.
// ---------------------------------------------------------------------------
TEST(UIEmitPerf, DISABLED_TextAreaWrapDiagnostic)
{
    std::string doc;
    for (int i = 0; i < 300; ++i)
        doc += "The quick brown fox jumps over the lazy dog and keeps running onward. ";

    const std::string xml = "<uielement id=\"root\"><scrollview id=\"sv\">"
                            "<textarea id=\"area\" value=\"" +
                            doc + "\"/></scrollview></uielement>";
    const char* css = R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; }
#sv { width: 780px; height: 560px; overflow: hidden; }
#area {
  width: 700px; height: 5000px;
  font-family: Roboto; font-size: 14px; color: #dddddd;
  padding: 8px;
  border-width: 1px;
  white-space: normal;
  background-color: #272727;
}
)";

    std::printf("\n=== H4 INSTRUMENT CHECK: does the specimen reach the mixed-space path? ===\n");
    std::printf("  config=%s\n", kConfig);
    for (float scale : {1.0f, 1.5f})
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(scale, xml, css);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();

        // Distinct glyph baselines == wrapped line count.
        const auto glyphs = fx.Primitives("area", GameEngine::UI::PrimitiveMode::Slug);
        std::vector<float> ys;
        for (const auto& g : glyphs)
        {
            const bool seen =
                std::any_of(ys.begin(), ys.end(), [&](float y) { return std::abs(y - g.Y) < 0.5f; });
            if (!seen)
                ys.push_back(g.Y);
        }
        std::printf("  scale %.2f : glyph primitives=%zu  distinct rows=%zu\n",
                    static_cast<double>(scale), glyphs.size(), ys.size());
    }
    std::printf("  (a row count that CHANGES with scale => the physical-WrapWidth bug is live\n"
                "   in this specimen; an UNCHANGED count => the specimen never wrapped and the\n"
                "   H4 timing measured nothing)\n");
}
