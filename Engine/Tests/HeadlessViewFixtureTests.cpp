// Gates over the headless view fixture — slice 1 of the byte-gate host the
// view-output-chain migration needs.
//
// Every gate here states the production change that reddens it, because the
// arc this fixture serves has repeatedly shipped assertions that could not
// fail. The amplitude gate additionally carries its own positive control: the
// SAME fixture, the SAME content and the SAME destination format run through
// FinalizeQuantizer::Presented with a supplied EIGHT-bit presented format
// measure a 4x larger dither than the ten-bit arms, so the instrument is
// shown to be able to see the failure it is looking for, without editing
// production code to demonstrate it.
//
// Skip policy (chain contract rule 6, three classes):
//   absent device          -> FAIL
//   absent staged asset    -> FAIL
//   absent format capability -> SKIP, naming the format and the usage bits.

#include "HeadlessViewFixture.h"
#include "RGPassQuery.h"

#include "Rendering/Passes/SRGBEncodePass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
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

// The dither and deband kill switches latch into function-local statics on
// first read, so they are a property of the PROCESS. Pinned at load, before any
// test body can trigger the latch: the dither is the thing under test and must
// be on; the deband is off so the encode stays a per-pixel function and the CPU
// models below remain exact (a nonzero gate makes it a 4-tap neighbourhood
// filter no model here reproduces).
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

constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 128;

// Half-width of the amplitude gate, in the destination's own codes.
//
// Set by what the projection resolves, not by where a defect sits: across the
// arms below it reads within 0.001 of its expectation, so 0.05 is fifty times
// the observed spread and still ten times inside the smallest sizing error
// anyone has proposed as plausible (a 2x scale, 0.5 away). The amplitude test
// additionally asserts this number stays well inside the smallest wrong-row
// error the production format table can express, so it cannot be widened into
// uselessness without that assertion failing first.
constexpr double kAmplitudeToleranceLsb = 0.05;

// The displacement-spread gate: what StdEncoded must read, in the
// destination's own codes.
//
// The amplitude projection alone cannot distinguish a working dither from a
// degenerate one, because its slope divides the pattern's shape out: any
// pattern the shader and TriangularDitherRef SHARE reads ~1.0 — measured on
// this fixture, a constant reads 0.998 and the collapsed near-constant IGN
// difference encode_srgb.frag records as previously shipped reads 0.999. The
// spread is the reference-free presence check the projection lacks: a 1-LSB
// TPDF plus rounding gives sqrt(1/6 + 1/12) = 0.5 destination LSB, every
// degenerate variant collapses to rounding alone, sqrt(1/12) = 0.2887, and no
// transcription can move it because it never reads the pattern. On the F16
// host arm there is no rounding term, so healthy is sqrt(1/6) = 0.4082 in the
// dither's own (eight-bit) step and degenerate is ~0.
//
// Tolerance 0.05: the healthy arms measure within ~0.002 of theory, and the
// nearest failure classes sit far outside the band — a halved amplitude reads
// 0.354 (0.10 below the band's floor), a degenerate pattern 0.2887 (0.16
// below).
constexpr double kHealthyDitherStdLsb = 0.5;
const double kNoLatticeDitherStdLsb = std::sqrt(1.0 / 6.0);
constexpr double kStdToleranceLsb = 0.05;

// A shallow linear ramp over the whole image: 0.010 to 0.510 linear, which
// encodes to roughly ten-bit codes 110..760. Spread over 16384 pixels that is
// ~25 pixels per destination code, so every sub-LSB phase is sampled — a flat
// field can sit on an exact code and hide a mis-sized dither completely. The
// ramp is deliberately shallow (0.04 codes per pixel) so the gate cannot be
// confounded by a half-texel sampling offset.
std::vector<float> MakeRampContent()
{
    std::vector<float> px(static_cast<size_t>(kWidth) * kHeight * 4);
    const size_t pixels = static_cast<size_t>(kWidth) * kHeight;
    for (size_t i = 0; i < pixels; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(pixels - 1);
        const float v = 0.010f + 0.500f * t;
        px[i * 4 + 0] = v;
        px[i * 4 + 1] = v;
        px[i * 4 + 2] = v;
        px[i * 4 + 3] = 1.0f;
    }
    return px;
}

// Every arm records what it measured, pass or fail. A band gate whose numbers
// only appear on failure cannot be audited for how much headroom it had.
//
// `unit` converts the fixture's encoded-[0,1] statistics into the unit the arm
// talks in, and `unitName` says which — the two travel together so a printed
// number can never be read in the wrong one.
void ReportArm(const char* arm, const char* unitName, double unit, const DitherAmplitude& a,
               const ResidualStats& r)
{
    std::cout << "[ MEASURED ] " << arm << ": dither amplitude " << a.Encoded * unit << " "
              << unitName << " (pattern correlation " << a.Correlation << "), displacement max "
              << r.MaxEncoded * unit << ", mean " << r.MeanEncoded * unit << ", std "
              << r.StdEncoded * unit << ", over " << r.Samples << " channel samples" << std::endl;
}

HeadlessViewDesc RampDesc(TextureFormat dstFormat, Passes::FinalizeQuantizer quantizer)
{
    HeadlessViewDesc d{};
    d.Width = kWidth;
    d.Height = kHeight;
    d.ClearLinear[0] = 0.0f;
    d.ClearLinear[1] = 0.0f;
    d.ClearLinear[2] = 0.0f;
    d.ClearLinear[3] = 1.0f;
    d.ContentLinearRgba = MakeRampContent();
    d.DestinationFormat = dstFormat;
    d.Quantizer = quantizer;
    return d;
}

// Stand the fixture up under the three-class skip policy. Returns false only
// after issuing a GTEST_SKIP, so callers must return immediately.
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

} // namespace

// ── Instrument check. Runs before anything reads a residual: a gate whose
// source tap is flat, or whose frame never ran the spine, would report a
// beautiful number about nothing.
//
// REDDENS IF: the spine stops declaring its GPU-driven culling stages; the
// per-view FinalColor publish (RenderServices::GetPipelineOutputRG) stops
// resolving; or the render graph stops ordering a CopyDst write after an
// earlier colour-attachment write of the same resource, which would leave the
// world pass's clear standing and the tap flat.
TEST(HeadlessViewFixture, SpineRunsAndTheSourceTapCarriesTheContentTheFinalizeSaw)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    const HeadlessViewDesc desc =
        RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination);
    HV_RENDER_OR_SKIP(fx, desc);

    std::cout << "[ MEASURED ] frame declared " << fx.PassNames().size() << " passes, "
              << RGQuery::CountIn(fx.PassNames(), RGQuery::Subtree{"GPUCulling"}) << " of them GPUCulling.*" << std::endl;
    EXPECT_GT(RGQuery::CountIn(fx.PassNames(), RGQuery::Subtree{"GPUCulling"}), 0u)
        << "the frame carries no culling dispatches — the real spine did not run, so this is "
           "not a world view";

    // The capability probe the skip class rests on must be able to answer NO.
    // IsTextureFormatSupported(fmt, 0) returns true unconditionally, so a probe
    // that forgot its usage bits would make the skip unreachable and every
    // 10-bit assertion above nominally universal. A depth format asked for as a
    // COLOUR attachment is the cheapest thing this device must refuse.
    EXPECT_TRUE(fx.Device()->IsTextureFormatSupported(TextureFormat::RGB10A2_UNORM,
                                                      HeadlessViewFixture::DestinationUsageFlags()))
        << "the arm under test reported unsupported after it had already rendered";
    EXPECT_FALSE(fx.Device()->IsTextureFormatSupported(TextureFormat::D32_FLOAT,
                                                       HeadlessViewFixture::DestinationUsageFlags()))
        << "the capability probe accepts a depth format as a colour attachment, so it cannot "
           "refuse anything and the format skip class is unreachable";

    const ViewStageTap& src = fx.SourceBeforeFinalize();
    ASSERT_EQ(src.Width, kWidth);
    ASSERT_EQ(src.Height, kHeight);
    ASSERT_EQ(src.Rgba.size(), static_cast<size_t>(kWidth) * kHeight * 4);
    ASSERT_EQ(src.Format, TextureFormat::R16G16B16A16_FLOAT);

    float lo = src.Rgba[0];
    float hi = src.Rgba[0];
    double worstRel = 0.0;
    for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i)
    {
        const float got = src.Rgba[i * 4];
        lo = std::min(lo, got);
        hi = std::max(hi, got);
        const float want = desc.ContentLinearRgba[i * 4];
        worstRel = std::max(worstRel, std::abs(static_cast<double>(got - want)) / want);
    }
    // Half precision holds ~11 significant bits, so 2^-11 is the floor a
    // faithful round trip can reach.
    EXPECT_LT(worstRel, 1.0 / 1024.0)
        << "the tap does not hold the authored content (worst relative error " << worstRel << ")";
    EXPECT_GT(hi - lo, 0.4f) << "the tap is flat (" << lo << ".." << hi
                             << ") — the world pass's clear won, so the injected content never "
                                "reached the finalize";

    EXPECT_EQ(fx.Facts().DestinationFormat, TextureFormat::RGB10A2_UNORM)
        << "the destination format is read off the graph resource, not off the desc";
    EXPECT_EQ(fx.Handoff().Format, TextureFormat::RGB10A2_UNORM);
    EXPECT_EQ(fx.Handoff().Bytes.size(), static_cast<size_t>(kWidth) * kHeight * 4);
}

// ── The headline gate: with FinalizeQuantizer::Destination the dither's
// amplitude is ONE code of the surface it is written to, wherever that
// surface's step happens to be — measured end-to-end on the GPU into a real
// 10-bit texture, with no device override anywhere in the fixture.
//
// The statistic is the observed displacement PROJECTED onto the shader's own
// TPDF pattern (MeasureDitherAmplitude), not a max. A max cannot do this job
// and the arithmetic says why: it sums the dither with the destination's
// rounding, and rounding alone reaches 0.5 LSB, so a dither sized four times
// too SMALL still measures about 0.76 LSB. Any lower bound placed under that
// number is a bound placed under the defect, and raising it only moves where
// the accident happens. MEASURED here by mutating EncodeDitherLsbForFormat so
// the eight-bit row returns the ten-bit step: max displacement 1.47549 ->
// 0.759131, which a 0.75 bound passes by 0.009, while the projection reads
// 0.998891 -> 0.250964 and passes nothing.
//
// The expectation is a fact about the FORMAT and not a reading of the amplitude
// table: a destination with N codes has an LSB of 1/N by definition, so a
// dither sized to its true step measures 1.0 in that destination's own codes.
// DestinationCodeScale supplies N from the format's bit depth, so mutating
// EncodeDitherLsbForFormat moves the measurement and leaves the expectation
// standing where it was — which is the whole difference between a gate and a
// literal compared against itself.
//
// ARMS 3 AND 4 are the Presented arms, both with a SUPPLIED presented format
// (the production shape post-P5 — every production caller engages the
// parameter). Arm 3 supplies RGB10A2 and must measure 1.0 of the TEN-bit step
// into the same ten-bit destination as arm 1 — the real ten-bit Presented
// assertion the fixture header once documented as impossible headless. Arm 4
// is the POSITIVE CONTROL: it supplies RGBA8_UNORM and must measure the
// table's own 8:10 ratio, proving the instrument resolves the wrong-depth
// error the other arms assert the absence of, rather than being unable to see
// anything at all.
//
// WHAT IT DOES NOT GATE — the quantizer's IDENTITY. Every arm here declares its
// quantizer, and the gate measures the amplitude that followed; it does not
// infer which quantizer was named. On an 8-bit destination it could not: a
// Presented arm sized to a supplied 8-bit format is numerically the same 1/255
// an 8-bit Destination yields, so arm 2 would read 1.0 under either. Only the
// enum can answer that question, and it is asked where the policy publishes it
// (HostFinalizePolicyDeclaresPresentedAndStampsSdrFinalized).
//
// REDDENS IF: FinalizeQuantizer::Destination stops reading the destination
// resource's format (e.g. collapsing the Destination case into the Presented
// one in SRGBEncodePass.cpp's dstQuantizerLsb selector — visible on arm 1,
// where the two steps differ, and NOT on arm 2, where they coincide); the
// RGB10A2_UNORM row is removed from EncodeDitherLsbForFormat, dropping it to
// the 8-bit default; the dither amplitude is scaled by anything other than 1;
// the dither is removed; or the dither pattern degenerates — a constant, or
// the collapsed IGN difference encode_srgb.frag records as previously shipped
// — EVEN IF TriangularDitherRef is resynced to the same degenerate pattern.
// The last class is the spread assertion's: the projection reads ~1.0 for any
// shared pattern, while the spread reads rounding-alone (0.2887) for every
// degenerate one and cannot be satisfied by a transcription.
TEST(HeadlessViewFixture, DitherAmplitudeIsOneCodeOfTheDestinationItIsWrittenTo)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);

    // What the band must be able to see. The production table's 8:10 ratio is
    // the SMALLEST amplitude error a wrong row in it can produce; the same
    // ratio computed from the two formats' real code counts owes that table
    // nothing, so the two disagreeing means a row has stopped naming its
    // format's true step.
    const double tableRatio =
        static_cast<double>(Passes::EncodeDitherLsbForFormat(TextureFormat::RGBA8_UNORM)) /
        static_cast<double>(Passes::EncodeDitherLsbForFormat(TextureFormat::RGB10A2_UNORM));
    const double trueRatio =
        static_cast<double>(DestinationCodeScale(TextureFormat::RGB10A2_UNORM)) /
        static_cast<double>(DestinationCodeScale(TextureFormat::RGBA8_UNORM));
    std::cout << "[ MEASURED ] amplitude table 8:10 ratio " << tableRatio << ", code-count ratio "
              << trueRatio << std::endl;
    EXPECT_NEAR(tableRatio, trueRatio, 1e-3)
        << "EncodeDitherLsbForFormat puts the eight-bit step at " << tableRatio
        << " ten-bit steps, but the formats' code counts put it at " << trueRatio
        << " — one of its rows no longer names its own format's real quantizer";
    // EXPECT, not ASSERT: when this fires the arms below still hold the direct
    // diagnosis — which destination was dithered at what — and aborting here
    // would replace it with an inference about a ratio.
    EXPECT_LT(kAmplitudeToleranceLsb, (tableRatio - 1.0) / 4.0)
        << "the tolerance " << kAmplitudeToleranceLsb
        << " no longer sits well inside the smallest wrong-row error the amplitude table can "
           "produce (ratio " << tableRatio
        << ") — passing this gate would stop meaning the right row was taken";

    // Arm 1 — Destination into a real 10-bit texture.
    HV_RENDER_OR_SKIP(fx,
                      RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination));
    const double tenBitCodes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    const DitherAmplitude tenBitAmp = fx.MeasureDitherAmplitude();
    const ResidualStats tenBit = fx.EncodeOnceResidual();
    ASSERT_GT(tenBitAmp.Samples, 0u);
    ReportArm("Destination -> RGB10A2_UNORM", "ten-bit LSB", tenBitCodes, tenBitAmp, tenBit);
    EXPECT_NEAR(tenBitAmp.Encoded * tenBitCodes, 1.0, kAmplitudeToleranceLsb)
        << "the ten-bit destination was dithered at " << tenBitAmp.Encoded * tenBitCodes
        << " of its own codes rather than 1. Residual map: "
        << fx.WriteResidualMap("destination-10bit");
    // The presence check the projection cannot make: the spread never reads
    // the reference pattern, so shader and reference degenerating TOGETHER —
    // which the projection scores ~1.0 — still collapses this to 0.2887.
    EXPECT_NEAR(tenBit.StdEncoded * tenBitCodes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << "the displacement's spread is " << tenBit.StdEncoded * tenBitCodes
        << " ten-bit LSB where a 1-LSB TPDF plus rounding gives 0.5 — at sqrt(1/12) = 0.2887 the "
           "pixels hold rounding alone and the amplitude the projection reported is not actually "
           "in them. Residual map: "
        << fx.WriteResidualMap("destination-10bit");
    // Total displacement, which the projection deliberately does not bound: it
    // reports the dither's correlated part and is blind to a constant offset.
    // A unit TPDF plus rounding cannot arithmetically exceed 1.5.
    EXPECT_LE(tenBit.MaxEncoded * tenBitCodes, 2.0)
        << "displacement reaches " << tenBit.MaxEncoded * tenBitCodes << " ten-bit LSB (mean "
        << tenBit.MeanEncoded * tenBitCodes
        << "), above the 1.5 a unit dither and rounding can reach. If the amplitude assertion "
           "above passed, the excess is something other than the dither moving these pixels";

    // Arm 2 — the same contract at 8 bits. The expectation is expressed in
    // THAT destination's codes, so passing both arms is what says the amplitude
    // tracks the destination rather than happening to suit one depth.
    HV_RENDER_OR_SKIP(fx,
                      RampDesc(TextureFormat::RGBA8_UNORM, Passes::FinalizeQuantizer::Destination));
    const double eightBitCodes = DestinationCodeScale(TextureFormat::RGBA8_UNORM);
    const DitherAmplitude eightBitAmp = fx.MeasureDitherAmplitude();
    const ResidualStats eightBit = fx.EncodeOnceResidual();
    ASSERT_GT(eightBitAmp.Samples, 0u);
    ReportArm("Destination -> RGBA8_UNORM", "eight-bit LSB", eightBitCodes, eightBitAmp, eightBit);
    EXPECT_NEAR(eightBitAmp.Encoded * eightBitCodes, 1.0, kAmplitudeToleranceLsb)
        << "the eight-bit destination was dithered at " << eightBitAmp.Encoded * eightBitCodes
        << " of its own codes rather than 1. Residual map: "
        << fx.WriteResidualMap("destination-8bit");
    EXPECT_NEAR(eightBit.StdEncoded * eightBitCodes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << "the displacement's spread is " << eightBit.StdEncoded * eightBitCodes
        << " eight-bit LSB where a 1-LSB TPDF plus rounding gives 0.5 (rounding alone is 0.2887). "
           "Residual map: "
        << fx.WriteResidualMap("destination-8bit");
    EXPECT_LE(eightBit.MaxEncoded * eightBitCodes, 2.0)
        << "displacement reaches " << eightBit.MaxEncoded * eightBitCodes
        << " eight-bit LSB, above the 1.5 a unit dither and rounding can reach";

    // Arm 3 — Presented with the presented format SUPPLIED as ten-bit: the
    // dither must land at 1.0 of the ten-bit step, same expectation as arm 1
    // reached via the other quantizer. REDDENS IF the pass stops consuming the
    // supplied format (falling back to the headless device's Unknown would
    // read trueRatio here, the 8-bit row).
    {
        HeadlessViewDesc presentedTen =
            RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Presented);
        presentedTen.PresentedFormat = TextureFormat::RGB10A2_UNORM;
        HV_RENDER_OR_SKIP(fx, presentedTen);
    }
    const DitherAmplitude presentedAmp = fx.MeasureDitherAmplitude();
    const ResidualStats presented = fx.EncodeOnceResidual();
    ASSERT_GT(presentedAmp.Samples, 0u);
    ReportArm("Presented(RGB10A2 supplied) -> RGB10A2_UNORM", "ten-bit LSB", tenBitCodes,
              presentedAmp, presented);
    EXPECT_NEAR(presentedAmp.Encoded * tenBitCodes, 1.0, kAmplitudeToleranceLsb)
        << "the Presented arm with a supplied ten-bit format measured "
        << presentedAmp.Encoded * tenBitCodes
        << " ten-bit LSB rather than 1 — the pass is not consuming the supplied presented format "
           "(trueRatio here means it fell back to the headless device's Unknown row)";
    EXPECT_NEAR(presented.StdEncoded * tenBitCodes, kHealthyDitherStdLsb, kStdToleranceLsb)
        << "the displacement's spread is " << presented.StdEncoded * tenBitCodes
        << " ten-bit LSB where a 1-LSB TPDF plus rounding gives 0.5";

    // Arm 4 — the positive control: the SAME arm with the presented format
    // supplied as EIGHT-bit must measure the table's 8:10 ratio. This proves
    // the instrument resolves the wrong-depth amplitude the arms above assert
    // the absence of — and it kills the mutant that hard-wires RGB10A2 into
    // the sizing path instead of reading the parameter.
    {
        HeadlessViewDesc presentedEight =
            RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Presented);
        presentedEight.PresentedFormat = TextureFormat::RGBA8_UNORM;
        HV_RENDER_OR_SKIP(fx, presentedEight);
    }
    const DitherAmplitude controlAmp = fx.MeasureDitherAmplitude();
    const ResidualStats control = fx.EncodeOnceResidual();
    ASSERT_GT(controlAmp.Samples, 0u);
    ReportArm("Presented(RGBA8 supplied) -> RGB10A2_UNORM (positive control)", "ten-bit LSB",
              tenBitCodes, controlAmp, control);
    EXPECT_NEAR(controlAmp.Encoded * tenBitCodes, trueRatio, kAmplitudeToleranceLsb)
        << "the Presented arm with a supplied eight-bit format measured "
        << controlAmp.Encoded * tenBitCodes << " ten-bit LSB where the 8-bit step is " << trueRatio
        << " of them — the instrument no longer resolves a wrong-depth amplitude and the arms "
           "above are no longer meaningful";
}

// ── Chain contract rule 3: where a step intentionally changes output, elect
// among three models rather than tolerating a band. Encode-once must win by a
// margin, not by a fudge factor.
//
// The scorer takes NO caller-supplied source: it reads the hand-off, the source
// tap and the declared facts from the fixture's own frame. A caller able to
// supply a source is a caller able to supply a wrong one and still be handed a
// winner.
//
// REDDENS IF: the pass stops applying the sRGB OETF (linear wins), applies it
// twice (double-encode wins), or takes a different encoding arm than the
// fixture predicted — the compare is what makes a wrong prediction observable
// instead of silent.
TEST(HeadlessViewFixture, EncodeOnceWinsTheThreeHypothesisCompareByAMargin)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HV_RENDER_OR_SKIP(fx,
                      RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination));

    const double tenBitCodes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    const HypothesisScores s = fx.ScoreEncodeHypotheses();
    ASSERT_GT(s.LinearNoEncode, 0.0) << "the scorer produced nothing";
    std::cout << "[ MEASURED ] hypotheses (ten-bit LSB): encode-once " << s.EncodeOnce * tenBitCodes
              << ", linear " << s.LinearNoEncode * tenBitCodes << ", double-encode "
              << s.DoubleEncode * tenBitCodes << std::endl;

    EXPECT_LT(s.EncodeOnce * tenBitCodes, 1.0)
        << "encode-once residual " << s.EncodeOnce * tenBitCodes
        << " ten-bit LSB: the winning hypothesis should sit inside the dither's own amplitude";
    EXPECT_LT(s.EncodeOnce * 20.0, s.LinearNoEncode)
        << "encode-once " << s.EncodeOnce * tenBitCodes << " vs linear "
        << s.LinearNoEncode * tenBitCodes
        << " ten-bit LSB — the two hypotheses are not separated. Residual map: "
        << fx.WriteResidualMap("hypothesis");
    EXPECT_LT(s.EncodeOnce * 20.0, s.DoubleEncode)
        << "encode-once " << s.EncodeOnce * tenBitCodes << " vs double-encode "
        << s.DoubleEncode * tenBitCodes << " ten-bit LSB — the two hypotheses are not separated";
}

// ── Chain contract rule 2: gate the (bytes, declared space) pair. This is the
// arm that runs the HOST's policy verbatim — Engine::Renderer::DeclareViewFinalize,
// the code the editor calls — rather than a re-implementation of it.
//
// It also pins the fact that the hand-off format is NOT the fixture's to
// choose on this path: the policy sizes its target from the source, so a desc
// asking for 10 bits here would be ignored, and Facts() reports what was
// really declared.
//
// The contract assertions read ViewFinalizeResult::Step — the policy's own
// publish. They are worth nothing read any other way: while the fixture stated
// the quantizer itself, flipping the policy's Presented to Destination (which
// turns the editor's per-view dither OFF, an F16 target having no step of its
// own) left all five tests here green with byte-identical numbers.
//
// REDDENS IF: DeclareViewFinalize returns the caller's space instead of
// SdrFinalized() on its success path; the eligibility predicate starts
// declining under SDR (the returned image would be the input); the policy stops
// pinning its destination to the source's format; it hands the pass a
// different input space, quantizer or presented format than it publishes; or
// it stops forwarding the supplied presented format — the absolute TEN-bit
// amplitude below is the assertion the fixture header once documented as
// impossible on this arm, and a policy that drops the parameter (the pass's
// device fallback reads Unknown headless) measures 4.01 ten-bit LSB here, not
// 1.0. (Against pre-P5 main this gate is a COMPILE red — the parameter did not
// exist — so the meaningful behavioural red is that forwards-nullopt mutation,
// exercised in this change's mutation gate, not the build break.)
TEST(HeadlessViewFixture, HostFinalizePolicyDeclaresPresentedAndStampsSdrFinalized)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HeadlessViewDesc desc =
        RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination);
    desc.Driver = FinalizeDriver::HostPolicy;
    // The production shape: the window owner supplies its swapchain's format.
    // Headless there is no window, so the desc states the ten-bit surface a
    // production host would have resolved.
    desc.PresentedFormat = TextureFormat::RGB10A2_UNORM;
    HV_RENDER_OR_SKIP(fx, desc);

    ASSERT_TRUE(fx.SourceBeforeFinalize().Space.has_value());
    EXPECT_EQ(*fx.SourceBeforeFinalize().Space, UI::UITextureSpace::DisplayLinearSdr())
        << "the pipeline's own stamp under SDR output";

    ASSERT_TRUE(fx.Handoff().Space.has_value());
    EXPECT_EQ(*fx.Handoff().Space, UI::UITextureSpace::SdrFinalized())
        << "a finalized view carries the sRGB curve; a linear stamp here washes the viewport out "
           "while every pixel comparison still passes";

    EXPECT_EQ(fx.Facts().DestinationFormat, TextureFormat::R16G16B16A16_FLOAT)
        << "the host policy pins its target to the source's format — the desc's 10-bit request "
           "must NOT have been honoured on this path";
    EXPECT_GT(RGQuery::CountIn(fx.PassNames(), RGQuery::Exact{"HeadlessView.Finalized"}), 0u)
        << "the host policy declined to declare — the returned image is the caller's own input";

    ASSERT_TRUE(fx.Facts().Step.has_value())
        << "the policy published no step, i.e. it declined to declare a finalize at all";
    EXPECT_EQ(fx.Facts().Step->Quantizer, Passes::FinalizeQuantizer::Presented)
        << "the policy writes an F16 intermediate the UI still composites onto, so its filters "
           "must be sized to the PRESENTED step; Destination here reads a float target's absent "
           "quantizer and silently turns the per-view dither off";
    EXPECT_EQ(fx.Facts().Step->InputSpace, Passes::FinalizeInputSpace::Linear)
        << "the finalize is the step that applies the OETF, so its input is scene-linear; "
           "EncodedSrgb here would make it a requantize and drop the transfer function";
    EXPECT_EQ(fx.Facts().Step->PresentedFormat, TextureFormat::RGB10A2_UNORM)
        << "the policy published a different presented format than the desc supplied — the "
           "publish is the contract downstream gates read, so it must be the value the pass "
           "was given";

    // WHICH quantizer was selected is the enum's business, asserted above. This
    // is the separate question of what the pass then DID — that a step of some
    // size was actually applied to the returned pixels, and which size.
    //
    // The supplied ten-bit format must reach the pass, so the step is 1/1023 —
    // an ABSOLUTE ten-bit expectation on the host-policy arm. The policy
    // dropping the parameter reads 4.01 here (the pass falls back to the
    // headless device's Unknown and the 8-bit row answers); Destination in
    // place of Presented reads ~0 (the F16 destination has no step of its
    // own). Measured, not assumed.
    const double tenBitCodes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);
    const DitherAmplitude amp = fx.MeasureDitherAmplitude();
    const ResidualStats r = fx.EncodeOnceResidual();
    ASSERT_GT(amp.Samples, 0u) << "the F16 hand-off produced no measurable samples";
    ReportArm("HostPolicy(RGB10A2 supplied) -> R16G16B16A16_FLOAT", "ten-bit LSB", tenBitCodes,
              amp, r);
    EXPECT_NEAR(amp.Encoded * tenBitCodes, 1.0, kAmplitudeToleranceLsb)
        << "the host policy's pass dithered the returned image at " << amp.Encoded
        << " encoded units (" << amp.Encoded * tenBitCodes
        << " ten-bit LSB) rather than 1.0 of the supplied ten-bit step. Zero means no step was "
           "applied to a float target that has none of its own; ~4 means the supplied presented "
           "format never reached the pass and the Unknown 8-bit row answered instead";
    // F16 has no code lattice, so the spread here is the TPDF alone:
    // sqrt(1/6) of the dither's own (here ten-bit) step, and ~0 for every
    // degenerate pattern — the projection reads ~1.0 for a pattern the shader
    // and reference share, this cannot.
    EXPECT_NEAR(r.StdEncoded * tenBitCodes, kNoLatticeDitherStdLsb, kStdToleranceLsb)
        << "the displacement's spread is " << r.StdEncoded * tenBitCodes
        << " ten-bit LSB where a 1-LSB TPDF into a lattice-free F16 target gives sqrt(1/6) = "
           "0.4082 — near zero the returned image holds no dither-shaped spread at all";

    const HypothesisScores s = fx.ScoreEncodeHypotheses();
    std::cout << "[ MEASURED ] host-policy hypotheses (encoded units): encode-once " << s.EncodeOnce
              << ", linear " << s.LinearNoEncode << ", double-encode " << s.DoubleEncode
              << std::endl;
    EXPECT_LT(s.EncodeOnce * 20.0, s.LinearNoEncode)
        << "encode-once " << s.EncodeOnce << " vs linear " << s.LinearNoEncode
        << ": the host policy's image does not hold singly-encoded values";
    EXPECT_LT(s.EncodeOnce * 20.0, s.DoubleEncode)
        << "encode-once " << s.EncodeOnce << " vs double-encode " << s.DoubleEncode;
}

// ── The refusal operand: a host's own "keep the linear chain this frame",
// stated to the policy rather than expressed by branching around the call.
// Models the production refusals — the Game View's HDR-movie frame and the
// Player's HUD-less frame — on the declined contract an ineligible device
// already resolves: the caller's own image and stamp come back, and no step
// is published.
//
// The desc is the declaring arm's desc (the test above) except for the
// operand, so what separates the two runs is the refusal alone; the culling
// control proves the zero pass count is a declined finalize on a real frame,
// not an empty graph or a query that stopped matching.
//
// REDDENS IF: DeclareViewFinalize stops honouring the operand — the
// pre-registered "declined arm stops declining" extraction failure, which at
// runtime loses every frame of an HDR movie recording. The pass-level pin
// (RenderPipelineDeclareTests.EncodedFinalizeRefusesHdrOverrideAndTakesBothQuantizers)
// stays green under exactly that mutation, because it exercises the pass
// directly; this host-layer arm is the gate that catches it.
TEST(HeadlessViewFixture, HostFinalizePolicyHonoursTheHostRefusalOperand)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);
    HeadlessViewDesc desc =
        RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination);
    desc.Driver = FinalizeDriver::HostPolicy;
    desc.PresentedFormat = TextureFormat::RGB10A2_UNORM;
    desc.HostRefusal = true;
    HV_RENDER_OR_SKIP(fx, desc);

    EXPECT_GT(RGQuery::CountIn(fx.PassNames(), RGQuery::Subtree{"GPUCulling"}), 0u)
        << "the refusal arm's frame carries no culling dispatches — the spine did not run, so "
           "the zero finalize count below would be about an empty graph, not about the policy";

    EXPECT_FALSE(fx.Facts().Step.has_value())
        << "the policy published a step on a frame its host refused — the declined arm stopped "
           "declining";
    EXPECT_EQ(RGQuery::CountIn(fx.PassNames(), RGQuery::Exact{"HeadlessView.Finalized"}), 0u)
        << "a finalize pass was declared under a host refusal";
    ASSERT_TRUE(fx.Handoff().Space.has_value());
    EXPECT_EQ(*fx.Handoff().Space, UI::UITextureSpace::DisplayLinearSdr())
        << "a refused frame's hand-off must carry the caller's own pipeline stamp — SdrFinalized "
           "here means the image was finalized (or stamped as if it were) against the host's "
           "stated refusal";
}

// ── The host policy forwards the VALUE it was given, not a constant.
//
// The ten-bit gate above cannot distinguish "forwards the parameter" from
// "hardcodes RGB10A2": both read 1.0 there. This arm supplies RGBA8_UNORM and
// must measure ONE eight-bit step — 4.01 ten-bit LSB — which the hardcoding
// mutant cannot produce. The second arm supplies nothing, i.e. Unknown, and
// must measure the same 1/255 through the format matrix's stated no-surface
// row: Unknown and a supplied 8-bit format coincide numerically BY DESIGN
// (the row assumes 8 bits), so the pair of arms pins the dataflow — supplied
// RGB10A2 -> 1/1023 (previous test), supplied RGBA8 -> 1/255, Unknown ->
// 1/255 — and the publish gate is what separates the last two.
TEST(HeadlessViewFixture, HostPolicyForwardsTheSuppliedPresentedFormatNotAConstant)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);

    // Arm 1 — supplied eight-bit.
    HeadlessViewDesc desc =
        RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination);
    desc.Driver = FinalizeDriver::HostPolicy;
    desc.PresentedFormat = TextureFormat::RGBA8_UNORM;
    HV_RENDER_OR_SKIP(fx, desc);

    ASSERT_TRUE(fx.Facts().Step.has_value());
    EXPECT_EQ(fx.Facts().Step->PresentedFormat, TextureFormat::RGBA8_UNORM);
    {
        const DitherAmplitude amp = fx.MeasureDitherAmplitude();
        const ResidualStats r = fx.EncodeOnceResidual();
        ASSERT_GT(amp.Samples, 0u);
        ReportArm("HostPolicy(RGBA8 supplied) -> R16G16B16A16_FLOAT", "eight-bit LSB", 255.0, amp,
                  r);
        EXPECT_NEAR(amp.Encoded * 255.0, 1.0, kAmplitudeToleranceLsb)
            << "the host policy's pass dithered at " << amp.Encoded * 255.0
            << " eight-bit LSB where the supplied RGBA8 format's step is 1 — ~0.25 here means "
               "the policy ignored the parameter and hardcoded a ten-bit format";
    }

    // Arm 2 — unset: Unknown, the honest no-surface statement, resolves the
    // matrix's 8-bit row. Same amplitude as arm 1 by design; the publish is
    // what states WHICH producer the value came from.
    HeadlessViewDesc unsetDesc =
        RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Destination);
    unsetDesc.Driver = FinalizeDriver::HostPolicy;
    HV_RENDER_OR_SKIP(fx, unsetDesc);

    ASSERT_TRUE(fx.Facts().Step.has_value());
    EXPECT_EQ(fx.Facts().Step->PresentedFormat, TextureFormat::Unknown)
        << "an unset presented format must be published as Unknown — the no-surface statement — "
           "never invented downstream";
    {
        const DitherAmplitude amp = fx.MeasureDitherAmplitude();
        const ResidualStats r = fx.EncodeOnceResidual();
        ASSERT_GT(amp.Samples, 0u);
        ReportArm("HostPolicy(unset -> Unknown) -> R16G16B16A16_FLOAT", "eight-bit LSB", 255.0,
                  amp, r);
        EXPECT_NEAR(amp.Encoded * 255.0, 1.0, kAmplitudeToleranceLsb)
            << "the Unknown row must size the dither at one eight-bit step (measured "
            << amp.Encoded * 255.0 << ")";
    }
}

// ── The boundary, measured rather than asserted in prose.
//
// An UNSET presented format is the "no presented surface" statement: the pass
// falls back to the headless device's GetSwapchainTextureFormat(), which is
// Unknown, and EncodeDitherLsbForFormat maps Unknown through its stated 8-bit
// row — NOT because an 8-bit swapchain exists, but because an undescribed
// destination is conservatively assumed to quantize at 8 bits. Post-P5 this is
// a CONTRACT pin, not a characterization: every production caller supplies the
// format (DeclareViewFinalize requires it), so the row this measures is the
// deliberate fallback for suppliers that state Unknown, and the matrix comment
// documents it as such. The paired arm proves the SAME desc with the format
// supplied reaches the true ten-bit step — the boundary is the supplied/unset
// choice, no longer a limit of the fixture.
TEST(HeadlessViewFixture, PresentedArmSizesToTheSuppliedFormatAndUnsetToTheUnknownRow)
{
    HeadlessViewFixture fx;
    HV_REQUIRE_UP(fx);

    const double tenBitCodes = DestinationCodeScale(TextureFormat::RGB10A2_UNORM);

    // Arm 1 — unset: the Unknown row answers at 1/255. 1/255 of full scale is
    // 4.012 ten-bit LSB; with rounding the displacement cannot exceed 4.51 and
    // a well-sampled ramp gets close to it.
    HV_RENDER_OR_SKIP(fx,
                      RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Presented));
    ASSERT_EQ(fx.Device()->GetSwapchainTextureFormat(), TextureFormat::Unknown)
        << "a headless device reported a swapchain format — the no-surface row this pin "
           "measures is not what an unset arm resolves here";
    {
        const DitherAmplitude amp = fx.MeasureDitherAmplitude();
        const ResidualStats r = fx.EncodeOnceResidual();
        ASSERT_GT(r.Samples, 0u);
        ReportArm("Presented(unset) -> RGB10A2_UNORM (Unknown row)", "ten-bit LSB", tenBitCodes,
                  amp, r);
        EXPECT_GT(r.MaxEncoded * tenBitCodes, 2.5)
            << "Presented-unset measured " << r.MaxEncoded * tenBitCodes
            << " ten-bit LSB — the Unknown 8-bit fallback row is no longer what an unset arm "
               "resolves";
        EXPECT_LE(r.MaxEncoded * tenBitCodes, 5.0)
            << "Presented-unset measured " << r.MaxEncoded * tenBitCodes
            << " ten-bit LSB, above what a 1/255 amplitude plus rounding can produce (4.51)";
    }

    // Arm 2 — the SAME desc with the format supplied: the true ten-bit step,
    // headless. A unit dither plus rounding cannot arithmetically exceed 1.5
    // ten-bit LSB, so the two arms are separated by their bounds alone.
    {
        HeadlessViewDesc supplied =
            RampDesc(TextureFormat::RGB10A2_UNORM, Passes::FinalizeQuantizer::Presented);
        supplied.PresentedFormat = TextureFormat::RGB10A2_UNORM;
        HV_RENDER_OR_SKIP(fx, supplied);
        const DitherAmplitude amp = fx.MeasureDitherAmplitude();
        const ResidualStats r = fx.EncodeOnceResidual();
        ASSERT_GT(r.Samples, 0u);
        ReportArm("Presented(RGB10A2 supplied) -> RGB10A2_UNORM (boundary pair)", "ten-bit LSB",
                  tenBitCodes, amp, r);
        EXPECT_NEAR(amp.Encoded * tenBitCodes, 1.0, kAmplitudeToleranceLsb)
            << "the supplied ten-bit format sized the dither at " << amp.Encoded * tenBitCodes
            << " ten-bit LSB rather than 1 — the parameter is not reaching the sizing path";
        EXPECT_LE(r.MaxEncoded * tenBitCodes, 2.0)
            << "displacement reaches " << r.MaxEncoded * tenBitCodes
            << " ten-bit LSB (a unit dither plus rounding cannot exceed 1.5) — the supplied "
               "format did not defeat the Unknown fallback";
    }
}
