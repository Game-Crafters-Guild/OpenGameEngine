// Chain-contract shapes B and C — the composite pins.
//
// WHAT THE ENGINE PROMISES AND NOTHING CHECKS: shape B (Player / editor Game
// View) hands a finalized image plus its SdrFinalized stamp to the HUD, and
// the HUD must blend ON THE ENCODED BYTES — UITargetSpace::ForPipelineOutput
// resolves the stamp to the EncodedSrgb target, whose documented contract
// (UITargetSpace.h) is the browser/Skia model: CSS paint enters undecoded and
// the ROP interpolates encoded values. Flip that space and every viewport
// washes out while UITargetSpaceTests stays green, because those pin enum
// plumbing, not pixels. Shape C (editor Scene View) writes overlays into the
// view colour BEFORE the finalize, so overlay ink is encoded exactly once,
// with the view.
//
// ── THE SPLIT, CARRIED FROM THE SPEC ────────────────────────────────────────
// Exactly ONE pin here gates production code that can break today: the
// blend-space compare. Its subject is UIManager::RenderRG + the ui_sdf
// shaders + UITargetSpace::ForPipelineOutput, so it runs the REAL UI
// compositor over the fixture's real finalized destination — a fixture-side
// synthetic blend would be a self-fit, the models scoring the fixture's own
// arithmetic. Everything else in this file — ordering, stamp pairing,
// transport-over-composite, the shape-C overlay — asserts WHERE pixels landed
// relative to a boundary, not how a blend computed them, and is soundly
// pinned with synthetic or opaque writers.
//
// ── THE WINNER MODEL IS THE DOCUMENTED CONTRACT, NOT THE SHADER ─────────────
// The three blend hypotheses below are derived from UITargetSpace.h's own
// words — EncodedSrgb: "CSS paint enters undecoded … the ROP interpolates
// encoded values"; LinearSdr: display-referred linear, i.e. authored-encoded
// paint is decoded into the blend space. Nothing here transcribes
// ui_sdf_common.glsl. If shader and doc ever disagree, that is a FINDING TO
// REPORT, not a reference to resync — "resync the reference" is the
// instruction that completed this fixture's previous self-fit.
//
// ── REDDENING MUTATIONS ─────────────────────────────────────────────────────
// Production, blend space:   UITargetSpace.cpp ForPipelineOutput resolving
//                            SdrFinalized to LinearSdr (the wash-out class);
//                            ui_sdf_common.glsl's encoded-arm paint adapter
//                            regaining a transfer function.
// Production, stamp pairing: ViewFinalize.cpp publishing a Space other than
//                            the one its image holds — the HostPolicy arm
//                            consumes the published pair through the
//                            production mapper, so the wrong stamp steers the
//                            real compositor into the wrong blend arm and the
//                            pixel gate reddens.
// Fixture-local (declared honestly, per the spec): the shape-C overlay's
// position relative to the finalize, and the transport's position relative to
// the composite, are sequenced by this fixture itself at step 0 — their red
// arms are LOCAL FIXTURE MUTATIONS (reorder the declares), not production
// ones. Their non-redundant life begins when step 1 hands sequencing to the
// chain.
//
// ── NO GOLDEN ───────────────────────────────────────────────────────────────
// Every composite expectation is same-frame relational: the models' destination
// operand is the SAME submission's pre-HUD readback (scheduled between the
// finalize and the UI pass by the recorded read-before-write hazard, and
// asserted to be), the paint operand is read from the emitted primitive the
// pass consumed, and byte-untouched claims compare two readbacks of one frame.
// The only non-relational inputs are the test's own authored stimuli.
//
// Skip policy (chain contract rule 6, three classes):
//   absent device            -> FAIL
//   absent staged asset      -> FAIL (ui_sdf shaders and an undeclared pass
//                                     are this class)
//   absent format capability -> SKIP, naming the format and the usage bits.

#include "HeadlessViewFixture.h"
#include "RGPassQuery.h"

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITargetSpace.h"
#include "UI/UITextureSpace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
namespace RGQuery = GameEngine::Testing::RGQuery;
using namespace GameEngine::Testing;
using GameEngine::Rendering::TextureFormat;
namespace Passes = GameEngine::Rendering::Passes;

namespace
{

void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Same process-wide latches as the sibling TUs, pinned here too because
// cross-TU initialization order is unspecified. Dither ON: shape B's
// destination bytes carry the finalize's dither, and the relational models
// must see the same bytes the compositor blended over.
struct PinEnvironment
{
    PinEnvironment()
    {
        SetEnv("GE_HEADLESS_TEST", "1");
        SetEnv("GE_OUTPUT_DITHER", "1");
        SetEnv("GE_DEBAND", "0");
        SetEnv("GE_DEBAND_THRESHOLD", "");
        SetEnv("GE_DEBAND_RADIUS", "");
    }
} g_pinEnvironment;

constexpr uint32_t kViewExtent = 130;

// HUD paint, authored once and cross-checked against the emitted primitive.
// #6633cc: bytes chosen so no channel's 10-bit code lands on a rounding
// boundary (102 -> 409.2, 51 -> 204.6, 204 -> 818.4).
constexpr int kPaintR = 0x66; // 102
constexpr int kPaintG = 0x33; // 51
constexpr int kPaintB = 0xcc; // 204

// The blend-gate bands, pre-registered and mechanism-derived (chain rule 5):
// the winner's residual is the destination's own quantization of an exact
// per-pixel model (|round error| averages 0.25 LSB on a 10-bit lattice), so
// 1.0 LSB is four times the mechanism; the losing hypotheses differ from the
// winner by alpha * (paint - decode/encode(paint)) ~ 0.10-0.13 encoded, i.e.
// >= 100 ten-bit LSB, so a 20-LSB floor is a fifth of the smallest predicted
// separation. F16 has no lattice: the winner bound is F16 rounding at these
// magnitudes (~5e-4) times four.
constexpr double kWinnerMaxLsb10 = 1.0;
constexpr double kLoserMinLsb10 = 20.0;
constexpr double kWinnerMaxF16 = 0.002;
constexpr double kLoserMinF16 = 0.05;

constexpr std::size_t kMaxReportedMismatches = 8;

float SrgbToLinearContent(float encoded)
{
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

uint32_t ContentHash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 0x9E3779B1u ^ (y + 0x85EBCA6Bu) * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    h ^= h >> 16;
    return h;
}

// Sharp per-pixel content (adjacent texels carry independent levels), so
// "untouched" and "carried verbatim" are claims about pixels that could have
// disagreed, and the finalize's dither has structure to ride on.
std::vector<float> MakeContent(uint32_t width, uint32_t height)
{
    constexpr float kEncodedFloor = 0.10f;
    constexpr float kLevelSpan = 0.76f;
    constexpr uint32_t kLevelMask = 63u;
    std::vector<float> px(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * width + x;
            const uint32_t h = ContentHash(x, y);
            for (int ch = 0; ch < 3; ++ch)
            {
                const uint32_t level = (h >> (ch * 8)) & kLevelMask;
                const float encoded =
                    kEncodedFloor + kLevelSpan * (static_cast<float>(level) /
                                                  static_cast<float>(kLevelMask));
                px[index * 4 + static_cast<size_t>(ch)] = SrgbToLinearContent(encoded);
            }
            px[index * 4 + 3] = 1.0f;
        }
    }
    return px;
}

// Two rects the HUD paints: #op opaque, #tr translucent. The root paints
// nothing (no background-color), so under RGLoadOp::Load every pixel outside
// the two rects is owed byte-untouched.
constexpr char kHudXml[] = R"(<uielement id="root">
  <uielement id="op"/>
  <uielement id="tr"/>
</uielement>)";

constexpr char kHudCss[] = R"(
#root { display: flex; flex-direction: column; width: 130px; height: 130px; }
#op { width: 64px; height: 24px; margin-left: 18px; margin-top: 16px; background-color: #6633cc; }
#tr { width: 64px; height: 24px; margin-left: 18px; margin-top: 12px; background-color: rgba(102, 51, 204, 0.6); }
)";

HeadlessViewDesc CompositeDesc(FinalizeDriver driver, TextureFormat format)
{
    HeadlessViewDesc d{};
    d.Width = kViewExtent;
    d.Height = kViewExtent;
    d.ContentLinearRgba = MakeContent(kViewExtent, kViewExtent);
    d.Driver = driver;
    d.DestinationFormat = format;
    d.InputSpace = Passes::FinalizeInputSpace::Linear;
    d.Quantizer = Passes::FinalizeQuantizer::Destination;
    d.Hud = HudDesc{kHudXml, kHudCss};
    return d;
}

#define HV_REQUIRE_UP(fx)                                                                         \
    do                                                                                            \
    {                                                                                             \
        const HeadlessViewStatus st = (fx).Up();                                                  \
        ASSERT_NE(st, HeadlessViewStatus::NoDevice)                                               \
            << "device absent is a FAILURE for this suite, not a skip: " << (fx).StatusMessage(); \
        ASSERT_NE(st, HeadlessViewStatus::StagedAssetMissing)                                     \
            << "staging is part of the test: " << (fx).StatusMessage();                           \
        ASSERT_EQ(st, HeadlessViewStatus::Ok) << (fx).StatusMessage();                            \
    } while (false)

#define HV_RENDER_OR_SKIP(fx, desc)                                                            \
    do                                                                                         \
    {                                                                                          \
        const HeadlessViewStatus st = (fx).Render(desc);                                       \
        if (st == HeadlessViewStatus::FormatUnsupported)                                       \
            GTEST_SKIP() << "capability skip: " << (fx).StatusMessage();                       \
        ASSERT_EQ(st, HeadlessViewStatus::Ok) << ToString(st) << ": " << (fx).StatusMessage(); \
    } while (false)

struct IntRect
{
    int X = 0;
    int Y = 0;
    int W = 0;
    int H = 0;

    bool Contains(uint32_t px, uint32_t py) const
    {
        const int x = static_cast<int>(px);
        const int y = static_cast<int>(py);
        return x >= X && x < X + W && y >= Y && y < Y + H;
    }
    IntRect Inset(int by) const { return {X + by, Y + by, W - 2 * by, H - 2 * by}; }
    IntRect Expand(int by) const { return Inset(-by); }
    bool Intersects(const IntRect& o) const
    {
        return X < o.X + o.W && o.X < X + W && Y < o.Y + o.H && o.Y < Y + H;
    }
};

// The element's layout, required to sit on integer pixels — the composite
// claims are per-pixel and a half-covered edge row would smear them.
bool LayoutRect(UIManager* hud, const char* id, IntRect& out, std::string& why)
{
    if (!hud || !hud->GetRootElement())
    {
        why = "no HUD root element";
        return false;
    }
    const UIElement* el = hud->GetRootElement()->FindById(id);
    if (!el)
    {
        why = std::string("HUD element '") + id + "' not found";
        return false;
    }
    const float vals[4] = {el->GetLayoutX(), el->GetLayoutY(), el->GetLayoutWidth(),
                           el->GetLayoutHeight()};
    for (const float v : vals)
    {
        if (std::abs(v - std::round(v)) > 1e-3f)
        {
            why = std::string("HUD element '") + id + "' layout is not on integer pixels";
            return false;
        }
    }
    out = {static_cast<int>(std::lround(vals[0])), static_cast<int>(std::lround(vals[1])),
           static_cast<int>(std::lround(vals[2])), static_cast<int>(std::lround(vals[3]))};
    if (out.W <= 8 || out.H <= 8)
    {
        why = std::string("HUD element '") + id + "' did not lay out (rect too small)";
        return false;
    }
    return true;
}

// The paint operand the pass actually consumed: the element's emitted Rect
// primitive's FillColor (R in the low byte). Reading the operand is not
// reading the blend — the models still differ in the blend function alone.
bool EmittedPaint(UIManager* hud, const char* id, uint32_t rgba[4], std::string& why)
{
    const UIElement* el = hud && hud->GetRootElement() ? hud->GetRootElement()->FindById(id)
                                                       : nullptr;
    if (!el)
    {
        why = std::string("HUD element '") + id + "' not found";
        return false;
    }
    for (uint16_t i = 0;; ++i)
    {
        const UI::UIPrimitive* p = hud->PeekPrimitiveForTesting(*el, i);
        if (!p)
            break;
        if (UI::GetMode(p->ModeAndFlags) != UI::PrimitiveMode::Rect)
            continue;
        rgba[0] = p->FillColor & 0xFFu;
        rgba[1] = (p->FillColor >> 8) & 0xFFu;
        rgba[2] = (p->FillColor >> 16) & 0xFFu;
        rgba[3] = (p->FillColor >> 24) & 0xFFu;
        return true;
    }
    why = std::string("HUD element '") + id + "' emitted no Rect primitive";
    return false;
}

// ── The three blend hypotheses, from UITargetSpace.h's documented contract ──
// EncodedSrgb (the winner the contract owes): paint undecoded, src-over on
// encoded bytes.       H_enc = a*p         + (1-a)*d
// LinearSdr arm reaching this attachment (the wash-out class): authored paint
// decoded into a linear blend space, ROP still interpolating raw values.
//                      H_lin = a*Dec(p)    + (1-a)*d
// Paint encoded a second time (the double-encode class).
//                      H_dbl = a*Enc(p)    + (1-a)*d
struct BlendScores
{
    double Encoded = 0.0;
    double LinearDecode = 0.0;
    double DoubleEncode = 0.0;
    std::size_t Samples = 0;
};

BlendScores ScoreBlendHypotheses(const ViewOutputBytes& pre, const ViewOutputBytes& post,
                                 const IntRect& region, const uint32_t paint[4])
{
    BlendScores s{};
    if (pre.Width != post.Width || pre.Height != post.Height || pre.Format != post.Format)
        return s;
    const double a = static_cast<double>(paint[3]) / 255.0;
    double sums[3] = {0.0, 0.0, 0.0};
    for (int y = region.Y; y < region.Y + region.H; ++y)
    {
        for (int x = region.X; x < region.X + region.W; ++x)
        {
            const std::size_t px =
                static_cast<std::size_t>(y) * post.Width + static_cast<std::size_t>(x);
            for (int ch = 0; ch < 3; ++ch)
            {
                float d = 0.0f;
                float obs = 0.0f;
                if (!ReadEncodedChannel(pre, px, ch, d) || !ReadEncodedChannel(post, px, ch, obs))
                    return {};
                const double p = static_cast<double>(paint[ch]) / 255.0;
                const double models[3] = {
                    a * p + (1.0 - a) * d,
                    a * SrgbToLinearContent(static_cast<float>(p)) + (1.0 - a) * d,
                    a * LinearToSrgbRef(static_cast<float>(p)) + (1.0 - a) * d};
                for (int m = 0; m < 3; ++m)
                    sums[m] += std::abs(static_cast<double>(obs) - models[m]);
                ++s.Samples;
            }
        }
    }
    if (s.Samples == 0)
        return s;
    s.Encoded = sums[0] / static_cast<double>(s.Samples);
    s.LinearDecode = sums[1] / static_cast<double>(s.Samples);
    s.DoubleEncode = sums[2] / static_cast<double>(s.Samples);
    return s;
}

// Byte-untouched outside the painted rects (plus a 1 px measurement halo
// around each, so the claim is not standing on the exact AA behaviour of the
// rect edge — the boundary claim proper is "ink INSIDE the rect", below).
struct UntouchedResult
{
    std::size_t Checked = 0;
    std::size_t Differing = 0;
    std::string Report;
};

UntouchedResult ExpectUntouchedOutside(const ViewOutputBytes& pre, const ViewOutputBytes& post,
                                       const IntRect& opHalo, const IntRect& trHalo)
{
    UntouchedResult r{};
    std::size_t reported = 0;
    for (uint32_t y = 0; y < post.Height; ++y)
    {
        for (uint32_t x = 0; x < post.Width; ++x)
        {
            if (opHalo.Contains(x, y) || trHalo.Contains(x, y))
                continue;
            const std::size_t px = static_cast<std::size_t>(y) * post.Width + x;
            uint32_t a[4] = {};
            uint32_t b[4] = {};
            if (!DestinationCodes(pre, px, a) || !DestinationCodes(post, px, b))
                return {};
            ++r.Checked;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
            {
                ++r.Differing;
                if (reported < kMaxReportedMismatches)
                {
                    ++reported;
                    r.Report += "\n  (" + std::to_string(x) + "," + std::to_string(y) + ") pre(" +
                                std::to_string(a[0]) + "," + std::to_string(a[1]) + "," +
                                std::to_string(a[2]) + ") post(" + std::to_string(b[0]) + "," +
                                std::to_string(b[1]) + "," + std::to_string(b[2]) + ")";
                }
            }
        }
    }
    return r;
}

// Fraction of interior pixels the composite changed — the "ink landed inside
// its declared boundary" instrument. A missing encoded shader variant, a
// Load that became Clear, or a compositor that never ran all land here.
double ChangedFraction(const ViewOutputBytes& pre, const ViewOutputBytes& post,
                       const IntRect& region)
{
    std::size_t total = 0;
    std::size_t changed = 0;
    for (int y = region.Y; y < region.Y + region.H; ++y)
    {
        for (int x = region.X; x < region.X + region.W; ++x)
        {
            const std::size_t px =
                static_cast<std::size_t>(y) * post.Width + static_cast<std::size_t>(x);
            uint32_t a[4] = {};
            uint32_t b[4] = {};
            if (!DestinationCodes(pre, px, a) || !DestinationCodes(post, px, b))
                return 0.0;
            ++total;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])
                ++changed;
        }
    }
    return total > 0 ? static_cast<double>(changed) / static_cast<double>(total) : 0.0;
}

// Shared instrument for the shape-B arms: rects, paint operands, schedule,
// the production publishes, ink presence, and byte-untouched outside.
struct HudProbe
{
    IntRect Op{};
    IntRect Tr{};
    uint32_t OpPaint[4] = {};
    uint32_t TrPaint[4] = {};
};

void ExpectHudInstrument(HeadlessViewFixture& fx, const char* arm, HudProbe& out)
{
    std::string why;
    ASSERT_TRUE(LayoutRect(fx.HudManager(), "op", out.Op, why)) << arm << ": " << why;
    ASSERT_TRUE(LayoutRect(fx.HudManager(), "tr", out.Tr, why)) << arm << ": " << why;
    // Full rect intersection, haloes included: the opaque claim, the blend
    // score and the byte-untouched region are three disjoint claims about the
    // same frame, and an overlap would make one of them score another's pixels.
    ASSERT_FALSE(out.Op.Expand(1).Intersects(out.Tr.Expand(1)))
        << arm << ": the two HUD rects (with their 1 px measurement haloes) overlap and the "
                  "region claims would double-count";

    ASSERT_TRUE(EmittedPaint(fx.HudManager(), "op", out.OpPaint, why)) << arm << ": " << why;
    ASSERT_TRUE(EmittedPaint(fx.HudManager(), "tr", out.TrPaint, why)) << arm << ": " << why;
    // The colour path is deterministic byte-for-byte; alpha is allowed the
    // parser's rounding of 0.6 and then USED as read, so the models stay
    // exact whichever byte it produced.
    EXPECT_EQ(out.OpPaint[0], static_cast<uint32_t>(kPaintR)) << arm;
    EXPECT_EQ(out.OpPaint[1], static_cast<uint32_t>(kPaintG)) << arm;
    EXPECT_EQ(out.OpPaint[2], static_cast<uint32_t>(kPaintB)) << arm;
    EXPECT_EQ(out.OpPaint[3], 255u) << arm << ": #op must be opaque";
    EXPECT_EQ(out.TrPaint[0], static_cast<uint32_t>(kPaintR)) << arm;
    EXPECT_NEAR(static_cast<double>(out.TrPaint[3]), 0.6 * 255.0, 1.0)
        << arm << ": #tr's alpha is not the authored 0.6";

    // Schedule: finalize -> pre-HUD tap -> UI pass, by scheduled position.
    // One finalize property, two arms: the direct arm declares
    // "HeadlessView.Finalize", the host policy "HeadlessView.Finalized";
    // exactly one is scheduled in any run.
    std::optional<std::size_t> finalizeIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalize"});
    if (!finalizeIdx)
        finalizeIdx =
            RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalized"});
    const std::optional<std::size_t> tapIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"RGReadback.HeadlessView.PreHudTap"});
    const std::optional<std::size_t> uiIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"UI Overlay"});
    ASSERT_TRUE(finalizeIdx.has_value()) << arm << ": no scheduled finalize pass";
    ASSERT_TRUE(tapIdx.has_value()) << arm << ": no scheduled pre-HUD tap";
    ASSERT_TRUE(uiIdx.has_value()) << arm << ": no scheduled UI pass — the compositor never ran";
    EXPECT_LT(*finalizeIdx, *tapIdx)
        << arm << ": the pre-HUD tap is scheduled before the finalize wrote its subject";
    EXPECT_LT(*tapIdx, *uiIdx)
        << arm << ": the pre-HUD tap is scheduled after the UI pass — it read the COMPOSITE, and "
                  "every relational model below would be comparing the composite to itself, which "
                  "is the vacuous route this assertion exists to close";

    // The production publishes the declare resolved.
    ASSERT_TRUE(fx.HudResolved().TargetSpace.has_value()) << arm;
    EXPECT_EQ(fx.HudResolved().TargetSpace->GetKind(), UI::UITargetSpace::Kind::EncodedSrgb)
        << arm << ": ForPipelineOutput did not resolve the hand-off stamp to the encoded target";
    EXPECT_EQ(fx.HudResolved().ResolvedOutputEncoding, 1)
        << arm << ": the declare did not resolve outputEncoding 1 (SDR encoded bytes)";

    // Ink landed inside its boundary. This is also the instrument that
    // notices a draw-skipped encoded declaration (missing shader variant):
    // the pass then changes nothing and this fails rather than skips.
    ASSERT_FALSE(fx.PreHud().Bytes.empty()) << arm << ": no pre-HUD readback";
    const double opChanged = ChangedFraction(fx.PreHud(), fx.Handoff(), out.Op.Inset(2));
    EXPECT_GE(opChanged, 0.9)
        << arm << ": only " << opChanged * 100.0
        << "% of the opaque rect's interior changed across the composite — the HUD did not land "
           "(draws skipped? wrong target? Load became Clear?)";

    // REDDENS IF (LOCAL FIXTURE MUTATION at step 0, not a production one): the
    // composite's load op becomes Clear instead of Load. The fixture is its own
    // sequencer until step 1 hands sequencing to the chain, so the load op is
    // chosen here rather than by production code. Demonstrated at 13468 of 13468
    // pixels outside the rects changing.
    const UntouchedResult untouched =
        ExpectUntouchedOutside(fx.PreHud(), fx.Handoff(), out.Op.Expand(1), out.Tr.Expand(1));
    ASSERT_GT(untouched.Checked, 0u) << arm;
    EXPECT_EQ(untouched.Differing, 0u)
        << arm << ": " << untouched.Differing << " of " << untouched.Checked
        << " pixels outside the HUD rects (plus a 1 px halo) changed across the composite — the "
           "UI pass wrote outside its primitives' boundaries, or the load-op discarded the "
           "finalized image." << untouched.Report;
}

void ReportBlend(const char* arm, double codeScale, const BlendScores& s)
{
    std::cout << "[ MEASURED ] " << arm << ": blend residuals over " << s.Samples
              << " samples (destination LSB where quantized): encoded " << s.Encoded * codeScale
              << ", linear-decode " << s.LinearDecode * codeScale << ", double-encode "
              << s.DoubleEncode * codeScale << std::endl;
}

} // namespace

// ── SHAPE B, arm 1: the real compositor over a PassDirect RGB10A2 finalize.
// The blend-space pin. The destination is quantized, so the winner's residual
// is the lattice's own rounding and the losing models sit >= 100 LSB away.
TEST(HeadlessViewShapeB, HudBlendsOnTheEncodedBytesOfTheFinalizedView)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HV_RENDER_OR_SKIP(fx, CompositeDesc(FinalizeDriver::PassDirect, TextureFormat::RGB10A2_UNORM));

    constexpr const char* kArm = "PassDirect RGB10A2 + HUD";
    ASSERT_EQ(fx.Handoff().Format, TextureFormat::RGB10A2_UNORM);
    ASSERT_EQ(fx.PreHud().Format, TextureFormat::RGB10A2_UNORM);

    HudProbe probe{};
    ExpectHudInstrument(fx, kArm, probe);
    if (::testing::Test::HasFatalFailure())
        return;

    const double codes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);

    // Opaque rect: coverage 1, alpha 1 — the composite owes the paint's own
    // code, undecoded (the EncodedSrgb contract's "the bytes ARE the blend
    // values"). +-1 code for the ROP's rounding of the exact rational.
    {
        const IntRect interior = probe.Op.Inset(2);
        std::size_t bad = 0;
        std::string report;
        std::size_t reported = 0;
        for (int y = interior.Y; y < interior.Y + interior.H; ++y)
        {
            for (int x = interior.X; x < interior.X + interior.W; ++x)
            {
                const std::size_t px = static_cast<std::size_t>(y) * fx.Handoff().Width +
                                       static_cast<std::size_t>(x);
                uint32_t got[4] = {};
                ASSERT_TRUE(DestinationCodes(fx.Handoff(), px, got));
                for (int ch = 0; ch < 3; ++ch)
                {
                    const double want =
                        std::round(static_cast<double>(probe.OpPaint[ch]) / 255.0 * codes);
                    if (std::abs(static_cast<double>(got[ch]) - want) > 1.0)
                    {
                        ++bad;
                        if (reported < kMaxReportedMismatches)
                        {
                            ++reported;
                            report += "\n  (" + std::to_string(x) + "," + std::to_string(y) +
                                      ") ch" + std::to_string(ch) + " = " +
                                      std::to_string(got[ch]) + " want ~" +
                                      std::to_string(static_cast<int>(want));
                        }
                    }
                }
            }
        }
        EXPECT_EQ(bad, 0u)
            << kArm << ": " << bad
            << " opaque-interior channels are not the authored paint's own code — the paint took "
               "a transfer function on its way into the blend (the wash-out or double-encode "
               "class), or the composite did not land." << report;
    }

    // Translucent rect: the three-hypothesis compare. Same-frame relational —
    // the destination operand is this submission's own pre-HUD bytes.
    const BlendScores s = ScoreBlendHypotheses(fx.PreHud(), fx.Handoff(), probe.Tr.Inset(2),
                                               probe.TrPaint);
    ReportBlend(kArm, codes, s);
    ASSERT_GT(s.Samples, 0u) << kArm << ": nothing to score";
    EXPECT_LE(s.Encoded * codes, kWinnerMaxLsb10)
        << kArm << ": the encoded-blend model misses by " << s.Encoded * codes
        << " LSB — more than the destination lattice can explain";
    EXPECT_GE(s.LinearDecode * codes, kLoserMinLsb10)
        << kArm << ": the linear-decode model scores " << s.LinearDecode * codes
        << " LSB — indistinguishably close; the compare has lost its subject";
    EXPECT_GE(s.DoubleEncode * codes, kLoserMinLsb10)
        << kArm << ": the double-encode model scores " << s.DoubleEncode * codes << " LSB";
    EXPECT_LT(s.Encoded, std::min(s.LinearDecode, s.DoubleEncode))
        << kArm << ": the documented contract's model did not win — the HUD is not blending on "
                   "encoded bytes (the wash-out class: UITargetSpace.cpp ForPipelineOutput, the "
                   "ui_sdf encoded paint adapter, or the pipeline selection)";
}

// ── SHAPE B, arm 2: the HostPolicy pair. DeclareViewFinalize publishes
// (Image, Space) together (I12), and this arm consumes that pair exactly the
// way production consumers do: the fixture hands the policy's OWN published
// stamp to UITargetSpace::ForPipelineOutput and lets the result steer the
// real compositor. A policy that stamps a space its image does not hold now
// has a pixel-level consequence: the compositor blends in the stamped space
// while the bytes hold the other one, and the hypothesis compare below flips
// its winner. That is the stamp-pairing pin the spec's I12 row calls for —
// relational throughout (this arm leaves PresentedFormat unset, so the pass
// resolves Unknown headless, and no absolute amplitude is asserted here).
TEST(HeadlessViewShapeB, ThePolicysPublishedPairDrivesTheCompositeSpace)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HeadlessViewDesc d = CompositeDesc(FinalizeDriver::HostPolicy, TextureFormat::RGB10A2_UNORM);
    HV_RENDER_OR_SKIP(fx, d);

    constexpr const char* kArm = "HostPolicy F16 + HUD";
    // The policy pins its destination to the source's format.
    ASSERT_EQ(fx.Handoff().Format, TextureFormat::R16G16B16A16_FLOAT);
    ASSERT_EQ(fx.PreHud().Format, TextureFormat::R16G16B16A16_FLOAT);

    // The published stamp itself — the policy's, not the fixture's (the
    // fixture only forwards it; FinalizeFacts documents that split).
    ASSERT_TRUE(fx.Handoff().Space.has_value()) << kArm;
    EXPECT_EQ(*fx.Handoff().Space, UI::UITextureSpace::SdrFinalized())
        << kArm << ": the policy finalized the image but published a different space — the exact "
                   "decoupling I12 forbids";

    HudProbe probe{};
    ExpectHudInstrument(fx, kArm, probe);
    if (::testing::Test::HasFatalFailure())
        return;

    // Opaque rect on a float destination: the stored value IS the paint,
    // undecoded, to F16 rounding.
    {
        const IntRect interior = probe.Op.Inset(2);
        std::size_t bad = 0;
        for (int y = interior.Y; y < interior.Y + interior.H; ++y)
        {
            for (int x = interior.X; x < interior.X + interior.W; ++x)
            {
                const std::size_t px = static_cast<std::size_t>(y) * fx.Handoff().Width +
                                       static_cast<std::size_t>(x);
                for (int ch = 0; ch < 3; ++ch)
                {
                    float got = 0.0f;
                    ASSERT_TRUE(ReadEncodedChannel(fx.Handoff(), px, ch, got));
                    const float want = static_cast<float>(probe.OpPaint[ch]) / 255.0f;
                    if (std::abs(got - want) > static_cast<float>(kWinnerMaxF16))
                        ++bad;
                }
            }
        }
        EXPECT_EQ(bad, 0u) << kArm
                           << ": opaque-interior values are not the authored paint undecoded";
    }

    const BlendScores s =
        ScoreBlendHypotheses(fx.PreHud(), fx.Handoff(), probe.Tr.Inset(2), probe.TrPaint);
    ReportBlend(kArm, 1.0, s);
    ASSERT_GT(s.Samples, 0u) << kArm << ": nothing to score";
    EXPECT_LE(s.Encoded, kWinnerMaxF16)
        << kArm << ": the encoded-blend model misses by " << s.Encoded
        << " encoded — more than F16 rounding can explain";
    EXPECT_GE(s.LinearDecode, kLoserMinF16) << kArm;
    EXPECT_GE(s.DoubleEncode, kLoserMinF16) << kArm;
    EXPECT_LT(s.Encoded, std::min(s.LinearDecode, s.DoubleEncode))
        << kArm << ": the winner is not the documented contract's model. If the space assertion "
                   "above ALSO failed, the root is the policy's stamp (ViewFinalize.cpp); if it "
                   "held, the root is the mapper or the compositor";
}

// ── SHAPE B, arm 3: transport over the composite. Synthetic-boundary pin:
// the upscale declared after the HUD must carry the COMPOSITE byte-verbatim —
// HUD ink included — and the letterbox stays black. At step 0 the
// finalize -> HUD -> transport sequence is the FIXTURE'S OWN declaration
// order (asserted below, but fixture-sequenced all the same), so this pin's
// red arm is a LOCAL FIXTURE MUTATION — declare the transport before the HUD
// and the mapped bytes match the pre-HUD image instead of the hand-off — not
// a production one. Its non-redundant life begins when step 1 hands the
// sequencing to the chain. The spec's shapes-B/C subsection names this split
// explicitly; per landing 0's bar the local red run is still required.
TEST(HeadlessViewShapeB, TransportAfterTheCompositeCarriesTheHudVerbatim)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HeadlessViewDesc d = CompositeDesc(FinalizeDriver::PassDirect, TextureFormat::RGB10A2_UNORM);
    TransportDesc t{};
    t.DestinationWidth = (kViewExtent - 2) * 2 + 16;
    t.DestinationHeight = (kViewExtent - 2) * 2 + 8;
    t.DestinationFormat = TextureFormat::RGB10A2_UNORM;
    t.Zoom = 2;
    t.InputSpace = Passes::FinalizeInputSpace::EncodedSrgb;
    d.Transport = t;
    HV_RENDER_OR_SKIP(fx, d);

    constexpr const char* kArm = "PassDirect RGB10A2 + HUD + transport";
    HudProbe probe{};
    ExpectHudInstrument(fx, kArm, probe);
    if (::testing::Test::HasFatalFailure())
        return;

    // Schedule: the transport after the UI pass, so its source is the
    // composite this test claims it carries.
    const std::optional<std::size_t> uiIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"UI Overlay"});
    const std::optional<std::size_t> transportIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Transport"});
    ASSERT_TRUE(transportIdx.has_value()) << kArm << ": no scheduled transport pass";
    EXPECT_LT(*uiIdx, *transportIdx)
        << kArm << ": the transport is scheduled before the HUD — it sampled the pre-composite "
                   "image and every byte comparison below is about the wrong operand";

    // Byte equality through the index map, zero tolerance, against the SAME
    // submission's post-composite hand-off.
    const ViewOutputBytes& srcImg = fx.Handoff();
    const ViewOutputBytes& dstImg = fx.TransportOutput();
    const std::vector<TransportIndex>& map = fx.TransportIndexMap();
    ASSERT_EQ(map.size(), static_cast<std::size_t>(dstImg.Width) * dstImg.Height) << kArm;
    const uint32_t fullAlpha = DestinationFullScaleCode(dstImg.Format, 3);
    std::size_t mapped = 0;
    std::size_t miscopied = 0;
    std::size_t letterboxBad = 0;
    std::string report;
    std::size_t reported = 0;
    for (std::size_t i = 0; i < map.size(); ++i)
    {
        const TransportIndex& e = map[i];
        uint32_t got[4] = {};
        ASSERT_TRUE(DestinationCodes(dstImg, i, got));
        if (e.Letterbox)
        {
            if (got[0] != 0u || got[1] != 0u || got[2] != 0u || got[3] != fullAlpha)
                ++letterboxBad;
            continue;
        }
        ++mapped;
        uint32_t want[4] = {};
        ASSERT_TRUE(DestinationCodes(
            srcImg, static_cast<std::size_t>(e.SourceY) * srcImg.Width + e.SourceX, want));
        if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2])
        {
            ++miscopied;
            if (reported < kMaxReportedMismatches)
            {
                ++reported;
                report += "\n  dst(" + std::to_string(i % dstImg.Width) + "," +
                          std::to_string(i / dstImg.Width) + ") <- src(" +
                          std::to_string(e.SourceX) + "," + std::to_string(e.SourceY) + ")";
            }
        }
    }
    ASSERT_GT(mapped, 0u) << kArm;
    EXPECT_EQ(miscopied, 0u)
        << kArm << ": " << miscopied << " of " << mapped
        << " mapped pixels do not carry their composite source texel's codes." << report;
    EXPECT_EQ(letterboxBad, 0u) << kArm;

    // The ink actually crossed: inside the opaque rect the composite differs
    // from the pre-HUD image, and (via the byte equality above) that
    // difference is what the transport replicated. Without this, a transport
    // reading the pre-HUD image AND a hand-off tap reading it too would agree
    // with each other and the byte gate could pass HUD-blind.
    const double changed = ChangedFraction(fx.PreHud(), fx.Handoff(), probe.Op.Inset(2));
    EXPECT_GE(changed, 0.9) << kArm << ": HUD ink is not in the transported operand";
}

// ── SHAPE C: the synthetic S1 overlay writer, finalized with the view.
// The producer is synthetic BY NECESSITY — the production S1 writer
// (SceneViewOverlaysRG) is editor-executable code that cannot link in this
// target — so this pin is contract-executable: it gates where the overlay's
// pixels landed (boundary), where the pass sits (schedule), and that its ink
// went through the same single encode the view took. It does NOT gate how the
// production overlay computes pixels. Its red arm at step 0 is a LOCAL
// FIXTURE MUTATION — declare the overlay after the finalize, and the encode
// gate below reads the gap between an image finalized without the ink and a
// tap that carries it — as the spec's shapes-B/C subsection states; per
// landing 0's bar that red run is required and was performed.
TEST(HeadlessViewShapeC, OverlayWrittenBeforeTheFinalizeIsEncodedExactlyOnce)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);

    HeadlessViewDesc d{};
    d.Width = kViewExtent;
    d.Height = kViewExtent;
    d.ContentLinearRgba = MakeContent(kViewExtent, kViewExtent);
    d.Driver = FinalizeDriver::PassDirect;
    d.DestinationFormat = TextureFormat::RGB10A2_UNORM;
    d.InputSpace = Passes::FinalizeInputSpace::Linear;
    d.Quantizer = Passes::FinalizeQuantizer::Destination;
    OverlayDesc o{};
    o.X = 32;
    o.Y = 40;
    o.Width = 48;
    o.Height = 32;
    o.LinearRgba[0] = 0.70f;
    o.LinearRgba[1] = 0.05f;
    o.LinearRgba[2] = 0.30f;
    o.LinearRgba[3] = 1.0f;
    d.Overlay = o;
    HV_RENDER_OR_SKIP(fx, d);

    constexpr const char* kArm = "PassDirect RGB10A2 + overlay";
    const IntRect rect{static_cast<int>(o.X), static_cast<int>(o.Y), static_cast<int>(o.Width),
                       static_cast<int>(o.Height)};

    // Schedule: injection -> overlay -> finalize.
    const std::optional<std::size_t> injectIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.ContentInject"});
    const std::optional<std::size_t> overlayIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Overlay"});
    std::optional<std::size_t> finalizeIdx =
        RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalize"});
    if (!finalizeIdx)
        finalizeIdx =
            RGQuery::IndexIn(fx.ScheduledPassNames(), RGQuery::Exact{"HeadlessView.Finalized"});
    ASSERT_TRUE(injectIdx.has_value()) << kArm;
    ASSERT_TRUE(overlayIdx.has_value()) << kArm << ": no scheduled overlay pass";
    ASSERT_TRUE(finalizeIdx.has_value()) << kArm;
    EXPECT_LT(*injectIdx, *overlayIdx) << kArm << ": the overlay ran under the content injection";
    EXPECT_LT(*overlayIdx, *finalizeIdx)
        << kArm << ": the overlay is scheduled after the finalize — its ink skipped the encode, "
                   "which is the S1-after-S2 ordering the chain contract forbids";

    // Boundary, in the source tap (same submission): inside the rect the tap
    // holds the overlay ink; outside it holds the injected content — which is
    // also what notices a sub-region copy discarding the rest of the
    // subresource (the CommandList currentState trap).
    const ViewStageTap& tap = fx.SourceBeforeFinalize();
    ASSERT_EQ(tap.Width, kViewExtent);
    constexpr float kF16Eps = 2e-3f;
    std::size_t insideBad = 0;
    std::size_t outsideBad = 0;
    std::size_t outsideChecked = 0;
    for (uint32_t y = 0; y < tap.Height; ++y)
    {
        for (uint32_t x = 0; x < tap.Width; ++x)
        {
            const std::size_t px = static_cast<std::size_t>(y) * tap.Width + x;
            if (rect.Contains(x, y))
            {
                for (int ch = 0; ch < 3; ++ch)
                    if (std::abs(tap.Rgba[px * 4 + static_cast<std::size_t>(ch)] -
                                 o.LinearRgba[ch]) > kF16Eps)
                    {
                        ++insideBad;
                        break;
                    }
            }
            else
            {
                ++outsideChecked;
                for (int ch = 0; ch < 3; ++ch)
                    if (std::abs(tap.Rgba[px * 4 + static_cast<std::size_t>(ch)] -
                                 d.ContentLinearRgba[px * 4 + static_cast<std::size_t>(ch)]) >
                        kF16Eps)
                    {
                        ++outsideBad;
                        break;
                    }
            }
        }
    }
    EXPECT_EQ(insideBad, 0u) << kArm << ": " << insideBad
                             << " pixels inside the overlay rect do not hold the overlay ink";
    ASSERT_GT(outsideChecked, 0u) << kArm;
    EXPECT_EQ(outsideBad, 0u)
        << kArm << ": " << outsideBad << " of " << outsideChecked
        << " pixels outside the overlay rect do not hold the injected content — the overlay "
           "wrote outside its boundary, or the sub-region copy discarded the subresource";

    // The encode gate: over the overlay rect the hand-off is the encode-once
    // of the tap's own values — the overlay ink took exactly the transfer the
    // view took, dither and lattice included. Relational per pixel; the band
    // is the mechanism's (a 1-LSB TPDF plus 0.5-LSB rounding averages ~0.45
    // LSB of |displacement|; 1.5 is three times that, and the red arm sits at
    // hundreds).
    const double codes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    double residualSum = 0.0;
    std::size_t samples = 0;
    for (int y = rect.Y; y < rect.Y + rect.H; ++y)
    {
        for (int x = rect.X; x < rect.X + rect.W; ++x)
        {
            const std::size_t px =
                static_cast<std::size_t>(y) * fx.Handoff().Width + static_cast<std::size_t>(x);
            for (int ch = 0; ch < 3; ++ch)
            {
                float obs = 0.0f;
                ASSERT_TRUE(ReadEncodedChannel(fx.Handoff(), px, ch, obs));
                const float lin =
                    std::max(0.0f, tap.Rgba[px * 4 + static_cast<std::size_t>(ch)]);
                residualSum += std::abs(static_cast<double>(obs) - LinearToSrgbRef(lin));
                ++samples;
            }
        }
    }
    ASSERT_GT(samples, 0u) << kArm;
    const double meanLsb = residualSum / static_cast<double>(samples) * codes;
    std::cout << "[ MEASURED ] " << kArm << ": overlay-region encode-once residual " << meanLsb
              << " ten-bit LSB over " << samples << " samples" << std::endl;
    EXPECT_LE(meanLsb, 1.5)
        << kArm << ": the overlay region's hand-off is " << meanLsb
        << " LSB from the encode-once of its own tap — the ink did not go through the finalize "
           "(declared after it, or bypassed it), or was encoded twice";

    // And the full-frame compare still elects encode-once with the overlay in
    // frame — the overlay is view content as far as S2 is concerned.
    const HypothesisScores hs = fx.ScoreEncodeHypotheses();
    EXPECT_LT(hs.EncodeOnce, std::min(hs.LinearNoEncode, hs.DoubleEncode))
        << kArm << ": with the overlay in frame the finalize no longer reads as encode-once";
}
