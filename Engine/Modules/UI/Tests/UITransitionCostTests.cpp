// Cost of a CSS transition, and of the offscreen content a full regen emits.
//
// DISABLED_ by the repo's opt-in-benchmark convention; run with
// --gtest_also_run_disabled_tests.
//
// WHAT THIS WORKLOAD IS: a flat editor-shaped property panel — N rows of
// icon + name label + value label, one stylesheet, real cascade, real Yoga
// solve, real Slug shaping, real primitive emission and a real headless
// Vulkan upload. The panel is clipped to 560 logical px and each row is 24px,
// so ~23 rows are inside the clip regardless of N.
//
// WHAT IT IS NOT: it has no docking, no nested scroll views, no virtualized
// ListView/TreeView (which opt out of clip culling on purpose), no textures,
// no Mounts and no overlays. Numbers here bound the *shape* of the cost
// (which phase, how it scales) — they are not a prediction of the editor's
// absolute per-frame milliseconds.
//
// Every scenario prints its own control frames. A cost claim in this file is
// always a DELTA against a control taken in the same process, same warm
// caches, same content.

#include "IsolatedUIFixture.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"

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

constexpr int kWarmupFrames = 12;
// 0.15s at the fixture's fixed 16ms step = 9.4 frames; 12 covers the tail.
constexpr int kTransitionFrames = 12;
constexpr int kControlFrames = 20;

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
    std::printf("  %-40s  %8.4f  %8.4f  %8.4f  %8.4f   %zu\n", label, s.Median, s.Mean, s.Min,
                s.Max, s.N);
}

void PrintHeader(const char* title)
{
    std::printf("\n=== %s | config=%s | machine=win11-x64 ===\n", title, kConfig);
    std::printf("  %-40s  %8s  %8s  %8s  %8s   %s\n", "metric (ms)", "median", "mean", "min", "max",
                "n");
}

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

// One frame's worth of every number this file reports.
struct FrameSample
{
    std::vector<double> Gen, Total, Wall, Transition, Cascade, BuildYoga, Yoga;
    uint32_t Skipped = 0;
    uint32_t DrainFrames = 0;
    uint32_t CauseOr = 0;
    uint32_t Elements = 0;
    // Per-frame, not OR-folded: an OR over a burst cannot tell "both triggers
    // fire every frame" from "one fired once". The claim under test is the
    // former, so the frames have to be shown individually.
    std::vector<uint32_t> CausePerFrame;
};

void Record(FrameSample& s, const UIManager& ui, double wallMs)
{
    const auto& f = LastFrame(ui);
    s.Gen.push_back(f.GenAllPrimitivesMs);
    s.Total.push_back(f.TotalMs);
    s.Wall.push_back(wallMs);
    s.Transition.push_back(f.TransitionMs);
    s.Cascade.push_back(f.CascadeComputeMs);
    s.BuildYoga.push_back(f.BuildYogaMs);
    s.Yoga.push_back(f.YogaMs);
    s.Skipped += f.GenAllPrimitivesSkipped;
    s.CauseOr |= f.GenAllPrimitivesRegenCause;
    s.CausePerFrame.push_back(f.GenAllPrimitivesRegenCause);
    if (f.DrainItems > 0)
        ++s.DrainFrames;
    s.Elements = f.ElementCount;
}

double StepAndRecord(IsolatedUIFixture& fx, FrameSample& s)
{
    const auto t0 = std::chrono::steady_clock::now();
    fx.StepFrame();
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double, std::milli>(t1 - t0).count();
    Record(s, fx.Manager(), wall);
    return wall;
}

void PrintSample(const char* name, const FrameSample& s, int frames)
{
    PrintRow("  GenAllPrimitivesMs", Summarize(s.Gen));
    PrintRow("  TotalMs (Update, profiler)", Summarize(s.Total));
    PrintRow("  TransitionMs (Advance)", Summarize(s.Transition));
    PrintRow("  CascadeComputeMs", Summarize(s.Cascade));
    PrintRow("  wall Update+Render (independent clock)", Summarize(s.Wall));
    std::printf("  %s: regen_skipped=%u/%d  drainFrames=%u  regenCause=0x%X  elements=%u\n", name,
                s.Skipped, frames, s.DrainFrames, s.CauseOr, s.Elements);
    std::printf("  %s: per-frame regenCause (0x1=StyleDirty MarkDirty, 0x40=HasActiveTransitions):",
                name);
    for (uint32_t c : s.CausePerFrame)
        std::printf(" 0x%X", c);
    std::printf("\n");
}

// Total emitted primitives across the whole tree — the payload a full regen
// re-memcpys into the persistent store and re-uploads to every ring slice.
size_t CountPrimitives(const IsolatedUIFixture& fx, int rows)
{
    size_t total = fx.Primitives("root").size() + fx.Primitives("panel").size();
    for (int i = 0; i < rows; ++i)
    {
        const std::string n = std::to_string(i);
        total += fx.Primitives("row" + n).size();
    }
    // Labels/icons carry no ids in this XML; walk them off the row elements.
    for (int i = 0; i < rows; ++i)
    {
        UIElement* row = fx.Element("row" + std::to_string(i));
        if (!row)
            continue;
        for (const auto& ch : row->GetChildren())
        {
            if (!ch)
                continue;
            for (uint16_t k = 0;; ++k)
            {
                if (!fx.Manager().PeekPrimitiveForTesting(*ch, k))
                    break;
                ++total;
            }
        }
    }
    return total;
}

// How many rows actually still hold an emitted range, and where the last one
// sits. The cull's premise is "content outside the clip"; a scenario that
// claims a cull ratio has to show the ratio it got, not the one it assumed.
struct RowCensus
{
    int RowsWithPrimitives = 0;
    float LastEmittedRowBottom = 0.0f;
    float PanelHeight = 0.0f;
};

RowCensus CensusRows(const IsolatedUIFixture& fx, int rows)
{
    RowCensus c{};
    c.PanelHeight = fx.BorderBox("panel").H;
    for (int i = 0; i < rows; ++i)
    {
        const std::string id = "row" + std::to_string(i);
        if (fx.Primitives(id).empty())
            continue;
        ++c.RowsWithPrimitives;
        const auto box = fx.BorderBox(id);
        c.LastEmittedRowBottom = std::max(c.LastEmittedRowBottom, box.Y + box.H);
    }
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// T1. Hover in, hover out, and the incremental path the transition never takes.
//
// Four arms over the SAME tree and the same content:
//   idle      — control: nothing changed. Must skip every regen or the
//               attribution below is void.
//   drain     — control: one row marked VisualDirty each frame. This is the
//               incremental path, driven by a stimulus that is allowed to use
//               it. It is the counterfactual cost of the transition frames.
//   hover-in  — one row gains .hot; background-color transitions over 0.15s.
//   hover-out — the same row loses .hot; it transitions back.
// ---------------------------------------------------------------------------
void RunHoverTransitionCost(int rows, bool transitionOnAllRows)
{
    std::string css = PanelCss("visible");
    css += transitionOnAllRows ? "\n.row { transition: background-color 0.15s ease; }\n"
                               : "\n#row5 { transition: background-color 0.15s ease; }\n";
    css += ".row.hot { background-color: #884444; }\n";

    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, BuildPanelXml(rows), css);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    UIManager& ui = fx.Manager();
    ui.SetUpdateProfilingEnabled(true);
    // Instrument check: a silent fallback face changes every shaping cost here.
    EXPECT_EQ(fx.ResolvedFontFamily("row0"), "Roboto");

    for (int i = 0; i < kWarmupFrames; ++i)
        fx.StepFrame();

    const size_t primitives = CountPrimitives(fx, rows);

    std::printf("\n\n########## T1  rows=%d  transition declared on %s  config=%s ##########\n",
                rows, transitionOnAllRows ? "ALL rows (.row)" : "ONE row (#row5)", kConfig);
    std::printf("  emitted primitives across the tree = %zu  (x %zu B = %.1f KB per full "
                "upload, per frame-in-flight slice)\n",
                primitives, sizeof(GameEngine::UI::UIPrimitive),
                (double)(primitives * sizeof(GameEngine::UI::UIPrimitive)) / 1024.0);

    // --- control: idle -----------------------------------------------------
    FrameSample idle;
    for (int i = 0; i < kControlFrames; ++i)
        StepAndRecord(fx, idle);
    PrintHeader("T1a CONTROL: idle frames (nothing dirty)");
    PrintSample("idle", idle, kControlFrames);

    // --- control: incremental drain ---------------------------------------
    FrameSample drain;
    for (int i = 0; i < kControlFrames; ++i)
    {
        UIElement* row = fx.Element("row5");
        ASSERT_NE(row, nullptr);
        row->MarkDirty(UIElement::VisualDirty);
        StepAndRecord(fx, drain);
    }
    PrintHeader("T1b CONTROL: one element VisualDirty per frame (drain path)");
    PrintSample("drain", drain, kControlFrames);

    // --- hover in ----------------------------------------------------------
    UIElement* hot = fx.Element("row5");
    ASSERT_NE(hot, nullptr);
    hot->AddClass("hot");
    FrameSample in;
    for (int i = 0; i < kTransitionFrames; ++i)
        StepAndRecord(fx, in);
    PrintHeader("T1c HOVER IN: one row transitions background-color 0.15s");
    PrintSample("hover-in", in, kTransitionFrames);

    // Let it settle so hover-out starts from a quiet tree.
    for (int i = 0; i < 6; ++i)
        fx.StepFrame();

    // --- hover out ---------------------------------------------------------
    hot->RemoveClass("hot");
    FrameSample out;
    for (int i = 0; i < kTransitionFrames; ++i)
        StepAndRecord(fx, out);
    PrintHeader("T1d HOVER OUT: the same row transitions back");
    PrintSample("hover-out", out, kTransitionFrames);

    const double idleGen = Summarize(idle.Gen).Median;
    const double drainGen = Summarize(drain.Gen).Median;
    const double inGen = Summarize(in.Gen).Median;
    const double idleTotal = Summarize(idle.Total).Median;
    const double drainTotal = Summarize(drain.Total).Median;
    const double inTotal = Summarize(in.Total).Median;
    const double idleWall = Summarize(idle.Wall).Median;
    const double drainWall = Summarize(drain.Wall).Median;
    const double inWall = Summarize(in.Wall).Median;

    std::printf("\n  DELTAS (median, ms) — %s\n", kConfig);
    std::printf("    GenAllPrimitives : idle %.4f | drain %.4f | hover %.4f | hover-idle %+.4f | "
                "hover-drain %+.4f\n",
                idleGen, drainGen, inGen, inGen - idleGen, inGen - drainGen);
    std::printf("    Update TotalMs   : idle %.4f | drain %.4f | hover %.4f | hover-idle %+.4f | "
                "hover-drain %+.4f\n",
                idleTotal, drainTotal, inTotal, inTotal - idleTotal, inTotal - drainTotal);
    std::printf("    wall Upd+Render  : idle %.4f | drain %.4f | hover %.4f | hover-idle %+.4f | "
                "hover-drain %+.4f\n",
                idleWall, drainWall, inWall, inWall - idleWall, inWall - drainWall);
    std::printf("    transition burst : %d frames x %+.4f ms of avoidable wall = %.3f ms over "
                "0.15 s\n",
                kTransitionFrames, inWall - drainWall,
                (inWall - drainWall) * (double)kTransitionFrames);
}

TEST(UITransitionCost, DISABLED_HoverRows200TransitionOnAllRows)
{
    RunHoverTransitionCost(200, true);
}
TEST(UITransitionCost, DISABLED_HoverRows200TransitionOnOneRow)
{
    RunHoverTransitionCost(200, false);
}
TEST(UITransitionCost, DISABLED_HoverRows800TransitionOnAllRows)
{
    RunHoverTransitionCost(800, true);
}

// ---------------------------------------------------------------------------
// T2. Does the cost of one row's colour animation scale with the panel's
// TOTAL content or with its VISIBLE content? The clip is 560px and rows are
// 24px, so the visible row count is ~23 in every arm; only N changes.
// ---------------------------------------------------------------------------
void RunTransitionScaling(const char* rowOverflow)
{
    std::printf("\n\n########## T2  transition cost vs panel size  rowOverflow=%s  config=%s "
                "##########\n",
                rowOverflow, kConfig);
    std::printf("  visible rows are ~23 in EVERY arm (560px clip / 24px row); only total rows "
                "change\n");
    std::printf("  %-6s %-9s %-8s %-9s %-9s %-11s %-12s %-12s\n", "rows", "elements", "prims",
                "rowsEmit", "lastY", "panelH", "hoverGen(ms)", "hoverWall(ms)");

    for (int rows : {50, 200, 800})
    {
        std::string css = PanelCss(rowOverflow);
        css += "\n.row { transition: background-color 0.15s ease; }\n";
        css += ".row.hot { background-color: #884444; }\n";

        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, BuildPanelXml(rows), css);
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();

        UIManager& ui = fx.Manager();
        ui.SetUpdateProfilingEnabled(true);
        for (int i = 0; i < kWarmupFrames; ++i)
            fx.StepFrame();

        const size_t prims = CountPrimitives(fx, rows);
        const RowCensus census = CensusRows(fx, rows);

        FrameSample idle;
        for (int i = 0; i < kControlFrames; ++i)
            StepAndRecord(fx, idle);

        UIElement* hot = fx.Element("row5");
        ASSERT_NE(hot, nullptr);
        hot->AddClass("hot");
        FrameSample in;
        for (int i = 0; i < kTransitionFrames; ++i)
            StepAndRecord(fx, in);

        std::printf("  %-6d %-9u %-8zu %-9d %-9.0f %-11.0f %-12.4f %-12.4f\n", rows, in.Elements,
                    prims, census.RowsWithPrimitives, census.LastEmittedRowBottom,
                    census.PanelHeight, Summarize(in.Gen).Median, Summarize(in.Wall).Median);
    }
}

TEST(UITransitionCost, DISABLED_ScalingRowsVisible) { RunTransitionScaling("visible"); }
TEST(UITransitionCost, DISABLED_ScalingRowsHidden) { RunTransitionScaling("hidden"); }
