// The authored per-terrain grass wind seed, and the rule that every noise field in the wind path
// takes it.
//
// The defect these pin: the gust fBm offset its sample point by the seed, but the cross-wind
// deflection, the strength detail octave and the flutter phase all sampled the noise directly. A
// seed the author changed moved the gusts and left the resting lean, the cross-wind direction and
// the flutter byte-identical on every terrain in the world — a knob that silently did a quarter of
// its job. Seeding one site and not the others is invisible in a screenshot of a single terrain,
// which is why the wiring is asserted here rather than left to the eye.
//
// Two independent instruments, because they fail in different directions:
//   * WindSeedOffset EXECUTES the shipped expression, lifted verbatim out of the shader at build
//     time (ExtractShaderBlock.cmake). It is what pins seed 0 as the identity — the claim that
//     unseeded content is bit-unchanged by the seeding fix.
//   * GrassWindSeedWiring SCANS the shader for the call sites. Execution of one function cannot
//     see whether the OTHER three fields call it, and that omission was the whole defect.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
using GameEngine::GlslShim::vec2;

namespace Shader
{
using GameEngine::GlslShim::vec2;
#include "GrassWindSeedOffsetExtracted.h"
} // namespace Shader

std::string ReadVertexModifierRaw()
{
    const std::string path =
        std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) + "/terrain_grass_vertex_modifier.glsl";
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "could not open " << path;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Line and block comments removed, so a call site that only APPEARS in prose cannot satisfy an
// assertion about the code. Mirrors the stripper GrassPlacementModelTests uses.
std::string StripComments(const std::string& src)
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
        else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/'))
                ++i;
            i = i + 2 < src.size() ? i + 2 : src.size();
        }
        else
        {
            out += src[i];
            ++i;
        }
    }
    return out;
}

// Every occurrence of `callee(`, brace-matched so a nested call comes back whole, split into the
// function's own DEFINITION and its CALL sites. The split matters: a definition's parameter list
// mentions none of the arguments a call passes, so folding the two together would make an
// "every site passes the seed" assertion fail on the definition and pass on nothing.
struct CalleeSites
{
    std::vector<std::string> CallArgs;
    size_t Definitions = 0;
};

CalleeSites FindSites(const std::string& src, const std::string& callee)
{
    CalleeSites sites;
    const std::string needle = callee + "(";
    size_t at = 0;
    while ((at = src.find(needle, at)) != std::string::npos)
    {
        // Reject an identifier that merely ENDS with the callee's name (snoise vs mysnoise).
        const bool boundedLeft = at == 0 || (!std::isalnum(static_cast<unsigned char>(src[at - 1])) &&
                                             src[at - 1] != '_');
        size_t i = at + needle.size();
        int depth = 1;
        const size_t start = i;
        for (; i < src.size() && depth > 0; ++i)
        {
            if (src[i] == '(')
                ++depth;
            else if (src[i] == ')')
                --depth;
        }
        if (boundedLeft && depth == 0)
        {
            // A definition's parameter list is followed by its body; a call is followed by an
            // operator, a separator or a terminator. Nothing else in GLSL puts `{` there.
            size_t after = i;
            while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after])))
                ++after;
            if (after < src.size() && src[after] == '{')
                ++sites.Definitions;
            else
                sites.CallArgs.push_back(src.substr(start, i - 1 - start));
        }
        at = at + needle.size();
    }
    return sites;
}

bool Mentions(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

// Collapse whitespace so an argument spanning a wrapped line compares as one token run.
std::string Squash(const std::string& text)
{
    std::string out;
    bool pendingSpace = false;
    for (char c : text)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace)
            out += ' ';
        pendingSpace = false;
        out += c;
    }
    return out;
}

} // namespace

// Seed 0 is the identity. This is the whole basis for "content that never authored a seed is
// bit-unchanged by the fix" — and it is a claim about the SHIPPED expression, so it is executed
// rather than restated. NOTE that the component's default is 3.0f, not 0 (TerrainGrass.h), so the
// unchanged set is content authored to exactly 0, not content that left the field alone.
TEST(WindSeedOffset, SeedZeroIsTheIdentityOffset)
{
    const vec2 offset = Shader::windSeedOffset(0.0f);
    EXPECT_FLOAT_EQ(offset.x, 0.0f);
    EXPECT_FLOAT_EQ(offset.y, 0.0f);
}

// Two different authored seeds have to land the noise in genuinely different places. Simplex noise
// has unit-scale features, so an offset that differed by a fraction of a unit would leave two
// terrains visibly correlated even though the arithmetic "used" the seed.
TEST(WindSeedOffset, DistinctSeedsSeparateTheFieldByManyNoiseFeatures)
{
    const vec2 a = Shader::windSeedOffset(1.0f);
    const vec2 b = Shader::windSeedOffset(2.0f);
    const vec2 delta = b - a;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    // One unit of authored seed must move the sample point by many noise features. 8 is a floor
    // chosen well below the shipped separation (~36), not a restatement of it.
    EXPECT_GT(distance, 8.0f) << "adjacent seeds sample nearly the same noise";
}

// The two axes must not move together, or the offset walks a single diagonal line through the
// noise and two seeds an integer apart can alias onto similar structure.
TEST(WindSeedOffset, TheTwoAxesMoveInOppositeDirections)
{
    const vec2 offset = Shader::windSeedOffset(1.0f);
    EXPECT_GT(offset.x, 0.0f);
    EXPECT_LT(offset.y, 0.0f);
}

// THE WIRING TEST. Every noise evaluation in the wind path must be offset by the seed. The
// extracted-function tests above pass whether or not anything calls it, so this is the assertion
// that actually fails when a field is left unseeded — the original defect.
//
// The wind path evaluates snoise at four sites: the fBm octave inside sfbm (offset once on the way
// in, so its own argument is the bare running point), the strength detail octave, the cross-wind
// deflection, and the flutter phase. Three of those pass the offset in the argument; sfbm applies
// it to its parameter first.
TEST(GrassWindSeedWiring, EverySnoiseSiteInTheWindPathIsOffsetByTheSeed)
{
    const std::string src = StripComments(ReadVertexModifierRaw());

    // Positive controls: a scanner that read the wrong file, or matched nothing, would satisfy
    // every loop below vacuously.
    ASSERT_GT(src.size(), 200u) << "the vertex modifier read back empty or stripped to nothing";
    ASSERT_TRUE(Mentions(src, "windSeedOffset"))
        << "the shader has no seed offset helper at all - this test's premise has changed";

    const CalleeSites noise = FindSites(src, "snoise");
    const std::vector<std::string>& noiseArgs = noise.CallArgs;
    // The definition count is the scanner's own control: snoise is defined in this file, so a
    // scanner that classified every site as a call (or as a definition) shows up here rather than
    // silently reshaping the call list below.
    ASSERT_EQ(noise.Definitions, 1u) << "expected snoise to be defined exactly once in this file";
    // Reconciled against the shader's four wind-path noise sites. A new site is a deliberate
    // decision about whether it is seeded, so it should land here and be classified, not slip
    // through a loop that only checks the sites that already existed.
    ASSERT_EQ(noiseArgs.size(), 4u)
        << "the number of snoise call sites changed; classify the new one as seeded or not";

    size_t seededInArgument = 0;
    size_t bareRunningPoint = 0;
    for (const std::string& arg : noiseArgs)
    {
        if (Mentions(arg, "windSeedOffset"))
            ++seededInArgument;
        else if (Squash(arg) == "p")
            ++bareRunningPoint;
        else
            ADD_FAILURE() << "snoise(" << Squash(arg)
                          << ") samples the wind field without the authored seed offset, so this "
                             "field is byte-identical on every terrain regardless of WindSeed";
    }

    EXPECT_EQ(seededInArgument, 3u)
        << "expected the detail octave, the cross-wind deflection and the flutter phase to carry "
           "the offset in their argument";
    EXPECT_EQ(bareRunningPoint, 1u) << "expected exactly one site (the fBm octave) to sample a "
                                       "point that was offset before the loop";

    // The one site that does NOT carry the offset in its argument is only correct because sfbm
    // offsets its parameter on the way in. Pin that, or the classification above becomes a licence
    // to leave a bare snoise(p) unseeded.
    EXPECT_TRUE(Mentions(src, "p += windSeedOffset("))
        << "sfbm no longer offsets its sample point, so its octaves are unseeded";
}

// The deflection is the site the defect lived at, and it feeds BOTH the animated wind direction and
// the static resting lean. Pin its seed by name rather than only by the aggregate count above, so a
// future edit that seeds some other site instead cannot keep the totals looking right.
TEST(GrassWindSeedWiring, TheCrossWindDeflectionTakesTheSeedFromItsCaller)
{
    const std::string src = StripComments(ReadVertexModifierRaw());

    const CalleeSites deflect = FindSites(src, "windDeflectedDir");
    ASSERT_EQ(deflect.Definitions, 1u)
        << "expected windDeflectedDir to be defined exactly once in this file";
    // Two callers: the animated field (sampleWindField) and the static resting lean
    // (restingLeanDir). Both must pass the terrain's authored seed.
    ASSERT_EQ(deflect.CallArgs.size(), 2u)
        << "expected exactly the animated field and the resting lean to deflect the wind direction";
    for (const std::string& call : deflect.CallArgs)
        EXPECT_TRUE(Mentions(call, "GrassWindSeed"))
            << "windDeflectedDir(" << Squash(call)
            << ") does not pass the authored seed, so the resting lean and cross-wind direction are "
               "identical on every terrain";
}
