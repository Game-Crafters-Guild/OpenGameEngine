// Dither-basis selector contract. The per-pixel Vogel rotation can be keyed on
// screen position (the shipped behaviour: the noise field is nailed to the
// screen and geometry slides under it as the camera moves) or on the shadow
// coordinate (welded to the shadow lattice, which the centre texel snap welds
// to the world).
//
// Neither is universally better and the trade is range-dependent, so the point
// of this slice is the toggle, not a winner. What these tests pin is the part
// that is NOT a judgement call:
//   * Screen stays the default, so a project that never touches the setting
//     renders exactly what it rendered before;
//   * the UBO has exactly the size of the block shadow_sampling.glsl declares,
//     because the GLSL mirrors ShadowDataGPU by hand and a size the block does
//     not match is silent garbage rather than a compile error.

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include <gtest/gtest.h>

using GameEngine::Engine::Renderer::ShadowDataGPU;
using GameEngine::Engine::Renderer::ShadowDitherBasis;
using GameEngine::Engine::Renderer::ShadowMapRenderFeature;

TEST(ShadowDitherBasis, UboMatchesTheShaderBlock)
{
    // 464 through shadowFilterParams, then the terrain's clearance map: four
    // vec4s of grid and one uvec4 of sources (terrainShadowGrid0..3,
    // terrainShadowSource). The struct is alignas(16), so a member that is not
    // a 16-byte multiple would land here as an unexpected size rather than as
    // padding nobody notices.
    EXPECT_EQ(sizeof(ShadowDataGPU), 544u);
}

// The dark-ship gate. Defaulting anywhere else would change the look of every
// existing scene on upgrade without anyone asking for it.
TEST(ShadowDitherBasis, DefaultsToScreen)
{
    ShadowMapRenderFeature feature;
    EXPECT_EQ(feature.GetDitherBasis(), ShadowDitherBasis::Screen);
}

TEST(ShadowDitherBasis, SetterRoundTrips)
{
    ShadowMapRenderFeature feature;

    feature.SetDitherBasis(ShadowDitherBasis::ShadowSpace);
    EXPECT_EQ(feature.GetDitherBasis(), ShadowDitherBasis::ShadowSpace);

    feature.SetDitherBasis(ShadowDitherBasis::Screen);
    EXPECT_EQ(feature.GetDitherBasis(), ShadowDitherBasis::Screen);
}

// The shader reads the basis as a float out of ge_shadowFilterParams.x and
// compares it against 0.5, so the enum's numeric values are part of the GPU
// contract rather than an implementation detail free to be reordered.
TEST(ShadowDitherBasis, EnumValuesAreTheGpuContract)
{
    EXPECT_EQ(static_cast<int>(ShadowDitherBasis::Screen), 0);
    EXPECT_EQ(static_cast<int>(ShadowDitherBasis::ShadowSpace), 1);
}
