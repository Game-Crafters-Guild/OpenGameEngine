// What reaches the GPU when CSS says rgba(255, 255, 255, 0.08) over #272727 —
// the hairline-overlay pair the editor's panel chrome is built from.
//
// The COMPOSITE is deliberately not asserted here. The blend runs in the
// shader, in the blend space of the pipeline variant the declared target
// space selected (Shaders/UI/ui_sdf_common.glsl `uiPaintColor`: raw bytes on
// an EncodedSrgb attachment, decoded linear on every other space).
// Encoded-space and linear-space source-over give visibly different answers
// for this pair — 56 against 88 out of 255 — and which one renders IS
// observed, by UIEncodedBlendParityTests, from an executed GPU readback
// referenced against Chrome's measured table. Transcribing either formula
// into C++ here would produce a test that only ever agrees with itself,
// which is exactly the failure mode this suite exists to avoid.
//
// What is pinned instead is everything upstream of that boundary: the two
// operands the shader is handed, through the cascade and through the emit,
// exactly as authored — including the quantisation the 8-bit path imposes on
// the authored alpha, and the channel order, which needs a chromatic specimen
// of its own because the editor's chrome pair is achromatic.

#include "IsolatedUIFixture.h"

#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using GameEngine::UI::PrimitiveMode;
using GameEngine::UI::UIPrimitive;
using GameEngine::UITesting::IsolatedUIFixture;

namespace
{

constexpr char kXml[] = R"(<uielement id="root">
  <uielement id="backdrop">
    <uielement id="overlay"/>
  </uielement>
  <uielement id="chromaBackdrop">
    <uielement id="chromaOverlay"/>
  </uielement>
</uielement>)";

constexpr char kCss[] = R"(
#root { display: flex; flex-direction: column; width: 400px; height: 300px; }
#backdrop { width: 200px; height: 100px; background-color: #272727; }
#overlay { width: 200px; height: 100px; background-color: rgba(255, 255, 255, 0.08); }
#chromaBackdrop { width: 200px; height: 100px; background-color: #123456; }
#chromaOverlay { width: 200px; height: 100px; background-color: rgba(255, 128, 0, 0.2); }
)";

// UIPrimitive.h: "Colors are packed RGBA8 ... R in low byte, A in high byte
// (matches GLSL unpackUnorm4x8)." Unpacked here from that stated layout rather
// than through the engine's own pack helper.
//
// Unpacking independently is not by itself a test of the layout, and the
// editor-chrome pair above cannot be one: #272727 and white are both
// achromatic, so they pack to the SAME uint32 under the documented channel
// order and under any permutation of R, G and B. Those two tests pin the
// values and the alpha byte's position; they are blind to channel order. The
// chromatic specimen at the bottom of this file is what closes that — every
// byte of both its operands differs from every other byte, so a swap of any
// two channels fails it.
struct Rgba8
{
    uint32_t R = 0;
    uint32_t G = 0;
    uint32_t B = 0;
    uint32_t A = 0;
};

Rgba8 UnpackPrimitiveColor(uint32_t packed)
{
    return {packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu, (packed >> 24) & 0xFFu};
}

// The backdrop's opaque grey and the overlay's translucent white, as the
// display-encoded bytes the shader decodes.
constexpr uint32_t kBackdropChannel = 0x27u; // 39
constexpr uint32_t kOverlayChannel = 0xFFu;  // 255

// round(0.08 * 255) = 20. The authored 0.08 is not representable in 8 bits, so
// what actually blends is 20/255 = 0.0784.
constexpr uint32_t kOverlayAlpha = 20u;

// The channel-order specimen: #123456 under rgba(255, 128, 0, 0.2). Eight
// distinct bytes across the two operands, so no permutation of the channels
// reproduces the expected values. The alpha is chosen to be unambiguous — 0.2
// * 255 = 51.0000008, which is 51 whether the conversion rounds or truncates —
// because this pair is about channel ORDER and the quantisation question is
// asked separately below.
constexpr uint32_t kChromaBackdropR = 0x12u;
constexpr uint32_t kChromaBackdropG = 0x34u;
constexpr uint32_t kChromaBackdropB = 0x56u;
constexpr uint32_t kChromaOverlayR = 0xFFu;
constexpr uint32_t kChromaOverlayG = 0x80u;
constexpr uint32_t kChromaOverlayB = 0x00u;
constexpr uint32_t kChromaOverlayA = 51u;

// Returns the element's first Rect-mode primitive, which is where a plain
// background-color lands.
const UIPrimitive* FirstRect(const std::vector<UIPrimitive>& prims)
{
    return prims.empty() ? nullptr : &prims.front();
}

} // namespace

// The cascade's answer, before any emit. VisualStyle stores colours as engine
// ARGB (0xAARRGGBB).
TEST(CssColorToPrimitive, CascadeResolvesBothOperandsAsAuthored)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const GameEngine::ResolvedStyle* backdrop = fx.Style("backdrop");
    const GameEngine::ResolvedStyle* overlay = fx.Style("overlay");
    ASSERT_NE(backdrop, nullptr);
    ASSERT_NE(overlay, nullptr);

    EXPECT_EQ(backdrop->Visual.BackgroundColor, 0xFF272727u);
    EXPECT_EQ(overlay->Visual.BackgroundColor,
              (kOverlayAlpha << 24) | 0x00FFFFFFu);
}

// The two operands as the shader receives them. Neither is pre-multiplied,
// pre-blended or otherwise altered on the way out of the cascade: the overlay
// still carries full-white RGB with a separate alpha, which is what makes the
// choice of blend space observable on screen in the first place.
TEST(CssColorToPrimitive, BothOperandsReachTheGpuUnblended)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const auto backdropRects = fx.Primitives("backdrop", PrimitiveMode::Rect);
    const auto overlayRects = fx.Primitives("overlay", PrimitiveMode::Rect);
    const UIPrimitive* backdrop = FirstRect(backdropRects);
    const UIPrimitive* overlay = FirstRect(overlayRects);
    ASSERT_NE(backdrop, nullptr) << "backdrop emitted no Rect primitive";
    ASSERT_NE(overlay, nullptr) << "overlay emitted no Rect primitive";

    const Rgba8 back = UnpackPrimitiveColor(backdrop->FillColor);
    EXPECT_EQ(back.R, kBackdropChannel);
    EXPECT_EQ(back.G, kBackdropChannel);
    EXPECT_EQ(back.B, kBackdropChannel);
    EXPECT_EQ(back.A, 255u);

    const Rgba8 over = UnpackPrimitiveColor(overlay->FillColor);
    EXPECT_EQ(over.R, kOverlayChannel);
    EXPECT_EQ(over.G, kOverlayChannel);
    EXPECT_EQ(over.B, kOverlayChannel);
    EXPECT_EQ(over.A, kOverlayAlpha);

    // Per-primitive opacity is a separate multiplier the shader applies on top
    // of the fill alpha; neither element sets `opacity`, so it must not be
    // silently carrying the CSS alpha as well.
    EXPECT_FLOAT_EQ(overlay->Opacity, 1.0f);
    EXPECT_FLOAT_EQ(backdrop->Opacity, 1.0f);

    // The overlay covers the backdrop exactly, so the pair the compositor sees
    // really is these two colours and nothing else.
    EXPECT_FLOAT_EQ(overlay->X, backdrop->X);
    EXPECT_FLOAT_EQ(overlay->Y, backdrop->Y);
    EXPECT_FLOAT_EQ(overlay->W, backdrop->W);
    EXPECT_FLOAT_EQ(overlay->H, backdrop->H);
}

// The authored alpha does not survive as authored, and the size of the gap
// matters for anyone reasoning about the result: 0.08 becomes 20/255 = 0.0784,
// a 2% relative loss of the overlay's strength before any blending happens.
TEST(CssColorToPrimitive, AuthoredAlphaIsQuantisedToTheNearestEighthBit)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    const auto overlayRects = fx.Primitives("overlay", PrimitiveMode::Rect);
    const UIPrimitive* overlay = FirstRect(overlayRects);
    ASSERT_NE(overlay, nullptr);

    const uint32_t alpha = UnpackPrimitiveColor(overlay->FillColor).A;
    EXPECT_EQ(alpha, kOverlayAlpha) << "0.08 * 255 = 20.4, rounded to nearest";

    // 0.0784, not 0.08 — and the direction is down, so the overlay is slightly
    // weaker than authored rather than stronger.
    const float effective = static_cast<float>(alpha) / 255.0f;
    EXPECT_NEAR(effective, 0.0784f, 0.0001f);
    EXPECT_LT(effective, 0.08f);
}

// Channel ORDER, which the achromatic pair above cannot see. A red authored in
// CSS has to arrive at the shader in the low byte and a blue in the third: swap
// any two of R, G and B anywhere between the parser and the emit and a specimen
// with eight distinct bytes says so, while #272727-under-white says nothing.
TEST(CssColorToPrimitive, ChromaticOperandsKeepTheirChannelOrderThroughTheEmit)
{
    IsolatedUIFixture fx;
    const bool built = fx.Build(1.0f, kXml, kCss);
    if (!fx.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fx.Diagnostic();

    // The cascade first: VisualStyle stores engine ARGB (0xAARRGGBB), a
    // different byte order from the primitive's, so an error in either mapping
    // is localised rather than cancelling out.
    const GameEngine::ResolvedStyle* backdropStyle = fx.Style("chromaBackdrop");
    const GameEngine::ResolvedStyle* overlayStyle = fx.Style("chromaOverlay");
    ASSERT_NE(backdropStyle, nullptr);
    ASSERT_NE(overlayStyle, nullptr);
    EXPECT_EQ(backdropStyle->Visual.BackgroundColor,
              0xFF000000u | (kChromaBackdropR << 16) | (kChromaBackdropG << 8) | kChromaBackdropB);
    EXPECT_EQ(overlayStyle->Visual.BackgroundColor,
              (kChromaOverlayA << 24) | (kChromaOverlayR << 16) | (kChromaOverlayG << 8) |
                  kChromaOverlayB);

    // Then the emit, in the primitive's own RGBA8 order.
    const auto backdropRects = fx.Primitives("chromaBackdrop", PrimitiveMode::Rect);
    const auto overlayRects = fx.Primitives("chromaOverlay", PrimitiveMode::Rect);
    const UIPrimitive* backdrop = FirstRect(backdropRects);
    const UIPrimitive* overlay = FirstRect(overlayRects);
    ASSERT_NE(backdrop, nullptr) << "chromaBackdrop emitted no Rect primitive";
    ASSERT_NE(overlay, nullptr) << "chromaOverlay emitted no Rect primitive";

    const Rgba8 back = UnpackPrimitiveColor(backdrop->FillColor);
    EXPECT_EQ(back.R, kChromaBackdropR) << "authored #123456: red is the low byte";
    EXPECT_EQ(back.G, kChromaBackdropG);
    EXPECT_EQ(back.B, kChromaBackdropB);
    EXPECT_EQ(back.A, 255u);

    const Rgba8 over = UnpackPrimitiveColor(overlay->FillColor);
    EXPECT_EQ(over.R, kChromaOverlayR) << "authored rgba(255, 128, 0, 0.2)";
    EXPECT_EQ(over.G, kChromaOverlayG);
    EXPECT_EQ(over.B, kChromaOverlayB);
    EXPECT_EQ(over.A, kChromaOverlayA);
}
