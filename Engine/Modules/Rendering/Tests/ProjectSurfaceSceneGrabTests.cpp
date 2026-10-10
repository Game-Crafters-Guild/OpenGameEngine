// A PROJECT-side surface shader may consume the screen-space grab bindings.
//
// Includes/ibl.glsl declares ge_sceneColor (set 0, binding 22) and includes
// Includes/screen_space.glsl (ge_sceneDepth at set 0, binding 17, plus
// GE_EyeDepthFromRaw) under GE_SCENECOLOR_GRAB, and the forward adapter includes
// ibl.glsl BEFORE the surface shader. Translucent water living in a project
// directory therefore reads the scene depth and the post-opaque grab WITHOUT
// declaring a descriptor set of its own — which Includes/surface_io.glsl forbids and
// which would otherwise risk perturbing the set-0 layout of the depth-only variant.
//
// That reachability is a contract with no other test and no compiler enforcement:
// moving the ibl.glsl include after the surface include, or moving the ge_sceneColor
// declaration, breaks every such surface silently at compose time. These tests pin
// it, in all three variants the material system actually builds:
//   grab      — the transmissive pass, grab succeeded: the block must compile IN
//   no-grab   — the transmissive pass with the grab declined: it must compile OUT
//   depth     — the depth/shadow variant: it must compile OUT
// plus the red arm: the same source WITHOUT the keyword guard must FAIL the no-grab
// variant, proving the guard is load-bearing rather than decorative.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestUtils.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

struct TempTree
{
    fs::path Root;

    explicit TempTree(const char* prefix)
    {
        static std::atomic<uint32_t> counter{0};
        Root = fs::temp_directory_path() /
               (std::string(prefix) + "_" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

// The guard the water shader uses, copied from the adapter's own glass-occlusion
// block: GE_SCENECOLOR_GRAB alone is not enough, because ibl.glsl (which owns the
// declarations) is itself included only for an IBL-enabled StandardPBR variant.
constexpr const char* kGuardedSurface = R"GLSL(
#if defined(GE_SCENECOLOR_GRAB) && defined(GE_IBL_ENABLED) && defined(LIGHTING_MODEL_STANDARD_PBR)
#define GE_PROBE_SCREEN_SPACE 1
#endif

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    o.baseColor = vec3(0.1, 0.2, 0.3);
#ifdef GE_PROBE_SCREEN_SPACE
    ivec2 size = max(textureSize(ge_sceneDepth, 0), ivec2(1));
    ivec2 texel = clamp(ivec2(sIn.screenUV * vec2(size)), ivec2(0), size - ivec2(1));
    float sceneEye = GE_EyeDepthFromRaw(texelFetch(ge_sceneDepth, texel, 0).r, sIn.screenUV);
    float fragEye = GE_EyeDepthFromRaw(gl_FragCoord.z, sIn.screenUV);
    float column = max(sceneEye - fragEye, 0.0);
    o.emissive = textureLod(ge_sceneColor, sIn.screenUV, 0.0).rgb * exp(-column);
#endif
    return o;
}
)GLSL";

// Identical, minus the guard: a surface that assumes the grab is always there.
constexpr const char* kUnguardedSurface = R"GLSL(
SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();
    o.baseColor = vec3(0.1, 0.2, 0.3);
    o.emissive = textureLod(ge_sceneColor, sIn.screenUV, 0.0).rgb;
    return o;
}
)GLSL";

// The keywords the transmissive pass sets on a water material, minus the grab.
// ParseWorldPassKeywords gives the pass the set authored on the node — for the
// shipped ForwardPlus graph the Transmissive node declares ForwardPlus/Instanced/
// Shadows/IBL — and MaterialRegistry adds Transmission from `enableTransmission`.
// Shadows is load-bearing rather than incidental: shadow_sampling.glsl (which owns
// the ge_ReceiveShadows global the adapter assigns) is pulled in by
// clustered_lighting.glsl under HAS_SHADOWS, while the adapter's assignment is
// guarded by HAS_SHADOWS || (FORWARD_PLUS && STANDARD_PBR) — so a Forward+ PBR
// variant without Shadows does not compile at all. The shipped Transmissive node
// always carries Shadows; ForwardPlus_DebugOverlay's world pass does not, and puts
// StandardPBR materials on exactly that uncompilable variant (pre-existing adapter
// defect). Mirroring the real node keeps this suite testing the real variant.
constexpr MaterialKeyword kTransmissiveBase =
    MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
    MaterialKeyword::IBL | MaterialKeyword::Transmission;

std::string ReadFileOrEmpty(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

MaterialBuildResult BuildProjectSurface(const TempTree& tree, const std::string& source,
                                        MaterialKeyword keywords)
{
    const fs::path materialDir = tree.Root / "Materials";
    std::error_code ec;
    fs::create_directories(materialDir, ec);
    {
        std::ofstream out(materialDir / "grab_probe.glsl", std::ios::binary | std::ios::trunc);
        out << source;
    }

    MaterialDocument doc{};
    doc.materialName = "GrabProbe";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "grab_probe.glsl";

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.CacheRoot = tree.Root / "Cache";
    return BuildMaterialToShaderPackage(doc, materialDir / "grab_probe.material", "grab_probe", ctx, ShaderSourceKind::SpirV,
                                        keywords);
}

#define SKIP_WITHOUT_SHADERC(result)                                     \
    do                                                                   \
    {                                                                    \
        for (const auto& e : (result).errors)                            \
            if (e.find("shaderc is not available") != std::string::npos) \
                GTEST_SKIP() << "shaderc not built into this target";    \
    } while (0)

void ExpectBuildSucceeded(const MaterialBuildResult& result)
{
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
}

// shaderc runs at optimization level zero here, so OpName debug info survives and a
// binding's GLSL name is searchable in the fragment SPIR-V.
bool FragmentSpvNames(const MaterialBuildResult& result, const std::string& needle)
{
    const auto& fs = result.package->stageBytes.at("fs");
    return std::search(fs.begin(), fs.end(), needle.begin(), needle.end()) != fs.end();
}

} // namespace

TEST(ProjectSurfaceSceneGrab, GrabVariantReachesSceneColorAndSceneDepth)
{
    TempTree tree("ge_grab_on");
    const auto result = BuildProjectSurface(tree, kGuardedSurface,
                                            kTransmissiveBase | MaterialKeyword::SceneColorGrab);
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
    // Positive control: without these the test would pass on a variant that quietly
    // compiled the block out, which is exactly the failure it exists to catch.
    EXPECT_TRUE(FragmentSpvNames(result, "ge_sceneColor"))
        << "the grab variant must actually bind ge_sceneColor for the surface to sample";
    EXPECT_TRUE(FragmentSpvNames(result, "ge_sceneDepth"))
        << "the grab variant must actually bind ge_sceneDepth for the water column";
}

TEST(ProjectSurfaceSceneGrab, NoGrabVariantCompilesWithTheBlockOut)
{
    // The transmissive pass drops the SceneColorGrab keyword whenever the grab
    // declines (a multisampled source, or no copy pipeline). The same surface source
    // must still build, and must not name a binding the pass will not bind.
    TempTree tree("ge_grab_off");
    const auto result = BuildProjectSurface(tree, kGuardedSurface, kTransmissiveBase);
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
    EXPECT_FALSE(FragmentSpvNames(result, "ge_sceneColor"))
        << "binding 22 must not appear in a variant whose pass does not bind it";
}

TEST(ProjectSurfaceSceneGrab, DepthOnlyVariantCompiles)
{
    // The material system builds depth variants for every material, and the depth
    // recorder strips the tangent/UV1 attributes on the way. The surface body is
    // still compiled into that stage, so the guard has to hold there too.
    TempTree tree("ge_grab_depth");
    const auto result = BuildProjectSurface(
        tree, kGuardedSurface, MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment);
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
}

TEST(ProjectSurfaceSceneGrab, UnguardedSurfaceFailsWithoutTheGrabKeyword)
{
    // The red arm. Dropping the keyword guard must be a COMPILE error, not a silent
    // black frame — if this ever passes, the guard above has stopped being the thing
    // that makes the other three tests meaningful.
    TempTree tree("ge_grab_unguarded");
    const auto result = BuildProjectSurface(tree, kUnguardedSurface, kTransmissiveBase);
    SKIP_WITHOUT_SHADERC(result);
    EXPECT_FALSE(result.success)
        << "an unguarded ge_sceneColor reference must fail the no-grab variant";
    // Attribute the failure. A red arm that goes red on an unrelated compile error is
    // a false instrument, and this one did exactly that before the keyword set above
    // was corrected to match the shipped Transmissive node.
    bool namesTheBinding = false;
    for (const auto& e : result.errors)
        if (e.find("ge_sceneColor") != std::string::npos)
            namesTheBinding = true;
    EXPECT_TRUE(namesTheBinding) << "the build must fail ON ge_sceneColor, not on something else:\n"
                                 << [&] {
                                        std::string all;
                                        for (const auto& e : result.errors)
                                            all += e + "\n";
                                        return all;
                                    }();
}

TEST(ProjectSurfaceSceneGrab, UnguardedSurfaceStillCompilesWithTheGrabKeyword)
{
    // The other half of the red arm: the unguarded source is otherwise valid, so the
    // failure above is attributable to the missing binding and nothing else.
    TempTree tree("ge_grab_unguarded_on");
    const auto result = BuildProjectSurface(tree, kUnguardedSurface,
                                            kTransmissiveBase | MaterialKeyword::SceneColorGrab);
    SKIP_WITHOUT_SHADERC(result);
    ExpectBuildSucceeded(result);
}

TEST(ProjectSurfaceSceneGrab, ExternalSurfaceCompilesInEveryVariant)
{
    // Smoke-compile a real project-side surface that cannot live in this repo (game
    // content). Point GE_PROJECT_SURFACE_GLSL at the .glsl and it is built in all
    // three variants the material system produces for a transmissive material. Same
    // opt-in shape as MaterialBridgeIntegrationTests' external converted-graph test.
    const char* external = std::getenv("GE_PROJECT_SURFACE_GLSL");
    if (!external || !*external)
        GTEST_SKIP() << "set GE_PROJECT_SURFACE_GLSL to a project surface .glsl to exercise this";

    const std::string source = ReadFileOrEmpty(fs::path(external));
    ASSERT_FALSE(source.empty()) << "could not read " << external;

    const std::pair<const char*, MaterialKeyword> variants[] = {
        {"grab", kTransmissiveBase | MaterialKeyword::SceneColorGrab},
        {"no-grab", kTransmissiveBase},
        {"depth-only", MaterialKeyword::Instanced | MaterialKeyword::DepthOnlyFragment},
    };
    for (const auto& [name, keywords] : variants)
    {
        SCOPED_TRACE(name);
        TempTree tree("ge_external_surface");
        const auto result = BuildProjectSurface(tree, source, keywords);
        SKIP_WITHOUT_SHADERC(result);
        ExpectBuildSucceeded(result);
    }
}
