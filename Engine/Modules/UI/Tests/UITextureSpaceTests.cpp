// Pins for the external-texture source space: the format cross-check table and
// the registration policy built on it (#767 P1a). The check is a pure function
// of (format, space), so most of this needs no device; the registration policy
// tests drive a real UIManager through the public registration API.
#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "Rendering/Core/Device.h"
#include "UI/UIManager.h"
#include "UI/UIPrimitive.h"
#include "UI/UITextureSpace.h"
#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using GameEngine::UI::CheckTextureSpaceAgainstFormat;
using GameEngine::UI::UITextureSpace;
using GameEngine::UI::UITextureSpaceCheck;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr const char* kName = "test_space_src";

} // namespace

TEST(UITextureSpaceTests, EncodedFormatsAcceptOnlyAuthoredSrgb)
{
    for (TextureFormat f : {TextureFormat::RGBA8_SRGB, TextureFormat::BGRA8_SRGB,
                            TextureFormat::BC7_SRGB, TextureFormat::BC1_SRGB})
    {
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::SrgbAuthored()),
                  UITextureSpaceCheck::Valid);
        // A finalized view must arrive UNDECODED — its texels already are blend
        // values. An sRGB view would decode them on read, so the pair is a lie.
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::SdrFinalized()),
                  UITextureSpaceCheck::ContradictsFormat);
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::DisplayLinearSdr()),
                  UITextureSpaceCheck::ContradictsFormat);
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::HdrLinear()),
                  UITextureSpaceCheck::ContradictsFormat);
    }
}

TEST(UITextureSpaceTests, EightBitUnormHoldsEncodedBytesButNotLinearContent)
{
    for (TextureFormat f : {TextureFormat::RGBA8_UNORM, TextureFormat::BGRA8_UNORM,
                            TextureFormat::BC7_UNORM})
    {
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::SrgbAuthored()),
                  UITextureSpaceCheck::Valid);
        // Encoded bytes behind a non-decoding 8-bit view: exactly what a finalize
        // to an 8-bit destination would store.
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::SdrFinalized()),
                  UITextureSpaceCheck::Valid);
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::DisplayLinearSdr()),
                  UITextureSpaceCheck::ContradictsFormat);
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::HdrLinear()),
                  UITextureSpaceCheck::ContradictsFormat);
    }
}

TEST(UITextureSpaceTests, WideFormatsHoldLinearContentButNotEncodedBytes)
{
    for (TextureFormat f : {TextureFormat::R16G16B16A16_FLOAT, TextureFormat::R32G32B32A32_FLOAT,
                            TextureFormat::R11G11B10_FLOAT, TextureFormat::RGB10A2_UNORM,
                            TextureFormat::R16G16B16A16_UNORM})
    {
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::DisplayLinearSdr()),
                  UITextureSpaceCheck::Valid);
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::HdrLinear()),
                  UITextureSpaceCheck::Valid);
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::SrgbAuthored()),
                  UITextureSpaceCheck::ContradictsFormat);
        // The row every finalized editor view takes: a plain wide view hands its
        // texels over exactly as written, which is what SdrFinalized promises.
        EXPECT_EQ(CheckTextureSpaceAgainstFormat(f, UITextureSpace::SdrFinalized()),
                  UITextureSpaceCheck::Valid);
    }
}

TEST(UITextureSpaceTests, UnknownFormatIsNotAValidBucket)
{
    // Unknown must never read as "fine" — a device that cannot report a format
    // is a check that did not happen, not a check that passed.
    EXPECT_EQ(CheckTextureSpaceAgainstFormat(TextureFormat::Unknown, UITextureSpace::SrgbAuthored()),
              UITextureSpaceCheck::FormatNotClassifiable);
    EXPECT_EQ(CheckTextureSpaceAgainstFormat(TextureFormat::Unknown, UITextureSpace::HdrLinear()),
              UITextureSpaceCheck::FormatNotClassifiable);
    EXPECT_EQ(CheckTextureSpaceAgainstFormat(TextureFormat::D32_FLOAT, UITextureSpace::HdrLinear()),
              UITextureSpaceCheck::FormatNotClassifiable);
}

TEST(UITextureSpaceTests, SpacesCompareByIdentity)
{
    EXPECT_EQ(UITextureSpace::SrgbAuthored(), UITextureSpace::SrgbAuthored());
    EXPECT_NE(UITextureSpace::SrgbAuthored(), UITextureSpace::DisplayLinearSdr());
    EXPECT_NE(UITextureSpace::DisplayLinearSdr(), UITextureSpace::HdrLinear());
}

// Registration policy. A refused registration is a changed frame, so only the
// contradiction whose producers cannot utter it is refused — DisplayLinearSdr
// producers (#767 P1b) render into wide formats by construction, so that pair
// can only be a new, wrong stamp. Any other contradiction is diagnosed and
// let through, because refusing it would blank real UI.
TEST(UITextureSpaceTests, RefusedRegistrationBecomesAnUnresolvedRequest)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIManager ui(dev.get());
    ui.SetExternalTextureRG(kName, 128, 64, UITextureSpace::DisplayLinearSdr(),
                            TextureFormat::RGBA8_SRGB);

    const std::vector<std::string> pending = ui.ConsumeUnresolvedExternalTextureRequests();
    EXPECT_NE(std::find(pending.begin(), pending.end(), kName), pending.end())
        << "a refused registration must reach the producer as a missing texture";
}

TEST(UITextureSpaceTests, TodaysContradictedStampsStillRegister)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    // A known-wrong stamp: an _SRGB frame declared HdrLinear. No production
    // site utters this pair any more (#767 P1b), but the policy still guards
    // any future wrong stamp — it warns, and must not refuse, because a
    // refusal would blank real UI.
    UIManager ui(dev.get());
    ui.SetExternalTextureRG(kName, 128, 64, UITextureSpace::HdrLinear(),
                            TextureFormat::RGBA8_SRGB);

    const std::vector<std::string> pending = ui.ConsumeUnresolvedExternalTextureRequests();
    EXPECT_EQ(std::find(pending.begin(), pending.end(), kName), pending.end())
        << "P1a must stay byte-identical: a known-wrong stamp warns, never refuses";
}

TEST(UITextureSpaceTests, ValidStampRegistersSilently)
{
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    UIManager ui(dev.get());
    ui.SetExternalTextureRG(kName, 128, 64, UITextureSpace::HdrLinear(),
                            TextureFormat::R16G16B16A16_FLOAT);

    const std::vector<std::string> pending = ui.ConsumeUnresolvedExternalTextureRequests();
    EXPECT_EQ(std::find(pending.begin(), pending.end(), kName), pending.end());
}

// The space -> shader-bit translation, observed on emitted primitives. Only
// HdrLinear composites straight (kPrimHdrTextureBit set); both SDR-referred
// spaces ride the [0,1]-clamp + HDR-UI-lift arm, which is what makes an
// SDR-rendered thumbnail follow the UI white under HDR output (#767 P1b).
// The source space reaches the shader as two mutually exclusive bits, and this
// pins all three states on REAL emitted primitives: HDR-linear takes the straight
// linear arm, a finalized view takes the identity sample adapter, and ordinary
// SDR content takes neither. The identity arm is the load-bearing one — with its
// bit clear, the encoded blend target would apply the sRGB curve to values that
// already carry it and darken the whole viewport.
TEST(UITextureSpaceTests, SourceSpaceSelectsExactlyOneShaderArm)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(
        1.0f,
        "<uielement id='root'><uielement id='thumb' /></uielement>",
        "#root { display: flex; width: 256px; height: 128px; }\n"
        "#thumb { width: 128px; height: 64px; background-image: engine(test_space_src); }\n");
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const auto flagsFor = [&fx](UITextureSpace space) -> uint32_t
    {
        fx.Manager().SetExternalTextureRG(kName, 128, 64, space,
                                          TextureFormat::R16G16B16A16_FLOAT);
        fx.Settle();
        const auto prims = fx.Primitives("thumb", UI::PrimitiveMode::Textured);
        EXPECT_EQ(prims.size(), 1u);
        return prims.empty() ? 0u : prims.front().ModeAndFlags;
    };

    const uint32_t hdr = flagsFor(UITextureSpace::HdrLinear());
    EXPECT_NE(hdr & UI::kPrimHdrTextureBit, 0u);
    EXPECT_EQ(hdr & UI::kPrimEncodedSourceBit, 0u);

    const uint32_t finalized = flagsFor(UITextureSpace::SdrFinalized());
    EXPECT_NE(finalized & UI::kPrimEncodedSourceBit, 0u)
        << "a finalized view did not reach the shader's identity sample adapter — the encoded "
           "blend target would apply the sRGB curve a second time";
    EXPECT_EQ(finalized & UI::kPrimHdrTextureBit, 0u);

    const uint32_t sdr = flagsFor(UITextureSpace::DisplayLinearSdr());
    EXPECT_EQ(sdr & (UI::kPrimHdrTextureBit | UI::kPrimEncodedSourceBit), 0u);
}

// Which spaces carry the transfer curve at rest — the one question a readback
// converter may ask, and the switch that decides whether a capture encodes or
// merely requantizes.
TEST(UITextureSpaceTests, FinalizedAndAuthoredAreEncodedAtRest)
{
    EXPECT_TRUE(UI::IsEncodedAtRest(UITextureSpace::SrgbAuthored()));
    EXPECT_TRUE(UI::IsEncodedAtRest(UITextureSpace::SdrFinalized()));
    EXPECT_FALSE(UI::IsEncodedAtRest(UITextureSpace::DisplayLinearSdr()));
    EXPECT_FALSE(UI::IsEncodedAtRest(UITextureSpace::HdrLinear()));

    // Names are read out of diagnostics and capture metadata; a blank one is a
    // missing switch arm.
    EXPECT_STREQ(UI::ToString(UITextureSpace::SdrFinalized()), "SdrFinalized");
}

// The space bits live in bit 11, which the gradient field used to span. Every
// GradientMode must therefore still round-trip with those bits set — a regression
// here would silently reinterpret a colour-picker wheel's gradient mode, or lose
// the identity sample adapter on a finalized view.
TEST(UITextureSpaceTests, SpaceBitsDoNotCollideWithTheGradientField)
{
    for (const UI::GradientMode mode :
         {UI::GradientMode::None, UI::GradientMode::Vertical, UI::GradientMode::Horizontal,
          UI::GradientMode::FourCorner, UI::GradientMode::PolarHSV, UI::GradientMode::HueVertical,
          UI::GradientMode::PolarHSVGrading})
    {
        UI::UIPrimitive prim{};
        prim.ModeAndFlags = UI::MakeFlags(UI::PrimitiveMode::Textured, mode, 1234);
        ASSERT_EQ(UI::GetGradient(prim.ModeAndFlags), mode);

        for (const uint32_t bits :
             {0u, UI::kPrimHdrTextureBit, UI::kPrimEncodedSourceBit})
        {
            UI::SetTextureSourceSpaceBits(prim, bits);
            EXPECT_EQ(UI::GetGradient(prim.ModeAndFlags), mode)
                << "space bits disturbed GradientMode " << static_cast<int>(mode);
            EXPECT_EQ(UI::GetMode(prim.ModeAndFlags), UI::PrimitiveMode::Textured);
            EXPECT_EQ(UI::GetClipIndex(prim.ModeAndFlags), 1234);
            EXPECT_EQ(prim.ModeAndFlags & (UI::kPrimHdrTextureBit | UI::kPrimEncodedSourceBit),
                      bits)
                << "the two space bits are not mutually exclusive";
        }
    }
}
