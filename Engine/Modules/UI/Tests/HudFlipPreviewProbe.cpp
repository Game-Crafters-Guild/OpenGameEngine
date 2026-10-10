// A MEASUREMENT harness for the #767 SDR-blend-space flip, not a behaviour
// test. It renders the SHIPPED game HUD (Apps/Editor/Assets/UI/GameHudComplex
// .uxml + .css, embedded verbatim below) at 1280x720 under BOTH target-space
// declarations and dumps the RGBA8 frames plus a probe table, so the Player
// slice's HUD shift can be judged against a real headless-Chrome render of the
// equivalent HTML/CSS rather than against synthetic patches.
//
// Nothing here asserts a browser number: the comparison is done outside this
// harness, against a headless-browser render. What it DOES assert is the
// instrument — that both arms rendered, that every probe patch is uniform (so
// no probe straddles an AA edge or an overlap), and that the opaque backdrop
// is byte-identical across the flip.
//
// DISABLED by default: it renders 4 full 1280x720 frames and writes ~14 MB of
// raw pixels, which is not a price every UITextLayoutTests run should pay.
//
//   UITextLayoutTests.exe --gtest_also_run_disabled_tests \
//               --gtest_filter="*HudFlipPreview*" > hud-probe.log
//
// Output directory comes from GE_HUD_PREVIEW_OUT (default: cwd). Frames are
// written as headerless RGBA8, 1280x720, top-left origin:
//   hud_<backdrop>_<arm>.rgba   backdrop in {dark,light}, arm in {A_linear,B_encoded}

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/UIElement.h"
#include "UI/UITargetSpace.h"
#include "UI/UITextureSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::PhysicalRect;
using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

constexpr uint32_t kW = 1280;
constexpr uint32_t kH = 720;

// ── The shipped HUD, verbatim ───────────────────────────────────────────────
// Apps/Editor/Assets/UI/GameHudComplex.uxml, wrapped in one opaque backdrop so
// the translucent fills have a defined destination (the HUD itself paints no
// background — in the Player it composites over the game frame).
constexpr char kHudXml[] = R"(<uielement id="backdrop">
<uielement id="hud-root" class="hud-root">
    <uielement id="health-panel" class="panel panel-tl">
        <label class="panel-title" text="HEALTH" />
        <uielement class="bar-track">
            <uielement class="bar-fill" />
        </uielement>
        <label class="panel-sub" text="78 / 100" />
    </uielement>

    <uielement id="score-panel" class="panel panel-tr">
        <label class="panel-title" text="SCORE" />
        <label class="score-value" text="12,450" />
    </uielement>

    <uielement id="ability-bar" class="ability-bar">
        <uielement class="slot slot-ready"><label class="slot-key" text="Q" /></uielement>
        <uielement class="slot slot-ready"><label class="slot-key" text="W" /></uielement>
        <uielement class="slot slot-cd"><label class="slot-key" text="E" /></uielement>
        <uielement class="slot slot-ult"><label class="slot-key" text="R" /></uielement>
    </uielement>
</uielement>
</uielement>)";

// Apps/Editor/Assets/UI/GameHudComplex.css, verbatim.
constexpr char kHudCss[] = R"(
.hud-root {
    position: absolute;
    left: 0;
    top: 0;
    width: 100%;
    height: 100%;
}

.panel {
    position: absolute;
    display: flex;
    flex-direction: column;
    padding: 14px 18px;
    gap: 8px;
    background-color: rgba(18, 22, 34, 0.82);
    border: 2px solid rgba(92, 152, 232, 0.85);
    border-radius: 10px;
    box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45);
}

.panel-tl { left: 32px; top: 32px; width: 280px; }
.panel-tr { right: 32px; top: 32px; width: 200px; align-items: flex-end; }

.panel-title {
    color: rgba(150, 190, 255, 0.95);
    font-size: 14px;
    letter-spacing: 2px;
}

.panel-sub { color: #ffffff; font-size: 16px; }

.bar-track {
    width: 100%;
    height: 16px;
    background-color: rgba(0, 0, 0, 0.5);
    border: 1px solid rgba(120, 170, 240, 0.6);
    border-radius: 8px;
}

.bar-fill {
    width: 78%;
    height: 100%;
    background-color: rgba(80, 220, 130, 0.95);
    border-radius: 8px;
}

.score-value {
    color: #ffd35a;
    font-size: 30px;
    letter-spacing: 1px;
}

.ability-bar {
    position: absolute;
    left: 0;
    bottom: 36px;
    width: 100%;
    flex-direction: row;
    justify-content: center;
    gap: 14px;
}

.slot {
    width: 64px;
    height: 64px;
    align-items: center;
    justify-content: center;
    background-color: rgba(20, 26, 40, 0.85);
    border: 2px solid rgba(120, 170, 240, 0.7);
    border-radius: 12px;
}

.slot-ready { border: 2px solid rgba(80, 220, 130, 0.9); }
.slot-cd { opacity: 0.45; }
.slot-ult {
    border: 2px solid rgba(255, 200, 80, 0.95);
    background-color: rgba(60, 44, 16, 0.85);
}

.slot-key { color: #ffffff; font-size: 22px; pointer-events: none; }

.slot:hover {
    background-color: rgba(90, 150, 230, 0.95);
    border: 2px solid rgba(190, 225, 255, 1.0);
}
.slot:active {
    background-color: rgba(150, 205, 255, 1.0);
}
)";

// The only authored addition: one opaque backdrop. Appended AFTER the shipped
// sheet (this engine's cascade is file order, not specificity) and touching a
// selector the shipped sheet never names.
std::string BackdropCss(const char* hexColor)
{
    return std::string(kHudCss) + "\n#backdrop { position: absolute; left: 0; top: 0; "
                                  "width: 100%; height: 100%; background-color: " +
           hexColor + "; }\n";
}

// ── Render one arm ──────────────────────────────────────────────────────────

std::vector<uint8_t> RenderToBytes(UIManager& ui, UI::UITargetSpace targetSpace)
{
    IDevice* dev = ui.GetDevice();
    if (!dev)
        return {};

    const size_t outBytes = static_cast<size_t>(kW) * kH * 8; // RGBA16F
    const BufferHandle readback = dev->CreateReadbackBuffer(outBytes, "HudPreview.Readback");
    if (!readback.IsValid())
        return {};

    ViewReadbackResult result;
    {
        RenderGraph::RGResourcePool persistent(dev);
        RenderGraph::RGTransientPool transient(dev);
        RenderGraph::RGUploadRing ring(dev, 2, 262144);
        RenderGraph::RGFrame frame(dev, &persistent, &transient, &ring);
        frame.BeginFrame(1);

        TextureDesc td{};
        td.width = kW;
        td.height = kH;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.sampleCount = 1;
        td.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource) |
                   static_cast<uint32_t>(TextureUsage::TransferSrc);
        td.debugName = "HudPreview.Target";
        const RenderGraph::RGTexture target = frame.CreateTexture("HudPreview.Target", td);
        if (!target.IsValid() || !ui.RenderRG(frame, target, targetSpace))
        {
            dev->DestroyBuffer(readback);
            return {};
        }

        frame.AddPass(
            "HudPreview.Readback", PassPhase::kFinalize,
            [&](RenderGraph::RGPassBuilder& p)
            {
                p.Read(target, RenderGraph::RGTextureRead::CopySrc);
                p.PreventCulling();
            },
            [target, readback](RenderGraph::RGContext& ctx)
            {
                ctx.Cmd->CopyTextureSubresourceToBuffer(ctx.GetTexture(target), 0, 0, readback, kW,
                                                        kH);
            });

        frame.Execute();
        dev->WaitForIdle();

        const void* mapped = dev->MapBuffer(readback);
        if (mapped)
        {
            result.width = kW;
            result.height = kH;
            result.format = TextureFormat::R16G16B16A16_FLOAT;
            result.pixels.assign(static_cast<const uint8_t*>(mapped),
                                 static_cast<const uint8_t*>(mapped) + outBytes);
            dev->UnmapBuffer(readback);
        }
    }
    dev->DestroyBuffer(readback);
    if (result.pixels.empty())
        return {};

    const UI::UITextureSpace sourceSpace =
        targetSpace == UI::UITargetSpace::EncodedSrgb() ? UI::UITextureSpace::SrgbAuthored()
                                                        : UI::UITextureSpace::DisplayLinearSdr();
    return ReadbackToRgba8Srgb(result, sourceSpace);
}

// ── Probe plumbing ──────────────────────────────────────────────────────────

struct Rgb
{
    int R = -1, G = -1, B = -1, A = -1;
};

Rgb PixelAt(const std::vector<uint8_t>& rgba, int x, int y)
{
    if (x < 0 || y < 0 || x >= static_cast<int>(kW) || y >= static_cast<int>(kH))
        return {};
    const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
    return {rgba[i + 0], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

// Uniformity extents are per-probe: a 2px border admits no 5x5 patch, so a
// border probe checks a 1-wide vertical strip instead. A non-uniform patch is
// a contaminated probe and fails loudly rather than averaging the mistake away.
bool UniformPatch(const std::vector<uint8_t>& rgba, int cx, int cy, int hx, int hy, Rgb& out,
                  std::string& why)
{
    const Rgb c = PixelAt(rgba, cx, cy);
    for (int dy = -hy; dy <= hy; ++dy)
    {
        for (int dx = -hx; dx <= hx; ++dx)
        {
            const Rgb p = PixelAt(rgba, cx + dx, cy + dy);
            if (p.R != c.R || p.G != c.G || p.B != c.B)
            {
                char buf[160];
                snprintf(buf, sizeof(buf), "patch not uniform at (%d,%d): %d,%d,%d vs %d,%d,%d",
                         cx + dx, cy + dy, p.R, p.G, p.B, c.R, c.G, c.B);
                why = buf;
                return false;
            }
        }
    }
    out = c;
    return true;
}

struct Probe
{
    const char* Name;
    int X, Y;   // absolute pixel
    int HX, HY; // uniformity half-extents
};

const UIElement* NthChild(const UIElement* parent, size_t n)
{
    if (!parent || n >= parent->GetChildren().size())
        return nullptr;
    return parent->GetChildren()[n].get();
}

PhysicalRect BoxOf(const UIElement* el)
{
    if (!el)
        return {};
    return {el->GetLayoutX(), el->GetLayoutY(), el->GetLayoutWidth(), el->GetLayoutHeight()};
}

void WriteRaw(const std::string& path, const std::vector<uint8_t>& bytes)
{
    FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, path.c_str(), "wb");
#else
    f = fopen(path.c_str(), "wb");
#endif
    if (!f)
    {
        printf("hud-probe: FAILED to open %s for writing\n", path.c_str());
        return;
    }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    printf("hud-probe: wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
}

std::string OutDir()
{
#ifdef _WIN32
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "GE_HUD_PREVIEW_OUT") == 0 && v)
    {
        std::string s(v);
        free(v);
        if (!s.empty() && s.back() != '\\' && s.back() != '/')
            s += '\\';
        return s;
    }
#else
    if (const char* v = getenv("GE_HUD_PREVIEW_OUT"))
    {
        std::string s(v);
        if (!s.empty() && s.back() != '/')
            s += '/';
        return s;
    }
#endif
    return {};
}

// Ink over a rect, measured in ENCODED byte space exactly as the #767 record
// measured it: coverage (P - B) / (F - B) per pixel, green channel, summed.
double InkSum(const std::vector<uint8_t>& rgba, const PhysicalRect& r, int fgByte, int bgByte)
{
    const double span = static_cast<double>(fgByte - bgByte);
    double ink = 0.0;
    const int x0 = static_cast<int>(r.X), x1 = static_cast<int>(r.X + r.W);
    const int y0 = static_cast<int>(r.Y), y1 = static_cast<int>(r.Y + r.H);
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            const Rgb p = PixelAt(rgba, x, y);
            ink += std::clamp((p.G - bgByte) / span, 0.0, 1.0);
        }
    }
    return ink;
}

struct BackdropCase
{
    const char* Name;
    const char* Hex;
};

constexpr BackdropCase kBackdrops[] = {{"dark", "#272727"}, {"light", "#b4b4b4"}};

} // namespace

TEST(HudFlipPreview, DISABLED_RenderShippedHudUnderBothBlendModels)
{
    const std::string dir = OutDir();

    for (const BackdropCase& bd : kBackdrops)
    {
        IsolatedUIFixture fx;
        const bool built = fx.Build(1.0f, kHudXml, BackdropCss(bd.Hex));
        if (!fx.DeviceAvailable())
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(built) << fx.Diagnostic();

        // The fixture's own viewport is 800x600; the HUD is judged at a
        // realistic Player resolution. contentScale is 1, so logical == physical.
        fx.Manager().SetLayoutSizeOverride(kW, kH);
        fx.Settle();

        printf("hud-probe: backdrop=%s font=%s\n", bd.Name,
               fx.ResolvedFontFamily("health-panel").c_str());

        // ── Layout, in the space the probes and the Chrome reference share ──
        const UIElement* root = fx.Element("backdrop");
        const UIElement* health = fx.Element("health-panel");
        const UIElement* score = fx.Element("score-panel");
        const UIElement* abilityBar = fx.Element("ability-bar");
        ASSERT_TRUE(root && health && score && abilityBar) << "HUD tree did not build";

        const UIElement* title = NthChild(health, 0);
        const UIElement* track = NthChild(health, 1);
        const UIElement* fill = NthChild(track, 0);
        const UIElement* sub = NthChild(health, 2);
        const UIElement* scoreValue = NthChild(score, 1);
        const UIElement* slotQ = NthChild(abilityBar, 0);
        const UIElement* slotW = NthChild(abilityBar, 1);
        const UIElement* slotE = NthChild(abilityBar, 2);
        const UIElement* slotR = NthChild(abilityBar, 3);
        ASSERT_TRUE(title && track && fill && sub && scoreValue && slotQ && slotW && slotE && slotR)
            << "HUD child indices did not resolve";

        const struct
        {
            const char* Name;
            const UIElement* El;
        } boxes[] = {{"health-panel", health}, {"panel-title", title}, {"bar-track", track},
                     {"bar-fill", fill},       {"panel-sub", sub},     {"score-panel", score},
                     {"score-value", scoreValue}, {"ability-bar", abilityBar},
                     {"slot-Q", slotQ},        {"slot-W", slotW},      {"slot-E", slotE},
                     {"slot-R", slotR}};
        for (const auto& b : boxes)
        {
            const PhysicalRect r = BoxOf(b.El);
            printf("hud-box: %s %.2f %.2f %.2f %.2f\n", b.Name, r.X, r.Y, r.W, r.H);
        }

        // ── Probe sites, derived from the solved boxes ───────────────────────
        const PhysicalRect hp = BoxOf(health);
        const PhysicalRect tr = BoxOf(track);
        const PhysicalRect fl = BoxOf(fill);
        const PhysicalRect sq = BoxOf(slotQ);
        const PhysicalRect se = BoxOf(slotE);
        const PhysicalRect sr = BoxOf(slotR);

        const std::vector<Probe> probes = {
            // Opaque control: the backdrop itself, far from every HUD element.
            {"backdrop-opaque", 640, 400, 2, 2},
            // rgba(18,22,34,0.82) panel fill, inside the padding band so no
            // child overlaps it.
            {"panel-fill", static_cast<int>(hp.X + 9), static_cast<int>(hp.Y + hp.H * 0.5f), 2, 2},
            // rgba(92,152,232,0.85) 2px panel border, left edge, mid-height.
            {"panel-border", static_cast<int>(hp.X + 1), static_cast<int>(hp.Y + hp.H * 0.5f), 0, 3},
            // rgba(0,0,0,0.5) track over the panel fill: two translucent layers.
            {"bar-track-empty", static_cast<int>(tr.X + tr.W * 0.93f),
             static_cast<int>(tr.Y + tr.H * 0.5f), 2, 2},
            // rgba(80,220,130,0.95) fill over track over panel: three layers.
            {"bar-fill", static_cast<int>(fl.X + fl.W * 0.4f), static_cast<int>(fl.Y + fl.H * 0.5f),
             2, 2},
            // rgba(20,26,40,0.85) slot fill, above the key glyph.
            {"slot-ready-fill", static_cast<int>(sq.X + sq.W * 0.5f), static_cast<int>(sq.Y + 10),
             2, 2},
            // rgba(80,220,130,0.9) 2px ready border, left edge.
            {"slot-ready-border", static_cast<int>(sq.X + 1), static_cast<int>(sq.Y + sq.H * 0.5f),
             0, 3},
            // opacity:0.45 applied over rgba(20,26,40,0.85).
            {"slot-cd-fill", static_cast<int>(se.X + se.W * 0.5f), static_cast<int>(se.Y + 10), 2,
             2},
            // rgba(60,44,16,0.85) ult fill.
            {"slot-ult-fill", static_cast<int>(sr.X + sr.W * 0.5f), static_cast<int>(sr.Y + 10), 2,
             2},
            // rgba(255,200,80,0.95) 2px ult border.
            {"slot-ult-border", static_cast<int>(sr.X + 1), static_cast<int>(sr.Y + sr.H * 0.5f), 0,
             3},
        };

        // ── Both arms ────────────────────────────────────────────────────────
        const std::vector<uint8_t> lin = RenderToBytes(fx.Manager(), UI::UITargetSpace::LinearSdr());
        const std::vector<uint8_t> enc =
            RenderToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
        ASSERT_FALSE(lin.empty()) << "linear arm produced no pixels";
        ASSERT_FALSE(enc.empty()) << "encoded arm produced no pixels";
        ASSERT_EQ(lin.size(), enc.size());

        WriteRaw(dir + "hud_" + bd.Name + "_A_linear.rgba", lin);
        WriteRaw(dir + "hud_" + bd.Name + "_B_encoded.rgba", enc);

        printf("hud-probe-table: backdrop probe x y A_r A_g A_b B_r B_g B_b\n");
        for (const Probe& pr : probes)
        {
            Rgb a{}, b{};
            std::string why;
            EXPECT_TRUE(UniformPatch(lin, pr.X, pr.Y, pr.HX, pr.HY, a, why))
                << "A/" << pr.Name << ": " << why;
            EXPECT_TRUE(UniformPatch(enc, pr.X, pr.Y, pr.HX, pr.HY, b, why))
                << "B/" << pr.Name << ": " << why;
            printf("hud-probe-table: %s %s %d %d %d %d %d %d %d %d\n", bd.Name, pr.Name, pr.X, pr.Y,
                   a.R, a.G, a.B, b.R, b.G, b.B);
        }

        // Opaque-content invariance: the backdrop is an opaque painted rect and
        // must survive the flip byte for byte.
        {
            Rgb a{}, b{};
            std::string why;
            ASSERT_TRUE(UniformPatch(lin, 640, 400, 2, 2, a, why)) << why;
            ASSERT_TRUE(UniformPatch(enc, 640, 400, 2, 2, b, why)) << why;
            EXPECT_EQ(a.R, b.R);
            EXPECT_EQ(a.G, b.G);
            EXPECT_EQ(a.B, b.B);
        }

        // Frame-wide: how much of the HUD frame the flip leaves untouched.
        size_t identicalPixels = 0;
        int maxDelta = 0;
        for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
        {
            const bool same = lin[i * 4] == enc[i * 4] && lin[i * 4 + 1] == enc[i * 4 + 1] &&
                              lin[i * 4 + 2] == enc[i * 4 + 2];
            if (same)
                ++identicalPixels;
            for (int c = 0; c < 3; ++c)
                maxDelta = std::max(maxDelta, std::abs(int(lin[i * 4 + c]) - int(enc[i * 4 + c])));
        }
        printf("hud-frame-stats: %s identical_px %zu of %u (%.3f%%) max_channel_delta %d\n",
               bd.Name, identicalPixels, kW * kH,
               100.0 * double(identicalPixels) / double(kW * kH), maxDelta);

        // Text ink over the score panel, both arms (the P4 arm's subject).
        const PhysicalRect sv = BoxOf(scoreValue);
        printf("hud-ink: %s score-value A %.1f B %.1f\n", bd.Name,
               InkSum(lin, sv, 0xff, 0x1a), InkSum(enc, sv, 0xff, 0x1a));
    }
}
