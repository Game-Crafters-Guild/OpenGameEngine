// Phase-parity contract for the LOD crossfade dither.
//
// The depth prepass and the colour pass both draw a fading instance's tail
// records, and the design only holds if they keep EXACTLY the same fragments:
// the prepass writes each level's depth on its own half of the dither, and the
// colour pass's GreaterOrEqual test then passes on that same half. If the two
// tests ever diverge — a forked helper, a different ordered-dither basis, a
// screen position derived from anything but gl_FragCoord, a different fade code
// — each level loses depth on part of its half and the surface shreds. That is
// the mutual z-kill the tail split exists to remove, reintroduced silently.
//
// The guarantee is structural: there is ONE definition of GE_LodCrossfadeKeep,
// it lives in a shared include, it reads nothing but its parameters, and every
// call site passes the identical argument expression. This pins that shape, so a
// refactor that breaks it reds here rather than in a capture.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT (dev-only anchor, same
// precedent as IblShaderContractTests): a staged copy only refreshes when its
// staging target rebuilds, which a shader-only edit does not trigger — the
// source file is the artifact under test and is never stale.

#include <gtest/gtest.h>

#include "Engine/Rendering/DepthDrawRecorder.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr const char* kKeepSignature = "bool GE_LodCrossfadeKeep(";
constexpr const char* kKeepCall      = "GE_LodCrossfadeKeep(vLodFadeCode, gl_FragCoord.xy)";
constexpr const char* kSharedInclude = "Engine/Modules/Rendering/Shaders/Includes/lod_crossfade.glsl";
constexpr const char* kForwardAdapter =
    "Engine/Modules/Rendering/Shaders/Adapters/adapter_forward.glsl";

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip //-comments so prose naming a symbol never counts as a use.
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

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

std::filesystem::path ShaderRoot()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    return std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders";
#endif
}

std::string LoadShaderWithoutComments(const char* repoRelative)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)repoRelative;
    return {};
#else
    return StripLineComments(
        ReadTextFile(std::filesystem::path(GE_RENDERER_REPO_ROOT) / repoRelative));
#endif
}

// Every .glsl/.comp/.vert/.frag under Shaders/, comments stripped.
std::vector<std::pair<std::string, std::string>> LoadShaderTree()
{
    std::vector<std::pair<std::string, std::string>> out;
    const std::filesystem::path root = ShaderRoot();
    if (root.empty() || !std::filesystem::exists(root))
        return out;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".glsl" && ext != ".comp" && ext != ".vert" && ext != ".frag")
            continue;
        out.emplace_back(entry.path().generic_string(),
                         StripLineComments(ReadTextFile(entry.path())));
    }
    return out;
}

// Function body = from the signature to the first '}' at column 0 (house style).
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

} // namespace

// One definition, in the shared include. A second copy is how the two passes
// start disagreeing, and it would not fail any compile.
TEST(LodCrossfadeShaderContract, TheDitherIsDefinedExactlyOnceAndInTheSharedInclude)
{
    const auto tree = LoadShaderTree();
    ASSERT_FALSE(tree.empty()) << "no shader sources found via GE_RENDERER_REPO_ROOT";

    std::vector<std::string> definers;
    for (const auto& [path, source] : tree)
    {
        const std::size_t count = CountOccurrences(source, kKeepSignature);
        for (std::size_t i = 0; i < count; ++i)
            definers.push_back(path);
    }
    ASSERT_EQ(definers.size(), 1u)
        << "GE_LodCrossfadeKeep must have exactly one definition; the depth pass and the "
           "colour pass compile the same one, and a fork silently splits the dither";
    EXPECT_NE(definers[0].find("Includes/lod_crossfade.glsl"), std::string::npos)
        << "the definition must live in the shared include both stages pull in, not in an "
           "adapter (found in " << definers[0] << ")";
}

// The dither must be a pure function of its arguments. Reading a varying or
// gl_FragCoord directly inside the body would make it stage-specific — exactly
// the coupling that lets one pass drift from the other.
TEST(LodCrossfadeShaderContract, TheDitherReadsNothingButItsParameters)
{
    const std::string shared = LoadShaderWithoutComments(kSharedInclude);
    ASSERT_FALSE(shared.empty()) << "lod_crossfade.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::string body = ExtractFunctionBody(shared, kKeepSignature);
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(body.find("vLodFadeCode"), std::string::npos)
        << "the dither must take the fade code as a parameter, never read the varying";
    EXPECT_EQ(body.find("gl_FragCoord"), std::string::npos)
        << "the dither must take the screen position as a parameter, never read gl_FragCoord";

    // The two phases are exact complements of one comparison; anything else
    // leaves pixels covered twice or not at all.
    EXPECT_NE(body.find("(d < weight) : (d >= weight)"), std::string::npos)
        << "the phase pair must be the strict complement of one threshold test";
}

// Every call site passes the identical expression. This is what makes "same
// function" mean "same answer": same record code, same pixel basis.
TEST(LodCrossfadeShaderContract, DepthAndColourCallTheDitherWithIdenticalArguments)
{
    const std::string adapter = LoadShaderWithoutComments(kForwardAdapter);
    ASSERT_FALSE(adapter.empty()) << "adapter_forward.glsl not found via GE_RENDERER_REPO_ROOT";

    // Four sites: the opaque depth-only early-out, the opaque motion early-out,
    // the shared coverage branch the masked depth-only and masked motion
    // variants both take, and the colour branch. A fading tail must dither
    // identically in all four — one that writes motion where the prepass wrote
    // no depth hands the temporal resolve an exact vector for a surface the
    // frame never shaded.
    EXPECT_EQ(CountOccurrences(adapter, "GE_LodCrossfadeKeep("), 4u)
        << "expected the dither at exactly the four adapter call sites";
    EXPECT_EQ(CountOccurrences(adapter, kKeepCall), 4u)
        << "every call site must pass (vLodFadeCode, gl_FragCoord.xy) — a different screen "
           "basis or fade source in one pass breaks phase parity with the other";

    // At least one site sits under the depth-only guard and at least one does
    // not, so neither pass can lose the discard while the other keeps it.
    const std::size_t depthGuard = adapter.find("GE_DEPTH_ONLY_FRAGMENT");
    ASSERT_NE(depthGuard, std::string::npos);
    const std::size_t colourCall = adapter.rfind(kKeepCall);
    ASSERT_NE(colourCall, std::string::npos);
    const std::size_t firstCall = adapter.find(kKeepCall);
    EXPECT_LT(firstCall, colourCall)
        << "the depth-only call sites must precede the colour one in the adapter";
}

// The prepass draw of an opaque fading tail must cost the dither and nothing
// else: no material fetch, no surface evaluation. The early-out is what keeps
// "only fading segments pay a fragment stage" from becoming "fading segments
// pay a full forward shade in the depth pass too".
TEST(LodCrossfadeShaderContract, AnOpaqueDepthOnlyCrossfadeFragmentShadesNothing)
{
    const std::string adapter = LoadShaderWithoutComments(kForwardAdapter);
    ASSERT_FALSE(adapter.empty()) << "adapter_forward.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::size_t guard = adapter.find(
        "#if defined(GE_DEPTH_ONLY_FRAGMENT) && defined(GE_LOD_CROSSFADE) && !defined(ALPHA_TEST)");
    ASSERT_NE(guard, std::string::npos)
        << "the opaque depth-only crossfade early-out must be guarded on exactly that triple";

    const std::size_t endGuard = adapter.find("#endif", guard);
    ASSERT_NE(endGuard, std::string::npos);
    const std::string block = adapter.substr(guard, endGuard - guard);
    EXPECT_NE(block.find(kKeepCall), std::string::npos);
    EXPECT_NE(block.find("return;"), std::string::npos)
        << "the early-out must return before the material fetch";
    EXPECT_EQ(block.find("EvaluateSurface"), std::string::npos);
    EXPECT_EQ(block.find("MaterialParams"), std::string::npos);
    EXPECT_LT(guard, adapter.find("ge_MatData = MaterialParams"))
        << "the early-out must precede the material SSBO fetch";
}

// The C++ half of phase parity: the depth recorder must hand a crossfading
// segment a per-material variant carrying LodCrossfade, and must never hand it
// the shared minimal depth pipeline — that VS emits no fade code, so its draw
// would write solid depth for both levels and z-kill the colour pass's dither on
// half of each. Settled segments keep the fragment-shader-free fast path, which
// is what scopes the cost to fading segments.
TEST(LodCrossfadeShaderContract, ACrossfadingDepthSegmentTakesTheDitherVariantNotTheSharedPipeline)
{
    using GameEngine::Engine::Renderer::ChooseDepthSegmentPipeline;
    using GameEngine::Rendering::HasKeyword;
    using GameEngine::Rendering::MaterialKeyword;

    // Opaque material: heads take the shared depth pipeline and no fragment.
    const auto opaqueHead = ChooseDepthSegmentPipeline(
        MaterialKeyword::Instanced, /*headUsesSharedDepth=*/true,
        /*headComposesFragment=*/false, /*crossfading=*/false);
    EXPECT_TRUE(opaqueHead.UseSharedDepth);
    EXPECT_FALSE(opaqueHead.ComposesFragment);
    EXPECT_FALSE(HasKeyword(opaqueHead.Keywords, MaterialKeyword::LodCrossfade))
        << "a settled segment must not compile the dither's discard";

    const auto opaqueTail = ChooseDepthSegmentPipeline(
        MaterialKeyword::Instanced, /*headUsesSharedDepth=*/true,
        /*headComposesFragment=*/false, /*crossfading=*/true);
    EXPECT_FALSE(opaqueTail.UseSharedDepth)
        << "the shared depth VS carries no fade code — a tail on it cannot dither";
    EXPECT_TRUE(opaqueTail.ComposesFragment);
    EXPECT_TRUE(HasKeyword(opaqueTail.Keywords, MaterialKeyword::LodCrossfade));
    EXPECT_TRUE(HasKeyword(opaqueTail.Keywords, MaterialKeyword::DepthOnlyFragment))
        << "the depth dither needs a fragment stage with no colour attachment";
    EXPECT_TRUE(HasKeyword(opaqueTail.Keywords, MaterialKeyword::Instanced))
        << "the pass keywords must survive";

    // Masked material: the head already composes a fragment (alpha discard); the
    // tail composes both discards, so its coverage is the same conjunction the
    // colour pass applies.
    const MaterialKeyword maskedHeadKeywords =
        MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment;
    const auto maskedTail = ChooseDepthSegmentPipeline(
        maskedHeadKeywords, /*headUsesSharedDepth=*/false,
        /*headComposesFragment=*/true, /*crossfading=*/true);
    EXPECT_TRUE(HasKeyword(maskedTail.Keywords, MaterialKeyword::DepthOnlyFragment));
    EXPECT_TRUE(HasKeyword(maskedTail.Keywords, MaterialKeyword::LodCrossfade));

    // The glass tint cascade keeps its own fragment mode; nothing there fades
    // (shadow slices publish no tail), but the rule must not corrupt its
    // keywords if a caller ever asks.
    const MaterialKeyword tintHead =
        MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyTransmissionColor;
    const auto tintSettled = ChooseDepthSegmentPipeline(
        tintHead, /*headUsesSharedDepth=*/false, /*headComposesFragment=*/true,
        /*crossfading=*/false);
    EXPECT_EQ(tintSettled.Keywords, tintHead);
    EXPECT_TRUE(tintSettled.ComposesFragment);
}
