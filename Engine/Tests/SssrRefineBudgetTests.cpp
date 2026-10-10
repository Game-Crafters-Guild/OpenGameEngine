// Refine-budget contract on sssr_intersect.comp.
//
// The march resolves a crossing by bisecting the bracket [previousT, t]. Whatever
// bracket is left over is the distance the reflected image quantises to, so a
// budget expressed as a fixed number of halvings makes the residual proportional
// to the coverage budget (maxDistance / maxSteps). At the shipped defaults that
// residual spans several pixels, and because the mirror branch traced every pixel
// from the same start offset it was screen-COHERENT — reflections of curved
// geometry banded into concentric iso-distance rings. The budget is therefore
// targeted in pixels at the hit, and the start offset is dithered on every
// surface, not only glossy ones.
//
// The constants are parsed out of the shader rather than mirrored here: a
// hand-copied mirror is exactly what drifts. Reads the repo shader source via
// GE_RENDERER_REPO_ROOT (dev-only anchor, same precedent as
// IblShaderContractTests): a staged copy only refreshes when its staging target
// rebuilds, which a shader-only edit does not trigger.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace
{

std::string ReadShaderSource()
{
#ifndef GE_RENDERER_REPO_ROOT
    return {};
#else
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Modules/Rendering/Shaders/ScreenSpaceReflections/sssr_intersect.comp";
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
#endif
}

// `const <type> <name> = <value>;` — the shader's own declaration is the source
// of truth for every number this test reasons about.
std::optional<double> ParseShaderConstant(const std::string& source, const std::string& name)
{
    const std::size_t at = source.find(name + " = ");
    if (at == std::string::npos)
        return std::nullopt;
    const std::size_t begin = at + name.size() + 3;
    const std::size_t end = source.find(';', begin);
    if (end == std::string::npos)
        return std::nullopt;
    try
    {
        return std::stod(source.substr(begin, end - begin));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

struct RefinePolicy
{
    int MinSteps = 0;
    int MaxSteps = 0;
    double TargetPixels = 0.0;

    // Mirrors RefineStepsFor() in sssr_intersect.comp.
    int StepsFor(double bracket, double pixelWorld) const
    {
        const double target = std::max(TargetPixels * pixelWorld, 1e-5);
        const double raw = std::ceil(std::log2(std::max(bracket / target, 1.0)));
        return std::clamp(static_cast<int>(raw), MinSteps, MaxSteps);
    }
};

// ge_proj[1][1] == 1/tan(fovY/2) and w_clip == z_view under the engine's LH ZO
// reverse-Z projection, so one pixel spans this many world units at depth z.
double PixelWorldSize(double depthVS, double fovYDegrees, double heightPixels)
{
    const double proj11 = 1.0 / std::tan(fovYDegrees * 0.5 * 3.14159265358979323846 / 180.0);
    return 2.0 * depthVS / (proj11 * heightPixels);
}

// One march step at HZB mip 0. The first crossing brackets [0, t0] with the
// jittered start t0 up to 1.5 baseStep, which is the widest bracket the refine
// ever sees; every later bracket is exactly one baseStep.
constexpr double kWidestBracketInStartSteps = 1.5;

double WidestBracket(double maxDistance, int maxSteps)
{
    return kWidestBracketInStartSteps * (maxDistance / static_cast<double>(maxSteps));
}

// The parameter envelope the sub-pixel guarantee is asserted over: realistic
// authoring plus camera/viewport setups the editor and Player actually produce.
// Deliberately NARROWER than the load clamps — MaxDistance loads unclamped
// above and ScreenSpaceReflectionsEffect::kMinSteps is 8 — so extreme authoring
// (maxSteps 8 with maxDistance beyond ~200 viewed close up) can push the needed
// halvings past kMaxRefineSteps and the residual past a pixel. Outside this
// envelope the policy degrades to noise rather than rings: the floor keeps
// every configuration at least as fine as the old fixed budget, and the
// dithered start keeps a cap-bound residual incoherent.
constexpr double kMaxDistances[] = {1.0, 10.0, 50.0, 100.0, 200.0};
constexpr int kMaxStepCounts[] = {24, 48, 96, 192};
constexpr double kDepths[] = {0.25, 1.0, 4.0, 20.0, 200.0};
constexpr double kFovYDegrees[] = {30.0, 55.0, 100.0};
constexpr double kViewportHeights[] = {540.0, 938.0, 2160.0};

// The budget the shader replaced.
constexpr int kPreviousFixedBudget = 6;

RefinePolicy LoadPolicy(const std::string& source)
{
    RefinePolicy policy;
    policy.MinSteps = static_cast<int>(ParseShaderConstant(source, "kMinRefineSteps").value_or(0));
    policy.MaxSteps = static_cast<int>(ParseShaderConstant(source, "kMaxRefineSteps").value_or(0));
    policy.TargetPixels = ParseShaderConstant(source, "kRefineTargetPixels").value_or(0.0);
    return policy;
}

} // namespace

// The refine loop must run the computed budget. A literal trip count is the
// defect: it ties the residual to maxDistance/maxSteps instead of to a pixel.
TEST(SssrRefineBudgetContract, RefineLoopRunsTheComputedBudget)
{
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty()) << "sssr_intersect.comp not found via GE_RENDERER_REPO_ROOT";

    EXPECT_NE(source.find("int refineSteps = RefineStepsFor(hi - lo, rayPos.z);"),
              std::string::npos)
        << "the crossing refine must size its budget from the bracket and the hit depth";
    EXPECT_NE(source.find("refine < refineSteps"), std::string::npos)
        << "the refine loop must be bounded by the computed budget";
    EXPECT_EQ(source.find("for (int refine = 0; refine < 6;"), std::string::npos)
        << "a literal refine budget quantises the hit to bracket/2^6 — the banding defect";
}

// The march start offset dithers the sampling grid along an already-fixed ray.
// Gating it on the mirror branch (as the VNDF direction correctly is) gave every
// mirror pixel the same t-grid, which is what made the residual band rather than
// average away.
TEST(SssrRefineBudgetContract, StartOffsetIsDitheredOnMirrorsToo)
{
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty()) << "sssr_intersect.comp not found via GE_RENDERER_REPO_ROOT";

    const std::size_t at = source.find("float startJitter");
    ASSERT_NE(at, std::string::npos) << "start offset dither missing from the march";
    const std::size_t end = source.find(';', at);
    ASSERT_NE(end, std::string::npos);
    const std::string decl = source.substr(at, end - at);

    EXPECT_EQ(decl.find("isMirror"), std::string::npos)
        << "the start offset must not be gated on the mirror branch: pinning it makes the "
           "refine residual screen-coherent and reflections band";
    EXPECT_NE(decl.find("GE_BlueNoise"), std::string::npos)
        << "the start offset must come from the per-pixel, per-frame blue-noise sequence";
}

// The dither must span a full step, or the bracket phase stays partly correlated
// across pixels and the residual keeps a coherent component.
TEST(SssrRefineBudgetContract, StartOffsetSpansAFullStep)
{
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty()) << "sssr_intersect.comp not found via GE_RENDERER_REPO_ROOT";
    EXPECT_NE(source.find("float t = baseStep * (0.5 + startJitter);"), std::string::npos)
        << "start offset must be uniform over one full baseStep (mean unchanged at 1 baseStep)";
}

// The guarantee itself: across the authored parameter envelope the leftover
// bracket lands inside one pixel at the hit.
TEST(SssrRefineBudgetContract, ResidualStaysSubPixelAcrossTheAuthoredEnvelope)
{
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty()) << "sssr_intersect.comp not found via GE_RENDERER_REPO_ROOT";
    const RefinePolicy policy = LoadPolicy(source);
    ASSERT_GT(policy.MaxSteps, 0);
    ASSERT_GT(policy.TargetPixels, 0.0);

    double worstPixels = 0.0;
    int worstBudget = 0;
    for (double maxDistance : kMaxDistances)
        for (int maxSteps : kMaxStepCounts)
            for (double depth : kDepths)
                for (double fovY : kFovYDegrees)
                    for (double height : kViewportHeights)
                    {
                        const double bracket = WidestBracket(maxDistance, maxSteps);
                        const double pixelWorld = PixelWorldSize(depth, fovY, height);
                        const int budget = policy.StepsFor(bracket, pixelWorld);
                        const double residualPixels =
                            bracket / std::pow(2.0, budget) / pixelWorld;
                        if (residualPixels > worstPixels)
                        {
                            worstPixels = residualPixels;
                            worstBudget = budget;
                        }
                        ASSERT_LE(residualPixels, 1.0)
                            << "hit quantises to " << residualPixels << " px at maxDistance "
                            << maxDistance << ", maxSteps " << maxSteps << ", depth " << depth
                            << ", fovY " << fovY << ", height " << height << " (budget "
                            << budget << ")";
                    }
    EXPECT_LT(worstBudget, policy.MaxSteps)
        << "the cap binds inside the authored envelope, so the sub-pixel guarantee rests on "
           "the clamp rather than on the target; worst residual " << worstPixels << " px";
}

// Discrimination: the same envelope under the budget this replaced. Without this
// the test above could pass on a policy that never actually improved anything.
TEST(SssrRefineBudgetContract, PreviousFixedBudgetViolatedTheGuarantee)
{
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty()) << "sssr_intersect.comp not found via GE_RENDERER_REPO_ROOT";
    const RefinePolicy policy = LoadPolicy(source);
    ASSERT_GT(policy.MaxSteps, 0);

    // The scene the artifact was reported from: maxDistance 100, maxSteps 96, a
    // 55-degree 938 px viewport, hit about 4 m out.
    const double bracket = WidestBracket(100.0, 96);
    const double pixelWorld = PixelWorldSize(4.0, 55.0, 938.0);
    const double oldResidualPixels =
        bracket / std::pow(2.0, kPreviousFixedBudget) / pixelWorld;
    EXPECT_GT(oldResidualPixels, 3.0)
        << "the reported banding needs a multi-pixel residual to exist; got "
        << oldResidualPixels;

    const double newResidualPixels =
        bracket / std::pow(2.0, policy.StepsFor(bracket, pixelWorld)) / pixelWorld;
    EXPECT_LT(newResidualPixels, policy.TargetPixels)
        << "the shipped policy must resolve that same crossing inside its pixel target";
}

// The policy may only ever refine harder than the budget it replaced, so no
// configuration gets a coarser hit than it had before.
TEST(SssrRefineBudgetContract, BudgetNeverFallsBelowThePreviousFixedCount)
{
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty()) << "sssr_intersect.comp not found via GE_RENDERER_REPO_ROOT";
    const RefinePolicy policy = LoadPolicy(source);
    EXPECT_GE(policy.MinSteps, kPreviousFixedBudget)
        << "a floor below the previous fixed budget would make some configurations coarser";

    for (double maxDistance : kMaxDistances)
        for (int maxSteps : kMaxStepCounts)
            for (double depth : kDepths)
            {
                const int budget = policy.StepsFor(WidestBracket(maxDistance, maxSteps),
                                                   PixelWorldSize(depth, 55.0, 938.0));
                ASSERT_GE(budget, kPreviousFixedBudget);
            }
}
