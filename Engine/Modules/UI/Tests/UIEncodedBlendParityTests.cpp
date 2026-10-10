// Pixel pins for the #767 SDR-BLEND-SPACE FLIP, slice (i): the offscreen
// harness declares UITargetSpace::EncodedSrgb and these tests execute REAL
// renders of both arms — the same fixture declared LinearSdr and EncodedSrgb —
// and read the bytes back through the production converter
// (ReadbackToRgba8Srgb), stating each arm's source space explicitly (P5).
//
// The references are the #767 record's measured Chrome table (chrome.exe
// --headless=new --force-device-scale-factor=1) and the measured engine-today
// values, which double as the instrument check for the linear arm:
//
//   rgba(255,255,255,0.08) over #272727 -> Chrome 56, engine-linear 88
//   rgba(255,255,255,0.5)  over #272727 -> Chrome 147, engine-linear 190
//   rgba(255,0,0,0.5) over #00ff00     -> Chrome (128,127,0), engine (188,187,0)
//
// Kill conditions (design of record, section 5): any settled case outside
// +-1 level; any differing pixel in the opacity-1 identity fixture. The text
// measurement pins the P4-arm selection: the +34.9%-thinner catastrophe (the
// encoded blend with the compensation disabled) must not appear, and the
// dark/light ink ratio must move toward 1.0 relative to the linear arm.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "IsolatedUIFixture.h"
#include "UIPixelReadback.h"
#include "UIRgTestHarness.h"

#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"
#include "UI/UITargetSpace.h"

using GameEngine::UITesting::IsolatedUIFixture;
using GameEngine::UITesting::kReadbackH;
using GameEngine::UITesting::kReadbackW;
using GameEngine::UITesting::PhysicalRect;
using GameEngine::UITesting::PixelAt;
using GameEngine::UITesting::RenderUiToBytes;
using GameEngine::UITesting::Rgb;
using GameEngine::UITesting::UniformCentre;
using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

bool BuildOrSkip(IsolatedUIFixture& fx, const char* xml, const std::string& css, std::string& why)
{
    const bool built = fx.Build(1.0f, xml, css);
    if (!fx.DeviceAvailable())
    {
        why = "No Vulkan device available";
        return false;
    }
    EXPECT_TRUE(built) << fx.Diagnostic();
    return built;
}

// ── Settled cases (kill gate 1) ─────────────────────────────────────────────

constexpr char kSettledXml[] = R"(<uielement id="root">
  <uielement id="ov08"/>
  <uielement id="ov50"/>
</uielement>)";

constexpr char kSettledCss[] = R"(
#root { display: flex; flex-direction: row; width: 800px; height: 600px; background-color: #272727; }
#ov08 { width: 200px; height: 200px; margin: 40px; background-color: rgba(255, 255, 255, 0.08); }
#ov50 { width: 200px; height: 200px; margin: 40px; background-color: rgba(255, 255, 255, 0.5); }
)";

constexpr char kDiscriminatorXml[] = R"(<uielement id="root">
  <uielement id="ov"/>
</uielement>)";

constexpr char kDiscriminatorCss[] = R"(
#root { display: flex; width: 800px; height: 600px; background-color: #00ff00; }
#ov { width: 200px; height: 200px; margin: 40px; background-color: rgba(255, 0, 0, 0.5); }
)";

} // namespace

TEST(UIEncodedBlendParity, SettledCasesMatchChromesMeasuredTable)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kSettledXml, kSettledCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    // Instrument arm first: the linear render must reproduce the engine-today
    // values from the #767 record before any encoded number is trusted.
    const std::vector<uint8_t> lin = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::LinearSdr());
    ASSERT_FALSE(lin.empty()) << "linear render/readback produced no pixels";

    Rgb p{};
    ASSERT_TRUE(UniformCentre(lin, fx.BorderBox("root"), p, why)) << why;
    EXPECT_EQ(p.R, 39) << "linear-arm background control (#272727 round-trip)";
    ASSERT_TRUE(UniformCentre(lin, fx.BorderBox("ov08"), p, why)) << why;
    EXPECT_NEAR(p.R, 88, 1) << "engine-today reference, rgba(255,255,255,0.08)";
    ASSERT_TRUE(UniformCentre(lin, fx.BorderBox("ov50"), p, why)) << why;
    EXPECT_NEAR(p.R, 190, 1) << "engine-today reference, rgba(255,255,255,0.5)";

    // The flip. Chrome's measured table, kill at +-1.
    const std::vector<uint8_t> enc = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(enc.empty()) << "encoded render/readback produced no pixels";

    ASSERT_TRUE(UniformCentre(enc, fx.BorderBox("root"), p, why)) << why;
    EXPECT_EQ(p.R, 39) << "encoded-arm background control (#272727 raw bytes)";
    ASSERT_TRUE(UniformCentre(enc, fx.BorderBox("ov08"), p, why)) << why;
    EXPECT_NEAR(p.R, 56, 1) << "KILL: Chrome 56 for rgba(255,255,255,0.08) over #272727";
    EXPECT_NEAR(p.G, 56, 1);
    EXPECT_NEAR(p.B, 56, 1);
    ASSERT_TRUE(UniformCentre(enc, fx.BorderBox("ov50"), p, why)) << why;
    EXPECT_NEAR(p.R, 147, 1) << "KILL: Chrome 147 for rgba(255,255,255,0.5) over #272727";
}

TEST(UIEncodedBlendParity, DiscriminatorIsolatesTheBlendSpace)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kDiscriminatorXml, kDiscriminatorCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const std::vector<uint8_t> lin = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::LinearSdr());
    ASSERT_FALSE(lin.empty());
    Rgb p{};
    ASSERT_TRUE(UniformCentre(lin, fx.BorderBox("ov"), p, why)) << why;
    EXPECT_NEAR(p.R, 188, 1) << "engine-today reference (linear blend)";
    EXPECT_NEAR(p.G, 187, 1);
    EXPECT_EQ(p.B, 0);

    const std::vector<uint8_t> enc = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(enc.empty());
    ASSERT_TRUE(UniformCentre(enc, fx.BorderBox("ov"), p, why)) << why;
    EXPECT_NEAR(p.R, 128, 1) << "KILL: Chrome (128,127,0) for rgba(255,0,0,0.5) over #00ff00";
    EXPECT_NEAR(p.G, 127, 1);
    EXPECT_EQ(p.B, 0);
}

// ── Opacity-1 byte identity (kill gate 2) ───────────────────────────────────
//
// Opaque colour through the linear arm is decode -> blend(no-op) -> encode;
// through the encoded arm it is the raw bytes end to end. Both must land the
// authored byte exactly, so the two renders must be byte-identical on every
// pixel. Rects sit on integer pixel edges (box-filter coverage is exactly 0
// or 1 there) so no pixel carries partial coverage — translucency is gate 1's
// subject, not this one's.

namespace
{
constexpr char kOpaqueXml[] = R"(<uielement id="root">
  <uielement id="c1"/><uielement id="c2"/><uielement id="c3"/>
  <uielement id="c4"/><uielement id="c5"/><uielement id="c6"/>
</uielement>)";

constexpr char kOpaqueCss[] = R"(
#root { display: flex; flex-direction: row; width: 800px; height: 600px; background-color: #000000; }
#c1 { width: 100px; height: 200px; background-color: #272727; }
#c2 { width: 100px; height: 200px; background-color: #808080; }
#c3 { width: 100px; height: 200px; background-color: #e5e5e5; }
#c4 { width: 100px; height: 200px; background-color: #ff0000; }
#c5 { width: 100px; height: 200px; background-color: #123456; }
#c6 { width: 100px; height: 200px; background-color: #ffffff; }
)";
} // namespace

TEST(UIEncodedBlendParity, OpaqueContentIsByteIdenticalAcrossTheFlip)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kOpaqueXml, kOpaqueCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }

    const std::vector<uint8_t> lin = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::LinearSdr());
    const std::vector<uint8_t> enc = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(lin.empty());
    ASSERT_EQ(lin.size(), enc.size());

    // Instrument gate: the mid-grey control must read its exact byte in BOTH
    // arms (the #767 record's opaque-#808080 control, carried over).
    Rgb p{};
    ASSERT_TRUE(UniformCentre(lin, fx.BorderBox("c2"), p, why)) << why;
    EXPECT_EQ(p.R, 128);
    ASSERT_TRUE(UniformCentre(enc, fx.BorderBox("c2"), p, why)) << why;
    EXPECT_EQ(p.R, 128);

    // KILL: any differing pixel.
    size_t firstDiff = SIZE_MAX;
    size_t diffCount = 0;
    for (size_t i = 0; i < lin.size(); ++i)
    {
        if (lin[i] != enc[i])
        {
            ++diffCount;
            if (firstDiff == SIZE_MAX)
                firstDiff = i;
        }
    }
    EXPECT_EQ(diffCount, 0u) << "first differing byte at index " << firstDiff << " (pixel "
                             << (firstDiff / 4) % kReadbackW << "," << (firstDiff / 4) / kReadbackW
                             << " ch " << firstDiff % 4 << "): linear=" << int(lin[firstDiff])
                             << " encoded=" << int(enc[firstDiff]);
}

// ── Text through the P4 arm (gate 3) ────────────────────────────────────────
//
// Ink is measured exactly as the #767 before-arm record measured it: coverage
// per pixel in ENCODED byte space, (P - B) / (F - B), summed over the frame.
//
// What the P4 arm promises at the shipped blendGamma 0 (linear TARGET): the
// encoded blend must land where today's linear blend lands — the flip is
// TEXT-APPEARANCE-PRESERVING wherever the correction's dst = 1 - src guess is
// exact. That is the polarity pairings (white-on-black, black-on-white),
// pinned here as cross-arm ink invariance. On the theme pairing the guess is
// inexact and the arm swap relocates the guess's residual (the P4 golden
// model's dst-aware sweep: ~5 encoded levels on theme) — a small band, whose
// measured direction is TOWARD Chrome's ink (the record's thm share was the
// engine's thinnest cell). The Chrome-normalized share table itself is a
// report deliverable, measured against real headless Chrome with this build's
// staged Roboto — not something this test can pin without embedding a
// Chrome-version-specific constant.
//
// The catastrophe pin: if the encoded blend ran with the compensation dark
// (the wiring bug P4 exists to prevent), displayed ink drops to the raw
// mask — measured at the record as the gap moving from +13.0% to +34.9%
// thin, i.e. the encoded arm's ink falling to ~0.75x the linear arm's. 0.85
// splits that event from the intended residual.

namespace
{

struct TextPairing
{
    const char* Name;
    const char* Fg;
    const char* Bg;
    int FgByte;
    int BgByte;
};

constexpr TextPairing kPairings[] = {
    {"bow", "#000000", "#ffffff", 0, 255},
    {"wob", "#ffffff", "#000000", 255, 0},
    {"thm", "#e5e5e5", "#272727", 229, 39},
};

constexpr int kTextSizes[] = {12, 16, 24};

// The record's pangram line, one line, no wrapping at 800px.
constexpr char kTextXml[] = R"(<uielement id="root">
  <label id="specimen">The quick brown fox jumps over the lazy dog 0123456789</label>
</uielement>)";

std::string TextCss(const TextPairing& pairing, int sizePx)
{
    char css[512];
    snprintf(css, sizeof(css),
             R"(
#root { display: flex; flex-direction: column; width: 800px; height: 600px; background-color: %s; }
#specimen { font-family: Roboto; font-size: %dpx; line-height: %dpx; color: %s; margin: 24px; }
)",
             pairing.Bg, sizePx, sizePx + 8, pairing.Fg);
    return css;
}

// Green channel; grayscale text keeps all channels equal, and green is the
// record's channel of reference.
double InkSum(const std::vector<uint8_t>& rgba, int fgByte, int bgByte)
{
    const double span = static_cast<double>(fgByte - bgByte);
    double ink = 0.0;
    const size_t pixelCount = static_cast<size_t>(kReadbackW) * kReadbackH;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const double c = (static_cast<double>(rgba[i * 4 + 1]) - bgByte) / span;
        ink += std::clamp(c, 0.0, 1.0);
    }
    return ink;
}

} // namespace

TEST(UIEncodedBlendParity, TextInkRatioMovesTowardParityThroughTheP4Arm)
{
    // ink[arm][pairing][size]
    double ink[2][3][3] = {};
    for (int pi = 0; pi < 3; ++pi)
    {
        for (int si = 0; si < 3; ++si)
        {
            IsolatedUIFixture fx;
            std::string why;
            if (!BuildOrSkip(fx, kTextXml, TextCss(kPairings[pi], kTextSizes[si]), why))
            {
                if (!why.empty())
                    GTEST_SKIP() << why;
                return;
            }
            if (fx.ResolvedFontFamily("specimen").find("Roboto") == std::string::npos)
                GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

            const std::vector<uint8_t> lin =
                RenderUiToBytes(fx.Manager(), UI::UITargetSpace::LinearSdr());
            const std::vector<uint8_t> enc =
                RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
            ASSERT_FALSE(lin.empty());
            ASSERT_FALSE(enc.empty());
            ink[0][pi][si] = InkSum(lin, kPairings[pi].FgByte, kPairings[pi].BgByte);
            ink[1][pi][si] = InkSum(enc, kPairings[pi].FgByte, kPairings[pi].BgByte);
            ASSERT_GT(ink[0][pi][si], 100.0)
                << kPairings[pi].Name << " " << kTextSizes[si] << "px: no ink measured";
        }
    }

    // Report the full table (the raw half of gate 3's deliverable — the
    // Chrome-normalized shares are computed against these inks), then assert.
    printf("gate3-table: pairing size ink_linear ink_encoded enc/lin\n");
    for (int pi = 0; pi < 3; ++pi)
    {
        for (int si = 0; si < 3; ++si)
        {
            const double crossArm = ink[1][pi][si] / ink[0][pi][si];
            printf("gate3-table: %s %d %.1f %.1f %.4f\n", kPairings[pi].Name, kTextSizes[si],
                   ink[0][pi][si], ink[1][pi][si], crossArm);
            ::testing::Test::RecordProperty(
                std::string(kPairings[pi].Name) + "_" + std::to_string(kTextSizes[si]) +
                    "_enc_over_lin",
                std::to_string(crossArm));

            // Catastrophe pin (STOP condition): the P4 arm must be selected.
            EXPECT_GT(crossArm, 0.85)
                << kPairings[pi].Name << " " << kTextSizes[si]
                << "px: encoded-arm ink collapsed — the +34.9% event; the Skia-direction "
                   "retarget is not being selected (wiring bug, STOP)";

            if (pi == 0 || std::strcmp(kPairings[pi].Name, "wob") == 0)
            {
                // Exact-guess pairings: the retarget reproduces the linear
                // blend's landing, so the flip must not move text ink.
                EXPECT_NEAR(crossArm, 1.0, 0.02)
                    << kPairings[pi].Name << " " << kTextSizes[si]
                    << "px: the flip changed polarity-pairing text ink — the retarget is "
                       "not landing on the linear-blend target";
            }
            else
            {
                // Theme pairing: the dst-guess residual band. Direction and
                // magnitude per the P4 golden model (~5 encoded levels);
                // outside this band the guess relocation is doing something
                // the model did not predict.
                EXPECT_GT(crossArm, 0.95) << "thm " << kTextSizes[si] << "px";
                EXPECT_LT(crossArm, 1.10) << "thm " << kTextSizes[si] << "px";
            }
        }
    }
}

// ── Gradient mix stays space-agnostic (gate 4) ──────────────────────────────
//
// The five gradient mixes run mix() on uiPaintColor outputs, so under the
// encoded target they interpolate raw bytes — which IS Chrome's (legacy,
// default) gradient interpolation space for CSS gradients. A black-to-white
// horizontal ramp therefore reads byte = round(255 * t) post-flip, while the
// linear arm reads round(255 * encode(t)): the two arms disagree by up to 60
// levels mid-ramp, so this fixture also discriminates the mix's input space.

namespace
{

// Gradients are emitted programmatically (UI::AddGradient), not authored in
// CSS, so the probe is a custom element. Body is pure (ctx.Emit only), which
// is the documented exemption from the off-thread escalation preamble.
class GradientProbeElement : public UIElement
{
  public:
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                              float x, float y, float w, float h) override
    {
        UI::UIPrimitive p = UI::MakeRect(x, y, w, h, UI::PackColorU8(0, 0, 0, 255));
        UI::AddGradient(p, UI::GradientMode::Horizontal, UI::PackColorU8(0, 0, 0, 255),
                        UI::PackColorU8(255, 255, 255, 255));
        ctx.Emit(p);
    }
};

constexpr char kGradientXml[] = R"(<uielement id="root"/>)";
constexpr char kGradientCss[] = R"(
#root { display: flex; width: 800px; height: 600px; background-color: #000000; }
#grad { width: 256px; height: 64px; }
)";

int SrgbEncodeByte(double linear01)
{
    const double c = std::clamp(linear01, 0.0, 1.0);
    const double e = (c <= 0.0031308) ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(e * 255.0));
}

} // namespace

TEST(UIEncodedBlendParity, GradientMixInterpolatesTheDeclaredBlendSpace)
{
    IsolatedUIFixture fx;
    std::string why;
    if (!BuildOrSkip(fx, kGradientXml, kGradientCss, why))
    {
        if (!why.empty())
            GTEST_SKIP() << why;
        return;
    }
    auto probe = std::make_unique<GradientProbeElement>();
    probe->SetId("grad");
    fx.Element("root")->AddChild(std::move(probe));
    fx.Settle();

    const PhysicalRect box = fx.BorderBox("grad");
    ASSERT_GT(box.W, 255.0f) << "gradient probe did not lay out";

    const std::vector<uint8_t> lin = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::LinearSdr());
    const std::vector<uint8_t> enc = RenderUiToBytes(fx.Manager(), UI::UITargetSpace::EncodedSrgb());
    ASSERT_FALSE(lin.empty());
    ASSERT_FALSE(enc.empty());

    const uint32_t y = static_cast<uint32_t>(box.Y + box.H * 0.5f);
    for (const uint32_t px : {31u, 127u, 128u, 224u})
    {
        const uint32_t sx = static_cast<uint32_t>(box.X) + px;
        const double t = (px + 0.5) / box.W; // shader: (p.x - rect.x) / rect.z at pixel centre
        const int chromeModel = static_cast<int>(std::lround(255.0 * t)); // encoded-space lerp
        const int linearModel = SrgbEncodeByte(t); // linear-space lerp, then the OETF

        const Rgb pe = PixelAt(enc, sx, y);
        EXPECT_NEAR(pe.G, chromeModel, 1)
            << "x=" << px << ": encoded arm must interpolate raw bytes (Chrome's gradient space)";
        const Rgb pl = PixelAt(lin, sx, y);
        EXPECT_NEAR(pl.G, linearModel, 1)
            << "x=" << px << ": linear arm control drifted from the linear-mix model";
    }
}
