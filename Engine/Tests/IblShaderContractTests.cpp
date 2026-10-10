// No-sky ambient-floor contract on ibl.glsl: the engine forces Env.iblIntensity
// to 0 whenever no environment source has active content (RenderServicesWorldPass,
// IBLGenNode, the ocean and grass contributors), so a GE_EvaluateIBL-tail multiply
// by Env.iblIntensity silently kills the authored AmbientLight floor in exactly the
// no-sky scenes it exists for. The intensity therefore lives INSIDE the environment
// samplers — only env-sourced radiance scales — and the floor is added after
// sampling. This test pins that shape of the shader source so a refactor cannot
// silently re-couple the floor to the sky's intensity slider.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT (dev-only anchor, same
// precedent as RenderPipelineCompilerTests' rendergraph fleet gate): a staged copy
// only refreshes when its staging target rebuilds, which a shader-only edit does
// not trigger — the source file is the artifact under test and is never stale.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip //-comments so prose mentioning Env.iblIntensity never counts as a use.
std::string StripLineComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    std::size_t pos = 0;
    while (pos < source.size())
    {
        const std::size_t comment = source.find("//", pos);
        const std::size_t lineEnd = source.find('\n', pos);
        if (comment == std::string::npos || (lineEnd != std::string::npos && comment > lineEnd))
        {
            if (lineEnd == std::string::npos)
            {
                out.append(source, pos, std::string::npos);
                break;
            }
            out.append(source, pos, lineEnd + 1 - pos);
            pos = lineEnd + 1;
            continue;
        }
        out.append(source, pos, comment - pos);
        if (lineEnd == std::string::npos)
            break;
        out.push_back('\n');
        pos = lineEnd + 1;
    }
    return out;
}

// Function body = from the signature to the first '}' at column 0 (house shader style).
std::string ExtractFunctionBody(const std::string& source, const std::string& signature)
{
    const std::size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const std::size_t end = source.find("\n}", begin);
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end + 2 - begin);
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// Lets a test pin a composed EXPRESSION without pinning its formatting.
std::string StripWhitespace(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    for (const char c : source)
        if (!std::isspace(static_cast<unsigned char>(c)))
            out.push_back(c);
    return out;
}

std::string LoadIblGlslWithoutComments()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/Includes/ibl.glsl";
    return StripLineComments(ReadTextFile(path));
#endif
}

} // namespace

// The env samplers own the intensity scale: with no active sky the engine zeroes
// Env.iblIntensity, and only these two functions may consume it.
TEST(IblShaderContract, EnvSamplersFoldIblIntensity)
{
    const std::string source = LoadIblGlslWithoutComments();
    ASSERT_FALSE(source.empty()) << "ibl.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::string irradiance =
        ExtractFunctionBody(source, "vec3 GE_SampleEnvironmentIrradiance");
    ASSERT_FALSE(irradiance.empty());
    EXPECT_NE(irradiance.find("Env.iblIntensity"), std::string::npos)
        << "GE_SampleEnvironmentIrradiance must scale by Env.iblIntensity";

    const std::string prefilter =
        ExtractFunctionBody(source, "vec3 GE_SampleEnvironmentPrefilter");
    ASSERT_FALSE(prefilter.empty());
    EXPECT_NE(prefilter.find("Env.iblIntensity"), std::string::npos)
        << "GE_SampleEnvironmentPrefilter must scale by Env.iblIntensity";
}

// No use of Env.iblIntensity outside the two samplers: a tail multiply on the
// GE_EvaluateIBL returns would zero the AmbientLight floor in no-sky scenes
// (the engine forces the intensity to 0 there) — the exact shipped regression
// this contract exists to prevent.
TEST(IblShaderContract, NoIblIntensityUseOutsideEnvSamplers)
{
    const std::string source = LoadIblGlslWithoutComments();
    ASSERT_FALSE(source.empty()) << "ibl.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::size_t total = CountOccurrences(source, "Env.iblIntensity");
    const std::size_t inIrradiance = CountOccurrences(
        ExtractFunctionBody(source, "vec3 GE_SampleEnvironmentIrradiance"), "Env.iblIntensity");
    const std::size_t inPrefilter = CountOccurrences(
        ExtractFunctionBody(source, "vec3 GE_SampleEnvironmentPrefilter"), "Env.iblIntensity");
    EXPECT_EQ(total, inIrradiance + inPrefilter)
        << "Env.iblIntensity referenced outside the env samplers — a tail rescale "
           "re-couples the ambient floor to the sky intensity and kills it in no-sky scenes";
}

// The floor joins the diffuse budget AFTER the (intensity-scaled) irradiance
// sample, never inside it: (irr + GE_AmbientFloor(N)) keeps the floor alive at
// iblIntensity == 0 and exactly zero without an AmbientLight component.
TEST(IblShaderContract, AmbientFloorAddedOutsideSampledIrradiance)
{
    const std::string source = LoadIblGlslWithoutComments();
    ASSERT_FALSE(source.empty()) << "ibl.glsl not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(source.find("irr + GE_AmbientFloor("), std::string::npos)
        << "diffuse budget must be (sampled irradiance + additive floor)";
}

// Material occlusion reaches indirect diffuse in EVERY variant. so.ao is the material's
// occlusion map (ORM.r); the screen-space GTAO only min()s into it (gtao_consume.glsl),
// and its keyword is set per-view only where an AO PostProcessVolume is active
// (WorldRenderNode). Branching the ambient term on GE_GTAO_ENABLED therefore discards
// occlusion maps in the default editor and every scene without an AO volume — the
// shipped regression this pins. Occlusion is an ambient-only term, so the assertion is
// deliberately about GE_EvaluateIBL and nothing in the direct lighting path.
TEST(IblShaderContract, IndirectDiffuseOcclusionIsNotGatedOnTheGtaoKeyword)
{
    const std::string source = LoadIblGlslWithoutComments();
    ASSERT_FALSE(source.empty()) << "ibl.glsl not found via GE_RENDERER_REPO_ROOT";

    EXPECT_EQ(CountOccurrences(source, "GE_GTAO_ENABLED"), 0u)
        << "ibl.glsl must not branch on the GTAO keyword: the occlusion response is "
           "variant-independent, and a #ifdef here drops material occlusion maps in "
           "every scene without an AO volume";

    const std::string body = ExtractFunctionBody(source, "vec3 GE_EvaluateIBL");
    ASSERT_FALSE(body.empty()) << "GE_EvaluateIBL not found";
    EXPECT_EQ(CountOccurrences(body, "vec3 baseAmbient ="), 1u)
        << "one unconditional baseAmbient definition — a second means a keyword branch "
           "put the occlusion term on only one side";
    EXPECT_NE(body.find("GE_GtaoMultiBounce(so.ao, so.baseColor)"), std::string::npos)
        << "indirect diffuse must carry the surface occlusion";
}

// Full visibility must be an EXACT no-op, because this term now multiplies the indirect
// diffuse of EVERY surface rather than only AO-volume views: the Jimenez fit overshoots
// 1.0 by ~1e-4 at visibility 1 for albedo above 0.8, so without the upper clamp content
// carrying no occlusion (so.ao == 1.0) would gain energy from this change.
// Pin the COMPOSED expression, not its parts: merely mentioning vec3(1.0) also matches
// max(max(vec3(visibility), fit), vec3(1.0)), which FLOORS the result at 1.0 — occlusion
// then darkens nothing and the term can only brighten. min() has to be the outer call.
// Boundary: this pins the source shape. There is no in-process GLSL evaluator here, so
// the numeric claim itself rests on the fit's closed form, not on execution.
TEST(IblShaderContract, MultiBounceCannotBrightenAtFullVisibility)
{
    const std::string source = LoadIblGlslWithoutComments();
    ASSERT_FALSE(source.empty()) << "ibl.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::string body =
        StripWhitespace(ExtractFunctionBody(source, "vec3 GE_GtaoMultiBounce"));
    ASSERT_FALSE(body.empty()) << "GE_GtaoMultiBounce not found";
    EXPECT_NE(body.find("min(max(vec3(visibility),fit),vec3(1.0))"), std::string::npos)
        << "GE_GtaoMultiBounce must return min(max(vec3(visibility), fit), vec3(1.0)): a "
           "lower bound at the visibility and an UPPER bound at 1.0, so full visibility is "
           "an exact no-op and occlusion can never brighten";
}
