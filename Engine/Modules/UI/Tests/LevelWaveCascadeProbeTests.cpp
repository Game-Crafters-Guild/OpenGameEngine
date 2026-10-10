// Level-wave cascade probe — the third (previously-untested) MT design for the
// UI style cascade, measured at editor scale AND scaled tree sizes to find the
// crossover N. This is a design-space PROBE, not a product path: it exercises
// UIManager::RunLevelWaveCascadeProbe, which re-drives the REAL per-element
// cascade (ResolveCascadeForElement) in serial vs level-synchronous-wave order.
//
// Two tests:
//   OffVsOnStyleIdentity  — default-run correctness gate. Serial and level-wave
//                           MUST produce byte-identical resolved styles at every
//                           N (the OFF==ON identity pattern), and identical donor
//                           counts. Runs at a few scales so the suite guards it.
//   DISABLED_CrossoverBench — the measurement vehicle. Interleaved same-binary
//                           A/B (serial vs level-wave) across editor scale + scaled
//                           trees, cold restyle + warm re-update, pairwise medians.
//                           Disabled so the normal UIStyleTests run stays fast/green;
//                           invoke with:
//   UIStyleTests --gtest_also_run_disabled_tests --gtest_filter=*CrossoverBench*
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "Rendering/Core/Device.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// A stylesheet with a handful of matching class rules (each carrying several
// declarations so the cascade does real per-element work) plus noise rules the
// matcher must scan past. All selectors are single-class — NOT share-sensitive —
// so identically-shaped siblings stay share-eligible (P4 donor sharing engages).
std::string WriteProbeCss()
{
    const auto css = std::filesystem::temp_directory_path() / "levelwave_cascade_probe.css";
    std::ofstream f(css);
    f << ".spine{display:flex;flex-direction:column;width:240px;padding:4px;"
         "margin:2px;background-color:#20242c;color:#d7dae2;font-size:14px;}"
         " .cell-a{width:120px;height:18px;padding:2px;margin:1px;"
         "background-color:#2a2f3d;color:#5cc8ff;font-size:13px;font-weight:600;}"
         " .cell-b{width:160px;height:22px;padding:3px;margin:1px;"
         "background-color:#171a21;color:#57d99a;font-size:15px;font-weight:400;}";
    // Noise rules: extra candidates the matcher must scan on a cold restyle.
    for (int i = 0; i < 40; ++i)
        f << " .noise-" << i << "{padding:" << (i % 7) << "px;margin:" << (i % 5)
          << "px;color:#" << (100 + i) << "aa;font-size:" << (10 + (i % 8)) << "px;}";
    f << "\n";
    return css.string();
}

// Wide-shallow "editor-shape" scene. `cols` independent spine columns hang off
// the root; each column is a chain of `depth` spine nodes, and every spine node
// parents a small run of `1 continuing spine + leaves` children. Leaves split
// into two classes (cell-a / cell-b) so each run has same-shape sibling groups.
//
// Element count = 1 + cols * depth * (1 + leaves). Scaling rule: HOLD depth and
// the run shape (leaves) CONSTANT, scale `cols` to grow N — "more runs" of the
// same wide-shallow family, matching the editor's fragmented-but-wide profile.
void BuildScene(UIElement* root, int cols, int depth, int leaves)
{
    for (int c = 0; c < cols; ++c)
    {
        UIElement* cur = root;
        for (int d = 0; d < depth; ++d)
        {
            for (int i = 0; i < leaves; ++i)
            {
                auto leaf = std::make_unique<UIElement>();
                leaf->AddClass(i % 2 == 0 ? "cell-a" : "cell-b");
                cur->AddChild(std::move(leaf));
            }
            auto spineOwner = std::make_unique<UIElement>();
            spineOwner->AddClass("spine");
            UIElement* spine = spineOwner.get();
            cur->AddChild(std::move(spineOwner));
            cur = spine;
        }
    }
}

// Depth and run shape are fixed for the whole probe (13 levels => 13 level-wave
// barriers; run of 5 = 1 spine + 2 cell-a + 2 cell-b). N = 1 + 60*cols.
constexpr int kDepth = 12;
constexpr int kLeaves = 4;
int ColsForTarget(int targetN) { return std::max(1, (targetN - 1) / (kDepth * (1 + kLeaves))); }

bool VisualEqual(const VisualStyle& a, const VisualStyle& b)
{
    return a.BackgroundColor == b.BackgroundColor && a.BackgroundTint == b.BackgroundTint &&
           a.Color == b.Color && a.FontSize == b.FontSize && a.LineHeight == b.LineHeight &&
           a.FontWeight == b.FontWeight && a.FontStyle == b.FontStyle &&
           a.FontVariant == b.FontVariant && a.TextAlign == b.TextAlign &&
           a.WordBreak == b.WordBreak && a.OverflowWrap == b.OverflowWrap &&
           a.WhiteSpace == b.WhiteSpace &&
           a.GlowRadius == b.GlowRadius && a.GlowColor == b.GlowColor &&
           a.ShadowOffsetX == b.ShadowOffsetX && a.ShadowOffsetY == b.ShadowOffsetY &&
           a.ShadowSoftness == b.ShadowSoftness && a.ShadowColor == b.ShadowColor &&
           a.ShadowInset == b.ShadowInset &&
           a.LocalOpacity == b.LocalOpacity && a.Visible == b.Visible &&
           a.PointerEvents == b.PointerEvents && a.Cursor == b.Cursor &&
           a.HasColor == b.HasColor && a.HasFontSize == b.HasFontSize &&
           a.HasFontFamily == b.HasFontFamily && a.HasFontWeight == b.HasFontWeight &&
           a.FontFamily == b.FontFamily;
}

bool StylesEqual(const ResolvedStyle& a, const ResolvedStyle& b)
{
    return a.Layout == b.Layout && VisualEqual(a.Visual, b.Visual);
}

void SnapshotStyles(const UIElement* el, std::vector<ResolvedStyle>& out)
{
    if (!el)
        return;
    out.push_back(el->GetResolvedStyle());
    for (const auto& ch : el->GetChildren())
        SnapshotStyles(ch.get(), out);
}

double Median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Build a fresh scene of N≈target, run one real Update to establish Yoga states +
// interned sheets, wire the pool. Returns the manager (owns the tree).
std::unique_ptr<UIManager> MakeBuiltScene(IDevice* dev, JobSystem::WorkStealingThreadPool* pool,
                                          int cols, const std::string& cssPath)
{
    auto root = std::make_unique<UIElement>();
    root->AddClass("spine");
    BuildScene(root.get(), cols, kDepth, kLeaves);

    auto ui = std::make_unique<UIManager>(dev);
    ui->SetJobSystem(pool);
    ui->SetSubtreeSkipEnabled(true);
    ui->SetRoot(std::move(root));
    ui->AttachStyleFromFile(cssPath);
    ui->Update(0.0f, /*interactive=*/true); // full build: Yoga states + first cascade
    return ui;
}
} // namespace

// --- Correctness gate (default-run): serial == level-wave, byte-identical. -----
TEST(LevelWaveCascadeProbe, OffVsOnStyleIdentity)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Headless device init failed";
    UIRegistration::RegisterBuiltInControls();

    JobSystem::WorkStealingThreadPool pool(std::max<size_t>(2, std::thread::hardware_concurrency() - 1));
    const std::string css = WriteProbeCss();

    // A tiny tree, editor scale, and a larger scale — identity must hold at each N.
    for (int targetN : {121, 1021, 4981})
    {
        const int cols = ColsForTarget(targetN);
        auto ui = MakeBuiltScene(dev, &pool, cols, css);

        for (bool cold : {true, false})
        {
            UIManager::CascadeProbeResult serial{}, wave{};
            ui->RunLevelWaveCascadeProbe(/*levelWave=*/false, cold, serial);
            std::vector<ResolvedStyle> serialSnap;
            SnapshotStyles(ui->GetRootElement(), serialSnap);

            ui->RunLevelWaveCascadeProbe(/*levelWave=*/true, cold, wave);
            std::vector<ResolvedStyle> waveSnap;
            SnapshotStyles(ui->GetRootElement(), waveSnap);

            ASSERT_EQ(serialSnap.size(), waveSnap.size())
                << "N=" << targetN << " cold=" << cold;
            ASSERT_EQ(serial.ElementCount, wave.ElementCount);
            EXPECT_GT(wave.ShareeCopies, 0u)
                << "no sharing engaged — scene is not share-eligible, probe is a no-op";
            // Donor decisions must match between the two orders (same keys, same
            // sharing) — a cross-check that the level-wave grouping reproduces the
            // serial shared-cache behaviour.
            EXPECT_EQ(serial.DonorComputes, wave.DonorComputes)
                << "N=" << targetN << " cold=" << cold;

            size_t mismatches = 0;
            for (size_t i = 0; i < serialSnap.size(); ++i)
                if (!StylesEqual(serialSnap[i], waveSnap[i]))
                    ++mismatches;
            EXPECT_EQ(mismatches, 0u)
                << mismatches << " / " << serialSnap.size()
                << " resolved styles differ (N=" << targetN << " cold=" << cold << ")";
        }
    }
}

// --- Measurement vehicle (disabled by default): the crossover curve. ----------
// Interleaved same-binary A/B (serial vs level-wave), pairwise medians, across
// editor scale + scaled trees, cold + warm, at TWO pool sizes (a commodity core
// count and the full box) so the crossover's core-count dependence is visible.
namespace
{
void RunCrossoverMatrix(IDevice* dev, JobSystem::WorkStealingThreadPool& pool, size_t workers,
                        const std::string& css, bool printHistogram)
{
    constexpr int kPairs = 9;   // >= 5 pairs per MEASUREMENT LAW
    constexpr int kWarmup = 3;
    const int targets[] = {1021, 1981, 4981, 10021, 19981};

    std::printf("\n########  POOL WORKERS = %zu  ########\n", workers);
    for (bool cold : {true, false})
    {
        std::printf("\n--- %s ---\n", cold ? "COLD restyle (rule cache invalidated each drive)"
                                            : "WARM re-update (caches populated)");
        std::printf("%8s %7s %8s %10s %12s %12s %9s %10s\n",
                    "N", "levels", "maxGrps", "donors", "serial_ms", "levelwave_ms",
                    "speedup", "group_ms");

        for (int targetN : targets)
        {
            const int cols = ColsForTarget(targetN);
            auto ui = MakeBuiltScene(dev, &pool, cols, css);

            UIManager::CascadeProbeResult meta{};
            ui->RunLevelWaveCascadeProbe(/*levelWave=*/true, cold, meta); // capture histogram/meta

            for (int w = 0; w < kWarmup; ++w) // warmup (also populates caches for WARM)
            {
                UIManager::CascadeProbeResult s{}, l{};
                ui->RunLevelWaveCascadeProbe(false, cold, s);
                ui->RunLevelWaveCascadeProbe(true, cold, l);
            }

            std::vector<double> serialMs, waveMs, groupMs;
            for (int k = 0; k < kPairs; ++k)
            {
                UIManager::CascadeProbeResult s{}, l{};
                ui->RunLevelWaveCascadeProbe(false, cold, s);   // A
                ui->RunLevelWaveCascadeProbe(true, cold, l);    // B (interleaved)
                serialMs.push_back(s.DriveMs);
                waveMs.push_back(l.DriveMs);
                groupMs.push_back(l.GroupingMs);
            }

            const double msSerial = Median(serialMs);
            const double msWave = Median(waveMs);
            const double speedup = (msWave > 0.0) ? msSerial / msWave : 0.0;
            std::printf("%8u %7u %8u %10u %12.4f %12.4f %8.2fx %10.4f\n",
                        meta.ElementCount, meta.LevelCount, meta.MaxLevelGroups,
                        meta.DonorComputes, msSerial, msWave, speedup, Median(groupMs));

            std::printf("         serial["); // raw samples for ambient/variance audit
            for (double v : serialMs) std::printf("%.3f ", v);
            std::printf("]\n         wave  [");
            for (double v : waveMs) std::printf("%.3f ", v);
            std::printf("]\n");

            if (printHistogram && targetN == 1021 && cold)
            {
                std::printf("  editor-shape per-level histogram (N=%u, %u levels = barriers):\n",
                            meta.ElementCount, meta.LevelCount);
                for (size_t d = 0; d < meta.LevelWidths.size(); ++d)
                    std::printf("    L%-2zu width=%-6u groups=%-6u\n",
                                d, meta.LevelWidths[d],
                                d < meta.LevelGroupCounts.size() ? meta.LevelGroupCounts[d] : 0);
            }
        }
    }
}
} // namespace

TEST(LevelWaveCascadeProbe, DISABLED_CrossoverBench)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Headless device init failed";
    UIRegistration::RegisterBuiltInControls();

    const std::string css = WriteProbeCss();
    const size_t hw = std::max<size_t>(2, std::thread::hardware_concurrency());
    const size_t fullWorkers = hw - 1;
    // A commodity core count (~8-core machine) alongside the full box, so the
    // crossover's dependence on available parallelism is measured, not assumed.
    const size_t commodityWorkers = std::min<size_t>(7, fullWorkers);

    std::printf("\n==== LEVEL-WAVE CASCADE PROBE — crossover bench ====\n");
    std::printf("hardware_concurrency=%u  depth=%d  run=%d(1 spine + %d leaves)\n",
                std::thread::hardware_concurrency(), kDepth, 1 + kLeaves, kLeaves);

    {
        JobSystem::WorkStealingThreadPool pool(commodityWorkers);
        RunCrossoverMatrix(dev, pool, commodityWorkers, css, /*printHistogram=*/true);
    }
    if (fullWorkers != commodityWorkers)
    {
        JobSystem::WorkStealingThreadPool pool(fullWorkers);
        RunCrossoverMatrix(dev, pool, fullWorkers, css, /*printHistogram=*/false);
    }
    std::printf("==== end bench ====\n\n");
}
