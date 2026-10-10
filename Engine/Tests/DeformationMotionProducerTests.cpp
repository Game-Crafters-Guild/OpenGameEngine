// The deforming-motion producer's decisions, each on its own instrument.
//
// The producer makes four decisions before it records anything, and every one
// of them is a correctness gate rather than a policy knob:
//   - which arm runs, given three independent switches that share one target;
//   - whether this frame's endpoint pair can be differenced at all, which is
//     the discontinuity policy;
//   - which materials are in its lane, which is also which instances the mover
//     lane must stop writing;
//   - which keywords a draw segment composes with, where two of the engine's
//     fragment shapes are mutually exclusive by name.
//
// None of these needs a device: they are the producer's arithmetic and its
// predicates. The rendered halves are elsewhere — the payload's reconstruction
// in DeformationMotionPayloadTests, the composed variant's shape in
// ShaderVariantKeyTests.

#include <gtest/gtest.h>

#include "Engine/Rendering/DeformationMotionParams.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialDeformationClassify.h"
#include "Engine/Rendering/ViewTemporalHistory.h"
#include "AssetCore/GUID.h"

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using GameEngine::Rendering::MaterialKeyword;

namespace
{
Material MakeMaterial(MaterialKeyword keywords, MaterialAlphaMode alphaMode)
{
    Material mat = Material::TestFactory::Create(GUID::Generate(), "DeformationProbe", 32u);
    Rendering::ShaderVariantKey key{};
    key.materialKeywords = keywords;
    Material::TestFactory::SetVariantKey(mat, key);
    Material::TestFactory::SetAlphaMode(mat, alphaMode);
    return mat;
}

DeformationMotionRaster MakeRaster()
{
    DeformationMotionRaster raster{};
    raster.Viewport = Mathematics::Rect{12.0f, 5.0f, 640.0f, 360.0f};
    raster.NdcJitterX = 0.25f;
    raster.NdcJitterY = -0.125f;
    return raster;
}
} // namespace

// Three switches, one target. Exactly one arm may run.
TEST(DeformationMotionProducer, EachSwitchSelectsItsOwnArmAndNoneIsTheDefault)
{
    EXPECT_EQ(ResolveDeformationMotionArm(false, false, false), DeformationMotionArm::None);
    EXPECT_EQ(ResolveDeformationMotionArm(true, false, false),
              DeformationMotionArm::ColorPassTarget);
    EXPECT_EQ(ResolveDeformationMotionArm(false, true, false), DeformationMotionArm::PrepassTarget);
    EXPECT_EQ(ResolveDeformationMotionArm(false, false, true), DeformationMotionArm::SeparatePass);
}

// Two producers writing one target would write every deforming pixel twice and
// let record order pick the survivor. The refusal is silence, not a coin toss.
TEST(DeformationMotionProducer, MoreThanOneEnabledArmIsRefused)
{
    EXPECT_EQ(ResolveDeformationMotionArm(true, true, false), DeformationMotionArm::None);
    EXPECT_EQ(ResolveDeformationMotionArm(true, false, true), DeformationMotionArm::None);
    EXPECT_EQ(ResolveDeformationMotionArm(false, true, true), DeformationMotionArm::None);
    EXPECT_EQ(ResolveDeformationMotionArm(true, true, true), DeformationMotionArm::None);
}

// The block the producer uploads, member by member, against values the test
// computes itself.
TEST(DeformationMotionProducer, TheBlockCarriesThePreviousEndpointAndTheRasterItWasBuiltFor)
{
    ViewTemporalSample previous{};
    previous.DeformationTimeSeconds = 41.5f;
    previous.DeformationScrollSeconds = 7.25f;
    previous.DeformationOrigin = 128.0;
    for (int i = 0; i < 16; ++i)
        previous.Camera.viewProj[i] = static_cast<float>(i) + 0.5f;

    DeformationMotionEndpoint endpoint{};
    endpoint.Previous = &previous;
    endpoint.PreviousValid = true;
    endpoint.CurrentOrigin = 128.0;

    const DeformationMotionRaster raster = MakeRaster();
    DeformationMotionParamsGPU params{};
    ASSERT_TRUE(BuildDeformationMotionParams(endpoint, raster, params));

    for (int i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(params.PrevViewProj[i], static_cast<float>(i) + 0.5f);
    EXPECT_FLOAT_EQ(params.PrevTimeParams[0], 41.5f);
    EXPECT_FLOAT_EQ(params.PrevTimeParams[1], 7.25f);
    // The rect is the one handed to SetViewport: top-left corner and size, in
    // framebuffer pixels, before the backend's negative-height flip.
    EXPECT_FLOAT_EQ(params.ViewportRect[0], 12.0f);
    EXPECT_FLOAT_EQ(params.ViewportRect[1], 5.0f);
    EXPECT_FLOAT_EQ(params.ViewportRect[2], 640.0f);
    EXPECT_FLOAT_EQ(params.ViewportRect[3], 360.0f);
    // Normalized-device-coordinate offsets become viewport-UV offsets by
    // halving, and y inverts: the jitter is ADDED in Y-up coordinates and
    // consumed in a Y-down UV. The rendered proof of the convention is
    // DeformationMotionPayload.JitterIsTakenBackOffInBothSigns; this row pins
    // the arithmetic that feeds it.
    EXPECT_FLOAT_EQ(params.JitterUv[0], 0.125f);
    EXPECT_FLOAT_EQ(params.JitterUv[1], 0.0625f);
}

// The discontinuity policy: no differenceable pair means no draw, so the
// surface keeps the clear sentinel. Never a zero vector, which would claim the
// surface did not move.
TEST(DeformationMotionProducer, AnUndifferenceableEndpointPairProducesNoBlock)
{
    const DeformationMotionRaster raster = MakeRaster();
    DeformationMotionParamsGPU params{};

    ViewTemporalSample previous{};
    previous.DeformationOrigin = 64.0;

    DeformationMotionEndpoint noHistory{};
    noHistory.Previous = nullptr;
    noHistory.PreviousValid = false;
    noHistory.CurrentOrigin = 64.0;
    EXPECT_FALSE(BuildDeformationMotionParams(noHistory, raster, params))
        << "a view's first frame has no previous rendered frame";

    DeformationMotionEndpoint standIn{};
    standIn.Previous = &previous;
    standIn.PreviousValid = false;
    standIn.CurrentOrigin = 64.0;
    EXPECT_FALSE(BuildDeformationMotionParams(standIn, raster, params))
        << "the history reports the current sample as a stand-in, not a previous frame";

    DeformationMotionEndpoint reanchored{};
    reanchored.Previous = &previous;
    reanchored.PreviousValid = true;
    reanchored.CurrentOrigin = 4096.0 + 64.0;
    EXPECT_FALSE(BuildDeformationMotionParams(reanchored, raster, params))
        << "two samples formed against different origins are not differenceable";

    DeformationMotionEndpoint valid{};
    valid.Previous = &previous;
    valid.PreviousValid = true;
    valid.CurrentOrigin = 64.0;
    DeformationMotionRaster empty = raster;
    empty.Viewport.Width = 0.0f;
    EXPECT_FALSE(BuildDeformationMotionParams(valid, empty, params))
        << "an empty viewport has no pixels to measure gl_FragCoord against";

    EXPECT_TRUE(BuildDeformationMotionParams(valid, raster, params))
        << "the control: the same endpoint with a real viewport DOES build";
}

// Lane membership. The three refusals mirror the adapter's own.
TEST(DeformationMotionProducer, OnlyTheSimpleModifierFormOnANonBlendedMaterialIsInTheLane)
{
    const Material simple = MakeMaterial(MaterialKeyword::HasVertexMod, MaterialAlphaMode::Opaque);
    EXPECT_TRUE(SupportsDeformationMotion(simple));

    const Material masked = MakeMaterial(
        MaterialKeyword::HasVertexMod | MaterialKeyword::AlphaTest, MaterialAlphaMode::Mask);
    EXPECT_TRUE(SupportsDeformationMotion(masked))
        << "a masked deformer runs the prepass's own cutoff in the motion fragment";

    const Material rigid = MakeMaterial(MaterialKeyword::None, MaterialAlphaMode::Opaque);
    EXPECT_FALSE(SupportsDeformationMotion(rigid))
        << "a rigid surface's motion is the mover lane's transform delta";

    const Material extended =
        MakeMaterial(MaterialKeyword::HasVertexOutputMod, MaterialAlphaMode::Opaque);
    EXPECT_FALSE(SupportsDeformationMotion(extended))
        << "the extended output form has no prevTransform to pair its previous endpoint with";

    const Material procedural = MakeMaterial(MaterialKeyword::HasVertexMod |
                                                 MaterialKeyword::HasVertexOutputMod |
                                                 MaterialKeyword::CustomVertexShader,
                                             MaterialAlphaMode::Opaque);
    EXPECT_FALSE(SupportsDeformationMotion(procedural))
        << "procedural geometry has no vertex buffer and always takes the extended form";

    const Material blended = MakeMaterial(MaterialKeyword::HasVertexMod, MaterialAlphaMode::Blend);
    EXPECT_FALSE(SupportsDeformationMotion(blended))
        << "a blended surface writes no prepass depth for the motion draw to agree with";
}

// The keyword the variant cache reads is the KEY, and two of the engine's
// fragment shapes claim the same output. A motion tail must dither without
// asking for the coverage shape.
TEST(DeformationMotionProducer, AMotionTailDithersWithoutTheCoverageFragmentKeyword)
{
    const MaterialKeyword motionHead =
        MaterialKeyword::Instanced | MaterialKeyword::MotionVectors;
    const auto tail = ChooseDepthSegmentPipeline(motionHead, /*headUsesSharedDepth=*/false,
                                                 /*headComposesFragment=*/true,
                                                 /*crossfading=*/true);
    EXPECT_TRUE(HasKeyword(tail.Keywords, MaterialKeyword::MotionVectors));
    EXPECT_TRUE(HasKeyword(tail.Keywords, MaterialKeyword::LodCrossfade));
    EXPECT_FALSE(HasKeyword(tail.Keywords, MaterialKeyword::DepthOnlyFragment))
        << "the adapter refuses motion beside the depth-only fragment shape by name";
    EXPECT_FALSE(tail.UseSharedDepth);
    EXPECT_TRUE(tail.ComposesFragment);

    // The depth pass's own tail is unchanged: it still asks for the coverage
    // shape, because that is the shape it draws with.
    const MaterialKeyword depthHead = MaterialKeyword::Instanced;
    const auto depthTail = ChooseDepthSegmentPipeline(depthHead, /*headUsesSharedDepth=*/true,
                                                      /*headComposesFragment=*/false,
                                                      /*crossfading=*/true);
    EXPECT_TRUE(HasKeyword(depthTail.Keywords, MaterialKeyword::DepthOnlyFragment));
    EXPECT_TRUE(HasKeyword(depthTail.Keywords, MaterialKeyword::LodCrossfade));

    // And a non-fading motion head composes the key it was handed, unchanged —
    // nothing from the shading path joins it on the way.
    const auto head = ChooseDepthSegmentPipeline(motionHead, /*headUsesSharedDepth=*/false,
                                                 /*headComposesFragment=*/true,
                                                 /*crossfading=*/false);
    EXPECT_EQ(head.Keywords, motionHead);
    EXPECT_FALSE(HasKeyword(head.Keywords, MaterialKeyword::SSSRNormalRoughness));
}
