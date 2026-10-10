// Pins for the host-declared UI target space (#767 P2a / #784): the display
// conversion, the SDF shader outputEncoding translation, and the subpixel
// activation gate consuming the TARGET-derived encoding. Pure, static,
// device-free — same contract style as TextCoverageCorrectionTests.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "UI/UIManager.h"
#include "UI/UITargetSpace.h"
#include "UI/UITextureSpace.h"

using GameEngine::UIManager;
using GameEngine::Rendering::HdrOutputMode;
using GameEngine::UI::UITargetSpace;
using GameEngine::UI::UITextureSpace;

TEST(UITargetSpaceTests, ForDisplayMapsEveryOutputMode)
{
    EXPECT_EQ(UITargetSpace::ForDisplay(HdrOutputMode::Off), UITargetSpace::LinearSdr());
    // Auto is a REQUESTED mode; an active mode is always resolved. If it ever
    // leaks in, the SDR arm is the only sane reading.
    EXPECT_EQ(UITargetSpace::ForDisplay(HdrOutputMode::Auto), UITargetSpace::LinearSdr());
    EXPECT_EQ(UITargetSpace::ForDisplay(HdrOutputMode::HDR10_PQ), UITargetSpace::HdrPq());
    EXPECT_EQ(UITargetSpace::ForDisplay(HdrOutputMode::HDR10Plus), UITargetSpace::HdrPq());
    EXPECT_EQ(UITargetSpace::ForDisplay(HdrOutputMode::HLG), UITargetSpace::Hlg());
    EXPECT_EQ(UITargetSpace::ForDisplay(HdrOutputMode::ScRGB), UITargetSpace::ScRgb());
}

TEST(UITargetSpaceTests, OutputEncodingMatchesTheSdfShaderConvention)
{
    // SdfPushConstants convention: 0 = SDR linear, 1 = SDR encoded bytes,
    // 2 = HDR10 PQ, 3 = HLG, 4 = scRGB (ui_sdf_common.glsl). The lift arm is
    // outputEncoding >= 2.
    EXPECT_EQ(UIManager::ResolveOutputEncoding(UITargetSpace::LinearSdr()), 0);
    EXPECT_EQ(UIManager::ResolveOutputEncoding(UITargetSpace::EncodedSrgb()), 1);
    EXPECT_EQ(UIManager::ResolveOutputEncoding(UITargetSpace::HdrPq()), 2);
    EXPECT_EQ(UIManager::ResolveOutputEncoding(UITargetSpace::Hlg()), 3);
    EXPECT_EQ(UIManager::ResolveOutputEncoding(UITargetSpace::ScRgb()), 4);
}

TEST(UITargetSpaceTests, SubpixelGateFollowsTheDeclaredTarget)
{
    // The activation gate consumes the TARGET-derived encoding (#784): both
    // SDR attachments keep subpixel eligible no matter what display is
    // connected — per-channel source-over on encoded bytes is precisely the
    // ClearType model, so the encoded target is subpixel-native — and every
    // HDR-flavoured attachment resolves grayscale.
    for (UITargetSpace space : {UITargetSpace::LinearSdr(), UITargetSpace::EncodedSrgb()})
    {
        EXPECT_TRUE(UIManager::ResolveTextSubpixelActive(
            /*enabled=*/true, UIManager::ResolveOutputEncoding(space),
            /*dualSourceBlending=*/true, /*pipelineAvailable=*/true))
            << ToString(space);
    }
    for (UITargetSpace space :
         {UITargetSpace::HdrPq(), UITargetSpace::Hlg(), UITargetSpace::ScRgb()})
    {
        EXPECT_FALSE(UIManager::ResolveTextSubpixelActive(
            /*enabled=*/true, UIManager::ResolveOutputEncoding(space),
            /*dualSourceBlending=*/true, /*pipelineAvailable=*/true))
            << ToString(space);
    }
}

TEST(UITargetSpaceTests, ForPipelineOutputCarriesTheProducersSpace)
{
    // An SDR-stamped pipeline output composites as SDR no matter what the
    // display is doing — this is #784's fix stated as a rule.
    for (HdrOutputMode mode : {HdrOutputMode::Off, HdrOutputMode::HDR10_PQ, HdrOutputMode::HLG,
                               HdrOutputMode::ScRGB})
    {
        EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::DisplayLinearSdr(), mode),
                  UITargetSpace::LinearSdr());
    }
    // An HDR-stamped output takes its flavour from the mode the pipeline's
    // tonemap keyed on.
    EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::HdrLinear(), HdrOutputMode::HDR10_PQ),
              UITargetSpace::HdrPq());
    EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::HdrLinear(), HdrOutputMode::HLG),
              UITargetSpace::Hlg());
    EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::HdrLinear(), HdrOutputMode::ScRGB),
              UITargetSpace::ScRgb());
    // Contradictions (HdrLinear with no HDR mode active; an encoded stamp)
    // resolve LinearSdr rather than inventing a lift.
    EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::HdrLinear(), HdrOutputMode::Off),
              UITargetSpace::LinearSdr());
    EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::SrgbAuthored(), HdrOutputMode::Off),
              UITargetSpace::LinearSdr());
    // A finalized view is the one producer stamp that DICTATES the blend space:
    // its values already carry the output curve, so only the encoded target
    // leaves them where the finalize put them. Blending it linear would read
    // code values as light and wash the viewport out.
    EXPECT_EQ(UITargetSpace::ForPipelineOutput(UITextureSpace::SdrFinalized(), HdrOutputMode::Off),
              UITargetSpace::EncodedSrgb());
}

TEST(UITargetSpaceTests, ToStringNamesEverySpace)
{
    EXPECT_STREQ(ToString(UITargetSpace::LinearSdr()), "LinearSdr");
    EXPECT_STREQ(ToString(UITargetSpace::EncodedSrgb()), "EncodedSrgb");
    EXPECT_STREQ(ToString(UITargetSpace::HdrPq()), "HdrPq");
    EXPECT_STREQ(ToString(UITargetSpace::Hlg()), "Hlg");
    EXPECT_STREQ(ToString(UITargetSpace::ScRgb()), "ScRgb");
}
