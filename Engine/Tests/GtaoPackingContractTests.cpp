// Contract: GTAO packs rgb = world-space bent normal, a = visibility (gtao.comp),
// and the world-pass consumer reads visibility from .a (gtao_consume.glsl). .r is
// a signed axis: a consumer that read it as a scale would invert its result
// whenever the bent normal points toward -X.

#include <gtest/gtest.h>

#include "SssrShaderSource.h"

using GE::Tests::ReadSssrShaderSource;

TEST(GtaoPackingContract, ProducerAndWorldConsumerAgreeOnVisibilityAlpha)
{
    const std::string gtao = ReadSssrShaderSource("AmbientOcclusion/gtao.comp");
    ASSERT_FALSE(gtao.empty()) << "AmbientOcclusion/gtao.comp not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(gtao.find("imageStore(uAO, pix, vec4(bent_world, visibility))"), std::string::npos)
        << "GTAO packing is rgb = bent normal, a = visibility; a change here must "
           "move the consumer in lockstep";

    const std::string consume = ReadSssrShaderSource("Includes/gtao_consume.glsl");
    ASSERT_FALSE(consume.empty()) << "gtao_consume.glsl not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(consume.find("so.ao = min(so.ao, g.a)"), std::string::npos)
        << "the world-pass consumer reads visibility from .a";
}
