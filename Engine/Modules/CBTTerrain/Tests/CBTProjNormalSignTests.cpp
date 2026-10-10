// THE SIGN of the terrain's triplanar normal mapping, executed rather than transcribed.
//
// The gap this closes. CBTLayout.TriplanarNormalSwizzlesFollowTheProjectionAxesTheUVsDeclare pins
// how each projection's tangent contributions are ROUTED to world axes, and says in its own header
// that it does not pin the SIGN of tangent Y: it reads `t.xy` and `t.z` as opaque tokens, so a
// `* vec2(1.0, -1.0)` written inside the decode never reaches its patterns. A globally inverted
// green channel is self-consistent across all three projections — it renders every authored bump as
// a dent and nothing about the silhouette looks wrong, only the lighting is inverted. Until this
// suite the convention was held by a runtime A/B against the standard_pbr mesh path alone,
// which no CI run repeats.
//
// So these tests EXECUTE the shipped decode. Both halves are lifted verbatim at build time by
// ExtractShaderBlock.cmake — GE_DecodeTangentNormal from Includes/surface_io.glsl (the same
// expression standard_pbr decodes through, which is what makes the A/B's two arms comparable) and
// CBT_ProjNormalFromSample from cbt_surface.glsl — and compiled here through GlslShim.h. The
// texture tap stays outside both blocks, which is exactly what leaves them pure enough to run on
// the host; a hand-written C++ mirror would agree forever while the shader drifted.
//
// THE VERDICT THESE ENCODE, matching the A/B record: on the Y-facing (XZ) projection — flat
// ground's single tap — a normal-map texel with green above 0.5 tilts the world normal toward
// +world-Z. The two steps of that claim are checked separately below, because they live in
// different files and fail independently: the decode+strength produces +tangent-Y (executed), and
// CBT_MaterialNormal routes tangent Y to world Z on that projection (derived from the shader
// source, the same way the axes test derives its expectations).

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <fstream>
#include <iterator>
#include <regex>
#include <string>

namespace
{
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::vec4;

namespace Shader
{
using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::vec2;
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::vec4;
#include "CBTTangentNormalDecodeExtracted.h"
#include "CBTProjNormalFromSampleExtracted.h"
} // namespace Shader

// A normal-map texel as sampled: unsigned [0,1], 0.5 being flat on that axis.
constexpr float kFlat = 0.5f;
constexpr float kFullStrength = 1.0f;

std::string SlurpShader(const std::string& absolutePath)
{
    std::ifstream in(absolutePath, std::ios::binary);
    EXPECT_TRUE(in.good()) << "could not open " << absolutePath;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string StripLineComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();)
    {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            while (i < src.size() && src[i] != '\n')
                ++i;
        }
        else
        {
            out += src[i];
            ++i;
        }
    }
    return out;
}

} // namespace

// Flat in, flat out: a texel of exactly 0.5 on both axes must decode to the tangent-space up
// vector. This is the control the sign tests below lean on — if the neutral texel did not land on
// (0, 0, 1), "greater than neutral" would not mean "positive".
TEST(CBTProjNormalSign, ANeutralTexelDecodesToTheTangentUpVector)
{
    const vec3 n = Shader::CBT_ProjNormalFromSample(vec3(kFlat, kFlat, kFlat), kFullStrength);
    EXPECT_NEAR(n.x, 0.0f, 1e-6f);
    EXPECT_NEAR(n.y, 0.0f, 1e-6f);
    EXPECT_NEAR(n.z, 1.0f, 1e-6f);
}

// THE SIGN. Green above neutral is +tangent Y. This is the assertion a `* vec2(1.0, -1.0)` written
// into the decode reds, and the one no test made before this file existed.
TEST(CBTProjNormalSign, GreenAboveNeutralIsPositiveTangentY)
{
    const vec3 up = Shader::CBT_ProjNormalFromSample(vec3(kFlat, 0.75f, kFlat), kFullStrength);
    EXPECT_GT(up.y, 0.0f) << "a normal map's green channel above 0.5 must decode to +tangent Y; "
                             "inverted, every authored bump on the terrain renders as a dent";
    EXPECT_NEAR(up.x, 0.0f, 1e-6f) << "green must not disturb tangent X";

    // And the mirror, so the test cannot pass on a decode that clamps everything positive.
    const vec3 down = Shader::CBT_ProjNormalFromSample(vec3(kFlat, 0.25f, kFlat), kFullStrength);
    EXPECT_LT(down.y, 0.0f);
    EXPECT_NEAR(up.y, -down.y, 1e-6f) << "the decode must be symmetric about the neutral texel";
}

// Red is the other tangent axis, and it must carry its own sign independently — a decode that
// derived one axis from the other would satisfy the green test alone.
TEST(CBTProjNormalSign, RedAboveNeutralIsPositiveTangentX)
{
    const vec3 right = Shader::CBT_ProjNormalFromSample(vec3(0.75f, kFlat, kFlat), kFullStrength);
    EXPECT_GT(right.x, 0.0f);
    EXPECT_NEAR(right.y, 0.0f, 1e-6f) << "red must not disturb tangent Y";
}

// NormalStrength scales the tangent XY and leaves Z as decoded — that asymmetry is what makes the
// knob a lean rather than a rescale, and 0 must collapse to the surface normal exactly.
TEST(CBTProjNormalSign, NormalStrengthScalesTangentXYAndNotZ)
{
    const vec3 full = Shader::CBT_ProjNormalFromSample(vec3(0.75f, 0.75f, kFlat), kFullStrength);
    const vec3 half = Shader::CBT_ProjNormalFromSample(vec3(0.75f, 0.75f, kFlat), 0.5f);
    EXPECT_NEAR(half.x, full.x * 0.5f, 1e-6f);
    EXPECT_NEAR(half.y, full.y * 0.5f, 1e-6f);
    EXPECT_NEAR(half.z, full.z, 1e-6f) << "strength must not touch the reconstructed Z";
    // Strength must not silently flip either axis on the way down.
    EXPECT_GT(half.x, 0.0f);
    EXPECT_GT(half.y, 0.0f);

    const vec3 off = Shader::CBT_ProjNormalFromSample(vec3(0.75f, 0.75f, kFlat), 0.0f);
    EXPECT_NEAR(off.x, 0.0f, 1e-6f);
    EXPECT_NEAR(off.y, 0.0f, 1e-6f);
    EXPECT_GT(off.z, 0.0f) << "strength 0 must leave the tangent up vector, not a zero vector";
}

// Z is reconstructed on the +hemisphere for every input, including a steep texel and a two-channel
// BC5 payload whose blue samples as 0. A negative Z would point the detail normal into the surface.
TEST(CBTProjNormalSign, ReconstructedZStaysOnThePositiveHemisphere)
{
    for (const float g : {0.0f, 0.25f, kFlat, 0.75f, 1.0f})
    {
        const vec3 n = Shader::CBT_ProjNormalFromSample(vec3(kFlat, g, 0.0f), kFullStrength);
        EXPECT_GE(n.z, 0.0f) << "green " << g << " reconstructed a negative tangent Z";
    }
}

// The second half of the world-space claim: the Y-facing projection must route the tangent V
// contribution to world Z. DERIVED from the shader rather than transcribed — the swizzle letter
// that lands on world Z has to be 'y', the pre-swizzle slot holding the V contribution.
//
// Composed with GreenAboveNeutralIsPositiveTangentY above, this is the A/B record's verdict:
// green > 0.5 on flat ground tilts the world normal toward +world-Z.
TEST(CBTProjNormalSign, TheYFacingProjectionRoutesTangentYToWorldZ)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string src =
        StripLineComments(SlurpShader(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    ASSERT_FALSE(src.empty()) << "cannot read cbt_surface.glsl";

    // The Y-facing block: uv.XZ spans world X and Z, so world Y is this projection's own axis.
    std::smatch m;
    const std::regex re(
        // `[^;]*` spans the rest of the decode call's arguments (the compat profile's explicit
        // gradients ride after the UV set) and stops at that call's own semicolon.
        R"(uv\.XZ[^;]*\);\s*acc\s*\+=\s*w\.[xyz]\s*\*\s*vec3\(t\.xy\s*\+\s*n\.[xyz][xyz]\s*,\s*)"
        R"(abs\(t\.z\)\s*\*\s*n\.[xyz]\)\.([xyz])([xyz])([xyz])\s*;)");
    ASSERT_TRUE(std::regex_search(src, m, re))
        << "CBT_MaterialNormal's Y-facing block is not in the shape this test can read; re-derive "
           "it rather than relaxing the pattern";

    // Slot 0 of the pre-swizzle vector is the U contribution, slot 1 the V contribution (tangent Y)
    // and slot 2 the projection axis. The swizzle letter sitting on world Z is m[3].
    const char worldZSource = m[3].str()[0];
    EXPECT_EQ(worldZSource, 'y')
        << "the Y-facing projection sends '" << worldZSource
        << "' to world Z, not the tangent V contribution; green would no longer tilt toward +Z and "
           "the A/B record's verdict would no longer describe this shader";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}
